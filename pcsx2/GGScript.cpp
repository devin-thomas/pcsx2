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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
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
//   stick <lx> <ly> <rx> <ry>  analog sticks, as the bytes the pad reports (decimal 0..255, up/left low, down/right high;
//                            this build's rest value is 127). Held until the next stick line. ggmod addition.
//
// Input log (GG_INPUT_LOG=<file>): while no script is playing (none given, or after the given one has run out
// without quitting), port 1's pad state is written to <file> every frame, run-length encoded, in the format above:
// numpad directions with "face right" (6 = Right), P K S H D for Square Cross Triangle Circle R1, PS2 names joined
// with '+' when other buttons are held. These are the button bits the game reads; the left stick is folded into
// the directions past half-way (the replay then has no analog stick, so a take played on the stick may not replay
// exactly; the log says so at the end). "rec start <name>" / "rec stop" go in at the frame the touchpad or F10
// toggled the layer recording, and "quit" at the frame the VM shut down. Fed back as GG_SCRIPT from the same start
// (the same save state, or the same script followed by the log), it plays the take again frame for frame.
// An existing file is never overwritten: <file> becomes <stem>-2<ext>, -3, ...
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
		Stick,
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
	static u32 s_mask = 0;
	static bool s_quit = false;
	static u64 s_frame = 0;
	static std::string s_script_path;
	static u8 s_stick[4] = {127, 127, 127, 127}; // lx ly rx ry bytes (ggmod)
	static u8 s_logged_stick[4] = {127, 127, 127, 127};
	static bool s_log_all = false; // GG_INPUT_LOG_ALL: log from frame 1 even while a script plays

	// Input log
	static const char* s_log_path = nullptr;
	static std::FILE* s_log = nullptr;
	static u64 s_log_frame = 0; // frames logged
	static u64 s_log_first = 0; // run frame of the first logged frame
	static u32 s_run_mask = 0;
	static u32 s_run_len = 0;
	static u64 s_run_start = 0;
	static bool s_log_rec = false; // layer recording on at the previous logged frame
	static u64 s_stick_frames = 0; // frames where the left stick added a direction the d-pad didn't have
	static std::mutex s_rec_mutex;
	static std::string s_rec_name;

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
			else if (cmd == "stick" && tok.size() == 5)
			{
				u32 v[4];
				for (int k = 0; k < 4; k++)
				{
					const std::optional<u32> n = StringUtil::FromChars<u32>(tok[k + 1]);
					if (!n.has_value() || n.value() > 255)
						return bad();
					v[k] = n.value();
				}
				s_steps.push_back({Kind::Stick, 0, v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24), {}, line_no});
			}
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

	// One stick axis: byte b is what PadDualshock2::Set reports for the value we hand it (its MERGE macro): above 127 the
	// "positive" input r gives 127 + (r + 1) / 2, below 127 the "negative" input r gives 127 - r / 2. The scale is 1.0 in
	// our pad settings (AxisScale = 1.00), so value * 255 is the raw byte.
	static void SetAxis(u32 pos, u32 neg, u8 b)
	{
		u32 rp = 0, rn = 0;
		if (b > 127)
			rp = 2 * (b - 127u) - 1;
		else if (b < 127)
			rn = 2 * (127u - b);
		Pad::SetControllerState(0, pos, rp ? (rp + 0.5f) / 255.0f : 0.0f);
		Pad::SetControllerState(0, neg, rn ? (rn + 0.5f) / 255.0f : 0.0f);
	}

	static void ApplySticks()
	{
		using I = PadDualshock2;
		SetAxis(I::PAD_L_RIGHT, I::PAD_L_LEFT, s_stick[0]);
		SetAxis(I::PAD_L_DOWN, I::PAD_L_UP, s_stick[1]);
		SetAxis(I::PAD_R_RIGHT, I::PAD_R_LEFT, s_stick[2]);
		SetAxis(I::PAD_R_DOWN, I::PAD_R_UP, s_stick[3]);
	}

	// Applied every frame, so a controller touched during a script can't leave a button held. Sticks follow the last
	// `stick` line (centred until there is one).
	static void ApplyButtons(u32 mask)
	{
		for (u32 i = 0; i <= PadDualshock2::PAD_R3; i++)
			Pad::SetControllerState(0, i, (mask & (1u << i)) ? 1.0f : 0.0f);
		ApplySticks();
	}

	// ------------------------------------------------------------------ input log

	// PadDualshock2::bitmaskMapping (private there): the bit of PadDualshock2::GetButtons() for each input, active low.
	static constexpr u8 s_button_bit[PadDualshock2::PAD_R3 + 1] = {12, 13, 14, 15, 4, 5, 6, 7, 8, 11, 2, 0, 3, 1, 9, 10};
	static constexpr u8 s_stick_fold = 128; // left stick past half-way counts as that direction

	// One frame's input in script notation: "5", "2D", "9", "6PK", or "Right+Start" when other buttons are held.
	static std::string Describe(u32 mask)
	{
		using I = PadDualshock2::Inputs;
		const u32 up = Bit(I::PAD_UP), right = Bit(I::PAD_RIGHT), down = Bit(I::PAD_DOWN), left = Bit(I::PAD_LEFT);
		static constexpr std::pair<char, I> letters[] = {
			{'P', I::PAD_SQUARE}, {'K', I::PAD_CROSS}, {'S', I::PAD_TRIANGLE}, {'H', I::PAD_CIRCLE}, {'D', I::PAD_R1}};
		u32 letter_mask = 0;
		for (const auto& [c, i] : letters)
			letter_mask |= Bit(i);
		const u32 dirs = mask & (up | right | down | left);
		const bool opposed = (dirs & (up | down)) == (up | down) || (dirs & (left | right)) == (left | right);
		std::string out;
		if (!opposed && (mask & ~(dirs | letter_mask)) == 0)
		{
			const int x = (dirs & right) ? 1 : (dirs & left) ? -1 : 0;
			const int y = (dirs & up) ? 1 : (dirs & down) ? -1 : 0;
			const char digit = static_cast<char>('1' + (y + 1) * 3 + (x + 1));
			if (digit != '5' || (mask & letter_mask) == 0)
				out += digit;
			for (const auto& [c, i] : letters)
			{
				if (mask & Bit(i))
					out += c;
			}
			return out;
		}
		static constexpr std::pair<std::string_view, I> names[] = {
			{"Up", I::PAD_UP}, {"Right", I::PAD_RIGHT}, {"Down", I::PAD_DOWN}, {"Left", I::PAD_LEFT},
			{"Triangle", I::PAD_TRIANGLE}, {"Circle", I::PAD_CIRCLE}, {"Cross", I::PAD_CROSS}, {"Square", I::PAD_SQUARE},
			{"Select", I::PAD_SELECT}, {"Start", I::PAD_START}, {"L1", I::PAD_L1}, {"L2", I::PAD_L2},
			{"R1", I::PAD_R1}, {"R2", I::PAD_R2}, {"L3", I::PAD_L3}, {"R3", I::PAD_R3},
		};
		for (const auto& [n, i] : names)
		{
			if (mask & Bit(i))
			{
				if (!out.empty())
					out += '+';
				out += n;
			}
		}
		return out;
	}

	static void LogLine(const std::string& step, u64 frame)
	{
		std::fprintf(s_log, "%-16s # f%llu\n", step.c_str(), static_cast<unsigned long long>(frame));
		std::fflush(s_log);
	}

	static void FlushRun()
	{
		if (s_run_len == 0)
			return;
		LogLine(s_run_mask == 0 ? fmt::format("wait {}", s_run_len) : fmt::format("{} {}", s_run_len, Describe(s_run_mask)),
			s_run_start);
		s_run_len = 0;
	}

	static bool OpenLog()
	{
		const std::string_view full(s_log_path);
		const std::string_view ext = Path::GetExtension(full);
		const std::string_view stem = ext.empty() ? full : full.substr(0, full.size() - ext.size() - 1);
		std::string path(full);
		for (int n = 2; FileSystem::FileExists(path.c_str()); n++)
			path = ext.empty() ? fmt::format("{}-{}", stem, n) : fmt::format("{}-{}.{}", stem, n, ext);
		const std::string dir(Path::GetDirectory(path));
		if (!dir.empty())
			FileSystem::CreateDirectoryPath(dir.c_str(), true);
		s_log = FileSystem::OpenCFile(path.c_str(), "wb");
		if (!s_log)
		{
			Console.Error("gg-log: can't write '%s'", path.c_str());
			Host::AddKeyedOSDMessage("GGLog", fmt::format("Can't write input log {}", path), Host::OSD_ERROR_DURATION);
			return false;
		}

		std::time_t now = std::time(nullptr);
		char stamp[32];
		std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
		std::fprintf(s_log, "# gg-layers input log, port 1, %s. Feed it back as GG_SCRIPT from the same start.\n", stamp);
		std::fprintf(s_log, "# Numpad directions with Sol facing right (6 = Right); P K S H D = Square Cross Triangle Circle R1.\n");
		std::fprintf(s_log, "# '# fN' is the logged frame each step starts on (f1 = the first logged frame).\n");
		if (!s_script_path.empty())
			std::fprintf(s_log, "# Logging started when script %s ran out, on frame %llu of the run.\n", s_script_path.c_str(),
				static_cast<unsigned long long>(s_frame));
		std::fprintf(s_log, "map D=R1\nface right\n");
		std::fflush(s_log);
		s_log_first = s_frame;
		s_log_rec = GSIsLayerRecording();
		Console.WriteLn("gg-log: logging port 1 to '%s' from frame %llu", path.c_str(), static_cast<unsigned long long>(s_frame));
		Host::AddKeyedOSDMessage("GGLog", fmt::format("Logging inputs to {}", Path::GetFileName(path)), Host::OSD_INFO_DURATION);
		return true;
	}

	static void LogFrame()
	{
		if (!s_log && (s_log_frame > 0 || !OpenLog()))
		{
			s_log_path = nullptr;
			return;
		}

		u32 mask = 0;
		PadBase* pad = Pad::GetPad(0, 0);
		if (pad && pad->GetType() == Pad::ControllerType::DualShock2)
		{
			const u32 buttons = pad->GetButtons();
			for (u32 i = 0; i <= PadDualshock2::PAD_R3; i++)
			{
				if (!(buttons & (1u << s_button_bit[i])))
					mask |= 1u << i;
			}
			u32 stick = 0;
			if (pad->GetRawInput(PadDualshock2::PAD_L_UP) >= s_stick_fold)
				stick |= Bit(PadDualshock2::PAD_UP);
			if (pad->GetRawInput(PadDualshock2::PAD_L_DOWN) >= s_stick_fold)
				stick |= Bit(PadDualshock2::PAD_DOWN);
			if (pad->GetRawInput(PadDualshock2::PAD_L_LEFT) >= s_stick_fold)
				stick |= Bit(PadDualshock2::PAD_LEFT);
			if (pad->GetRawInput(PadDualshock2::PAD_L_RIGHT) >= s_stick_fold)
				stick |= Bit(PadDualshock2::PAD_RIGHT);
			if (stick & ~mask)
				s_stick_frames++;
			mask |= stick;
		}

		u8 sb[4] = {127, 127, 127, 127};
		if (pad && pad->GetType() == Pad::ControllerType::DualShock2)
		{
			using I = PadDualshock2;
			// PadDualshock2::Set's MERGE, for the non-inverted sticks
			auto axis = [&](u32 pos, u32 neg) -> u8 {
				const u8 rp = pad->GetRawInput(pos), rn = pad->GetRawInput(neg);
				return rp ? static_cast<u8>(127u + (rp + 1u) / 2u) : static_cast<u8>(127u - rn / 2u);
			};
			sb[0] = axis(I::PAD_L_RIGHT, I::PAD_L_LEFT);
			sb[1] = axis(I::PAD_L_DOWN, I::PAD_L_UP);
			sb[2] = axis(I::PAD_R_RIGHT, I::PAD_R_LEFT);
			sb[3] = axis(I::PAD_R_DOWN, I::PAD_R_UP);
		}
		if (std::memcmp(sb, s_logged_stick, 4) != 0)
		{
			FlushRun();
			LogLine(fmt::format("stick {} {} {} {}", sb[0], sb[1], sb[2], sb[3]), s_log_frame + 1);
			std::memcpy(s_logged_stick, sb, 4);
		}

		s_log_frame++;
		const bool rec = GSIsLayerRecording();
		if (rec != s_log_rec)
		{
			FlushRun();
			std::string name;
			{
				std::unique_lock lock(s_rec_mutex);
				name = s_rec_name;
			}
			LogLine(rec ? fmt::format("rec start {}", name) : std::string("rec stop"), s_log_frame);
			Console.WriteLn("gg-log: frame %llu rec %s", static_cast<unsigned long long>(s_log_frame), rec ? "start" : "stop");
			s_log_rec = rec;
		}
		if (s_run_len > 0 && mask == s_run_mask)
		{
			s_run_len++;
			return;
		}
		FlushRun();
		s_run_mask = mask;
		s_run_len = 1;
		s_run_start = s_log_frame;
	}

	static void CloseLog(const char* last_step)
	{
		if (!s_log)
			return;
		FlushRun();
		if (last_step)
			LogLine(last_step, s_log_frame + 1);
		std::fprintf(s_log, "# %llu frames logged (run frames %llu-%llu).\n", static_cast<unsigned long long>(s_log_frame),
			static_cast<unsigned long long>(s_log_first), static_cast<unsigned long long>(s_log_first + s_log_frame - 1));
		if (s_stick_frames > 0)
			std::fprintf(s_log, "# WARNING: the left stick added a direction on %llu frames. The game read the stick itself, so the replay may differ.\n",
				static_cast<unsigned long long>(s_stick_frames));
		std::fclose(s_log);
		s_log = nullptr;
		Console.WriteLn("gg-log: closed after %llu frames", static_cast<unsigned long long>(s_log_frame));
	}

	static void PlayFrame()
	{
		while (s_left == 0)
		{
			if (s_pos >= s_steps.size())
			{
				s_mask = 0;
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
					s_mask = step.buttons;
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

				case Kind::Stick:
					for (int k = 0; k < 4; k++)
						s_stick[k] = static_cast<u8>(step.buttons >> (8 * k));
					break;

				case Kind::Quit:
					ApplyButtons(0);
					s_done = true;
					s_quit = true;
					Console.WriteLn("gg-script: quit at frame %llu", s_frame);
					Host::RequestVMShutdown(false, false, false);
					return;
			}
		}
		ApplyButtons(s_mask);
		s_left--;
	}
} // namespace GGScript

void GGScript::OnPollInput()
{
	if (!s_loaded)
	{
		s_loaded = true;
		const char* path = std::getenv("GG_SCRIPT");
		s_done = !path || !Load(path);
		if (path)
			s_script_path = path;
		const char* log = std::getenv("GG_INPUT_LOG");
		s_log_path = (log && *log) ? log : nullptr;
		s_log_all = std::getenv("GG_INPUT_LOG_ALL") != nullptr;
	}

	s_frame++;
	if (!s_done)
		PlayFrame();
	if ((s_done || s_log_all) && !s_quit && s_log_path)
		LogFrame();
}

void GGScript::OnLayerRecordingStarted(const std::string& dir)
{
	std::unique_lock lock(s_rec_mutex);
	s_rec_name = Path::GetFileName(dir);
}

void GGScript::OnStateLoaded()
{
	if (!s_log)
		return;
	FlushRun();
	std::fprintf(s_log, "# WARNING: a save state was loaded after frame %llu; the steps after this don't follow from the ones before.\n",
		static_cast<unsigned long long>(s_log_frame));
	std::fflush(s_log);
}

void GGScript::OnShutdown()
{
	CloseLog("quit");
}
