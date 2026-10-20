// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "ControlBridge.h"
#include "ControlBridgeProtocol.h"
#include "Achievements.h"
#include "Config.h"
#include "GSDumpReplayer.h"
#include "Host.h"
#include "MemoryTypes.h"
#include "MTGS.h"
#include "MTVU.h"
#include "Recording/InputRecording.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "VMManager.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

namespace ControlBridge
{
	static bool s_executing = false;
	static bool s_transition = false;
	static uint64_t Now()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	static State s_state(Now());

	bool Enabled()
	{
		static const bool enabled = [] {
			const char* value = std::getenv("PCSX2_CONTROL_BRIDGE");
			return value && std::strcmp(value, "1") == 0;
		}();
		return enabled;
	}

	class NativeEnvironment final : public Environment
	{
	public:
		bool Paused() const override { return VMManager::GetState() == VMState::Paused; }
		bool Executing() const override { return s_executing; }
		uint64_t Now() const override { return ControlBridge::Now(); }
		bool SupportedPad() const override
		{
			return VMManager::HasValidVM() && Pad::GetPad(0) && Pad::GetPad(0)->GetType() == Pad::ControllerType::DualShock2;
		}
		bool Conflict() const override
		{
			return EmuConfig.EnableRecordingTools || g_InputRecording.isActive() ||
			       VMManager::IsFrameAdvanceInProgress() ||
			       Achievements::IsHardcoreModeActive() || GSDumpReplayer::IsReplayingDump() ||
			       std::getenv("GG_SCRIPT") || std::getenv("GG_INPUT_LOG");
		}
		bool Neutral() const override
		{
			const PadBase* pad = Pad::GetPad(0);
			if ((pad->GetButtons() & 0x3ffff) != 0x3ffff ||
				pad->GetRawLeftAnalog() != std::tuple<u8, u8>{127, 127} ||
				pad->GetRawRightAnalog() != std::tuple<u8, u8>{127, 127})
				return false;
			for (u32 bind = 0; bind < PadDualshock2::Inputs::LENGTH; ++bind)
			{
				if (pad->GetRawInput(bind) != 0)
					return false;
			}
			return true;
		}
		void Apply(uint16_t mask) override
		{
			if (!SupportedPad())
				return; // Lifecycle invalidation can follow pad removal.
			PadBase* pad = Pad::GetPad(0);
			for (u32 bind = 0; bind < PadDualshock2::Inputs::LENGTH; ++bind)
				pad->Set(bind, 0.0f);
			// Wire bits are UI bind order, not the DS2 serial active-low bit order.
			static constexpr u32 binds[] = {
				PadDualshock2::PAD_UP, PadDualshock2::PAD_RIGHT, PadDualshock2::PAD_DOWN, PadDualshock2::PAD_LEFT,
				PadDualshock2::PAD_TRIANGLE, PadDualshock2::PAD_CIRCLE, PadDualshock2::PAD_CROSS, PadDualshock2::PAD_SQUARE,
				PadDualshock2::PAD_SELECT, PadDualshock2::PAD_START, PadDualshock2::PAD_L1, PadDualshock2::PAD_L2,
				PadDualshock2::PAD_R1, PadDualshock2::PAD_R2, PadDualshock2::PAD_L3, PadDualshock2::PAD_R3};
			for (u32 bit = 0; bit < 16; ++bit)
				pad->SetRawPressureButton(binds[bit], {bool(mask & (1u << bit)), (mask & (1u << bit)) ? u8(255) : u8(0)});
			pad->SetRawAnalogs({127, 127}, {127, 127});
		}
		void Resume() override
		{
			s_transition = true;
			VMManager::SetState(VMState::Running);
			s_transition = false;
		}
		void Pause() override
		{
			if (VMManager::GetState() != VMState::Running)
				return;
			s_transition = true;
			VMManager::SetState(VMState::Paused);
			s_transition = false;
		}
		void Synchronize() override
		{
			vu1Thread.WaitVU();
			MTGS::WaitGS(false);
		}
		void Read(const Range& range, std::vector<uint8_t>& data) override
		{
			data.insert(data.end(), eeMem->Main + range.address, eeMem->Main + range.address + range.length);
		}
	};
	static NativeEnvironment s_environment;

	bool OwnsPad() { return Enabled() && s_state.Leased(); }
	bool InExecution() { return Enabled() && s_executing; }

	bool AllowVMStateChange(VMState state)
	{
		if (!OwnsPad() || s_transition)
			return true;
		s_state.Abort(s_environment, Status::Invalidated);
		// An external resume invalidates the lease but must not resume the VM.
		return state != VMState::Running;
	}
	void Invalidate()
	{
		if (Enabled())
			s_state.Invalidate(s_environment);
	}
	void ExecutionStarted() { s_executing = true; }
	void ExecutionStopped()
	{
		s_executing = false;
		if (Enabled())
			s_state.ExecutionStopped(s_environment, Now());
	}
	void BoundaryCompleted()
	{
		if (Enabled())
			s_state.Boundary(s_environment, Now());
	}
	void Poll()
	{
		if (!Enabled())
			return;
		s_state.Tick(s_environment, Now());
		if (s_state.Leased() && (s_environment.Conflict() || !s_environment.SupportedPad()))
			s_state.Abort(s_environment, Status::Conflict);
	}
	bool FinishInputPoll(bool previously_owned)
	{
		Poll();
		if (previously_owned && !s_state.Leased())
			s_environment.Apply(0);
		return previously_owned || OwnsPad();
	}
	void Disconnect(uint64_t connection)
	{
		if (Enabled())
			Host::RunOnCPUThread([connection] { s_state.Disconnect(connection, s_environment); });
	}
	std::vector<uint8_t> Transact(uint64_t connection, std::span<const uint8_t> payload)
	{
		Receipt error;
		error.epoch = 0;
		error.status = Status::Malformed;
		if (!Enabled())
		{
			error.status = Status::Disabled;
			return EncodeReply(error);
		}
		Request request;
		if (!DecodeRequest(payload, request))
		{
			error.request = request.id;
			Disconnect(connection);
			return EncodeReply(error);
		}
		error.request = request.id;
		struct Transaction
		{
			std::mutex mutex;
			std::condition_variable ready;
			bool done = false;
			bool abandoned = false;
			Receipt receipt;
		};
		const auto transaction = std::make_shared<Transaction>();
		Host::RunOnCPUThread([transaction, connection, request = std::move(request)] {
			std::unique_lock lock(transaction->mutex);
			if (transaction->abandoned)
			{
				s_state.Disconnect(connection, s_environment);
				return;
			}
			lock.unlock();
			Receipt result = s_state.Handle(connection, request, s_environment, Now());
			lock.lock();
			if (transaction->abandoned)
			{
				s_state.Disconnect(connection, s_environment);
				return;
			}
			transaction->receipt = std::move(result);
			transaction->done = true;
			transaction->ready.notify_one();
		});
		std::unique_lock lock(transaction->mutex);
		if (!transaction->ready.wait_for(lock, std::chrono::seconds(2), [&] { return transaction->done; }))
		{
			transaction->abandoned = true;
			error.status = Status::Timeout;
			error.action_status = Status::Timeout;
			Disconnect(connection);
			return EncodeReply(error); // No claim about how many boundaries ran.
		}
		return EncodeReply(transaction->receipt);
	}
} // namespace ControlBridge
