// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>
#include <vector>

// This state machine has no emulator/thread dependencies. Its sole caller in the
// runtime is the CPU thread; the IPC thread only sees serialized copies.
namespace ControlBridge
{
	constexpr uint16_t VERSION = 1;
	constexpr uint32_t MAX_BOUNDARIES = 300;
	constexpr uint32_t MAX_READ_BYTES = 4096;
	constexpr uint16_t MAX_RANGES = 32;
	constexpr uint64_t LEASE_NS = 10000000000ull;
	enum class Command : uint8_t
	{
		Hello,
		Acquire,
		Step,
		Query,
		Snapshot,
		Cancel,
		Release
	};
	enum class Status : uint16_t
	{
		Ok,
		Pending,
		Disabled,
		Malformed,
		Version,
		Handshake,
		RequestID,
		Epoch,
		Owner,
		Busy,
		VMState,
		Pad,
		Conflict,
		NotNeutral,
		Bounds,
		Cancelled,
		Timeout,
		Disconnected,
		Invalidated,
		Unsupported
	};
	struct Range
	{
		uint32_t address;
		uint32_t length;
	};
	struct Request
	{
		uint16_t version = VERSION;
		Command command = Command::Hello;
		uint8_t port = 0;
		uint64_t epoch = 0;
		uint64_t id = 0;
		uint32_t mask = 0;
		uint32_t count = 0;
		std::vector<Range> ranges;
	};
	struct Receipt
	{
		Status status = Status::Ok;
		Status action_status = Status::Ok;
		uint64_t epoch = 1;
		uint64_t request = 0;
		uint64_t action = 0;
		uint64_t action_epoch = 0;
		uint64_t current = 0;
		uint64_t applied = 0;
		uint64_t completed = 0;
		uint64_t applied_ns = 0;
		uint64_t completed_ns = 0;
		uint32_t executed_mask = 0;
		uint32_t released_mask = 0;
		uint32_t requested_count = 0;
		uint32_t completed_count = 0;
		bool leased = false;
		bool applied_valid = false;
		bool completed_valid = false;
		std::vector<uint8_t> data;
	};
	class Environment
	{
	public:
		virtual ~Environment() = default;
		virtual bool Paused() const = 0;
		virtual bool Executing() const = 0;
		virtual bool SupportedPad() const = 0;
		virtual bool Conflict() const = 0;
		virtual bool Neutral() const = 0;
		virtual uint64_t Now() const = 0;
		virtual void Apply(uint16_t mask) = 0;
		virtual void Resume() = 0;
		virtual void Pause() = 0;
		virtual void Synchronize() = 0;
		virtual void Read(const Range& range, std::vector<uint8_t>& data) = 0;
	};
	class State
	{
	public:
		explicit State(uint64_t epoch = 1) { m_receipt.epoch = epoch; }
		bool Leased() const { return m_receipt.leased; }
		bool Active() const { return m_active; }
		uint64_t Connection() const { return m_connection; }

		Receipt Handle(uint64_t connection, const Request& request, Environment& env, uint64_t now)
		{
			Tick(env, now);
			auto reply = [&](Status status) {
				if (status != Status::Ok && status != Status::Pending && Leased() &&
					m_hello && connection == m_connection)
					Abort(env, status);
				Receipt result = m_receipt;
				result.status = status;
				result.request = request.id;
				return result;
			};
			if (request.version != VERSION)
				return reply(Status::Version);
			if (request.port != 0)
				return reply(Status::Pad);
			if (request.command > Command::Release)
				return reply(Status::Unsupported);
			if (request.command == Command::Hello)
			{
				if (Leased() || (m_connection == connection && m_hello))
					return reply(Status::Busy);
				if (!request.id || request.epoch)
					return reply(Status::RequestID);
				m_connection = connection;
				m_last_id = request.id;
				m_hello = true;
				return reply(Status::Ok);
			}
			if (!m_hello || connection != m_connection)
				return reply(Status::Handshake);
			if (!request.id || request.id <= m_last_id)
				return reply(Status::RequestID);
			m_last_id = request.id; // Rejected well-formed requests also consume IDs.
			if (request.epoch != m_receipt.epoch)
				return reply(Status::Epoch);
			if (request.command == Command::Acquire)
			{
				if (Leased())
					return reply(Status::Owner);
				if (!env.Paused() || env.Executing())
					return reply(Status::VMState);
				if (!env.SupportedPad())
					return reply(Status::Pad);
				if (env.Conflict())
					return reply(Status::Conflict);
				if (!env.Neutral())
					return reply(Status::NotNeutral);
				const uint64_t boundary = m_receipt.current;
				const uint64_t epoch = m_receipt.epoch + 1;
				m_receipt = {};
				m_receipt.epoch = epoch;
				m_receipt.current = boundary;
				m_receipt.leased = true;
				env.Apply(0);
				m_deadline = now + LEASE_NS;
				return reply(Status::Ok);
			}
			if (!Leased())
				return reply(Status::Owner);
			if (env.Conflict() || !env.SupportedPad())
			{
				Abort(env, Status::Conflict);
				return reply(Status::Conflict);
			}
			switch (request.command)
			{
				case Command::Step:
					if (m_active)
						return reply(Status::Busy);
					if (!request.count || request.count > MAX_BOUNDARIES || request.mask > 0xffff)
						return reply(Status::Bounds);
					if (!env.Paused() || env.Executing())
						return reply(Status::VMState);
					m_receipt.action = request.id;
					m_receipt.action_epoch = m_receipt.epoch;
					m_receipt.action_status = Status::Pending;
					m_receipt.executed_mask = request.mask;
					m_receipt.released_mask = 0;
					m_receipt.requested_count = request.count;
					m_receipt.completed_count = 0;
					m_receipt.applied = m_receipt.current;
					m_receipt.applied_valid = true;
					m_receipt.completed_valid = false;
					m_receipt.completed = 0;
					m_receipt.completed_ns = 0;
					m_active = true;
					m_finishing = false;
					env.Apply(static_cast<uint16_t>(request.mask));
					m_receipt.applied_ns = env.Now();
					env.Resume();
					m_deadline = now + LEASE_NS;
					return reply(Status::Pending);
				case Command::Query:
					return reply(Status::Ok);
				case Command::Snapshot:
				{
					if (m_active || !env.Paused() || env.Executing() || !m_receipt.completed_valid)
						return reply(Status::Busy);
					uint32_t total = 0;
					if (request.ranges.empty() || request.ranges.size() > MAX_RANGES)
						return reply(Status::Bounds);
					for (const Range& range : request.ranges)
					{
						if (!range.length || range.length > MAX_READ_BYTES || range.address >= 0x2000000 ||
							range.length > 0x2000000 - range.address || range.length > MAX_READ_BYTES - total)
							return reply(Status::Bounds);
						total += range.length;
					}
					env.Synchronize(); // EE stopped, VU/GS drained before copying physical RAM.
					Receipt result = reply(Status::Ok);
					result.data.reserve(total);
					for (const Range& range : request.ranges)
						env.Read(range, result.data);
					m_deadline = now + LEASE_NS;
					return result;
				}
				case Command::Cancel:
					Abort(env, Status::Cancelled);
					return reply(Status::Cancelled);
				case Command::Release:
					if (m_active)
					{
						Abort(env, Status::Cancelled);
						return reply(Status::Cancelled);
					}
					env.Apply(0);
					env.Pause();
					m_receipt.leased = false;
					++m_receipt.epoch;
					return reply(Status::Ok);
				default:
					return reply(Status::Unsupported);
			}
		}

		void Boundary(Environment& env, uint64_t now)
		{
			++m_receipt.current;
			if (m_active && !m_finishing)
				++m_receipt.completed_count;
			Tick(env, now);
			if (!m_active || m_finishing)
				return;
			if (m_receipt.completed_count == m_receipt.requested_count)
			{
				m_finishing = true;
				env.Apply(0);
				env.Pause();
			}
		}
		void ExecutionStopped(Environment& env, uint64_t now)
		{
			Tick(env, now);
			if (!m_active)
				return;
			if (!m_finishing || !env.Paused())
			{
				Abort(env, Status::Invalidated);
				return;
			}
			env.Synchronize();
			Tick(env, env.Now());
			if (!m_active)
				return;
			m_receipt.completed = m_receipt.current;
			m_receipt.completed_ns = env.Now();
			m_receipt.completed_valid = true;
			m_receipt.action_status = Status::Ok;
			m_active = false;
			m_finishing = false;
		}
		void Tick(Environment& env, uint64_t now)
		{
			if (Leased() && now >= m_deadline)
				Abort(env, Status::Timeout);
		}
		void Abort(Environment& env, Status reason)
		{
			if (!Leased())
				return;
			env.Apply(0);
			env.Pause();
			m_receipt.action_status = reason;
			m_receipt.completed_valid = false;
			m_receipt.leased = false;
			m_active = false;
			m_finishing = false;
			++m_receipt.epoch;
		}
		void Disconnect(uint64_t connection, Environment& env)
		{
			if (connection == m_connection)
			{
				Abort(env, Status::Disconnected);
				m_hello = false;
			}
		}
		void Invalidate(Environment& env)
		{
			if (Leased())
				Abort(env, Status::Invalidated);
			else
			{
				++m_receipt.epoch;
				m_receipt.completed_valid = false;
				m_receipt.action_status = Status::Invalidated;
			}
		}

	private:
		Receipt m_receipt;
		uint64_t m_connection = 0;
		uint64_t m_last_id = 0;
		uint64_t m_deadline = 0;
		bool m_hello = false;
		bool m_active = false;
		bool m_finishing = false;
	};
} // namespace ControlBridge
