/*
OBS HDR Toolkit
Copyright (C) 2026 srl01-coding

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * HDR Color (hdr_toolkit_color_v1) - P2: global stages.
 *
 * Brief sections 5-7: white balance (Bradford, daylight locus), global
 * exposure, contrast around a pivot, global colour wheel, saturation and
 * grade mix, on straight linear Rec.709 RGB in nominal nits, in the fixed v1
 * order. Tonal zones and soft clipping (P3) slot into that order; their
 * defaults are neutral, so grades made with this version are unchanged.
 *
 * Render contract identical to the other toolkit filters
 * (docs/COLOR_PIPELINE.md).
 */

#include "color-math.hpp"
#include "../shared/obs-color-context.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <mutex>
#include <string>

namespace {

using hdrtk::color::GlobalParams;
using hdrtk::color::ShaderParams;

constexpr int kSchemaVersion = 1;
constexpr float kAlphaEpsilon = 1e-6f;

struct Snapshot {
	bool neutral = true;
	bool force_render = false;
	ShaderParams sp{};
};

struct ColorFilter {
	obs_source_t *context = nullptr;
	gs_effect_t *effect = nullptr;

	gs_eparam_t *p_npu = nullptr, *p_upn = nullptr, *p_alpha_eps = nullptr;
	gs_eparam_t *p_wb[3] = {nullptr, nullptr, nullptr};
	gs_eparam_t *p_exposure = nullptr, *p_cm1 = nullptr, *p_pivot = nullptr, *p_gray = nullptr;
	gs_eparam_t *p_wheel = nullptr, *p_sat = nullptr, *p_mix = nullptr;
	gs_eparam_t *p_use_wb = nullptr, *p_use_contrast = nullptr, *p_use_wheel = nullptr, *p_use_sat = nullptr,
		    *p_use_mix = nullptr;

	std::mutex mutex; // guards snapshot / have_valid
	Snapshot snapshot;
	bool have_valid = false;

	// graphics thread only
	hdrtk::RenderContext last_ctx;
	bool have_ctx = false;
};

const char *color_get_name(void *)
{
	return obs_module_text("Color.Name");
}

GlobalParams read_params(obs_data_t *s)
{
	GlobalParams p;
	p.exposure_ev = obs_data_get_double(s, "global_exposure_ev");
	p.contrast = obs_data_get_double(s, "contrast_factor");
	p.pivot_nits = obs_data_get_double(s, "pivot_nits");
	p.gray_nits = obs_data_get_double(s, "gray_reference_nits");
	p.saturation = obs_data_get_double(s, "global_saturation");
	p.wb_mired = obs_data_get_double(s, "wb_mired_shift");
	p.wb_tint = obs_data_get_double(s, "wb_tint");
	p.wheel_x = obs_data_get_double(s, "global_wheel_x");
	p.wheel_y = obs_data_get_double(s, "global_wheel_y");
	p.grade_mix = obs_data_get_double(s, "grade_mix");
	return p;
}

void color_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<ColorFilter *>(data);
	GlobalParams p = read_params(settings);
	const std::string problems = hdrtk::color::sanitize(p);
	if (!problems.empty())
		obs_log(LOG_WARNING, "[color] '%s': %s(clamped)", obs_source_get_name(f->context), problems.c_str());

	Snapshot s;
	std::string error;
	if (!hdrtk::color::make_shader_params(p, s.sp, &error)) {
		obs_log(LOG_WARNING, "[color] '%s': %s; keeping the last valid grade", obs_source_get_name(f->context),
			error.c_str());
		std::lock_guard<std::mutex> lock(f->mutex);
		if (f->have_valid)
			return;
		GlobalParams neutral;
		hdrtk::color::make_shader_params(neutral, s.sp);
		p = neutral;
	}
	s.neutral = hdrtk::color::is_neutral(p);
	s.force_render = obs_data_get_bool(settings, "force_render_identity");

	std::lock_guard<std::mutex> lock(f->mutex);
	f->snapshot = s;
	f->have_valid = true;
}

gs_eparam_t *param(gs_effect_t *e, const char *name)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (!p)
		obs_log(LOG_ERROR, "[color] effect parameter '%s' missing", name);
	return p;
}

void *color_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new ColorFilter();
	f->context = source;

	char *path = obs_module_file("effects/hdr-color.effect");
	char *errors = nullptr;
	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(path, &errors);
	if (f->effect) {
		gs_effect_t *e = f->effect;
		f->p_npu = param(e, "nits_per_unit");
		f->p_upn = param(e, "units_per_nit");
		f->p_alpha_eps = param(e, "alpha_epsilon");
		f->p_wb[0] = param(e, "wb_row0");
		f->p_wb[1] = param(e, "wb_row1");
		f->p_wb[2] = param(e, "wb_row2");
		f->p_exposure = param(e, "exposure_gain");
		f->p_cm1 = param(e, "contrast_minus_one");
		f->p_pivot = param(e, "log2_pivot_over_gray");
		f->p_gray = param(e, "gray_nits");
		f->p_wheel = param(e, "wheel_delta");
		f->p_sat = param(e, "saturation");
		f->p_mix = param(e, "grade_mix");
		f->p_use_wb = param(e, "use_wb");
		f->p_use_contrast = param(e, "use_contrast");
		f->p_use_wheel = param(e, "use_wheel");
		f->p_use_sat = param(e, "use_saturation");
		f->p_use_mix = param(e, "use_mix");
	}
	obs_leave_graphics();

	if (!f->effect) {
		obs_log(LOG_ERROR, "[color] failed to compile %s: %s", path ? path : "(null)",
			errors ? errors : "(no compiler output)");
		bfree(errors);
		bfree(path);
		delete f;
		return nullptr;
	}
	bfree(errors);
	bfree(path);

	color_update(f, settings);
	return f;
}

void color_destroy(void *data)
{
	auto *f = static_cast<ColorFilter *>(data);
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	obs_leave_graphics();
	delete f;
}

void color_defaults(obs_data_t *s)
{
	// Every new instance is neutral (brief 1.3).
	obs_data_set_default_int(s, "schema_version", kSchemaVersion);
	obs_data_set_default_double(s, "global_exposure_ev", 0.0);
	obs_data_set_default_double(s, "contrast_factor", 1.0);
	obs_data_set_default_double(s, "pivot_nits", 18.0);
	obs_data_set_default_double(s, "gray_reference_nits", 18.0);
	obs_data_set_default_double(s, "global_saturation", 1.0);
	obs_data_set_default_double(s, "wb_mired_shift", 0.0);
	obs_data_set_default_double(s, "wb_tint", 0.0);
	obs_data_set_default_double(s, "global_wheel_x", 0.0);
	obs_data_set_default_double(s, "global_wheel_y", 0.0);
	obs_data_set_default_double(s, "grade_mix", 1.0);
	obs_data_set_default_bool(s, "force_render_identity", false);
}

bool reset_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *f = static_cast<ColorFilter *>(data);
	obs_data_t *s = obs_source_get_settings(f->context);
	const char *keys[] = {"global_exposure_ev", "contrast_factor", "pivot_nits",
			      "global_saturation",  "wb_mired_shift",  "wb_tint",
			      "global_wheel_x",     "global_wheel_y",  "grade_mix"};
	for (const char *k : keys)
		obs_data_unset_user_value(s, k);
	obs_source_update(f->context, s);
	obs_data_release(s);
	return true;
}

obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double lo, double hi, double step,
		       const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, obs_module_text(text), lo, hi, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

obs_properties_t *color_properties(void *data)
{
	auto *f = static_cast<ColorFilter *>(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_t *g = obs_properties_create();
	slider(g, "global_exposure_ev", "Color.Exposure", -6.0, 6.0, 0.01, " EV");
	slider(g, "contrast_factor", "Color.Contrast", 0.5, 2.0, 0.001, nullptr);
	slider(g, "pivot_nits", "Color.Pivot", 0.1, 1000.0, 0.1, " nits");
	slider(g, "global_saturation", "Color.Saturation", 0.0, 2.0, 0.001, nullptr);
	obs_properties_add_group(props, "global", obs_module_text("Color.Global"), OBS_GROUP_NORMAL, g);

	obs_properties_t *wb = obs_properties_create();
	slider(wb, "wb_mired_shift", "Color.Temperature", -90.0, 90.0, 0.5, " mired");
	slider(wb, "wb_tint", "Color.Tint", -100.0, 100.0, 0.5, nullptr);
	obs_properties_add_group(props, "white_balance", obs_module_text("Color.WhiteBalance"), OBS_GROUP_NORMAL, wb);

	obs_properties_t *wh = obs_properties_create();
	obs_properties_add_text(wh, "wheel_info", obs_module_text("Color.Wheel.Info"), OBS_TEXT_INFO);
	slider(wh, "global_wheel_x", "Color.Wheel.X", -1.0, 1.0, 0.001, nullptr);
	slider(wh, "global_wheel_y", "Color.Wheel.Y", -1.0, 1.0, 0.001, nullptr);
	obs_properties_add_group(props, "wheel", obs_module_text("Color.Wheel"), OBS_GROUP_NORMAL, wh);

	slider(props, "grade_mix", "Color.GradeMix", 0.0, 1.0, 0.001, nullptr);
	obs_properties_add_button2(props, "reset_grade", obs_module_text("Color.Reset"), reset_clicked, f);

	obs_properties_t *adv = obs_properties_create();
	slider(adv, "gray_reference_nits", "Color.GrayReference", 1.0, 100.0, 0.1, " nits");
	obs_properties_add_bool(adv, "force_render_identity", obs_module_text("Color.ForceRender"));
	obs_properties_add_group(props, "advanced", obs_module_text("Color.Advanced"), OBS_GROUP_NORMAL, adv);
	return props;
}

void set3(gs_eparam_t *p, const float v[3])
{
	struct vec3 x;
	vec3_set(&x, v[0], v[1], v[2]);
	gs_effect_set_vec3(p, &x);
}

void color_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<ColorFilter *>(data);
	obs_source_t *target = obs_filter_get_target(f->context);
	obs_source_t *parent = obs_filter_get_parent(f->context);
	if (!target || !parent) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	Snapshot s;
	{
		std::lock_guard<std::mutex> lock(f->mutex);
		s = f->snapshot;
	}

	const hdrtk::RenderContext ctx = hdrtk::capture_context(f->context, target);
	if (!f->have_ctx || ctx != f->last_ctx) {
		obs_log(LOG_INFO, "[color] '%s': %s", obs_source_get_name(f->context), ctx.describe().c_str());
		f->last_ctx = ctx;
		f->have_ctx = true;
	}
	if (!ctx.width || !ctx.height) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	if (s.neutral && !s.force_render) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (!obs_source_process_filter_begin_with_color_space(f->context, ctx.staging_format, ctx.input_space,
							      OBS_NO_DIRECT_RENDERING))
		return;

	// Nits scale is read live so a change of OBS SDR white level applies immediately.
	const double npu = hdrtk::nits_per_unit(ctx.input_space);
	gs_effect_set_float(f->p_npu, (float)npu);
	gs_effect_set_float(f->p_upn, (float)(1.0 / npu));
	gs_effect_set_float(f->p_alpha_eps, kAlphaEpsilon);
	for (int i = 0; i < 3; i++)
		set3(f->p_wb[i], s.sp.wb_row[i]);
	gs_effect_set_float(f->p_exposure, s.sp.exposure_gain);
	gs_effect_set_float(f->p_cm1, s.sp.contrast_minus_one);
	gs_effect_set_float(f->p_pivot, s.sp.log2_pivot_over_gray);
	gs_effect_set_float(f->p_gray, s.sp.gray_nits);
	set3(f->p_wheel, s.sp.wheel_delta);
	gs_effect_set_float(f->p_sat, s.sp.saturation);
	gs_effect_set_float(f->p_mix, s.sp.grade_mix);
	gs_effect_set_float(f->p_use_wb, s.sp.use_wb);
	gs_effect_set_float(f->p_use_contrast, s.sp.use_contrast);
	gs_effect_set_float(f->p_use_wheel, s.sp.use_wheel);
	gs_effect_set_float(f->p_use_sat, s.sp.use_saturation);
	gs_effect_set_float(f->p_use_mix, s.sp.use_mix);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_tech_end(f->context, f->effect, 0, 0, "Grade");
	gs_blend_state_pop();
}

enum gs_color_space color_space(void *data, size_t, const enum gs_color_space *)
{
	auto *f = static_cast<ColorFilter *>(data);
	return hdrtk::query_native_space(obs_filter_get_target(f->context));
}

} // namespace

extern "C" void hdrtk_register_color_filter(void)
{
	struct obs_source_info info = {};
	info.id = "hdr_toolkit_color_v1";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = color_get_name;
	info.create = color_create;
	info.destroy = color_destroy;
	info.update = color_update;
	info.get_defaults = color_defaults;
	info.get_properties = color_properties;
	info.video_render = color_render;
	info.video_get_color_space = color_space;
	obs_register_source(&info);
}
