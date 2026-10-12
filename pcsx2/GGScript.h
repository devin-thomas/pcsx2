// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// gg-layers: a frame-by-frame input script for controller port 1, read from the file named by the GG_SCRIPT
// environment variable. It presses buttons in fighting-game numpad notation, starts and stops layer recordings,
// takes screenshots, saves states and quits, so captures can run with nobody at the controller.
// Needs proper testing outside Guilty Gear XX Accent Core Plus.
namespace GGScript
{
	/// Called once per frame on the CPU thread, after host input has been polled.
	void OnPollInput();
} // namespace GGScript
