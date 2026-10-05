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
 * HDR Program Denoise (denoise brief v1.2).
 *
 * P0 - the hook (docs/denoise/DENOISE.md). After each video mix has drawn its
 * texture, OBS (32.2.2 libobs/obs-video.c, render_main_texture) calls the
 * main-rendered callbacks with that mix's texture still bound as the render
 * target. That is once per *mix*, so the hook acts only when the bound target is
 * the main canvas texture and the video frame time is new. Previews, projectors
 * and multiview draw the main texture without calling the callback.
 *
 * P1/P2 - HQDN3D-style spatial + temporal (docs/HQDN3D_DESIGN.md; independent
 * implementation, no FFmpeg/MPlayer code). Per unique frame, all on the GPU, no
 * readback in the processing path:
 *   copy main -> cur
 *   spatial B (when on): SpatialH(cur) -> sp_tmp, SpatialV(sp_tmp) -> sp_out; src = sp_out
 *   (otherwise src = cur)
 *   metric: MetricBlocks(src, hist) -> l1, MetricReduce -> l2, MetricFinal -> metric (1x1)
 *   Temporal(src, hist, metric) -> hist_next          (history update)
 *   Output(cur, hist_next) -> main texture            (Mix, debug views vs the input)
 * Textures keep the main texture's size and format (RGBA16F, native canvas space).
 *
 * P2 compute spike - "Compute identity (D3D11 spike)": a native D3D11 compute round
 * trip on the program texture (src/denoise/d3d11-compute.cpp), Windows only, with a
 * transparent fallback.
 *
 * Telemetry (only while counter logging is on): GPU timer queries and a 1x1 staged
 * copy of the metric, read 2-3 frames later, for timing and cut counts.
 *
 * Controls: Tools > HDR Program Denoise (private controller source); settings in
 * the plugin config folder (program-denoise.json), development schema.
 */

#include "hqdn3d-math.hpp"
#include "spatial-math.hpp"
#include "d3d11-compute.hpp"
#include "nvof-probe.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

using hdrtk::denoise::Params;
using hdrtk::denoise::SpatialParams;
namespace compute = hdrtk::denoise::compute;

constexpr const char *kSourceId = "hdr_toolkit_program_denoise_v1";
constexpr const char *kConfigFile = "program-denoise.json";
constexpr int kSchemaVersion = 1; // development schema (brief 2.7): no migrations yet
constexpr uint64_t kLogIntervalNs = 10ull * 1000000000ull;
constexpr int kTelemetryRing = 4;

enum Algorithm { AlgoOff = 0, AlgoIdentity = 1, AlgoHqdn3d = 2, AlgoComputeIdentity = 3 };
enum DebugView { ViewNormal = 0, ViewDifference = 1, ViewHistory = 2, ViewMetric = 3, ViewExact = 4 };

struct Counters {
	std::atomic<uint64_t> callbacks{0};          // every main-rendered callback (all mixes)
	std::atomic<uint64_t> other_mix_skipped{0};  // render target was not the main texture
	std::atomic<uint64_t> unique_frames{0};      // distinct main-mix frames (program_frames_seen)
	std::atomic<uint64_t> duplicates_skipped{0}; // same frame time again on the main texture
	std::atomic<uint64_t> dispatches{0};         // processing passes drawn into the main texture
	std::atomic<uint64_t> history_updates{0};    // temporal history writes
	std::atomic<uint64_t> history_resets{0};     // history (re)initialised: start, resize, mode, manual
	std::atomic<uint64_t> cut_resets{0};         // cuts detected on the GPU (telemetry only)
	std::atomic<uint64_t> failures{0};           // frames passed through for lack of resources
	std::atomic<uint64_t> spatial_passes{0};     // frames that ran the spatial passes
};

struct Settings {
	int algorithm = AlgoOff;
	Params p;
	SpatialParams sp;
	bool force_spatial = false; // development: run the spatial passes even at strength 0
	int compute_variant = compute::VariantTwoCopies;
	int noise_profile = hdrtk::denoise::ProfileIdentity; // development option (curve not frozen)
	double noise_profile_max = 3.0;
	double mix = 1.0;
	int debug_view = ViewNormal;
	double debug_gain = 16.0;
	bool log_counters = false;
};

struct Telemetry {
	gs_timer_range_t *range[kTelemetryRing] = {};
	gs_timer_t *timer[kTelemetryRing] = {};
	gs_stagesurf_t *stage[kTelemetryRing] = {};
	bool pending[kTelemetryRing] = {};
	bool staged[kTelemetryRing] = {};
	int idx = 0;
	std::vector<double> ms;
	double m_sum = 0, m_max = 0;
	int m_n = 0;
};

struct Denoise {
	obs_source_t *controller = nullptr;

	std::mutex mutex; // guards settings
	Settings settings;
	std::atomic<bool> log_now{false};
	std::atomic<bool> reset_counters{false};
	std::atomic<bool> reset_history{false};

	Counters c;

	// graphics thread only
	gs_effect_t *effect = nullptr;
	bool effect_failed = false;
	gs_texture_t *cur = nullptr;
	gs_texture_t *hist[2] = {nullptr, nullptr};
	gs_texture_t *l1 = nullptr, *l2 = nullptr;
	gs_texture_t *metric[2] = {nullptr, nullptr};
	gs_texture_t *sp_tmp = nullptr, *sp_out = nullptr; // spatial H result, spatial result
	gs_texture_t *scratch = nullptr;                   // compute identity: result copy for the exact view
	compute::Spike *spike = nullptr;
	std::string compute_fail_reason;
	int hi = 0, mi = 0;
	bool history_valid = false;
	int last_algo = AlgoOff;
	uint32_t width = 0, height = 0;
	enum gs_color_format format = GS_UNKNOWN;
	bool have_frame_time = false;
	uint64_t last_frame_time = 0;
	uint64_t last_log_ns = 0;
	uint32_t total_frames_at_reset = 0;
	uint32_t lagged_frames_at_reset = 0;
	bool warned = false;
	Telemetry tel;
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

const char *algo_name(int a)
{
	switch (a) {
	case AlgoIdentity:
		return "identity";
	case AlgoHqdn3d:
		return "hqdn3d";
	case AlgoComputeIdentity:
		return "compute-identity";
	default:
		return "off";
	}
}

double nits_per_unit(enum gs_color_space space)
{
	return space == GS_CS_709_SCRGB ? 80.0 : (double)obs_get_video_sdr_white_level();
}

// ---- telemetry ---------------------------------------------------------------

void telemetry_free(Telemetry &t)
{
	for (int i = 0; i < kTelemetryRing; i++) {
		gs_timer_range_destroy(t.range[i]);
		gs_timer_destroy(t.timer[i]);
		gs_stagesurface_destroy(t.stage[i]);
		t.range[i] = nullptr;
		t.timer[i] = nullptr;
		t.stage[i] = nullptr;
		t.pending[i] = false;
		t.staged[i] = false;
	}
}

void telemetry_collect(Denoise *d, double m_cut, bool cut_enabled)
{
	Telemetry &t = d->tel;
	// the oldest slot (written kTelemetryRing - 1 frames ago)
	const int i = (t.idx + 1) % kTelemetryRing;
	if (!t.pending[i])
		return;
	t.pending[i] = false;
	const bool staged = t.staged[i];
	t.staged[i] = false;
	uint64_t ticks = 0, freq = 0;
	bool disjoint = true;
	if (t.range[i] && t.timer[i] && gs_timer_range_get_data(t.range[i], &disjoint, &freq) && !disjoint && freq &&
	    gs_timer_get_data(t.timer[i], &ticks)) {
		t.ms.push_back((double)ticks * 1000.0 / (double)freq);
		if (t.ms.size() > 3000)
			t.ms.erase(t.ms.begin(), t.ms.begin() + 1000);
	}
	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	if (staged && t.stage[i] && gs_stagesurface_map(t.stage[i], &data, &linesize)) {
		float v[4];
		memcpy(v, data, sizeof(v));
		gs_stagesurface_unmap(t.stage[i]);
		if (v[2] > 0.5f) { // metric valid (not a reset frame)
			t.m_sum += v[0];
			t.m_max = std::max(t.m_max, (double)v[0]);
			t.m_n++;
			if (cut_enabled && v[0] >= m_cut)
				d->c.cut_resets++;
		}
	}
}

std::string timing_summary(Telemetry &t)
{
	if (t.ms.empty())
		return "gpu time n/a";
	std::vector<double> v = t.ms;
	std::sort(v.begin(), v.end());
	double sum = 0;
	for (double x : v)
		sum += x;
	const double p95 = v[std::min(v.size() - 1, (size_t)(0.95 * (double)v.size()))];
	char buf[160];
	snprintf(buf, sizeof(buf), "gpu ms avg %.2f p95 %.2f max %.2f (n=%zu)", sum / (double)v.size(), p95, v.back(),
		 v.size());
	t.ms.clear();
	return buf;
}

void log_counters(Denoise *d, const char *why)
{
	const uint32_t total = obs_get_total_frames() - d->total_frames_at_reset;
	const uint32_t lagged = obs_get_lagged_frames() - d->lagged_frames_at_reset;
	const uint64_t unique = d->c.unique_frames.load();
	const uint64_t disp = d->c.dispatches.load();
	int algo, variant, profile;
	double profile_max;
	SpatialParams sp;
	bool force_spatial;
	{
		std::lock_guard<std::mutex> lock(d->mutex);
		algo = d->settings.algorithm;
		sp = d->settings.sp;
		force_spatial = d->settings.force_spatial;
		variant = d->settings.compute_variant;
		profile = d->settings.noise_profile;
		profile_max = d->settings.noise_profile_max;
	}
	char detail[200] = "";
	if (algo == AlgoHqdn3d)
		snprintf(detail, sizeof(detail),
			 " spatial=B S_L %.2f S_C %.2f R %d%s spatial_passes=%" PRIu64 " noise_profile=%s (max %.1f)",
			 sp.luma, sp.chroma, sp.radius, force_spatial ? " (forced)" : "", d->c.spatial_passes.load(),
			 profile == hdrtk::denoise::ProfileMeasured20261005 ? "measured-2026-10-05" : "identity",
			 profile_max);
	else if (algo == AlgoComputeIdentity)
		snprintf(detail, sizeof(detail), " compute_variant=%s",
			 variant == compute::VariantOneCopy    ? "one-copy"
			 : variant == compute::VariantDeferred ? "deferred-context"
							       : "two-copies");
	const std::string timing = timing_summary(d->tel);
	char metric[96] = "metric n/a";
	if (d->tel.m_n > 0)
		snprintf(metric, sizeof(metric), "metric avg %.4f max %.4f", d->tel.m_sum / d->tel.m_n, d->tel.m_max);
	d->tel.m_sum = d->tel.m_max = 0;
	d->tel.m_n = 0;

	const char *verdict = "off (no dispatch expected)";
	if (algo != AlgoOff)
		verdict = disp == unique ? "dispatches == unique frames: OK" : "MISMATCH dispatches != unique frames";
	if (algo == AlgoHqdn3d && d->c.history_updates.load() != disp)
		verdict = "MISMATCH history_updates != dispatches";
	obs_log(LOG_INFO,
		"[denoise] %s: algorithm=%s%s callbacks=%" PRIu64 " other_mix_skipped=%" PRIu64
		" unique_program_frames=%" PRIu64 " duplicate_callbacks_skipped=%" PRIu64 " denoise_dispatches=%" PRIu64
		" history_updates=%" PRIu64 " history_resets=%" PRIu64 " cut_resets=%" PRIu64 " failures=%" PRIu64
		" | obs_total_frames=%u obs_lagged_frames=%u | %s | %s | %s",
		why, algo_name(algo), detail, d->c.callbacks.load(), d->c.other_mix_skipped.load(), unique,
		d->c.duplicates_skipped.load(), disp, d->c.history_updates.load(), d->c.history_resets.load(),
		d->c.cut_resets.load(), d->c.failures.load(), total, lagged, timing.c_str(), metric, verdict);
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
	d->c.cut_resets = 0;
	d->c.failures = 0;
	d->c.spatial_passes = 0;
	d->total_frames_at_reset = obs_get_total_frames();
	d->lagged_frames_at_reset = obs_get_lagged_frames();
	d->tel.ms.clear();
}

// ---- resources ---------------------------------------------------------------

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

void free_spatial(Denoise *d)
{
	for (gs_texture_t **t : {&d->sp_tmp, &d->sp_out, &d->scratch}) {
		gs_texture_destroy(*t);
		*t = nullptr;
	}
}

void free_temporal(Denoise *d)
{
	free_spatial(d);
	for (gs_texture_t **t : {&d->hist[0], &d->hist[1], &d->l1, &d->l2, &d->metric[0], &d->metric[1]}) {
		gs_texture_destroy(*t);
		*t = nullptr;
	}
	d->history_valid = false;
}

// cur: same size/format as the program frame. Recreated on any change.
bool ensure_cur(Denoise *d, gs_texture_t *main_tex)
{
	const uint32_t w = gs_texture_get_width(main_tex);
	const uint32_t h = gs_texture_get_height(main_tex);
	const enum gs_color_format f = gs_texture_get_color_format(main_tex);
	if (d->cur && w == d->width && h == d->height && f == d->format)
		return true;

	gs_texture_destroy(d->cur);
	free_temporal(d); // never reuse old-size history (brief 18)
	d->cur = gs_texture_create(w, h, f, 1, nullptr, GS_RENDER_TARGET);
	d->width = w;
	d->height = h;
	d->format = f;

	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	obs_log(LOG_INFO,
		"[denoise] program frame %ux%u %s, canvas colour space %s, SDR white %.0f nits, HDR nominal peak %.0f nits%s",
		w, h, format_name(f), colorspace_name(ovi.colorspace), obs_get_video_sdr_white_level(),
		obs_get_video_hdr_nominal_peak_level(), d->cur ? "" : " - FAILED to create a texture");
	return d->cur != nullptr;
}

bool ensure_temporal(Denoise *d)
{
	if (d->hist[0] && d->hist[1] && d->l1 && d->l2 && d->metric[0] && d->metric[1])
		return true;
	free_temporal(d);
	const uint32_t w1 = (d->width + 15) / 16, h1 = (d->height + 15) / 16;
	const uint32_t w2 = (w1 + 15) / 16, h2 = (h1 + 15) / 16;
	for (int i = 0; i < 2; i++) {
		d->hist[i] = gs_texture_create(d->width, d->height, d->format, 1, nullptr, GS_RENDER_TARGET);
		d->metric[i] = gs_texture_create(1, 1, GS_RGBA32F, 1, nullptr, GS_RENDER_TARGET);
	}
	d->l1 = gs_texture_create(w1, h1, GS_R32F, 1, nullptr, GS_RENDER_TARGET);
	d->l2 = gs_texture_create(w2, h2, GS_R32F, 1, nullptr, GS_RENDER_TARGET);
	const bool ok = d->hist[0] && d->hist[1] && d->l1 && d->l2 && d->metric[0] && d->metric[1];
	const double mib = (double)d->width * d->height * 8.0 * 3.0 / (1024.0 * 1024.0);
	obs_log(LOG_INFO, "[denoise] temporal resources %s: history 2 x %ux%u %s + current frame copy, about %.0f MiB",
		ok ? "created" : "FAILED", d->width, d->height, format_name(d->format), mib);
	if (!ok)
		free_temporal(d);
	return ok;
}

// Spatial intermediates, created when the spatial passes first run (same size/format).
bool ensure_spatial(Denoise *d)
{
	if (d->sp_tmp && d->sp_out)
		return true;
	d->sp_tmp = d->sp_tmp ? d->sp_tmp
			      : gs_texture_create(d->width, d->height, d->format, 1, nullptr, GS_RENDER_TARGET);
	d->sp_out = d->sp_out ? d->sp_out
			      : gs_texture_create(d->width, d->height, d->format, 1, nullptr, GS_RENDER_TARGET);
	const bool ok = d->sp_tmp && d->sp_out;
	obs_log(LOG_INFO, "[denoise] spatial resources %s: 2 x %ux%u %s, about %.0f MiB", ok ? "created" : "FAILED",
		d->width, d->height, format_name(d->format),
		(double)d->width * d->height * 8.0 * 2.0 / (1024.0 * 1024.0));
	return ok;
}

bool ensure_scratch(Denoise *d)
{
	if (!d->scratch)
		d->scratch = gs_texture_create(d->width, d->height, d->format, 1, nullptr, GS_RENDER_TARGET);
	return d->scratch != nullptr;
}

// ---- drawing -------------------------------------------------------------------

void set_tex(gs_effect_t *e, const char *name, gs_texture_t *t)
{
	gs_effect_set_texture(gs_effect_get_param_by_name(e, name), t);
}

void set_f(gs_effect_t *e, const char *name, float v)
{
	gs_effect_set_float(gs_effect_get_param_by_name(e, name), v);
}

void set_v4(gs_effect_t *e, const char *name, float x, float y, float z, float w)
{
	struct vec4 v;
	vec4_set(&v, x, y, z, w);
	gs_effect_set_vec4(gs_effect_get_param_by_name(e, name), &v);
}

void set_v2(gs_effect_t *e, const char *name, float x, float y)
{
	struct vec2 v;
	vec2_set(&v, x, y);
	gs_effect_set_vec2(gs_effect_get_param_by_name(e, name), &v);
}

// Parameters shared by every technique. gs_technique_end() resets all effect
// parameters, so this runs before each technique.
struct Frame {
	const Settings *s;
	hdrtk::denoise::ShaderParams sp;
	hdrtk::denoise::SpatialShaderParams ssp;
	float npu;
	bool reset;
};

void set_common(Denoise *d, const Frame &f)
{
	gs_effect_t *e = d->effect;
	set_v2(e, "tex_size", (float)d->width, (float)d->height);
	set_f(e, "nits_per_unit", f.npu);
	set_f(e, "units_per_nit", 1.0f / f.npu);
	set_f(e, "knee", f.sp.k);
	set_f(e, "t_luma", f.sp.t_luma);
	set_f(e, "t_chroma", f.sp.t_chroma);
	set_f(e, "m_cut", f.sp.m_cut);
	set_f(e, "protect_amount", f.sp.protect_amount);
	set_f(e, "cut_enabled", f.sp.cut_enabled);
	set_f(e, "reset", f.reset ? 1.0f : 0.0f);
	set_f(e, "mix_amount", (float)f.s->mix);
	set_f(e, "debug_mode", (float)f.s->debug_view);
	set_f(e, "debug_gain", (float)f.s->debug_gain);
	set_f(e, "s_t_luma", f.ssp.t_luma);
	set_f(e, "s_t_chroma", f.ssp.t_chroma);
	set_f(e, "s_radius", (float)f.ssp.radius);
	const hdrtk::denoise::ShaderProfile &np = f.sp.profile;
	set_v4(e, "np0", np.a[0][0], np.a[0][1], np.a[1][0], np.a[1][1]);
	set_v4(e, "np1", np.a[2][0], np.a[2][1], np.a[3][0], np.a[3][1]);
	set_v4(e, "np2", np.a[4][0], np.a[4][1], np.a[5][0], np.a[5][1]);
	set_f(e, "np_count", np.count);
	set_f(e, "np_min", np.min_mult);
	set_f(e, "np_max", np.max_mult);
}

// Draw a full-target quad with `tech` into `target` (size w x h).
void run_pass(Denoise *d, gs_texture_t *target, enum gs_color_space space, const char *tech)
{
	const uint32_t w = gs_texture_get_width(target), h = gs_texture_get_height(target);
	gs_set_render_target_with_color_space(target, nullptr, space);
	gs_set_viewport(0, 0, (int)w, (int)h);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	set_v2(d->effect, "out_size", (float)w, (float)h);
	while (gs_effect_loop(d->effect, tech))
		gs_draw_sprite(nullptr, 0, w, h);
}

struct StateGuard {
	bool prev_srgb;
	StateGuard()
	{
		prev_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		gs_blend_state_push();
		gs_enable_blending(false);
		gs_viewport_push();
		gs_projection_push();
		gs_matrix_push();
		gs_matrix_identity();
	}
	~StateGuard()
	{
		gs_matrix_pop();
		gs_projection_pop();
		gs_viewport_pop();
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(prev_srgb);
	}
};

void draw_identity(Denoise *d, gs_texture_t *main_tex, const Frame &f)
{
	const enum gs_color_space space = gs_get_color_space();
	gs_copy_texture(d->cur, main_tex);
	StateGuard guard;
	set_common(d, f);
	set_tex(d->effect, "image", d->cur);
	run_pass(d, main_tex, space, "Identity");
}

// GPU timing around one frame's processing (telemetry only). Returns the slot.
int tel_begin(Denoise *d, bool telemetry)
{
	Telemetry &t = d->tel;
	const int slot = t.idx;
	if (!telemetry)
		return slot;
	if (!t.range[slot])
		t.range[slot] = gs_timer_range_create();
	if (!t.timer[slot])
		t.timer[slot] = gs_timer_create();
	if (t.range[slot])
		gs_timer_range_begin(t.range[slot]);
	if (t.timer[slot])
		gs_timer_begin(t.timer[slot]);
	return slot;
}

// metric: the 1x1 metric texture to stage for the cut/metric log, or nullptr.
void tel_end(Denoise *d, int slot, bool telemetry, gs_texture_t *metric)
{
	Telemetry &t = d->tel;
	if (!telemetry)
		return;
	if (metric) {
		if (!t.stage[slot])
			t.stage[slot] = gs_stagesurface_create(1, 1, GS_RGBA32F);
		if (t.stage[slot]) {
			gs_stage_texture(t.stage[slot], metric);
			t.staged[slot] = true;
		}
	}
	if (t.timer[slot])
		gs_timer_end(t.timer[slot]);
	if (t.range[slot])
		gs_timer_range_end(t.range[slot]);
	t.pending[slot] = true;
	t.idx = (t.idx + 1) % kTelemetryRing;
}

bool spatial_on(const Frame &f)
{
	return f.ssp.t_luma > 0.0f || f.ssp.t_chroma > 0.0f || f.s->force_spatial;
}

// Spatial (when on) + temporal. The caller has ensured every texture exists.
void draw_hqdn3d(Denoise *d, gs_texture_t *main_tex, const Frame &f, bool telemetry)
{
	const enum gs_color_space space = gs_get_color_space();
	gs_effect_t *e = d->effect;
	const int slot = tel_begin(d, telemetry);

	gs_copy_texture(d->cur, main_tex);
	{
		StateGuard guard;
		gs_texture_t *hist = d->hist[d->hi];
		gs_texture_t *hist_next = d->hist[d->hi ^ 1];
		gs_texture_t *m_prev = d->metric[d->mi];
		gs_texture_t *m_next = d->metric[d->mi ^ 1];

		// spatial before temporal (design section 8.1); cur keeps the unfiltered input
		gs_texture_t *src = d->cur;
		if (spatial_on(f)) {
			set_common(d, f);
			set_tex(e, "image", d->cur);
			run_pass(d, d->sp_tmp, space, "SpatialH");
			set_common(d, f);
			set_tex(e, "image", d->sp_tmp);
			run_pass(d, d->sp_out, space, "SpatialV");
			src = d->sp_out;
			d->c.spatial_passes++;
		}

		if (!f.reset) {
			set_common(d, f);
			set_tex(e, "image", src);
			set_tex(e, "hist_tex", hist);
			run_pass(d, d->l1, space, "MetricBlocks");

			set_common(d, f);
			set_tex(e, "image", d->l1);
			set_v2(e, "in_size", (float)gs_texture_get_width(d->l1), (float)gs_texture_get_height(d->l1));
			run_pass(d, d->l2, space, "MetricReduce");
		}
		set_common(d, f);
		set_tex(e, "image", d->l2);
		set_tex(e, "prev_metric_tex", m_prev);
		set_v2(e, "in_size", (float)gs_texture_get_width(d->l2), (float)gs_texture_get_height(d->l2));
		run_pass(d, m_next, space, "MetricFinal");

		set_common(d, f);
		set_tex(e, "image", src);
		set_tex(e, "hist_tex", hist);
		set_tex(e, "metric_tex", m_next);
		run_pass(d, hist_next, space, "Temporal");

		set_common(d, f);
		set_tex(e, "image", d->cur);
		set_tex(e, "filt_tex", hist_next);
		set_tex(e, "hist_tex", hist);
		set_tex(e, "metric_tex", m_next);
		run_pass(d, main_tex, space, "Output"); // leaves the main texture bound, as OBS had it

		d->hi ^= 1;
		d->mi ^= 1;
		tel_end(d, slot, telemetry, m_next);
	}
}

// Compute identity spike. Returns false (frame untouched) when the compute path is
// unavailable. With the Difference or Exact view, the result is compared with a copy
// of the input taken before the round trip.
bool draw_compute_identity(Denoise *d, gs_texture_t *main_tex, const Frame &f, bool telemetry)
{
	const bool compare = f.s->debug_view == ViewDifference || f.s->debug_view == ViewExact;
	if (compare && !ensure_scratch(d))
		return false;
	if (compare)
		gs_copy_texture(d->cur, main_tex);

	const int slot = tel_begin(d, telemetry);
	const char *why = "";
	const bool ok = compute::identity(d->spike, main_tex, f.s->compute_variant, &why);
	tel_end(d, slot, telemetry, nullptr);
	if (!ok) {
		if (d->compute_fail_reason != why) {
			obs_log(LOG_WARNING, "[denoise] compute identity unavailable (%s): passing the program through",
				why);
			d->compute_fail_reason = why;
		}
		return false;
	}
	d->compute_fail_reason.clear();

	if (compare) {
		const enum gs_color_space space = gs_get_color_space();
		gs_copy_texture(d->scratch, main_tex);
		StateGuard guard;
		set_common(d, f);
		set_tex(d->effect, "image", d->cur);
		set_tex(d->effect, "filt_tex", d->scratch);
		run_pass(d, main_tex, space, "Output");
	}
	return true;
}

// ---- the hook ----------------------------------------------------------------

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

	Settings s;
	{
		std::lock_guard<std::mutex> lock(d->mutex);
		s = d->settings;
	}
	if (s.algorithm != d->last_algo) {
		if (s.algorithm == AlgoHqdn3d)
			d->history_valid = false; // entering a temporal mode: start from the current frame
		if (d->last_algo == AlgoHqdn3d || d->last_algo == AlgoComputeIdentity) {
			free_temporal(d); // release history/spatial VRAM when not in use
			telemetry_free(d->tel);
		}
		if (d->last_algo == AlgoComputeIdentity) {
			compute::destroy(d->spike);
			d->spike = nullptr;
			d->compute_fail_reason.clear();
		}
		d->last_algo = s.algorithm;
	}
	if (d->reset_history.exchange(false))
		d->history_valid = false;

	if (s.algorithm != AlgoOff) {
		Frame f;
		f.s = &s;
		f.sp = hdrtk::denoise::make_shader_params(s.p);
		f.ssp = hdrtk::denoise::make_spatial_shader_params(s.sp);
		f.npu = (float)nits_per_unit(gs_get_color_space());
		f.reset = !d->history_valid;

		bool ok = ensure_effect(d) && ensure_cur(d, main_tex);
		if (ok && s.algorithm == AlgoHqdn3d) {
			ok = ensure_temporal(d);
			if (ok && spatial_on(f))
				ok = ensure_spatial(d);
			else if (ok && d->sp_tmp)
				free_spatial(d);     // spatial switched off: release its two frames
			f.reset = !d->history_valid; // ensure_* may have dropped the history
		}
		if (ok && s.algorithm == AlgoComputeIdentity) {
			if (!d->spike)
				d->spike = compute::create();
			if (s.log_counters)
				telemetry_collect(d, 0.0, false);
			ok = draw_compute_identity(d, main_tex, f, s.log_counters);
			if (ok)
				d->c.dispatches++;
			else
				d->c.failures++; // transparent fallback: the frame is untouched
		} else if (ok) {
			if (s.algorithm == AlgoIdentity) {
				draw_identity(d, main_tex, f);
			} else {
				if (s.log_counters)
					telemetry_collect(d, f.sp.m_cut, f.sp.cut_enabled > 0.5f);
				draw_hqdn3d(d, main_tex, f, s.log_counters);
				d->c.history_updates++;
				if (f.reset)
					d->c.history_resets++;
				d->history_valid = true;
			}
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
	} else if (s.log_counters && now - d->last_log_ns >= kLogIntervalNs) {
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

void ctl_update(void *, obs_data_t *data)
{
	if (!g)
		return;
	Settings s;
	s.algorithm = (int)obs_data_get_int(data, "algorithm");
	if (s.algorithm != AlgoOff && s.algorithm != AlgoIdentity && s.algorithm != AlgoHqdn3d &&
	    !(s.algorithm == AlgoComputeIdentity && compute::available()))
		s.algorithm = AlgoOff; // unknown or unavailable value: fail safe
	s.p.temporal_luma = obs_data_get_double(data, "temporal_luma");
	s.p.temporal_chroma = obs_data_get_double(data, "temporal_chroma");
	s.p.k_nits = obs_data_get_double(data, "comparison_knee_nits");
	s.p.cut_reset = obs_data_get_bool(data, "scene_cut_reset");
	s.p.cut_sensitivity = obs_data_get_double(data, "cut_sensitivity");
	s.p.protection = obs_data_get_bool(data, "transition_protection");
	s.p.protection_amount = obs_data_get_double(data, "protection_amount");
	hdrtk::denoise::sanitize(s.p);
	s.sp.luma = obs_data_get_double(data, "spatial_luma");
	s.sp.chroma = obs_data_get_double(data, "spatial_chroma");
	s.sp.radius = (int)obs_data_get_int(data, "spatial_radius");
	s.sp.k_nits = s.p.k_nits;
	hdrtk::denoise::sanitize(s.sp);
	s.force_spatial = obs_data_get_bool(data, "force_spatial");
	s.noise_profile = (int)obs_data_get_int(data, "noise_profile");
	if (s.noise_profile < 0 || s.noise_profile >= hdrtk::denoise::ProfileCount)
		s.noise_profile = hdrtk::denoise::ProfileIdentity;
	s.noise_profile_max = std::clamp(obs_data_get_double(data, "noise_profile_max"), 1.0, 8.0);
	s.p.profile = hdrtk::denoise::builtin_profile(s.noise_profile, s.noise_profile_max);
	s.sp.profile = s.p.profile;
	s.compute_variant = (int)obs_data_get_int(data, "compute_variant");
	if (s.compute_variant < compute::VariantTwoCopies || s.compute_variant > compute::VariantDeferred)
		s.compute_variant = compute::VariantTwoCopies;
	s.mix = std::clamp(obs_data_get_double(data, "mix"), 0.0, 1.0);
	s.debug_view = (int)obs_data_get_int(data, "debug_view");
	if (s.debug_view < ViewNormal || s.debug_view > ViewExact)
		s.debug_view = ViewNormal;
	s.debug_gain = std::clamp(obs_data_get_double(data, "debug_gain"), 1.0, 256.0);
	s.log_counters = obs_data_get_bool(data, "log_counters");

	int prev;
	{
		std::lock_guard<std::mutex> lock(g->mutex);
		prev = g->settings.algorithm;
		g->settings = s;
	}
	if (prev != s.algorithm)
		obs_log(LOG_INFO, "[denoise] algorithm %s", algo_name(s.algorithm));
	save_settings(data);
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
	obs_data_set_default_double(s, "mix", 1.0);
	// HQDN3D-style starting values; tuning happens in OBS (brief 24), not copied from FFmpeg
	obs_data_set_default_double(s, "temporal_luma", 4.0);
	obs_data_set_default_double(s, "temporal_chroma", 6.0);
	// P2 spatial: off until P1 has run on real footage (decision section 22); radius provisional
	obs_data_set_default_double(s, "spatial_luma", 0.0);
	obs_data_set_default_double(s, "spatial_chroma", 0.0);
	obs_data_set_default_int(s, "spatial_radius", 8);
	obs_data_set_default_bool(s, "force_spatial", false);
	obs_data_set_default_int(s, "compute_variant", compute::VariantTwoCopies);
	obs_data_set_default_int(s, "noise_profile", hdrtk::denoise::ProfileIdentity);
	obs_data_set_default_double(s, "noise_profile_max", 3.0);
	obs_data_set_default_double(s, "comparison_knee_nits", 0.1);
	obs_data_set_default_bool(s, "scene_cut_reset", true);
	obs_data_set_default_double(s, "cut_sensitivity", 50.0);
	obs_data_set_default_bool(s, "transition_protection", true);
	obs_data_set_default_double(s, "protection_amount", 0.8);
	obs_data_set_default_int(s, "debug_view", ViewNormal);
	obs_data_set_default_double(s, "debug_gain", 16.0);
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

bool nvof_probe_clicked(obs_properties_t *, obs_property_t *, void *)
{
	hdrtk::denoise::nvof_probe_async();
	return false;
}

bool reset_history_clicked(obs_properties_t *, obs_property_t *, void *)
{
	if (g)
		g->reset_history = true;
	return false;
}

obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double lo, double hi, double step)
{
	return obs_properties_add_float_slider(props, key, obs_module_text(text), lo, hi, step);
}

obs_properties_t *ctl_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "info", obs_module_text("Denoise.Info"), OBS_TEXT_INFO);
	obs_property_t *p = obs_properties_add_list(props, "algorithm", obs_module_text("Denoise.Algorithm"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Denoise.Algorithm.Off"), AlgoOff);
	obs_property_list_add_int(p, obs_module_text("Denoise.Algorithm.Hqdn3d"), AlgoHqdn3d);
	obs_property_list_add_int(p, obs_module_text("Denoise.Algorithm.Identity"), AlgoIdentity);
	if (compute::available())
		obs_property_list_add_int(p, obs_module_text("Denoise.Algorithm.ComputeIdentity"), AlgoComputeIdentity);
	slider(props, "mix", "Denoise.Mix", 0.0, 1.0, 0.01);
	obs_properties_add_button2(props, "reset_history", obs_module_text("Denoise.ResetHistory"),
				   reset_history_clicked, nullptr);

	obs_properties_t *hq = obs_properties_create();
	slider(hq, "spatial_luma", "Denoise.SpatialLuma", 0.0, 20.0, 0.1);
	slider(hq, "spatial_chroma", "Denoise.SpatialChroma", 0.0, 20.0, 0.1);
	slider(hq, "temporal_luma", "Denoise.TemporalLuma", 0.0, 20.0, 0.1);
	slider(hq, "temporal_chroma", "Denoise.TemporalChroma", 0.0, 20.0, 0.1);
	obs_properties_add_bool(hq, "scene_cut_reset", obs_module_text("Denoise.SceneCutReset"));
	slider(hq, "cut_sensitivity", "Denoise.CutSensitivity", 0.0, 100.0, 1.0);
	obs_properties_add_bool(hq, "transition_protection", obs_module_text("Denoise.TransitionProtection"));
	slider(hq, "protection_amount", "Denoise.ProtectionAmount", 0.0, 1.0, 0.01);
	slider(hq, "comparison_knee_nits", "Denoise.Knee", 0.01, 1.0, 0.01);
	obs_properties_add_group(props, "hqdn3d", obs_module_text("Denoise.Hqdn3d"), OBS_GROUP_NORMAL, hq);

	obs_properties_t *dbg = obs_properties_create();
	p = obs_properties_add_list(dbg, "debug_view", obs_module_text("Denoise.DebugView"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Denoise.DebugView.Normal"), ViewNormal);
	obs_property_list_add_int(p, obs_module_text("Denoise.DebugView.Difference"), ViewDifference);
	obs_property_list_add_int(p, obs_module_text("Denoise.DebugView.History"), ViewHistory);
	obs_property_list_add_int(p, obs_module_text("Denoise.DebugView.Metric"), ViewMetric);
	obs_property_list_add_int(p, obs_module_text("Denoise.DebugView.Exact"), ViewExact);
	slider(dbg, "debug_gain", "Denoise.DebugGain", 1.0, 64.0, 1.0);
	obs_properties_add_bool(dbg, "log_counters", obs_module_text("Denoise.LogCounters"));
	obs_properties_add_button2(dbg, "log_now", obs_module_text("Denoise.LogNow"), log_now_clicked, nullptr);
	obs_properties_add_button2(dbg, "reset_counters", obs_module_text("Denoise.ResetCounters"), reset_clicked,
				   nullptr);
	obs_properties_add_group(props, "debug", obs_module_text("Denoise.Debug"), OBS_GROUP_NORMAL, dbg);

	// development options (decision: radius chosen by measurement, not normal UI)
	obs_properties_t *dev = obs_properties_create();
	p = obs_properties_add_list(dev, "spatial_radius", obs_module_text("Denoise.SpatialRadius"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int r : {6, 8, 12}) {
		char name[16];
		snprintf(name, sizeof(name), "%d", r);
		obs_property_list_add_int(p, name, r);
	}
	obs_properties_add_bool(dev, "force_spatial", obs_module_text("Denoise.ForceSpatial"));
	p = obs_properties_add_list(dev, "noise_profile", obs_module_text("Denoise.NoiseProfile"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Denoise.NoiseProfile.Identity"), hdrtk::denoise::ProfileIdentity);
	obs_property_list_add_int(p, obs_module_text("Denoise.NoiseProfile.Measured"),
				  hdrtk::denoise::ProfileMeasured20261005);
	slider(dev, "noise_profile_max", "Denoise.NoiseProfileMax", 1.0, 8.0, 0.1);
	if (compute::available()) {
		p = obs_properties_add_list(dev, "compute_variant", obs_module_text("Denoise.ComputeVariant"),
					    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(p, obs_module_text("Denoise.ComputeVariant.TwoCopies"),
					  compute::VariantTwoCopies);
		obs_property_list_add_int(p, obs_module_text("Denoise.ComputeVariant.OneCopy"),
					  compute::VariantOneCopy);
		obs_property_list_add_int(p, obs_module_text("Denoise.ComputeVariant.Deferred"),
					  compute::VariantDeferred);
	}
	if (hdrtk::denoise::nvof_probe_available())
		obs_properties_add_button2(dev, "nvof_probe", obs_module_text("Denoise.NvofProbe"), nvof_probe_clicked,
					   nullptr);
	obs_properties_add_group(props, "dev", obs_module_text("Denoise.Dev"), OBS_GROUP_NORMAL, dev);
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
	gs_texture_destroy(g->cur);
	free_temporal(g);
	telemetry_free(g->tel);
	compute::destroy(g->spike);
	g->spike = nullptr;
	obs_leave_graphics();
	delete g;
	g = nullptr;
}
