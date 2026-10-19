// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string>

// gg-layers: a frame-by-frame input script for controller port 1, read from the file named by the GG_SCRIPT
// environment variable. It presses buttons in fighting-game numpad notation, starts and stops layer recordings,
// takes screenshots, saves states and quits, so captures can run with nobody at the controller.
// GG_INPUT_LOG is the other direction: while no script is playing, port 1's pad state is logged every frame to that
// file in the same format, with the layer recording toggles, so a played take can be replayed as a script.
// Needs proper testing outside Guilty Gear XX Accent Core Plus.
namespace GGScript
{
	/// Called once per frame on the CPU thread, after host input has been polled.
	void OnPollInput();

	/// Called on the GS thread when a layer recording starts (hotkey or script), with its folder.
	void OnLayerRecordingStarted(const std::string& dir);

	/// Called on the CPU thread after a save state is loaded.
	void OnStateLoaded();

	/// Called on the CPU thread when the VM shuts down.
	void OnShutdown();
} // namespace GGScript
