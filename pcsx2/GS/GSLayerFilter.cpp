// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSLayerFilter.h"
#include "GS/GSPerfMon.h"
#include "GS/GSState.h"
#include "GS/GSUtil.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include "rapidjson/document.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string_view>
#include <vector>

namespace GSLayerFilter
{
	using F = DrawFields;

	static constexpr const char* s_field_names[F::COUNT] = {
		"FRAME_N", "DRAW_N", "S_N",
		"PRIM", "IIP", "TME", "FGE", "ABE", "AA1", "FST", "CTXT",
		"TBP0", "TBW", "PSM", "TW", "TH", "TCC", "TFX", "CBP", "CPSM", "CSM", "CSA", "CLD",
		"FBP", "FBW", "FPSM", "FBMSK", "ZBP", "ZPSM", "ZMSK",
		"ALPHA_A", "ALPHA_B", "ALPHA_C", "ALPHA_D", "ALPHA_FIX", "PABE", "FBA",
		"ATE", "ATST", "AREF", "AFAIL", "DATE", "DATM", "ZTE", "ZTST",
		"SCAX0", "SCAX1", "SCAY0", "SCAY1",
		"X0", "Y0", "X1", "Y1", "W", "H",
		"U0", "V0", "U1", "V1",
		"NV", "R", "G", "B", "A",
	};

	struct Condition
	{
		u32 field;
		s64 lo, hi;
		std::vector<s64> any_of; // when non-empty, lo/hi are ignored
	};

	struct Rule
	{
		u32 layer;
		bool recolor;
		std::vector<Condition> when;
	};

	static bool s_active = false;
	static std::vector<std::string> s_layers;
	static std::vector<bool> s_keep;
	static std::vector<u64> s_counts;
	static std::vector<Rule> s_rules;
	static u32 s_default_layer = 0;
	static std::optional<u32> s_bg_rgba;
	static std::FILE* s_census = nullptr;

	static u32 LayerIndex(std::string_view name)
	{
		for (u32 i = 0; i < s_layers.size(); i++)
		{
			if (s_layers[i] == name)
				return i;
		}
		s_layers.emplace_back(name);
		return static_cast<u32>(s_layers.size() - 1);
	}

	static std::optional<u32> FieldIndex(std::string_view name)
	{
		for (u32 i = 0; i < F::COUNT; i++)
		{
			if (name == s_field_names[i])
				return i;
		}
		return std::nullopt;
	}

	static std::optional<s64> ParseNumber(const rapidjson::Value& v)
	{
		if (v.IsInt64())
			return v.GetInt64();
		if (v.IsString())
		{
			const std::string_view s(v.GetString(), v.GetStringLength());
			if (s.starts_with("0x") || s.starts_with("0X"))
				return StringUtil::FromChars<s64>(s.substr(2), 16);
			return StringUtil::FromChars<s64>(s);
		}
		return std::nullopt;
	}

	static bool ParseCondition(std::string_view field, const rapidjson::Value& v, Condition* out, std::string* error)
	{
		const std::optional<u32> idx = FieldIndex(field);
		if (!idx.has_value())
		{
			*error = fmt::format("unknown field '{}'", field);
			return false;
		}
		out->field = idx.value();
		if (v.IsArray() && v.Size() == 2 && ParseNumber(v[0]).has_value() && ParseNumber(v[1]).has_value())
		{
			out->lo = ParseNumber(v[0]).value();
			out->hi = ParseNumber(v[1]).value();
			return true;
		}
		if (v.IsObject() && v.HasMember("in") && v["in"].IsArray())
		{
			for (const rapidjson::Value& item : v["in"].GetArray())
			{
				const std::optional<s64> n = ParseNumber(item);
				if (!n.has_value())
				{
					*error = fmt::format("field '{}': non-numeric value in \"in\" list", field);
					return false;
				}
				out->any_of.push_back(n.value());
			}
			return !out->any_of.empty();
		}
		if (const std::optional<s64> n = ParseNumber(v); n.has_value())
		{
			out->lo = out->hi = n.value();
			return true;
		}
		*error = fmt::format("field '{}': expected a number, [min, max] or {{\"in\": [...]}}", field);
		return false;
	}

	static bool LoadRules(const std::string& path, std::string* error)
	{
		const std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
		if (!text.has_value())
		{
			*error = fmt::format("can't read rules file '{}'", path);
			return false;
		}
		rapidjson::Document doc;
		doc.Parse(text->c_str(), text->size());
		if (doc.HasParseError() || !doc.IsObject())
		{
			*error = fmt::format("rules file '{}' is not a JSON object", path);
			return false;
		}
		if (doc.HasMember("layers") && doc["layers"].IsArray())
		{
			for (const rapidjson::Value& name : doc["layers"].GetArray())
			{
				if (name.IsString())
					LayerIndex(name.GetString());
			}
		}
		s_default_layer = LayerIndex(doc.HasMember("default") && doc["default"].IsString() ? doc["default"].GetString() : "other");
		if (!doc.HasMember("rules") || !doc["rules"].IsArray())
		{
			*error = "rules file has no \"rules\" array";
			return false;
		}
		for (const rapidjson::Value& r : doc["rules"].GetArray())
		{
			if (!r.IsObject() || !r.HasMember("layer") || !r["layer"].IsString())
			{
				*error = "every rule needs a \"layer\" name";
				return false;
			}
			Rule rule;
			rule.layer = LayerIndex(r["layer"].GetString());
			rule.recolor = r.HasMember("recolor") && r["recolor"].IsBool() && r["recolor"].GetBool();
			if (r.HasMember("when") && r["when"].IsObject())
			{
				for (auto it = r["when"].MemberBegin(); it != r["when"].MemberEnd(); ++it)
				{
					Condition c{};
					if (!ParseCondition(std::string_view(it->name.GetString(), it->name.GetStringLength()), it->value, &c, error))
					{
						*error = fmt::format("rule for '{}': {}", r["layer"].GetString(), *error);
						return false;
					}
					rule.when.push_back(std::move(c));
				}
			}
			s_rules.push_back(std::move(rule));
		}
		return true;
	}
} // namespace GSLayerFilter

bool GSLayerFilter::Configure(const std::string& rules_path, const std::string& keep, const std::string& bg_hex,
	const std::string& census_path, std::string* error)
{
	Shutdown();
	s_layers.clear();
	s_rules.clear();
	s_default_layer = LayerIndex("other");

	if (!rules_path.empty() && !LoadRules(rules_path, error))
		return false;

	s_keep.assign(s_layers.size(), keep.empty() || keep == "all");
	if (!keep.empty() && keep != "all")
	{
		for (const std::string_view name : StringUtil::SplitString(keep, ','))
		{
			const std::string_view trimmed = StringUtil::StripWhitespace(name);
			const auto it = std::find(s_layers.begin(), s_layers.end(), trimmed);
			if (it == s_layers.end())
			{
				*error = fmt::format("-keep names unknown layer '{}'", trimmed);
				return false;
			}
			s_keep[it - s_layers.begin()] = true;
		}
	}

	if (!bg_hex.empty())
	{
		const std::optional<u32> rgb = StringUtil::FromChars<u32>(bg_hex, 16);
		if (bg_hex.size() != 6 || !rgb.has_value())
		{
			*error = fmt::format("-bg expects RRGGBB, got '{}'", bg_hex);
			return false;
		}
		s_bg_rgba = rgb.value();
	}

	if (!census_path.empty())
	{
		s_census = FileSystem::OpenCFile(census_path.c_str(), "wb");
		if (!s_census)
		{
			*error = fmt::format("can't write census '{}'", census_path);
			return false;
		}
		for (u32 i = 0; i < F::COUNT; i++)
			std::fprintf(s_census, "%s,", s_field_names[i]);
		std::fputs("LAYER\n", s_census);
	}

	s_counts.assign(s_layers.size(), 0);
	s_active = true;
	return true;
}

bool GSLayerFilter::IsActive()
{
	return s_active;
}

u32 GSLayerFilter::GetBackgroundRGBA()
{
	return s_bg_rgba.value_or(0);
}

void GSLayerFilter::Shutdown()
{
	if (s_active)
	{
		std::string summary;
		for (u32 i = 0; i < s_layers.size(); i++)
			summary += fmt::format(" {}={}", s_layers[i], s_counts[i]);
		Console.WriteLn("gg-layers: draws per layer:%s", summary.c_str());
	}
	if (s_census)
	{
		std::fclose(s_census);
		s_census = nullptr;
	}
	s_active = false;
	s_bg_rgba.reset();
}

GSLayerFilter::Action GSLayerFilter::Decide(const DrawFields& f)
{
	const Rule* match = nullptr;
	for (const Rule& rule : s_rules)
	{
		const bool ok = std::all_of(rule.when.begin(), rule.when.end(), [&f](const Condition& c) {
			const s64 v = f.v[c.field];
			if (!c.any_of.empty())
				return std::find(c.any_of.begin(), c.any_of.end(), v) != c.any_of.end();
			return v >= c.lo && v <= c.hi;
		});
		if (ok)
		{
			match = &rule;
			break;
		}
	}

	const u32 layer = match ? match->layer : s_default_layer;
	s_counts[layer]++;

	if (s_census)
	{
		for (u32 i = 0; i < F::COUNT; i++)
			std::fprintf(s_census, "%lld,", static_cast<long long>(f.v[i]));
		std::fprintf(s_census, "%s\n", s_layers[layer].c_str());
	}

	if (match && match->recolor && s_bg_rgba.has_value())
		return Action::Recolor;
	return s_keep[layer] ? Action::Draw : Action::Skip;
}

GSLayerFilter::Action GSState::ApplyLayerFilter()
{
	using F = GSLayerFilter::DrawFields;

	static int s_last_frame = -1;
	static s64 s_draw_in_frame = 0;
	const int frame = g_perfmon.GetFrame();
	if (frame != s_last_frame)
	{
		s_last_frame = frame;
		s_draw_in_frame = 0;
	}

	F f;
	s64* v = f.v;
	v[F::FRAME_N] = frame;
	v[F::DRAW_N] = s_draw_in_frame++;
	v[F::S_N] = static_cast<s64>(s_n);

	v[F::PRIM] = PRIM->PRIM;
	v[F::IIP] = PRIM->IIP;
	v[F::TME] = PRIM->TME;
	v[F::FGE] = PRIM->FGE;
	v[F::ABE] = PRIM->ABE;
	v[F::AA1] = PRIM->AA1;
	v[F::FST] = PRIM->FST;
	v[F::CTXT] = PRIM->CTXT;

	const GSDrawingContext& ctx = *m_context;
	v[F::TBP0] = ctx.TEX0.TBP0;
	v[F::TBW] = ctx.TEX0.TBW;
	v[F::PSM] = ctx.TEX0.PSM;
	v[F::TW] = ctx.TEX0.TW;
	v[F::TH] = ctx.TEX0.TH;
	v[F::TCC] = ctx.TEX0.TCC;
	v[F::TFX] = ctx.TEX0.TFX;
	v[F::CBP] = ctx.TEX0.CBP;
	v[F::CPSM] = ctx.TEX0.CPSM;
	v[F::CSM] = ctx.TEX0.CSM;
	v[F::CSA] = ctx.TEX0.CSA;
	v[F::CLD] = ctx.TEX0.CLD;
	v[F::FBP] = ctx.FRAME.FBP;
	v[F::FBW] = ctx.FRAME.FBW;
	v[F::FPSM] = ctx.FRAME.PSM;
	v[F::FBMSK] = ctx.FRAME.FBMSK;
	v[F::ZBP] = ctx.ZBUF.ZBP;
	v[F::ZPSM] = ctx.ZBUF.PSM;
	v[F::ZMSK] = ctx.ZBUF.ZMSK;
	v[F::ALPHA_A] = ctx.ALPHA.A;
	v[F::ALPHA_B] = ctx.ALPHA.B;
	v[F::ALPHA_C] = ctx.ALPHA.C;
	v[F::ALPHA_D] = ctx.ALPHA.D;
	v[F::ALPHA_FIX] = ctx.ALPHA.FIX;
	v[F::PABE] = m_draw_env->PABE.PABE;
	v[F::FBA] = ctx.FBA.FBA;
	v[F::ATE] = ctx.TEST.ATE;
	v[F::ATST] = ctx.TEST.ATST;
	v[F::AREF] = ctx.TEST.AREF;
	v[F::AFAIL] = ctx.TEST.AFAIL;
	v[F::DATE] = ctx.TEST.DATE;
	v[F::DATM] = ctx.TEST.DATM;
	v[F::ZTE] = ctx.TEST.ZTE;
	v[F::ZTST] = ctx.TEST.ZTST;
	v[F::SCAX0] = ctx.SCISSOR.SCAX0;
	v[F::SCAX1] = ctx.SCISSOR.SCAX1;
	v[F::SCAY0] = ctx.SCISSOR.SCAY0;
	v[F::SCAY1] = ctx.SCISSOR.SCAY1;

	GSVertexBuff& vtx_buff = *m_vertex;
	GSIndexBuff& idx_buff = *m_index;
	const float tw = static_cast<float>(1 << ctx.TEX0.TW);
	const float th = static_cast<float>(1 << ctx.TEX0.TH);

	// Geometry and colour fields for the primitives [first, last) (counted in indices).
	auto measure = [&](u32 first, u32 last, F& out) {
		float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
		float u0 = FLT_MAX, t0 = FLT_MAX, u1 = -FLT_MAX, t1 = -FLT_MAX;
		for (u32 i = first; i < last; i++)
		{
			const GSVertex& vx = vtx_buff.buff[idx_buff.buff[i]];
			const float x = (static_cast<int>(vx.XYZ.X) - static_cast<int>(ctx.XYOFFSET.OFX)) / 16.0f;
			const float y = (static_cast<int>(vx.XYZ.Y) - static_cast<int>(ctx.XYOFFSET.OFY)) / 16.0f;
			x0 = std::min(x0, x);
			y0 = std::min(y0, y);
			x1 = std::max(x1, x);
			y1 = std::max(y1, y);
			float u, t;
			if (PRIM->FST)
			{
				u = vx.U / 16.0f;
				t = vx.V / 16.0f;
			}
			else
			{
				const float q = (vx.RGBAQ.Q != 0.0f) ? vx.RGBAQ.Q : 1.0f;
				u = vx.ST.S / q * tw;
				t = vx.ST.T / q * th;
			}
			u0 = std::min(u0, u);
			t0 = std::min(t0, t);
			u1 = std::max(u1, u);
			t1 = std::max(t1, t);
		}
		s64* o = out.v;
		if (last > first)
		{
			o[F::X0] = static_cast<s64>(std::floor(x0));
			o[F::Y0] = static_cast<s64>(std::floor(y0));
			o[F::X1] = static_cast<s64>(std::ceil(x1));
			o[F::Y1] = static_cast<s64>(std::ceil(y1));
			o[F::W] = o[F::X1] - o[F::X0];
			o[F::H] = o[F::Y1] - o[F::Y0];
			o[F::U0] = static_cast<s64>(std::floor(u0));
			o[F::V0] = static_cast<s64>(std::floor(t0));
			o[F::U1] = static_cast<s64>(std::ceil(u1));
			o[F::V1] = static_cast<s64>(std::ceil(t1));
			const GSVertex& v0 = vtx_buff.buff[idx_buff.buff[first]];
			o[F::R] = v0.RGBAQ.R;
			o[F::G] = v0.RGBAQ.G;
			o[F::B] = v0.RGBAQ.B;
			o[F::A] = v0.RGBAQ.A;
		}
		o[F::NV] = last - first;
	};

	// PCSX2 batches consecutive primitives until the GS state changes, so one "draw" can hold more than one layer
	// (AC+ draws a character's floor shadow and body with the same texture, only the vertex alpha differs). Split the
	// batch into runs of primitives with the same vertex alpha and label each run on its own.
	const u32 per_prim = std::max(1, GSUtil::GetClassVertexCount(GSUtil::GetPrimClass(PRIM->PRIM)));
	const u32 nidx = idx_buff.tail - (idx_buff.tail % per_prim);
	struct Run
	{
		u32 first, last;
		GSLayerFilter::Action action;
	};
	std::vector<Run> runs;
	for (u32 i = 0; i < nidx; i += per_prim)
	{
		const u8 a = vtx_buff.buff[idx_buff.buff[i]].RGBAQ.A;
		if (!runs.empty() && vtx_buff.buff[idx_buff.buff[runs.back().first]].RGBAQ.A == a)
			runs.back().last = i + per_prim;
		else
			runs.push_back({i, i + per_prim, GSLayerFilter::Action::Draw});
	}
	if (runs.empty())
		runs.push_back({0, idx_buff.tail, GSLayerFilter::Action::Draw});

	bool any_kept = false, all_kept = true, any_recolor = false;
	for (Run& r : runs)
	{
		F sub = f;
		measure(r.first, r.last, sub);
		r.action = GSLayerFilter::Decide(sub);
		any_kept |= (r.action != GSLayerFilter::Action::Skip);
		all_kept &= (r.action != GSLayerFilter::Action::Skip);
		any_recolor |= (r.action == GSLayerFilter::Action::Recolor);
	}
	if (!any_kept)
		return GSLayerFilter::Action::Skip;

	if (any_recolor)
	{
		// Paint those primitives with the solid background colour.
		const u32 bg = GSLayerFilter::GetBackgroundRGBA();
		for (const Run& r : runs)
		{
			if (r.action != GSLayerFilter::Action::Recolor)
				continue;
			for (u32 i = r.first; i < r.last; i++)
			{
				GSVertex& vx = vtx_buff.buff[idx_buff.buff[i]];
				vx.RGBAQ.R = (bg >> 16) & 0xFF;
				vx.RGBAQ.G = (bg >> 8) & 0xFF;
				vx.RGBAQ.B = bg & 0xFF;
				vx.RGBAQ.A = 0x80;
			}
		}
	}
	if (!all_kept)
	{
		// Keep only the primitives of selected layers, in their original order.
		u32 out = 0;
		for (const Run& r : runs)
		{
			if (r.action == GSLayerFilter::Action::Skip)
				continue;
			for (u32 i = r.first; i < r.last; i++)
				idx_buff.buff[out++] = idx_buff.buff[i];
		}
		idx_buff.tail = out;
	}
	if (any_recolor || !all_kept)
		m_vt.Update(vtx_buff.buff, idx_buff.buff, vtx_buff.tail, idx_buff.tail, GSUtil::GetPrimClass(PRIM->PRIM));
	return GSLayerFilter::Action::Draw;
}
