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
 * HDR Program Denoise - P0 (denoise brief v1.2, sections 3, 4, 26).
 *
 * Processes the finished program frame (after scene switching and transitions)
 * exactly once per frame, before stream / record / Program monitor see it.
 *
 * Mechanism (OBS 32.2.2 libobs/obs-video.c, render_main_texture): after each
 * video mix has drawn its texture, OBS calls the main-rendered callbacks with
 * that mix's texture still bound as the render target. The callback runs once
 * per *mix*, not once per frame, so it acts only when
 *   - the bound render target is the main canvas texture (obs_get_main_texture),
 *     which skips secondary mixes, including ones that reused the main texture;
 *   - the video frame time differs from the last processed frame.
 * Previews, projectors and multiview draw the main texture but do not call the
 * callback, so they cannot advance any history.
 *
 * Processing: copy the main texture to a scratch texture of the same size and
 * format, then draw the result back into the main texture (blend off,
 * framebuffer sRGB off). The texture stays in OBS's native canvas space
 * (RGBA16F / GS_CS_709_EXTENDED on an HDR canvas): no decode, no encode, no clamp.
 *
 * P0 algorithms: Off, Identity (exact shader copy). HQDN3D / NLMeans come later.
 *
 * Controls: a private controller source, opened from Tools > HDR Program Denoise.
 * Settings persist in the plugin's config folder (program-denoise.json).
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <mutex>
#include <string>

namespace {

constexpr const char *kSourceId = "hdr_toolkit_program_denoise_v1";
constexpr const char *kConfigFile = "program-denoise.json";
constexpr int kSchemaVersion = 1; // development schema (brief 2.7): no migrations yet
constexpr uint64_t kLogIntervalNs = 10ull * 1000000000ull;

enum Algorithm { AlgoOff = 0, AlgoIdentity = 1 };

struct Counters {
	std::atomic<uint64_t> callbacks{0};          // every main-rendered callback (all mixes)
	std::atomic<uint64_t> other_mix_skipped{0};  // render target was not the main texture
	std::atomic<uint64_t> unique_frames{0};      // program_frames_seen: distinct main-mix frames
	std::atomic<uint64_t> duplicates_skipped{0}; // same frame time seen again on the main texture
	std::atomic<uint64_t> dispatches{0};         // processing passes drawn into the main texture
	std::atomic<uint64_t> history_updates{0};    // temporal history writes (none in P0)
	std::atomic<uint64_t> history_resets{0};     // temporal history (re)initialisations
	std::atomic<uint64_t> failures{0};           // pass skipped because resources were missing
};

struct Denoise {
	obs_source_t *controller = nullptr;

	std::atomic<int> algorithm{AlgoOff};
	std::atomic<bool> periodic_log{false};
	std::atomic<bool> log_now{false};
	std::atomic<bool> reset_counters{false};

	Counters c;

	// graphics thread only
	gs_effect_t *effect = nullptr;
	bool effect_failed = false;
	gs_texture_t *scratch = nullptr;
	uint32_t width = 0, height = 0;
	enum gs_color_format format = GS_UNKNOWN;
	bool have_frame_time = false;
	uint64_t last_frame_time = 0;
	uint64_t last_log_ns = 0;
	uint32_t total_frames_at_reset = 0;
	uint32_t lagged_frames_at_reset = 0;
	bool warned = false;
};

Denoise *g = nullptr;

const char *format_name(enum gs_color_format f)
{
	switch (f) {
	case GS_RGBA16F:
		return "RGBA16F";
	case GS_RGBA32F:
		return "RGBA32F";
	case GS_RGBA:
		return "RGBA8";
	case GS_BGRA:
		return "BGRA8";
	case GS_R10G10B10A2:
		return "R10G10B10A2";
	case GS_RGBA16:
		return "RGBA16";
	default:
		return "other";
	}
}

const char *colorspace_name(enum video_colorspace cs)
{
	switch (cs) {
	case VIDEO_CS_2100_HLG:
		return "Rec.2100 HLG";
	case VIDEO_CS_2100_PQ:
		return "Rec.2100 PQ";
	case VIDEO_CS_709:
		return "Rec.709";
	case VIDEO_CS_SRGB:
		return "sRGB";
	case VIDEO_CS_601:
		return "Rec.601";
	default:
		return "default";
	}
}

void log_counters(Denoise *d, const char *why)
{
	const uint32_t total = obs_get_total_frames() - d->total_frames_at_reset;
	const uint32_t lagged = obs_get_lagged_frames() - d->lagged_frames_at_reset;
	const uint64_t unique = d->c.unique_frames.load();
	const uint64_t disp = d->c.dispatches.load();
	const int algo = d->algorithm.load();
	obs_log(LOG_INFO,
		"[denoise] %s: algorithm=%s callbacks=%" PRIu64 " other_mix_skipped=%" PRIu64
		" unique_program_frames=%" PRIu64 " duplicate_callbacks_skipped=%" PRIu64 " denoise_dispatches=%" PRIu64
		" history_updates=%" PRIu64 " history_resets=%" PRIu64 " failures=%" PRIu64
		" | obs_total_frames=%u obs_lagged_frames=%u | %s",
		why, algo == AlgoIdentity ? "identity" : "off", d->c.callbacks.load(), d->c.other_mix_skipped.load(),
		unique, d->c.duplicates_skipped.load(), disp, d->c.history_updates.load(), d->c.history_resets.load(),
		d->c.failures.load(), total, lagged,
		algo == AlgoOff ? "off (no dispatch expected)"
				: (disp == unique ? "dispatches == unique frames: OK"
						  : "MISMATCH dispatches != unique frames"));
}

void reset_counters_now(Denoise *d)
{
	d->c.callbacks = 0;
	d->c.other_mix_skipped = 0;
	d->c.unique_frames = 0;
	d->c.duplicates_skipped = 0;
	d->c.dispatches = 0;
	d->c.history_updates = 0;
	d->c.history_resets = 0;
	d->c.failures = 0;
	d->total_frames_at_reset = obs_get_total_frames();
	d->lagged_frames_at_reset = obs_get_lagged_frames();
}

bool ensure_effect(Denoise *d)
{
	if (d->effect)
		return true;
	if (d->effect_failed)
		return false;
	char *path = obs_module_file("effects/hdr-program-denoise.effect");
	char *errors = nullptr;
	d->effect = gs_effect_create_from_file(path, &errors);
	if (!d->effect) {
		obs_log(LOG_ERROR, "[denoise] failed to compile %s: %s", path ? path : "(null)",
			errors ? errors : "(no compiler output)");
		d->effect_failed = true; // pass-through from now on, warn once
	}
	bfree(errors);
	bfree(path);
	return d->effect != nullptr;
}

// (Re)create the scratch texture when the program frame's size or format changes.
bool ensure_scratch(Denoise *d, gs_texture_t *main_tex)
{
	const uint32_t w = gs_texture_get_width(main_tex);
	const uint32_t h = gs_texture_get_height(main_tex);
	const enum gs_color_format f = gs_texture_get_color_format(main_tex);
	if (d->scratch && w == d->width && h == d->height && f == d->format)
		return true;

	gs_texture_destroy(d->scratch);
	d->scratch = gs_texture_create(w, h, f, 1, nullptr, 0);
	d->width = w;
	d->height = h;
	d->format = f;
	d->c.history_resets++; // brief 8: any resource change resets temporal history

	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	obs_log(LOG_INFO,
		"[denoise] program frame %ux%u %s, canvas colour space %s, SDR white %.0f nits, HDR nominal peak %.0f nits%s",
		w, h, format_name(f), colorspace_name(ovi.colorspace), obs_get_video_sdr_white_level(),
		obs_get_video_hdr_nominal_peak_level(), d->scratch ? "" : " - FAILED to create scratch texture");
	return d->scratch != nullptr;
}

void draw_identity(Denoise *d, gs_texture_t *main_tex)
{
	gs_copy_texture(d->scratch, main_tex);

	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_enable_blending(false);
	gs_viewport_push();
	gs_projection_push();
	gs_matrix_push();
	gs_matrix_identity();
	gs_set_viewport(0, 0, (int)d->width, (int)d->height);
	gs_ortho(0.0f, (float)d->width, 0.0f, (float)d->height, -100.0f, 100.0f);

	struct vec2 size;
	vec2_set(&size, (float)d->width, (float)d->height);
	gs_effect_set_texture(gs_effect_get_param_by_name(d->effect, "image"), d->scratch);
	gs_effect_set_vec2(gs_effect_get_param_by_name(d->effect, "tex_size"), &size);
	while (gs_effect_loop(d->effect, "Identity"))
		gs_draw_sprite(d->scratch, 0, d->width, d->height);

	gs_matrix_pop();
	gs_projection_pop();
	gs_viewport_pop();
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_srgb);
}

void on_main_rendered(void *param)
{
	auto *d = static_cast<Denoise *>(param);
	d->c.callbacks++;

	if (d->reset_counters.exchange(false)) {
		reset_counters_now(d);
		obs_log(LOG_INFO, "[denoise] counters reset");
	}

	gs_texture_t *main_tex = obs_get_main_texture();
	if (!main_tex || gs_get_render_target() != main_tex) {
		d->c.other_mix_skipped++;
		return;
	}
	const uint64_t t = obs_get_video_frame_time();
	if (d->have_frame_time && t == d->last_frame_time) {
		d->c.duplicates_skipped++;
		return;
	}
	d->have_frame_time = true;
	d->last_frame_time = t;
	d->c.unique_frames++;

	const int algo = d->algorithm.load();
	if (algo == AlgoIdentity) {
		if (ensure_effect(d) && ensure_scratch(d, main_tex)) {
			draw_identity(d, main_tex);
			d->c.dispatches++;
		} else {
			d->c.failures++; // brief 18: pass the frame through untouched, warn once
			if (!d->warned) {
				obs_log(LOG_WARNING, "[denoise] resources unavailable: passing the program through");
				d->warned = true;
			}
		}
	}

	const uint64_t now = os_gettime_ns();
	if (d->log_now.exchange(false)) {
		log_counters(d, "on request");
	} else if (d->periodic_log.load() && now - d->last_log_ns >= kLogIntervalNs) {
		log_counters(d, "periodic");
		d->last_log_ns = now;
	}
}

// ---- controller source (settings UI) --------------------------------------

const char *ctl_get_name(void *)
{
	return obs_module_text("Denoise.Name");
}

void save_settings(obs_data_t *settings)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *file = obs_module_config_path(kConfigFile);
	if (file) {
		obs_data_save_json_safe(settings, file, "tmp", "bak");
		bfree(file);
	}
}

void ctl_update(void *, obs_data_t *settings)
{
	if (!g)
		return;
	int algo = (int)obs_data_get_int(settings, "algorithm");
	if (algo != AlgoOff && algo != AlgoIdentity)
		algo = AlgoOff; // unknown future value: fail safe
	const int prev = g->algorithm.exchange(algo);
	g->periodic_log = obs_data_get_bool(settings, "log_counters");
	if (prev != algo)
		obs_log(LOG_INFO, "[denoise] algorithm %s", algo == AlgoIdentity ? "identity" : "off");
	save_settings(settings);
}

void *ctl_create(obs_data_t *settings, obs_source_t *source)
{
	if (g)
		g->controller = source;
	ctl_update(nullptr, settings);
	return g;
}

void ctl_destroy(void *) {}

void ctl_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "schema_version", kSchemaVersion);
	obs_data_set_default_int(s, "algorithm", AlgoOff); // new installs are neutral
	obs_data_set_default_bool(s, "log_counters", false);
}

bool log_now_clicked(obs_properties_t *, obs_property_t *, void *)
{
	if (g)
		g->log_now = true;
	return false;
}

bool reset_clicked(obs_properties_t *, obs_property_t *, void *)
{
	if (g)
		g->reset_counters = true;
	return false;
}

obs_properties_t *ctl_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "info", obs_module_text("Denoise.Info"), OBS_TEXT_INFO);
	obs_property_t *p = obs_properties_add_list(props, "algorithm", obs_module_text("Denoise.Algorithm"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Denoise.Algorithm.Off"), AlgoOff);
	obs_property_list_add_int(p, obs_module_text("Denoise.Algorithm.Identity"), AlgoIdentity);

	obs_properties_t *dbg = obs_properties_create();
	obs_properties_add_bool(dbg, "log_counters", obs_module_text("Denoise.LogCounters"));
	obs_properties_add_button2(dbg, "log_now", obs_module_text("Denoise.LogNow"), log_now_clicked, nullptr);
	obs_properties_add_button2(dbg, "reset_counters", obs_module_text("Denoise.ResetCounters"), reset_clicked,
				   nullptr);
	obs_properties_add_group(props, "debug", obs_module_text("Denoise.Debug"), OBS_GROUP_NORMAL, dbg);
	return props;
}

void open_properties(void *)
{
	if (g && g->controller)
		obs_frontend_open_source_properties(g->controller);
}

} // namespace

extern "C" void hdrtk_denoise_load(void)
{
	struct obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_CAP_DISABLED | OBS_SOURCE_DO_NOT_DUPLICATE; // controller only, no video
	info.get_name = ctl_get_name;
	info.create = ctl_create;
	info.destroy = ctl_destroy;
	info.update = ctl_update;
	info.get_defaults = ctl_defaults;
	info.get_properties = ctl_properties;
	obs_register_source(&info);

	g = new Denoise();
	obs_add_main_rendered_callback(on_main_rendered, g);
	obs_frontend_add_tools_menu_item(obs_module_text("Denoise.Menu"), open_properties, nullptr);
	obs_log(LOG_INFO, "[denoise] program hook registered (once per unique main-canvas frame)");
}

// Create the controller after all modules are loaded (settings from the config file).
extern "C" void hdrtk_denoise_post_load(void)
{
	if (!g || g->controller)
		return;
	char *file = obs_module_config_path(kConfigFile);
	obs_data_t *settings = file ? obs_data_create_from_json_file_safe(file, "bak") : nullptr;
	bfree(file);
	if (!settings)
		settings = obs_data_create();
	obs_source_t *src = obs_source_create_private(kSourceId, "HDR Program Denoise", settings);
	obs_data_release(settings);
	g->controller = src;
}

extern "C" void hdrtk_denoise_unload(void)
{
	if (!g)
		return;
	obs_remove_main_rendered_callback(on_main_rendered, g);
	if (g->controller) {
		obs_source_t *src = g->controller;
		g->controller = nullptr;
		obs_source_release(src);
	}
	obs_enter_graphics();
	gs_effect_destroy(g->effect);
	gs_texture_destroy(g->scratch);
	obs_leave_graphics();
	delete g;
	g = nullptr;
}
