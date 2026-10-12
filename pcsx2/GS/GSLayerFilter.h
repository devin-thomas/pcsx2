// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

// gg-layers: labels every GS draw with a named layer from a JSON rules file, then keeps, drops or recolours it.
// pcsx2-gsrunner uses it to render isolated layers of a GS dump over a solid background, and to write a per-draw
// census CSV. Inactive unless configured. Built for Guilty Gear XX Accent Core Plus; needs proper testing elsewhere.
namespace GSLayerFilter
{
	enum class Action
	{
		Draw,
		Skip,
		Recolor,
	};

	struct DrawFields;

	/// rules_path: JSON rules file (may be empty when only writing a census).
	/// keep: comma separated layer names, or "all".
	/// bg_hex: RRGGBB colour painted by "recolor" rules, or empty to draw them unchanged.
	/// census_path: CSV written with one row per draw, or empty.
	bool Configure(const std::string& rules_path, const std::string& keep, const std::string& bg_hex,
		const std::string& census_path, std::string* error);
	bool IsActive();
	void Shutdown();

	/// Called by GSState once per draw, with the fields already gathered.
	Action Decide(const DrawFields& f);
	u32 GetBackgroundRGBA();
} // namespace GSLayerFilter

struct GSLayerFilter::DrawFields
{
	enum Field : u32
	{
		FRAME_N, DRAW_N, S_N,
		PRIM, IIP, TME, FGE, ABE, AA1, FST, CTXT,
		TBP0, TBW, PSM, TW, TH, TCC, TFX, CBP, CPSM, CSM, CSA, CLD,
		FBP, FBW, FPSM, FBMSK, ZBP, ZPSM, ZMSK,
		ALPHA_A, ALPHA_B, ALPHA_C, ALPHA_D, ALPHA_FIX, PABE, FBA,
		ATE, ATST, AREF, AFAIL, DATE, DATM, ZTE, ZTST,
		SCAX0, SCAX1, SCAY0, SCAY1,
		X0, Y0, X1, Y1, W, H,
		U0, V0, U1, V1,
		NV, R, G, B, A,
		COUNT
	};

	s64 v[COUNT] = {};
};
