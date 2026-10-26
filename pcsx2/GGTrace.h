// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

// gg-trace: the Lockstep Trace recorder for a Guilty Gear X Plus native port (the trace format is documented in
// the port's LOCKSTEP.md). Given a lockstep map (GG_TRACE_MAP, made by the port's tools), it
// watches a few EE addresses in the game's own code:
//   - the top of main()'s loop, where one field ends: it hashes EE RAM and the scratchpad there (State Hash v1,
//     with the map's masks) and writes the field's record;
//   - every call the game makes into an SDK library the port replaces with a shim: the return value and the
//     bytes the PS2 wrote back that the game reads (pad data, sound driver replies, ...), in call order.
// The trace goes to GG_TRACE. Inputs come from GGScript (GG_SCRIPT) or from a person at the pad; either way the
// trace records them as the game read them. GG_TRACE_DUMP=<field>[,<field>...] also writes the masked EE RAM and
// scratchpad at those fields next to the trace (game data: never commit them), for diffing a mismatch.
// Hooks run in both the interpreter and the recompiler. Needs proper testing beyond GGX Plus.
namespace GGTrace
{
	extern bool g_enabled;
	extern u8 g_hook_bits[0x02000000 / 4 / 8];

	/// True if pc is one of the map's addresses.
	static inline bool IsHook(u32 pc)
	{
		return g_enabled && pc < 0x02000000u && ((g_hook_bits[pc >> 5] >> ((pc >> 2) & 7)) & 1);
	}

	/// Reads GG_TRACE_MAP and opens GG_TRACE. Called on the CPU thread when the VM boots.
	void Initialize();

	/// Called with cpuRegs.pc at a hook, before the instruction there runs (interpreter and recompiler).
	void OnHook();

	/// Called on the CPU thread when the VM shuts down: writes the trace's end record.
	void Shutdown();
} // namespace GGTrace
