// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ControlBridgeState.h"
#include <span>

enum class VMState;

namespace ControlBridge
{
	bool Enabled();
	bool OwnsPad(); // CPU thread only.
	bool InExecution(); // CPU thread only; includes the remainder of a paused VSync callback.
	bool AllowVMStateChange(VMState state);
	void Invalidate();
	void ExecutionStarted();
	void ExecutionStopped();
	void BoundaryCompleted();
	void Poll();
	bool FinishInputPoll(bool previously_owned);
	void Disconnect(uint64_t connection); // IPC thread, queues cleanup without waiting.
	std::vector<uint8_t> Transact(uint64_t connection, std::span<const uint8_t> payload);
} // namespace ControlBridge
