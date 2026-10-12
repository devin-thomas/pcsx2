// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GGScript.h"
#include "GS.h"
#include "GS/GS.h"
#include "Host.h"
#include "MTGS.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include <cstdlib>
#include <string>
#include <vector>

// Script format, one step per line ('#' starts a comment):
//   map D=R1 P=Square        letters P K S H D -> PS2 buttons (defaults P=Square K=Cross S=Triangle H=Circle D=R1)
//   face right|left          numpad directions are relative to the facing (6 = forward)
//   <frames> <input>         hold an input: numpad digit and/or letters, or PS2 names joined with '+',
//                            e.g. "1 9", "3 2D", "10 5", "1 Start", "2 Down+Cross"
//   wait <frames>            neutral
//   rec start [name]         start a GS dump + reference video into $GG_CAPTURE_ROOT/<name>
//   rec stop
//   shot <name>              screenshot to $GG_CAPTURE_ROOT/shots/<name>.png
//   save <path>              save state
//   turbo on|off             run unthrottled (menus) or at normal speed
//   log <text>
//   quit                     shut the VM down (pcsx2-qt -batch then exits)
namespace GGScript
{
	enum class Kind
	{
		Hold,
		RecStart,
		RecStop,
		Shot,
		Save,
		Log,
		Turbo,
		Quit,
	};

	struct Step
	{
		Kind kind;
		u32 frames;
		u32 buttons; // bitmask of PadDualshock2::Inputs
		std::string arg;
		u32 line;
	};

	static bool s_loaded = false;
	static bool s_done = false;
	static std::vector<Step> s_steps;
	static size_t s_pos = 0;
	static u32 s_left = 0;
	static u64 s_frame = 0;

	static constexpr u32 Bit(PadDualshock2::Inputs i)
	{
		return 1u << static_cast<u32>(i);
	}

	static std::string CaptureRoot()
	{
		const char* root = std::getenv("GG_CAPTURE_ROOT");
		return root ? std::string(root) : Path::Combine(EmuFolders::Snapshots, "gg-captures");
	}

	static std::optional<u32> ButtonByName(std::string_view name)
	{
		using I = PadDualshock2::Inputs;
		static constexpr std::pair<std::string_view, I> names[] = {
			{"Up", I::PAD_UP}, {"Right", I::PAD_RIGHT}, {"Down", I::PAD_DOWN}, {"Left", I::PAD_LEFT},
			{"Triangle", I::PAD_TRIANGLE}, {"Circle", I::PAD_CIRCLE}, {"Cross", I::PAD_CROSS}, {"Square", I::PAD_SQUARE},
			{"Select", I::PAD_SELECT}, {"Start", I::PAD_START}, {"L1", I::PAD_L1}, {"L2", I::PAD_L2},
			{"R1", I::PAD_R1}, {"R2", I::PAD_R2}, {"L3", I::PAD_L3}, {"R3", I::PAD_R3},
		};
		for (const auto& [n, i] : names)
		{
			if (StringUtil::compareNoCase(n, name))
				return Bit(i);
		}
		return std::nullopt;
	}

	static bool Load(const char* path)
	{
		const std::optional<std::string> text = FileSystem::ReadFileToString(path);
		if (!text.has_value())
		{
			Console.Error("gg-script: can't read '%s'", path);
			return false;
		}

		u32 letters[5] = {Bit(PadDualshock2::PAD_SQUARE), Bit(PadDualshock2::PAD_CROSS), Bit(PadDualshock2::PAD_TRIANGLE),
			Bit(PadDualshock2::PAD_CIRCLE), Bit(PadDualshock2::PAD_R1)};
		static constexpr std::string_view letter_names = "PKSHD";
		bool face_right = true;

		auto numpad = [&face_right](char c) -> u32 {
			const u32 fwd = Bit(face_right ? PadDualshock2::PAD_RIGHT : PadDualshock2::PAD_LEFT);
			const u32 back = Bit(face_right ? PadDualshock2::PAD_LEFT : PadDualshock2::PAD_RIGHT);
			const u32 up = Bit(PadDualshock2::PAD_UP), down = Bit(PadDualshock2::PAD_DOWN);
			switch (c)
			{
				case '1': return down | back;
				case '2': return down;
				case '3': return down | fwd;
				case '4': return back;
				case '6': return fwd;
				case '7': return up | back;
				case '8': return up;
				case '9': return up | fwd;
				default: return 0;
			}
		};

		u32 line_no = 0;
		for (std::string_view line : StringUtil::SplitString(text.value(), '\n', false))
		{
			line_no++;
			if (const size_t hash = line.find('#'); hash != std::string_view::npos)
				line = line.substr(0, hash);
			line = StringUtil::StripWhitespace(line);
			if (line.empty())
				continue;

			const std::vector<std::string_view> tok = StringUtil::SplitString(line, ' ', true);
			const std::string_view cmd = tok[0];
			const std::string rest = tok.size() > 1 ? std::string(StringUtil::StripWhitespace(line.substr(cmd.size()))) : std::string();
			auto bad = [&]() {
				Console.Error("gg-script: line %u not understood: %.*s", line_no, static_cast<int>(line.size()), line.data());
				return false;
			};

			if (cmd == "map")
			{
				for (size_t i = 1; i < tok.size(); i++)
				{
					const size_t eq = tok[i].find('=');
					const size_t li = (eq == 1) ? letter_names.find(tok[i][0]) : std::string_view::npos;
					const std::optional<u32> b = (li != std::string_view::npos) ? ButtonByName(tok[i].substr(2)) : std::nullopt;
					if (!b.has_value())
						return bad();
					letters[li] = b.value();
				}
			}
			else if (cmd == "face" && tok.size() == 2)
				face_right = (tok[1] != "left");
			else if (cmd == "wait" && tok.size() == 2 && StringUtil::FromChars<u32>(tok[1]).has_value())
				s_steps.push_back({Kind::Hold, StringUtil::FromChars<u32>(tok[1]).value(), 0, {}, line_no});
			else if (cmd == "rec" && tok.size() >= 2 && tok[1] == "start")
				s_steps.push_back({Kind::RecStart, 0, 0, tok.size() > 2 ? std::string(tok[2]) : std::string(), line_no});
			else if (cmd == "rec" && tok.size() == 2 && tok[1] == "stop")
				s_steps.push_back({Kind::RecStop, 0, 0, {}, line_no});
			else if (cmd == "shot" && tok.size() == 2)
				s_steps.push_back({Kind::Shot, 0, 0, std::string(tok[1]), line_no});
			else if (cmd == "save" && !rest.empty())
				s_steps.push_back({Kind::Save, 0, 0, rest, line_no});
			else if (cmd == "turbo" && tok.size() == 2)
				s_steps.push_back({Kind::Turbo, 0, 0, std::string(tok[1]), line_no});
			else if (cmd == "log")
				s_steps.push_back({Kind::Log, 0, 0, rest, line_no});
			else if (cmd == "quit")
				s_steps.push_back({Kind::Quit, 0, 0, {}, line_no});
			else if (const std::optional<u32> frames = StringUtil::FromChars<u32>(cmd); frames.has_value() && tok.size() == 2)
			{
				u32 mask = 0;
				const std::string_view input = tok[1];
				if (input.find('+') != std::string_view::npos || (input.size() > 1 && ButtonByName(input).has_value()))
				{
					for (const std::string_view name : StringUtil::SplitString(input, '+'))
					{
						const std::optional<u32> b = ButtonByName(name);
						if (!b.has_value())
							return bad();
						mask |= b.value();
					}
				}
				else
				{
					for (const char c : input)
					{
						if (c >= '1' && c <= '9')
							mask |= numpad(c);
						else if (const size_t li = letter_names.find(c); li != std::string_view::npos)
							mask |= letters[li];
						else
							return bad();
					}
				}
				s_steps.push_back({Kind::Hold, frames.value(), mask, {}, line_no});
			}
			else
				return bad();
		}

		Console.WriteLn("gg-script: loaded %zu steps from '%s'", s_steps.size(), path);
		return true;
	}

	static void ApplyButtons(u32 mask)
	{
		for (u32 i = 0; i <= PadDualshock2::PAD_R3; i++)
			Pad::SetControllerState(0, i, (mask & (1u << i)) ? 1.0f : 0.0f);
	}
} // namespace GGScript

void GGScript::OnPollInput()
{
	if (!s_loaded)
	{
		s_loaded = true;
		const char* path = std::getenv("GG_SCRIPT");
		s_done = !path || !Load(path);
	}
	if (s_done)
		return;

	s_frame++;
	while (s_left == 0)
	{
		if (s_pos >= s_steps.size())
		{
			ApplyButtons(0);
			s_done = true;
			Console.WriteLn("gg-script: finished at frame %llu", s_frame);
			return;
		}

		const Step& step = s_steps[s_pos++];
		switch (step.kind)
		{
			case Kind::Hold:
				s_left = step.frames;
				ApplyButtons(step.buttons);
				break;

			case Kind::RecStart:
			{
				const std::string name = step.arg.empty() ? fmt::format("script-{}", s_frame) : step.arg;
				const std::string dir = Path::Combine(CaptureRoot(), name);
				MTGS::RunOnGSThread([dir]() { GSStartLayerRecording(dir); });
				MTGS::WaitGS(false, false, false);
				Console.WriteLn("gg-script: frame %llu rec start %s", s_frame, dir.c_str());
				break;
			}

			case Kind::RecStop:
				MTGS::RunOnGSThread([]() { GSStopLayerRecording(); });
				MTGS::WaitGS(false, false, false);
				Console.WriteLn("gg-script: frame %llu rec stop", s_frame);
				break;

			case Kind::Shot:
			{
				const std::string dir = Path::Combine(CaptureRoot(), "shots");
				FileSystem::CreateDirectoryPath(dir.c_str(), false);
				const std::string png = Path::Combine(dir, step.arg + ".png");
				MTGS::RunOnGSThread([png]() { GSQueueSnapshot(png); });
				break;
			}

			case Kind::Save:
				VMManager::SaveState(step.arg.c_str(), false, false, [](const std::string& error) {
					Console.Error("gg-script: save state failed: %s", error.c_str());
				});
				break;

			case Kind::Turbo:
				VMManager::SetLimiterMode(step.arg == "on" ? LimiterModeType::Unlimited : LimiterModeType::Nominal);
				break;

			case Kind::Log:
				Console.WriteLn("gg-script: frame %llu: %s", s_frame, step.arg.c_str());
				break;

			case Kind::Quit:
				ApplyButtons(0);
				s_done = true;
				Console.WriteLn("gg-script: quit at frame %llu", s_frame);
				Host::RequestVMShutdown(false, false, false);
				return;
		}
	}
	s_left--;
}
