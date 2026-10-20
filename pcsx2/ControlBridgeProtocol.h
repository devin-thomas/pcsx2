// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ControlBridgeState.h"
#include <span>

namespace ControlBridge
{
	template <typename T>
	inline void Append(std::vector<uint8_t>& out, T value)
	{
		for (size_t i = 0; i < sizeof(T); ++i)
			out.push_back(static_cast<uint8_t>(value >> (i * 8)));
	}
	template <typename T>
	inline T Take(std::span<const uint8_t> bytes, size_t offset)
	{
		T value = 0;
		for (size_t i = 0; i < sizeof(T); ++i)
			value |= T(bytes[offset + i]) << (i * 8);
		return value;
	}
	inline std::vector<uint8_t> EncodeReply(const Receipt& result)
	{
		std::vector<uint8_t> out;
		Append(out, VERSION);
		Append(out, static_cast<uint16_t>(result.status));
		Append(out, static_cast<uint16_t>(result.action_status));
		Append(out, uint16_t((result.leased ? 1 : 0) | (result.applied_valid ? 2 : 0) | (result.completed_valid ? 4 : 0)));
		Append(out, result.epoch);
		Append(out, result.request);
		Append(out, result.action);
		Append(out, result.current);
		Append(out, result.applied);
		Append(out, result.completed);
		Append(out, result.applied_ns);
		Append(out, result.completed_ns);
		Append(out, result.executed_mask);
		Append(out, result.released_mask);
		Append(out, result.requested_count);
		Append(out, result.completed_count);
		Append(out, MAX_BOUNDARIES);
		Append(out, MAX_READ_BYTES);
		Append(out, MAX_RANGES);
		Append(out, uint16_t(0x000f)); // digital P1, centered sticks, async receipt, physical RAM.
		Append(out, static_cast<uint32_t>(result.data.size()));
		Append(out, result.action_epoch);
		out.insert(out.end(), result.data.begin(), result.data.end());
		return out;
	}

	inline bool DecodeRequest(std::span<const uint8_t> payload, Request& request)
	{
		if (payload.size() < 32)
			return false;
		request.version = Take<uint16_t>(payload, 0);
		request.command = static_cast<Command>(payload[2]);
		request.port = payload[3];
		request.epoch = Take<uint64_t>(payload, 4);
		request.id = Take<uint64_t>(payload, 12);
		request.mask = Take<uint32_t>(payload, 20);
		request.count = Take<uint32_t>(payload, 24);
		const uint16_t ranges = Take<uint16_t>(payload, 28);
		if (ranges > MAX_RANGES || payload.size() != 32u + ranges * 8u || Take<uint16_t>(payload, 30) ||
			(request.command != Command::Snapshot && ranges) ||
			(request.command != Command::Step && (request.mask || request.count)))
			return false;
		request.ranges.clear();
		for (uint16_t i = 0; i < ranges; ++i)
			request.ranges.push_back({Take<uint32_t>(payload, 32 + i * 8), Take<uint32_t>(payload, 36 + i * 8)});
		return true;
	}
} // namespace ControlBridge
