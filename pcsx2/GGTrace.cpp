// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GGTrace.h"
#include "BuildVersion.h"
#include "Config.h"
#include "Counters.h"
#include "Memory.h"
#include "R5900.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

// Trace format version 1 (LOCKSTEP.md in the port's repository). All values little-endian.
//
// Header:
//   0   "GGXTRACE"
//   8   u32 trace format version (1)
//   12  u32 State Hash version (1)
//   16  u32 release: 1 = KR (SLPM-645.25), 2 = JP (SLPS-201.37)
//   20  u32 header size: offset of the first record
//   24  32 bytes: the boot ELF's SHA-256 (from the map)
//   56  u64 XXH3 of the lockstep map file
//   64  u32 mask count
//   68  u32 flags: bit 0 = records carry page hashes (their low 32 bits)
//   72  64 bytes: recorder id, NUL padded
//   136 64 bytes: emulator settings that change results, NUL padded
//   200 mask count x (u32 EE address, u32 size)
//   ... u32 output spec count, then per spec: u8 env id, u8 output count, per output u8 pointer argument
//       (0-7: a0-a3, t0-t3), the argument the call's recorded bytes are written through; with bit 7 set, a u8 env
//       id follows, and the pointer is that argument of the last call to that function (libmc writes
//       sceMcGetInfo's results when sceMcSync completes)
//   ... u32 note length, note bytes (UTF-8): where the inputs came from
// Records, each starting with a tag byte:
//   'F' a field: varint field (0 = the boot, before main()'s loop); u8 PS2 vblanks since the previous record
//       (capped at 255); varint call record count, then per record: u8 env id (bit 7 set: a varint repeat count
//       follows, for a run of identical calls), varint return value (u32), u8 output count, per output u8 length
//       and the bytes; varint transient mask count, then per mask varint address, varint size, varint stride,
//       varint count (count ranges of size bytes, stride apart); u64 State Hash of ram and spr; u64 ram region hash; u64 spr region hash; varint changed page count,
//       then per page varint (index - previous index - 1) and the page hash's low 32 bits (u32). Page indices: 0-8191 ram,
//       8192-8195 spr. Before field 0 every page's hash is that of a zero page.
//   'E' the end: varint fields recorded.
namespace GGTrace
{
	bool g_enabled = false;
	u8 g_hook_bits[0x02000000 / 4 / 8];

	static constexpr u32 RAM_SIZE = 0x02000000;
	static constexpr u32 SPR_BASE = 0x70000000;
	static constexpr u32 SPR_SIZE = 0x4000;
	static constexpr u32 PAGE = 4096;
	static constexpr u32 RAM_PAGES = RAM_SIZE / PAGE;
	static constexpr u32 PAGES = RAM_PAGES + SPR_SIZE / PAGE;
	static constexpr u32 MAX_OUT = 64;

	enum class HookKind : u8
	{
		Field,
		Entry,
		Return,
	};

	struct Hook
	{
		u32 addr;
		u32 check; // the low 16 bits of XXH3-64 of (address, instruction word), both u32 LE
		HookKind kind;
		u8 env; // Entry
	};

	struct OutSpec
	{
		u8 ptr_reg;
		bool len_is_reg;
		u32 len; // bytes, or a register number
		u8 src_env; // nonzero: the pointer is that call's argument, from its last call (libmc's sync-time outputs)
	};

	struct Env
	{
		std::string name;
		std::vector<OutSpec> outs;
	};

	struct Inflight
	{
		u8 start, ptr_reg, len_reg, end;
		u32 mul, end_value;
	};

	struct Pending
	{
		u8 env;
		u32 ra;
		u32 args[8]; // a0-a3, t0-t3
	};

	struct Range
	{
		u32 base, size;
	};

	struct ActiveTransfer
	{
		u8 start;
		Range r;
	};

	// A field of each entry of a structure the game allocates, found through the pointer that holds it.
	struct PtrMask
	{
		u32 ptr, offset, size, stride, count;
		u32 when, when_value; // only while the u32 at when holds when_value (when 0: always)
		u32 key_offset, key_size, key_value; // only entries whose key field holds key_value (key_size 0: all)
	};

	// A transient mask in a field record: count ranges of size bytes, stride apart.
	struct Strided
	{
		u32 base, size, stride, count;
	};

	static std::vector<Hook> s_hooks; // sorted by address
	static std::vector<u32> s_watched; // return addresses, sorted
	static Env s_env[256];
	static std::vector<Inflight> s_inflight;
	static std::vector<Range> s_masks;
	static std::vector<PtrMask> s_ptr_masks;
	static std::vector<Range> s_page_masks[PAGES];
	static std::vector<ActiveTransfer> s_active;
	static std::vector<Pending> s_pending;
	static std::vector<u8> s_record; // the field being recorded: its calls
	static u32 s_calls = 0; // call records (a run of identical calls is one)
	static std::vector<u8> s_last_call; // the previous call's encoding, and how often it repeated
	static u32 s_last_repeat = 0;
	static u64 s_page_hash[PAGES];
	static u32 s_field = 0;
	static bool s_started = false;
	static u32 s_last_vblank = 0;
	static std::FILE* s_out = nullptr;
	static std::string s_out_path;
	static std::vector<u32> s_dump_fields;
	static u32 s_release = 0;
	static u8 s_elf_sha[32];
	static u64 s_map_hash = 0;
	static u32 s_stray = 0; // entries whose return was never seen
	static u32 s_last_args[256][8]; // each env's last call's arguments

	static void PutVarint(std::vector<u8>& v, u64 x)
	{
		while (x >= 0x80)
		{
			v.push_back(static_cast<u8>(x) | 0x80);
			x >>= 7;
		}
		v.push_back(static_cast<u8>(x));
	}

	static void FlushCall();

	static void Put32(std::vector<u8>& v, u32 x)
	{
		for (int i = 0; i < 4; i++)
			v.push_back(static_cast<u8>(x >> (8 * i)));
	}

	static void Put64(std::vector<u8>& v, u64 x)
	{
		Put32(v, static_cast<u32>(x));
		Put32(v, static_cast<u32>(x >> 32));
	}

	static std::optional<u8> RegByName(std::string_view n)
	{
		static constexpr std::string_view names[] = {"a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3"};
		for (u8 i = 0; i < 8; i++)
		{
			if (n == names[i])
				return static_cast<u8>(4 + i);
		}
		return std::nullopt;
	}

	// An EE address the game passed, as an offset into main RAM or the scratchpad (and which), or nullptr.
	static u8* EEPtr(u32 addr, u32 len)
	{
		if ((addr & 0xF0000000u) == SPR_BASE)
		{
			const u32 off = addr & 0x0FFFFFFFu;
			return (off + len <= SPR_SIZE) ? &eeMem->Scratch[off] : nullptr;
		}
		const u32 phys = addr & 0x1FFFFFFFu; // kseg0/1 and the uncached mirrors
		return (phys < RAM_SIZE && phys + len <= RAM_SIZE) ? &eeMem->Main[phys] : nullptr;
	}

	static void SetBit(u32 addr)
	{
		g_hook_bits[addr >> 5] |= static_cast<u8>(1u << ((addr >> 2) & 7));
	}

	static bool LoadMap(const char* path)
	{
		const std::optional<std::string> text = FileSystem::ReadFileToString(path);
		if (!text.has_value())
		{
			Console.Error("gg-trace: can't read map '%s'", path);
			return false;
		}
		s_map_hash = XXH3_64bits(text->data(), text->size());
		u32 line_no = 0;
		bool header = false;
		for (std::string_view line : StringUtil::SplitString(text.value(), '\n', false))
		{
			line_no++;
			if (const size_t hash = line.find('#'); hash != std::string_view::npos)
				line = line.substr(0, hash);
			line = StringUtil::StripWhitespace(line);
			if (line.empty())
				continue;
			const std::vector<std::string_view> t = StringUtil::SplitString(line, ' ', true);
			auto hex = [](std::string_view s) { return StringUtil::FromChars<u32>(s, 16); };
			auto dec = [](std::string_view s) { return StringUtil::FromChars<u32>(s, 10); };
			auto bad = [&]() {
				Console.Error("gg-trace: map line %u not understood: %.*s", line_no, static_cast<int>(line.size()), line.data());
				return false;
			};
			if (t[0] == "ggx-lockstep-map")
			{
				if (t.size() != 2 || t[1] != "1")
					return bad();
				header = true;
			}
			else if (t[0] == "release" && t.size() >= 3 && dec(t[2]))
				s_release = dec(t[2]).value();
			else if (t[0] == "elf_sha256" && t.size() == 2 && t[1].size() == 64)
			{
				for (int i = 0; i < 32; i++)
					s_elf_sha[i] = static_cast<u8>(StringUtil::FromChars<u32>(t[1].substr(i * 2, 2), 16).value_or(0));
			}
			else if (t[0] == "field" && t.size() == 3 && hex(t[1]) && hex(t[2]))
				s_hooks.push_back({hex(t[1]).value(), hex(t[2]).value(), HookKind::Field, 0});
			else if (t[0] == "env" && t.size() >= 6 && dec(t[1]) && hex(t[3]) && hex(t[4]) && dec(t[1]).value() < 256)
			{
				const u8 id = static_cast<u8>(dec(t[1]).value());
				s_env[id].name = std::string(t[2]);
				for (size_t i = 5; i < t.size(); i++)
				{
					if (t[i] == "-")
						continue;
					// [<env id>.]<reg>:<bytes | len reg>
					std::string_view spec = t[i];
					u8 src = 0;
					if (const size_t dot = spec.find('.'); dot != std::string_view::npos)
					{
						if (!dec(spec.substr(0, dot)) || dec(spec.substr(0, dot)).value() >= 256)
							return bad();
						src = static_cast<u8>(dec(spec.substr(0, dot)).value());
						spec = spec.substr(dot + 1);
					}
					const size_t colon = spec.find(':');
					if (colon == std::string_view::npos)
						return bad();
					const std::optional<u8> p = RegByName(spec.substr(0, colon));
					const std::string_view l = spec.substr(colon + 1);
					if (!p.has_value())
						return bad();
					if (const std::optional<u8> lr = RegByName(l))
						s_env[id].outs.push_back({p.value(), true, lr.value(), src});
					else if (dec(l))
						s_env[id].outs.push_back({p.value(), false, dec(l).value(), src});
					else
						return bad();
				}
				s_hooks.push_back({hex(t[3]).value(), hex(t[4]).value(), HookKind::Entry, id});
			}
			else if (t[0] == "ret" && t.size() == 3 && hex(t[1]) && hex(t[2]))
			{
				s_hooks.push_back({hex(t[1]).value(), hex(t[2]).value(), HookKind::Return, 0});
				s_watched.push_back(hex(t[1]).value());
			}
			else if (t[0] == "inflight" && t.size() == 7 && dec(t[1]) && RegByName(t[2]) && RegByName(t[3]) && dec(t[4]) &&
					 dec(t[5]) && dec(t[6]))
			{
				s_inflight.push_back({static_cast<u8>(dec(t[1]).value()), RegByName(t[2]).value(), RegByName(t[3]).value(),
					static_cast<u8>(dec(t[5]).value()), dec(t[4]).value(), dec(t[6]).value()});
			}
			else if (t[0] == "mask" && t.size() >= 3 && hex(t[1]) && hex(t[2]))
				s_masks.push_back({hex(t[1]).value(), hex(t[2]).value()});
			else if (t[0] == "ptrmask" && t.size() >= 11 && hex(t[1]) && hex(t[2]) && hex(t[3]) && hex(t[4]) && dec(t[5]) &&
					 hex(t[6]) && hex(t[7]) && hex(t[8]) && dec(t[9]) && hex(t[10]))
				s_ptr_masks.push_back({hex(t[1]).value(), hex(t[2]).value(), hex(t[3]).value(), hex(t[4]).value(), dec(t[5]).value(),
					hex(t[6]).value(), hex(t[7]).value(), hex(t[8]).value(), dec(t[9]).value(), hex(t[10]).value()});
			else
				return bad();
		}
		if (!header || s_release == 0)
		{
			Console.Error("gg-trace: '%s' is not a lockstep map", path);
			return false;
		}
		std::sort(s_hooks.begin(), s_hooks.end(), [](const Hook& a, const Hook& b) { return a.addr < b.addr; });
		std::sort(s_watched.begin(), s_watched.end());
		for (const Hook& h : s_hooks)
		{
			if (h.addr >= RAM_SIZE || (h.addr & 3))
			{
				Console.Error("gg-trace: hook %08x is not in EE RAM", h.addr);
				return false;
			}
			SetBit(h.addr);
		}
		return true;
	}

	static u32 PageBase(u32 p)
	{
		return p < RAM_PAGES ? p * PAGE : SPR_BASE + (p - RAM_PAGES) * PAGE;
	}

	static const u8* PageData(u32 p)
	{
		return p < RAM_PAGES ? &eeMem->Main[p * PAGE] : &eeMem->Scratch[(p - RAM_PAGES) * PAGE];
	}

	// Adds r's overlap with each page to that page's list.
	static void AddPageMask(std::vector<Range>* lists, const Range& r)
	{
		const u32 first = r.base < RAM_SIZE ? r.base / PAGE : 0;
		for (u32 p = first; p < PAGES; p++)
		{
			if (r.base < RAM_SIZE && p < RAM_PAGES && PageBase(p) >= r.base + r.size)
				break;
			const u32 b = PageBase(p);
			const u32 lo = std::max(b, r.base), hi = std::min(b + PAGE, r.base + r.size);
			if (lo < hi)
				lists[p].push_back({lo - b, hi - lo});
		}
	}

	static u64 HashPage(u32 p, const std::vector<Range>* transient)
	{
		const u8* data = PageData(p);
		if (s_page_masks[p].empty() && (!transient || transient[p].empty()))
			return XXH3_64bits(data, PAGE);
		alignas(64) u8 buf[PAGE];
		std::memcpy(buf, data, PAGE);
		for (const Range& m : s_page_masks[p])
			std::memset(buf + m.base, 0, m.size);
		if (transient)
		{
			for (const Range& m : transient[p])
				std::memset(buf + m.base, 0, m.size);
		}
		return XXH3_64bits(buf, PAGE);
	}

	static u64 RegionHash(u32 id, u32 base, u32 size, const u64* pages, u32 n)
	{
		std::vector<u8> h;
		Put32(h, id);
		Put32(h, base);
		Put32(h, size);
		for (u32 i = 0; i < n; i++)
			Put64(h, pages[i]);
		return XXH3_64bits(h.data(), h.size());
	}

	static void WriteDump(u32 field, const std::vector<Range>* transient)
	{
		const std::string path = fmt::format("{}.f{}.ram", s_out_path, field);
		std::FILE* f = FileSystem::OpenCFile(path.c_str(), "wb");
		if (!f)
			return;
		alignas(64) u8 buf[PAGE];
		for (u32 p = 0; p < PAGES; p++)
		{
			std::memcpy(buf, PageData(p), PAGE);
			for (const Range& m : s_page_masks[p])
				std::memset(buf + m.base, 0, m.size);
			for (const Range& m : transient[p])
				std::memset(buf + m.base, 0, m.size);
			std::fwrite(buf, 1, PAGE, f);
		}
		std::fclose(f);
		Console.WriteLn("gg-trace: wrote %s", path.c_str());
	}

	static void EndField()
	{
		static std::vector<Range> transient[PAGES];
		for (std::vector<Range>& t : transient)
			t.clear();
		FlushCall();
		std::vector<u8> rec;
		rec.push_back('F');
		PutVarint(rec, s_field);
		const u32 vb = g_FrameCount - s_last_vblank;
		s_last_vblank = g_FrameCount;
		rec.push_back(static_cast<u8>(std::min<u32>(vb, 255)));
		PutVarint(rec, s_calls);
		rec.insert(rec.end(), s_record.begin(), s_record.end());
		std::vector<Strided> masks;
		for (const ActiveTransfer& a : s_active)
			masks.push_back({a.r.base, a.r.size, 0, 1});
		for (const PtrMask& m : s_ptr_masks)
		{
			const u32 ptr = *reinterpret_cast<const u32*>(&eeMem->Main[m.ptr]) & 0x1FFFFFFFu;
			if (!ptr || ptr + m.stride * m.count > RAM_SIZE)
				continue;
			if (m.when && *reinterpret_cast<const u32*>(&eeMem->Main[m.when]) != m.when_value)
				continue;
			if (m.key_size == 0)
			{
				masks.push_back({ptr + m.offset, m.size, m.stride, m.count});
				continue;
			}
			for (u32 k = 0; k < m.count; k++)
			{
				u32 key = 0;
				std::memcpy(&key, &eeMem->Main[ptr + k * m.stride + m.key_offset], std::min<u32>(m.key_size, 4));
				if (key == m.key_value)
					masks.push_back({ptr + k * m.stride + m.offset, m.size, 0, 1});
			}
		}
		PutVarint(rec, masks.size());
		for (const Strided& m : masks)
		{
			PutVarint(rec, m.base);
			PutVarint(rec, m.size);
			PutVarint(rec, m.stride);
			PutVarint(rec, m.count);
			for (u32 k = 0; k < m.count; k++)
				AddPageMask(transient, {m.base + k * m.stride, m.size});
		}

		u64 hashes[PAGES];
		for (u32 p = 0; p < PAGES; p++)
			hashes[p] = HashPage(p, transient);
		const u64 ram = RegionHash(1, 0, RAM_SIZE, hashes, RAM_PAGES);
		const u64 spr = RegionHash(2, SPR_BASE, SPR_SIZE, hashes + RAM_PAGES, PAGES - RAM_PAGES);
		std::vector<u8> sh = {'G', 'G', 'X', 'H'};
		Put32(sh, 1);
		Put32(sh, 2);
		Put32(sh, 1);
		Put64(sh, ram);
		Put32(sh, 2);
		Put64(sh, spr);
		Put64(rec, XXH3_64bits(sh.data(), sh.size()));
		Put64(rec, ram);
		Put64(rec, spr);
		u32 changed = 0;
		for (u32 p = 0; p < PAGES; p++)
			changed += hashes[p] != s_page_hash[p];
		PutVarint(rec, changed);
		s64 prev = -1;
		for (u32 p = 0; p < PAGES; p++)
		{
			if (hashes[p] == s_page_hash[p])
				continue;
			PutVarint(rec, static_cast<u64>(static_cast<s64>(p) - prev - 1));
			Put32(rec, static_cast<u32>(hashes[p])); // the low 32 bits: page hashes only locate a mismatch
			prev = p;
			s_page_hash[p] = hashes[p];
		}
		std::fwrite(rec.data(), 1, rec.size(), s_out);
		if (std::find(s_dump_fields.begin(), s_dump_fields.end(), s_field) != s_dump_fields.end())
			WriteDump(s_field, transient);
		if ((s_field % 600) == 0)
		{
			std::fflush(s_out);
			Console.WriteLn("gg-trace: field %u, %u calls, %u pages changed", s_field, s_calls, changed);
		}
		s_record.clear();
		s_calls = 0;
		s_field++;
	}

	// A call record: u8 env id (bit 7 set: a varint repeat count follows), varint return value, the outputs.
	static void FlushCall()
	{
		if (s_last_repeat == 0)
			return;
		if (s_last_repeat == 1)
			s_record.insert(s_record.end(), s_last_call.begin(), s_last_call.end());
		else
		{
			s_record.push_back(static_cast<u8>(s_last_call[0] | 0x80));
			PutVarint(s_record, s_last_repeat);
			s_record.insert(s_record.end(), s_last_call.begin() + 1, s_last_call.end());
		}
		s_calls++;
		s_last_repeat = 0;
	}

	static void OnEntry(const Hook& h)
	{
		const u32 ra = cpuRegs.GPR.n.ra.UL[0];
		if (!std::binary_search(s_watched.begin(), s_watched.end(), ra))
			return; // called from inside the SDK: the port never sees it
		Pending p;
		p.env = h.env;
		p.ra = ra;
		for (int i = 0; i < 8; i++)
			p.args[i] = cpuRegs.GPR.r[4 + i].UL[0];
		if (s_pending.size() > 16)
		{
			s_stray += static_cast<u32>(s_pending.size());
			s_pending.clear();
		}
		s_pending.push_back(p);
	}

	static void OnReturn(u32 pc)
	{
		if (s_pending.empty() || s_pending.back().ra != pc)
			return;
		const Pending p = s_pending.back();
		s_pending.pop_back();
		const u32 v0 = cpuRegs.GPR.n.v0.UL[0];
		std::vector<u8> call;
		PutVarint(call, v0);
		const Env& e = s_env[p.env];
		call.push_back(static_cast<u8>(e.outs.size()));
		auto arg = [&p](u8 reg) { return p.args[reg - 4]; };
		std::memcpy(s_last_args[p.env], p.args, sizeof(p.args));
		for (const OutSpec& o : e.outs)
		{
			const u32 len = std::min<u32>(o.len_is_reg ? arg(static_cast<u8>(o.len)) : o.len, MAX_OUT);
			const u32 ptr = o.src_env ? s_last_args[o.src_env][o.ptr_reg - 4] : arg(o.ptr_reg);
			const u8* src = len ? EEPtr(ptr, len) : nullptr;
			if (!src)
			{
				call.push_back(0);
				continue;
			}
			call.push_back(static_cast<u8>(len));
			call.insert(call.end(), src, src + len);
		}
		call.insert(call.begin(), p.env);
		if (s_last_repeat > 0 && call == s_last_call)
			s_last_repeat++;
		else
		{
			FlushCall();
			s_last_call = std::move(call);
			s_last_repeat = 1;
		}

		for (const Inflight& f : s_inflight)
		{
			if (f.start == p.env)
			{
				const u32 base = arg(f.ptr_reg) & 0x1FFFFFFFu;
				const u32 size = arg(f.len_reg) * f.mul;
				if (base < RAM_SIZE && size)
					s_active.push_back({f.start, {base, std::min(size, RAM_SIZE - base)}});
			}
			if (f.end == p.env && v0 == f.end_value)
			{
				s_active.erase(std::remove_if(s_active.begin(), s_active.end(),
								   [&f](const ActiveTransfer& a) { return a.start == f.start; }),
					s_active.end());
			}
		}
	}

	static std::string Settings()
	{
		static constexpr const char* modes[] = {"nearest", "negative", "positive", "chop"};
		const auto& c = EmuConfig.Cpu;
		return fmt::format("ee={} fpu={} div={} clamp={}{}{} gamefixes={}", c.Recompiler.EnableEE ? "rec" : "interp",
			modes[static_cast<int>(c.FPUFPCR.GetRoundMode())], modes[static_cast<int>(c.FPUDivFPCR.GetRoundMode())],
			c.Recompiler.fpuOverflow ? "o" : "-", c.Recompiler.fpuExtraOverflow ? "x" : "-", c.Recompiler.fpuFullMode ? "f" : "-",
			EmuConfig.EnableGameFixes ? 1 : 0);
	}

	static bool OpenTrace(const char* path)
	{
		const std::string_view full(path);
		const std::string_view ext = Path::GetExtension(full);
		const std::string_view stem = ext.empty() ? full : full.substr(0, full.size() - ext.size() - 1);
		std::string p(full);
		for (int n = 2; FileSystem::FileExists(p.c_str()); n++)
			p = ext.empty() ? fmt::format("{}-{}", stem, n) : fmt::format("{}-{}.{}", stem, n, ext);
		const std::string dir(Path::GetDirectory(p));
		if (!dir.empty())
			FileSystem::CreateDirectoryPath(dir.c_str(), true);
		s_out = FileSystem::OpenCFile(p.c_str(), "wb");
		if (!s_out)
		{
			Console.Error("gg-trace: can't write '%s'", p.c_str());
			return false;
		}
		s_out_path = p;

		const char* note_env = std::getenv("GG_TRACE_NOTE");
		std::string note = note_env ? note_env : "";
		if (const char* script = std::getenv("GG_SCRIPT"))
			note += fmt::format("{}script={}", note.empty() ? "" : "; ", Path::GetFileName(script));
		std::vector<u8> h(200, 0);
		std::memcpy(h.data(), "GGXTRACE", 8);
		auto at32 = [&h](size_t o, u32 v) { std::memcpy(&h[o], &v, 4); };
		at32(8, 1);
		at32(12, 1);
		at32(16, s_release);
		std::memcpy(&h[24], s_elf_sha, 32);
		std::memcpy(&h[56], &s_map_hash, 8);
		at32(64, static_cast<u32>(s_masks.size()));
		at32(68, 1);
		const std::string id = fmt::format("pcsx2-gg-trace 1 {}", BuildVersion::GitRev);
		std::memcpy(&h[72], id.data(), std::min<size_t>(id.size(), 63));
		const std::string settings = Settings();
		std::memcpy(&h[136], settings.data(), std::min<size_t>(settings.size(), 63));
		for (const Range& m : s_masks)
		{
			Put32(h, m.base);
			Put32(h, m.size);
		}
		u32 nspec = 0;
		for (const Env& e : s_env)
			nspec += !e.outs.empty();
		Put32(h, nspec);
		for (u32 id = 0; id < 256; id++)
		{
			if (s_env[id].outs.empty())
				continue;
			h.push_back(static_cast<u8>(id));
			h.push_back(static_cast<u8>(s_env[id].outs.size()));
			for (const OutSpec& o : s_env[id].outs)
			{
				h.push_back(static_cast<u8>((o.ptr_reg - 4) | (o.src_env ? 0x80 : 0)));
				if (o.src_env)
					h.push_back(o.src_env);
			}
		}
		Put32(h, static_cast<u32>(note.size()));
		h.insert(h.end(), note.begin(), note.end());
		at32(20, static_cast<u32>(h.size()));
		std::fwrite(h.data(), 1, h.size(), s_out);
		std::fflush(s_out);
		Console.WriteLn("gg-trace: recording to '%s' (%s)", p.c_str(), settings.c_str());
		return true;
	}
} // namespace GGTrace

void GGTrace::Initialize()
{
	if (g_enabled || s_out)
		return;
	const char* map = std::getenv("GG_TRACE_MAP");
	const char* out = std::getenv("GG_TRACE");
	if (!map || !*map || !out || !*out)
		return;
	if (!LoadMap(map))
		return;
	for (const Range& m : s_masks)
		AddPageMask(s_page_masks, m);
	const u64 zero = XXH3_64bits(std::vector<u8>(PAGE, 0).data(), PAGE);
	for (u64& h : s_page_hash)
		h = zero;
	if (const char* dumps = std::getenv("GG_TRACE_DUMP"))
	{
		for (const std::string_view f : StringUtil::SplitString(dumps, ',', true))
		{
			if (const std::optional<u32> n = StringUtil::FromChars<u32>(f))
				s_dump_fields.push_back(n.value());
		}
	}
	if (!OpenTrace(out))
		return;
	g_enabled = true;
}

void GGTrace::OnHook()
{
	const u32 pc = cpuRegs.pc;
	const auto it = std::lower_bound(s_hooks.begin(), s_hooks.end(), pc, [](const Hook& h, u32 a) { return h.addr < a; });
	const u32 bytes[2] = {pc, *reinterpret_cast<const u32*>(&eeMem->Main[pc])};
	const u32 check = static_cast<u32>(XXH3_64bits(bytes, sizeof(bytes)) & 0xffff);
	for (auto h = it; h != s_hooks.end() && h->addr == pc; ++h)
	{
		if (h->check != check)
			continue; // not the game's code (the BIOS, before the game loads)
		switch (h->kind)
		{
			case HookKind::Field:
				if (!s_started)
				{
					s_started = true;
					s_last_vblank = g_FrameCount;
				}
				EndField();
				break;
			case HookKind::Entry:
				OnEntry(*h); // calls before the first field are the boot's (field 0)
				break;
			case HookKind::Return:
				OnReturn(pc);
				break;
		}
	}
}

void GGTrace::Shutdown()
{
	if (!s_out)
		return;
	std::vector<u8> rec = {'E'};
	PutVarint(rec, s_field);
	std::fwrite(rec.data(), 1, rec.size(), s_out);
	std::fclose(s_out);
	s_out = nullptr;
	g_enabled = false;
	Console.WriteLn("gg-trace: closed '%s' after %u fields (%u calls never returned)", s_out_path.c_str(), s_field,
		s_stray + static_cast<u32>(s_pending.size()));
}
