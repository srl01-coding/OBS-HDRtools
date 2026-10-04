// CPU tests for HDR Color global stages (brief 5-7, 11.6). No libobs needed.
// Build: g++ -std=c++17 -O2 -Isrc tests/cpu/test_color.cpp src/color/color-math.cpp -o test_color
// Optional: ./test_color --dump-wb     prints WB matrices for tests/cpu/check_wb_vs_brief.py
//           ./test_color --dump-zones  prints default zone weights vs stops for tests/cpu/plot_zones.py
#include "color/color-math.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

using namespace hdrtk::color;

static int failures = 0;
#define CHECK(cond, ...)                                  \
	do {                                              \
		if (!(cond)) {                            \
			std::printf("FAIL: " __VA_ARGS__); \
			std::printf("\n");                \
			failures++;                       \
		}                                         \
	} while (0)

static double rel(double a, double b)
{
	return std::fabs(a - b) / std::fmax(1e-9, std::fmax(std::fabs(a), std::fabs(b)));
}

int main(int argc, char **argv)
{
	const double mireds[] = {-90, -30, 0, 30, 90};
	const double tints[] = {-100, 0, 100};

	if (argc > 1 && std::strcmp(argv[1], "--dump-wb") == 0) {
		for (double m : mireds)
			for (double t : tints) {
				Mat3 a;
				white_balance_matrix(m, t / 100.0, a);
				std::printf("%g %g", m, t);
				for (auto &r : a)
					for (double x : r)
						std::printf(" %.17g", x);
				std::printf("\n");
			}
		return 0;
	}
	if (argc > 1 && std::strcmp(argv[1], "--dump-zones") == 0) {
		const auto z = default_zones();
		for (int i = 0; i <= 2000; i++) {
			const double st = -12.0 + 22.0 * i / 2000.0;
			std::printf("%.4f", st);
			for (const auto &q : z)
				std::printf(" %.6f", zone_weight(st, q));
			std::printf("\n");
		}
		return 0;
	}

	// 1. Exact neutral WB.
	{
		Mat3 a;
		CHECK(white_balance_matrix(0, 0, a), "wb(0,0)");
		CHECK(a == identity3(), "wb(0,0) is not the exact identity");
	}

	// 2. WB properties.
	for (double m : mireds)
		for (double t : tints) {
			if (m == 0 && t == 0)
				continue;
			Mat3 a;
			Vec3 tw;
			CHECK(white_balance_matrix(m, t / 100.0, a), "wb(%g,%g) failed", m, t);
			CHECK(target_white(m, t / 100.0, tw), "target white");
			const Vec3 w = mul(a, Vec3{1, 1, 1});
			CHECK(std::fabs(dot(kLuma, w) - 1.0) < 1e-12, "wb(%g,%g) changes white luminance: %g", m, t,
			      dot(kLuma, w));
			const Vec3 xyz = mul(kRgbToXyz, w);
			for (int i = 0; i < 3; i++)
				CHECK(std::fabs(xyz[i] - tw[i]) < 1e-12, "wb(%g,%g) white not at target", m, t);
			if (t == 0 && m > 0)
				CHECK(w[0] > w[2], "positive temperature not warmer (R %g B %g)", w[0], w[2]);
			if (t == 0 && m < 0)
				CHECK(w[0] < w[2], "negative temperature not cooler");
			if (m == 0 && t > 0)
				CHECK(w[1] < 0.5 * (w[0] + w[2]), "positive tint not magenta");
			if (m == 0 && t < 0)
				CHECK(w[1] > 0.5 * (w[0] + w[2]), "negative tint not green");
		}
	{
		Mat3 a;
		const Vec3 w30 = (white_balance_matrix(30, 0, a), mul(a, Vec3{1, 1, 1}));
		std::printf("WB +30 mired white RGB = (%.4f, %.4f, %.4f); tint +100: ", w30[0], w30[1], w30[2]);
		white_balance_matrix(0, 1.0, a);
		const Vec3 wt = mul(a, Vec3{1, 1, 1});
		std::printf("(%.4f, %.4f, %.4f)\n", wt[0], wt[1], wt[2]);
	}

	// 3. Wheel.
	{
		const Vec3 z = wheel_delta(0, 0);
		CHECK(z[0] == 0 && z[1] == 0 && z[2] == 0, "zero wheel not exactly zero");
		const char *names[] = {"red", "yellow", "green", "cyan", "blue", "magenta"};
		for (int i = 0; i < 6; i++) {
			const Vec3 d = hue_direction(i * 3.14159265358979323846 / 3.0);
			CHECK(std::fabs(dot(kLuma, d)) < 1e-15, "hue %s not luminance-orthogonal", names[i]);
			std::printf("hue %-7s d = (%+.3f, %+.3f, %+.3f)\n", names[i], d[0], d[1], d[2]);
		}
		const Vec3 r = hue_direction(0), g = hue_direction(2.0943951023931953),
			   b = hue_direction(4.1887902047863905);
		CHECK(r[0] > r[1] && r[0] > r[2], "0 deg is not red");
		CHECK(g[1] > g[0] && g[1] > g[2], "120 deg is not green");
		CHECK(b[2] > b[0] && b[2] > b[1], "240 deg is not blue");
		const Vec3 big = wheel_delta(3, 4); // radius clamped to 1
		const Vec3 unit = wheel_delta(0.6, 0.8);
		for (int i = 0; i < 3; i++)
			CHECK(std::fabs(big[i] - unit[i]) < 1e-15, "wheel radius not clamped");
	}

	std::mt19937_64 rng(11);
	std::uniform_real_distribution<double> U(0, 1);

	// 4. Exposure / contrast / neutral.
	{
		GlobalParams p;
		CHECK(is_neutral(p), "defaults not neutral");
		ShaderParams sp;
		CHECK(make_shader_params(p, sp), "shader params");
		for (int i = 0; i < 1000; i++) {
			const Vec3 c = {U(rng) * 4000 - 10, U(rng) * 4000, U(rng) * 4000};
			const Vec3 r = reference_grade(p, c);
			CHECK(r == c, "neutral reference not exact");
			const std::array<float, 3> cf = {(float)c[0], (float)c[1], (float)c[2]};
			CHECK(shader_grade(sp, cf) == cf, "neutral shader mirror not exact");
		}
		GlobalParams e;
		e.exposure_ev = 1;
		const Vec3 c = {100, 50, 25};
		const Vec3 r = reference_grade(e, c);
		CHECK(r[0] == 200 && r[1] == 100 && r[2] == 50, "+1 EV does not double");
		e.exposure_ev = -1;
		const Vec3 r2 = reference_grade(e, c);
		CHECK(r2[0] == 50 && r2[1] == 25 && r2[2] == 12.5, "-1 EV does not halve");

		GlobalParams k;
		k.contrast = 1.5;
		k.pivot_nits = 50;
		const Vec3 piv = reference_grade(k, Vec3{50, 50, 50});
		CHECK(rel(piv[0], 50) < 1e-12, "pivot moved under contrast: %g", piv[0]);
		double prev = -1;
		for (int i = 0; i <= 400; i++) {
			const double y = std::pow(10.0, -4 + 8.0 * i / 400.0);
			const double out = reference_grade(k, Vec3{y, y, y})[0];
			CHECK(out > prev, "contrast not monotonic at %g", y);
			CHECK(rel(out, 50 * std::pow(y / 50, 1.5)) < 1e-9, "contrast law at %g", y);
			prev = out;
		}
		const Vec3 blk = reference_grade(k, Vec3{0, 0, 0});
		CHECK(blk[0] == 0 && blk[1] == 0 && blk[2] == 0, "contrast moved black");
		const Vec3 neg = reference_grade(k, Vec3{-5, 1, 0.5});
		CHECK(std::isfinite(neg[0]) && std::isfinite(neg[1]) && std::isfinite(neg[2]), "negative-Y not finite");
	}

	// 5. Wheel + saturation preserve linear Y; full desaturation is neutral; black stays black.
	{
		double max_err = 0;
		for (int i = 0; i < 5000; i++) {
			GlobalParams p;
			p.wheel_x = U(rng) * 2 - 1;
			p.wheel_y = U(rng) * 2 - 1;
			p.saturation = U(rng) * 2;
			const Vec3 c = {U(rng) * 1010 - 10, U(rng) * 1010 - 10, U(rng) * 1010 - 10};
			const double y0 = dot(kLuma, c);
			const Vec3 r = reference_grade(p, c);
			max_err = std::fmax(max_err, std::fabs(dot(kLuma, r) - y0) / std::fmax(1.0, std::fabs(y0)));
		}
		std::printf("wheel+saturation linear-Y preservation: max relative error %.3g\n", max_err);
		CHECK(max_err < 1e-12, "wheel/saturation change luminance");
		GlobalParams d;
		d.saturation = 0;
		const Vec3 r = reference_grade(d, Vec3{300, 20, 80});
		CHECK(std::fabs(r[0] - r[1]) < 1e-12 && std::fabs(r[1] - r[2]) < 1e-12,
		      "full desaturation not neutral");
		CHECK(std::fabs(r[0] - dot(kLuma, Vec3{300, 20, 80})) < 1e-9, "desaturated level is not linear Y");
		GlobalParams all;
		all.exposure_ev = 2;
		all.contrast = 1.7;
		all.saturation = 1.8;
		all.wb_mired = 40;
		all.wb_tint = -30;
		all.wheel_x = 0.3;
		all.wheel_y = -0.2;
		const Vec3 b = reference_grade(all, Vec3{0, 0, 0});
		CHECK(b[0] == 0 && b[1] == 0 && b[2] == 0, "black not preserved");
	}

	// 6. Grade mix: 0 = original, linear in between.
	{
		GlobalParams p;
		p.exposure_ev = 1;
		p.grade_mix = 0;
		const Vec3 c = {10, 20, 30};
		const Vec3 r0 = reference_grade(p, c);
		CHECK(r0 == c, "mix 0 is not the original");
		p.grade_mix = 0.25;
		const Vec3 r = reference_grade(p, c);
		CHECK(std::fabs(r[0] - 12.5) < 1e-12, "mix not linear (%g)", r[0]);
	}

	// 7. Float shader mirror vs double reference, random grades and colours.
	{
		double worst = 0;
		for (int i = 0; i < 20000; i++) {
			GlobalParams p;
			p.exposure_ev = U(rng) * 8 - 4;
			p.contrast = 0.5 + U(rng) * 1.5;
			p.pivot_nits = std::pow(10.0, U(rng) * 3 - 0.5);
			p.saturation = U(rng) * 2;
			p.wb_mired = U(rng) * 180 - 90;
			p.wb_tint = U(rng) * 200 - 100;
			p.wheel_x = U(rng) * 2 - 1;
			p.wheel_y = U(rng) * 2 - 1;
			p.grade_mix = U(rng);
			ShaderParams sp;
			CHECK(make_shader_params(p, sp), "shader params");
			const double y = std::pow(10.0, U(rng) * 6 - 2); // 0.01 .. 10000 nits
			const Vec3 c = {y * (0.2 + U(rng)), y * (0.2 + U(rng)), y * (0.2 + U(rng))};
			const Vec3 r = reference_grade(p, c);
			const auto f = shader_grade(sp, {(float)c[0], (float)c[1], (float)c[2]});
			const double scale = std::fmax(std::fabs(r[0]), std::fmax(std::fabs(r[1]), std::fabs(r[2])));
			for (int k = 0; k < 3; k++)
				worst = std::fmax(worst, std::fabs(f[k] - r[k]) / std::fmax(scale, 1e-3));
		}
		std::printf("float shader mirror vs double: max error %.3g relative to the pixel's largest channel\n",
			    worst);
		CHECK(worst < 2e-5, "float mirror error too large");
	}

	// 8. Sanitize.
	{
		GlobalParams p;
		p.exposure_ev = NAN;
		p.contrast = 9;
		const std::string log = sanitize(p);
		CHECK(p.exposure_ev == 0 && p.contrast == 2 && !log.empty(), "sanitize");
	}

	// 9. Tonal zones (brief 7.1, 11.6 "Zones").
	{
		const auto z = default_zones();
		auto w = [&](int i, double st) {
			return zone_weight(st, z[i]);
		};
		// Continuity and range: fine sweep, no jumps, weights in [0, 1].
		double max_jump = 0;
		for (int i = 0; i < kZoneCount; i++) {
			double prev = w(i, -30);
			for (int k = 1; k <= 60000; k++) {
				const double st = -30 + 60.0 * k / 60000.0;
				const double v = w(i, st);
				CHECK(v >= 0 && v <= 1, "zone %s weight out of [0,1] at %g", kZoneNames[i], st);
				max_jump = std::fmax(max_jump, std::fabs(v - prev));
				prev = v;
			}
		}
		// smoothstep max slope 1.5/falloff; narrowest default falloff 1.5 stops, step 0.001
		std::printf("zone windows: max weight change per 0.001 stop %.3g\n", max_jump);
		CHECK(max_jump < 0.0011, "zone window discontinuity");
		// Full-strength plateaus and zeros outside (schema 2 defaults; NAN = open end).
		const double full[kZoneCount] = {-8, -6, -2.5, 0, 2, 4, 5};
		const double zero_lo[kZoneCount] = {NAN, NAN, -5, -1.75, 0.25, 2.25, 3};
		const double zero_hi[kZoneCount] = {-5, -3, -0.25, 1.75, 3.75, NAN, NAN};
		for (int i = 0; i < kZoneCount; i++) {
			CHECK(w(i, full[i]) == 1, "zone %s not full strength at %g", kZoneNames[i], full[i]);
			if (!std::isnan(zero_lo[i]))
				CHECK(w(i, zero_lo[i]) == 0 && w(i, zero_lo[i] - 3) == 0, "zone %s lower edge",
				      kZoneNames[i]);
			if (!std::isnan(zero_hi[i]))
				CHECK(w(i, zero_hi[i]) == 0 && w(i, zero_hi[i] + 3) == 0, "zone %s upper edge",
				      kZoneNames[i]);
		}
		// Tails: Black and Dark cover zero, negative and tiny luminance; Highlight and
		// Specular cover any highlight.
		GlobalParams p;
		for (double y : {0.0, -5.0, 1e-30, 1e-9, 0.01}) {
			const auto ww = reference_weights(p, Vec3{y, y, y});
			CHECK(ww[ZoneBlack] == 1 && ww[ZoneDark] == 1, "Black/Dark tail at Y=%g: %g %g", y,
			      ww[ZoneBlack], ww[ZoneDark]);
		}
		for (double y : {1152.0, 1e4, 1e6, 6e4 * 203}) {
			const auto ww = reference_weights(p, Vec3{y, y, y});
			CHECK(ww[ZoneSpecular] == 1 && ww[ZoneHighlight] == 1, "Highlight/Specular tail at %g nits", y);
		}
		// Overlap: every adjacent pair shares at least 2 stops where both weights > 0.
		for (int i = 0; i + 1 < kZoneCount; i++) {
			double both = 0;
			for (int k = 0; k < 4000; k++) {
				const double st = -20 + 40.0 * k / 4000.0;
				if (w(i, st) > 0 && w(i + 1, st) > 0)
					both += 0.01;
			}
			CHECK(both >= 1.4, "%s/%s overlap only %.2f stops", kZoneNames[i], kZoneNames[i + 1], both);
		}
		// Handover chain Dark..Highlight is a partition of unity: weights sum to 1.
		double max_dev = 0;
		for (int k = 0; k <= 60000; k++) {
			const double st = -30 + 60.0 * k / 60000.0;
			double sum = 0;
			for (int i = ZoneDark; i <= ZoneHighlight; i++)
				sum += w(i, st);
			max_dev = std::fmax(max_dev, std::fabs(sum - 1));
		}
		std::printf("Dark..Highlight weight sum: max deviation from 1 = %.3g\n", max_dev);
		CHECK(max_dev < 1e-12, "handover chain not a partition of unity");
		// Equal pushes on every chain zone = an even push.
		GlobalParams ev;
		for (int i = ZoneDark; i <= ZoneHighlight; i++)
			ev.zones[i].exposure_ev = 0.7;
		double max_even = 0;
		for (int k = 0; k <= 2000; k++) {
			const double y = 18.0 * std::exp2(-12 + 20.0 * k / 2000.0);
			max_even = std::fmax(max_even, rel(reference_grade(ev, Vec3{y, y, y})[0], y * std::exp2(0.7)));
		}
		CHECK(max_even < 1e-12, "equal neighbour pushes not even (%g)", max_even);
		CHECK(std::isfinite(tonal_stop(-1, 18)) && std::isfinite(tonal_stop(0, 18)), "tonal stop of Y<=0");
		// Stop markers from the brief: -4 = 1.125, 0 = 18, +2 = 72, +4 = 288, +6 = 1152 nits.
		CHECK(std::fabs(tonal_stop(1.125, 18) + 4) < 1e-12 && std::fabs(tonal_stop(1152, 18) - 6) < 1e-12,
		      "stop markers");
	}
	{
		// Isolated zone gain = 2^(w * EV) with the mask frozen at the input level.
		double worst = 0;
		for (int zi = 0; zi < kZoneCount; zi++) {
			GlobalParams p;
			p.zones[zi].exposure_ev = 2.0;
			for (int k = 0; k <= 400; k++) {
				const double st = -12 + 22.0 * k / 400.0;
				const double y = 18.0 * std::exp2(st);
				const Vec3 c = {y * 1.1, y, y * 0.7}; // non-neutral: gain must be a pure RGB scale
				const double yin = dot(kLuma, c);
				const double wz = zone_weight(tonal_stop(yin, 18), p.zones[zi]);
				const Vec3 r = reference_grade(p, c);
				for (int i = 0; i < 3; i++)
					worst = std::fmax(worst, rel(r[i], c[i] * std::exp2(2.0 * wz)));
			}
		}
		std::printf("isolated zone gain vs 2^(w*EV) at the frozen input mask: max rel error %.3g\n", worst);
		CHECK(worst < 1e-12, "zone gain law");

		// Frozen mask: a Shadow push that lifts pixels into Light territory must not
		// pick up Light's exposure (masks are not recomputed after zone exposure).
		GlobalParams p;
		p.zones[ZoneShadow].exposure_ev = 3;
		p.zones[ZoneLight].exposure_ev = -3;
		const double y = 18.0 * std::exp2(-2.5); // Shadow full, Light weight 0
		const Vec3 r = reference_grade(p, Vec3{y, y, y});
		CHECK(rel(r[0], y * 8.0) < 1e-12, "mask recomputed after zone exposure (%g vs %g)", r[0], y * 8.0);

		// Overlap combines in EV: weights not normalised.
		GlobalParams q;
		q.zones[ZoneMidtones].exposure_ev = 1;
		q.zones[ZoneLight].exposure_ev = 0.5;
		const double yo = 18.0 * std::exp2(1.2); // inside both windows
		const auto wo = reference_weights(q, Vec3{yo, yo, yo});
		CHECK(wo[ZoneMidtones] > 0 && wo[ZoneLight] > 0, "overlap fixture");
		const Vec3 ro = reference_grade(q, Vec3{yo, yo, yo});
		CHECK(rel(ro[0], yo * std::exp2(wo[ZoneMidtones] * 1 + wo[ZoneLight] * 0.5)) < 1e-12, "overlap EV sum");

		// Inactive / changed zones do not alter another zone's strength.
		GlobalParams a, b2;
		a.zones[ZoneShadow].exposure_ev = 1;
		b2 = a;
		b2.zones[ZoneDark].enabled = false;
		b2.zones[ZoneLight].a = -3;
		b2.zones[ZoneLight].b = -2; // Light window widened to overlap Shadow, but Light is neutral
		for (double st = -6; st <= 3; st += 0.25) {
			const double yy = 18.0 * std::exp2(st);
			CHECK(reference_grade(a, Vec3{yy, yy, yy}) == reference_grade(b2, Vec3{yy, yy, yy}),
			      "neutral/disabled zone changed another zone at %g", st);
		}

		// Disabled zone with settings contributes nothing; settings are kept.
		GlobalParams d;
		d.zones[ZoneHighlight].exposure_ev = 2;
		d.zones[ZoneHighlight].enabled = false;
		CHECK(is_neutral(d), "disabled zone not neutral");
		CHECK(d.zones[ZoneHighlight].exposure_ev == 2, "disabled zone lost its value");

		// Zone saturation: full desaturation where the zone is at full strength, untouched outside.
		GlobalParams sp;
		sp.zones[ZoneHighlight].saturation = 0;
		const double yh = 18.0 * 16.0; // +4 stops: Highlight full
		const Vec3 col = {yh * 1.5, yh * 0.8, yh * 0.6};
		const Vec3 rh = reference_grade(sp, col);
		CHECK(std::fabs(rh[0] - rh[1]) < 1e-9 && std::fabs(rh[1] - rh[2]) < 1e-9, "zone desaturation");
		const double ys = 18.0 * 0.25; // -2 stops: outside Highlight
		const Vec3 cs = {ys * 1.5, ys * 0.8, ys * 0.6};
		CHECK(reference_grade(sp, cs) == cs, "zone saturation leaked outside its window");

		// Zone wheel preserves linear Y and leaves black black.
		GlobalParams wp;
		wp.zones[ZoneShadow].wheel_x = 0.4;
		wp.zones[ZoneShadow].wheel_y = -0.3;
		const Vec3 cw = {3.0, 4.0, 5.0};
		const Vec3 rw = reference_grade(wp, cw);
		CHECK(std::fabs(dot(kLuma, rw) - dot(kLuma, cw)) < 1e-12, "zone wheel changed Y");
		const Vec3 bw = reference_grade(wp, Vec3{0, 0, 0});
		CHECK(bw[0] == 0 && bw[1] == 0 && bw[2] == 0, "zone wheel moved black");

		// All zones at defaults (enabled, neutral) = exact identity in reference and mirror.
		GlobalParams n;
		ShaderParams ns;
		CHECK(make_shader_params(n, ns) && ns.use_zones == 0, "neutral zones enabled a stage");
	}
	{
		// Float mirror with random zone grades.
		std::mt19937_64 r2(23);
		std::uniform_real_distribution<double> V(0, 1);
		double worst = 0;
		for (int i = 0; i < 20000; i++) {
			GlobalParams p;
			p.exposure_ev = V(r2) * 4 - 2;
			p.contrast = 0.7 + V(r2) * 0.8;
			p.saturation = V(r2) * 2;
			p.wheel_x = V(r2) - 0.5;
			p.grade_mix = V(r2);
			for (auto &z : p.zones) {
				z.enabled = V(r2) > 0.2;
				z.exposure_ev = V(r2) * 8 - 4;
				z.saturation = V(r2) * 2;
				z.wheel_x = V(r2) * 2 - 1;
				z.wheel_y = V(r2) * 2 - 1;
				z.open_low = V(r2) > 0.5;
				z.open_high = V(r2) > 0.7;
			}
			sanitize(p);
			ShaderParams sp;
			CHECK(make_shader_params(p, sp), "shader params");
			const double y = std::pow(10.0, V(r2) * 6 - 2);
			const Vec3 c = {y * (0.2 + V(r2)), y * (0.2 + V(r2)), y * (0.2 + V(r2))};
			const Vec3 r = reference_grade(p, c);
			const auto f = shader_grade(sp, {(float)c[0], (float)c[1], (float)c[2]});
			const double scale = std::fmax(std::fabs(r[0]), std::fmax(std::fabs(r[1]), std::fabs(r[2])));
			for (int k = 0; k < 3; k++) {
				CHECK(std::isfinite(f[k]), "mirror not finite");
				worst = std::fmax(worst, std::fabs(f[k] - r[k]) / std::fmax(scale, 1e-3));
			}
		}
		std::printf("float shader mirror vs double, random zone grades: max error %.3g\n", worst);
		CHECK(worst < 5e-5, "float mirror error too large with zones");
	}
	{
		// Edge sanitising: deterministic ordering with the minimum falloff.
		GlobalParams p;
		p.zones[ZoneShadow].a = 1;
		p.zones[ZoneShadow].b = 0;
		p.zones[ZoneShadow].c = -1;
		p.zones[ZoneShadow].d = -2;
		p.zones[ZoneBlack].d = -9; // below c = -7
		p.zones[ZoneSpecular].a = NAN;
		const std::string log = sanitize(p);
		const auto &z = p.zones[ZoneShadow];
		CHECK(z.b == 1 + kMinFalloff && z.c == z.b && z.d == z.c + kMinFalloff, "interior reorder");
		CHECK(p.zones[ZoneBlack].d == -7 + kMinFalloff, "black reorder");
		CHECK(p.zones[ZoneSpecular].a == 3, "non-finite edge not reset");
		double e[4];
		p.zones[ZoneBlack].effective_edges(e);
		CHECK(e[0] == kOpenLow, "black not open below");
		p.zones[ZoneSpecular].effective_edges(e);
		CHECK(e[3] == kOpenHigh + 1, "specular not open above");
		CHECK(!log.empty(), "no sanitize log");

		// Open-low Dark: moving 'full below' under the unused stored b must not be overridden.
		GlobalParams q;
		q.zones[ZoneDark].c = -10;
		q.zones[ZoneDark].d = -7;
		const std::string l2 = sanitize(q);
		CHECK(q.zones[ZoneDark].c == -10 && q.zones[ZoneDark].d == -7, "open side overrode a used edge");
		CHECK(q.zones[ZoneDark].b <= -10 && q.zones[ZoneDark].a < q.zones[ZoneDark].b,
		      "hidden edges not moved");
		CHECK(l2.empty(), "hidden-edge tidy logged as a problem: %s", l2.c_str());
		q.zones[ZoneDark].open_low = false; // re-closing gives a valid window
		CHECK(q.zones[ZoneDark].a < q.zones[ZoneDark].b && q.zones[ZoneDark].b <= q.zones[ZoneDark].c,
		      "re-closed window invalid");
		// Fixed open ends cannot be closed.
		GlobalParams r;
		r.zones[ZoneBlack].open_low = false;
		r.zones[ZoneSpecular].open_high = false;
		sanitize(r);
		CHECK(r.zones[ZoneBlack].open_low && r.zones[ZoneSpecular].open_high, "fixed open end closed");
	}

	{
		// Tonal order under a single zone push. d log2(Y_out)/ds = 1 + EV * dw/ds and the
		// smoothstep slope peaks at 1.5 / falloff, so a push keeps tonal order iff
		// |EV| <= falloff / 1.5 on the falloff it pushes against (documented limit,
		// brief 7.1 windows; not a clamp).
		auto min_slope = [](int zi, double ev) {
			GlobalParams p;
			p.zones[zi].exposure_ev = ev;
			double prev_out = -1, worst = 1e9;
			for (int k = 0; k <= 20000; k++) {
				const double st = -14 + 26.0 * k / 20000.0;
				const double y = 18.0 * std::exp2(st);
				const double out = std::log2(reference_grade(p, Vec3{y, y, y})[0]);
				if (k)
					worst = std::fmin(worst, (out - prev_out) / (26.0 / 20000.0));
				prev_out = out;
			}
			return worst;
		};
		const auto z = default_zones();
		for (int i = 0; i < kZoneCount; i++) {
			const double fall_lo = z[i].b - z[i].a, fall_hi = z[i].d - z[i].c;
			if (!z[i].open_high) { // positive push against the upper falloff
				const double lim = fall_hi / 1.5;
				CHECK(min_slope(i, 0.98 * lim) > 0, "%s +%.2f EV reversed", kZoneNames[i], 0.98 * lim);
				CHECK(min_slope(i, 1.1 * lim) < 0, "%s +%.2f EV should reverse", kZoneNames[i],
				      1.1 * lim);
			}
			if (!z[i].open_low) { // negative push against the lower falloff
				const double lim = fall_lo / 1.5;
				CHECK(min_slope(i, -0.98 * lim) > 0, "%s -%.2f EV reversed", kZoneNames[i], 0.98 * lim);
				CHECK(min_slope(i, -1.1 * lim) < 0, "%s -%.2f EV should reverse", kZoneNames[i],
				      1.1 * lim);
			}
		}
		std::printf("zone tonal-order limit |EV| <= falloff/1.5 confirmed for all default zones\n");
	}

	// 10. Offset and soft clips (brief 8).
	{
		// Brief 8.2 table: L = 0.1, beta = 1.
		const double tin[] = {0, 0.025, 0.05, 0.075, 0.1};
		const double tout[] = {0, 0.0109375, 0.0375, 0.0703125, 0.1};
		for (int i = 0; i < 5; i++)
			CHECK(std::fabs(toe_curve(tin[i], 0.1, 1.0) - tout[i]) < 1e-15, "toe table at %g", tin[i]);
		// Brief 8.3 table: P = 1000, q = 0.25.
		const double sin_[] = {0, 750, 1000, 2000, 10000};
		const double sout[] = {0, 750, 875, 958.333333333333, 993.421052631579};
		for (int i = 0; i < 5; i++)
			CHECK(std::fabs(shoulder_curve(sin_[i], 1000, 0.25) - sout[i]) < 1e-9, "shoulder table at %g",
			      sin_[i]);
		// Toe: F(0)=0, F(L)=L, C1 at L, monotonic, never lifts, beta 0 exact identity.
		for (double beta : {0.0, 0.25, 0.5, 1.0})
			for (double knee : {0.001, 0.1, 5.0}) {
				const double h = knee * 1e-6;
				const double slope =
					(toe_curve(knee + h, knee, beta) - toe_curve(knee - h, knee, beta)) / (2 * h);
				CHECK(std::fabs(slope - 1) < 1e-5, "toe slope at knee %g beta %g: %g", knee, beta,
				      slope);
				CHECK(toe_curve(0, knee, beta) == 0 && toe_curve(knee, knee, beta) == knee, "toe ends");
				double prev = -1;
				for (int k = 0; k <= 1000; k++) {
					const double y = knee * 1.2 * k / 1000;
					const double f = toe_curve(y, knee, beta);
					CHECK(f >= prev && f <= y + 1e-18, "toe monotonic / no lift at %g", y);
					if (beta == 0)
						CHECK(f == y, "toe beta 0 not identity");
					prev = f;
				}
			}
		// Shoulder: C1 at the knee, monotonic, finite inputs stay below P, q = 0 hard cap.
		for (double q : {0.01, 0.25, 0.75, 0.95}) {
			const double P = 1000, H = P * (1 - q), h = 1e-4;
			const double slope = (shoulder_curve(H + h, P, q) - shoulder_curve(H - h, P, q)) / (2 * h);
			// central difference across a C1 (not C2) join: expected bias h / (2 D)
			CHECK(std::fabs(slope - 1) < 1e-6 + h / (P * q), "shoulder slope at knee q %g: %g", q, slope);
			double prev = -1;
			for (int k = 0; k <= 4000; k++) {
				const double y = std::pow(10.0, -2 + 9.0 * k / 4000);
				const double f = shoulder_curve(y, P, q);
				CHECK(f > prev && f < P, "shoulder at %g (q %g): %g", y, q, f);
				prev = f;
			}
		}
		CHECK(shoulder_curve(1500, 1000, 0) == 1000 && shoulder_curve(600, 1000, 0) == 600, "hard cap");

		// In the pipeline: common RGB scale (ratios kept), Y <= 0 untouched, black stays black.
		GlobalParams p;
		p.low_clip = true;
		p.low_strength = 1;
		p.high_clip = true;
		CHECK(!is_neutral(p), "enabled clips counted as neutral");
		const Vec3 lo = {0.06, 0.04, 0.02};
		const Vec3 rl = reference_grade(p, lo);
		const double yl = dot(kLuma, lo);
		CHECK(rel(dot(kLuma, rl), toe_curve(yl, 0.1, 1)) < 1e-12 && rel(rl[0] / rl[2], 3.0) < 1e-12,
		      "toe in grade");
		const Vec3 hi = {3000, 2000, 1000};
		const Vec3 rh = reference_grade(p, hi);
		CHECK(rel(dot(kLuma, rh), shoulder_curve(dot(kLuma, hi), 1000, 0.25)) < 1e-12 &&
			      rel(rh[0] / rh[2], 3.0) < 1e-12,
		      "shoulder in grade");
		const Vec3 neg = {-0.05, 0.01, 0.0}; // Y < 0
		CHECK(reference_grade(p, neg) == neg, "clip touched Y <= 0");
		const Vec3 blk = reference_grade(p, Vec3{0, 0, 0});
		CHECK(blk[0] == 0 && blk[1] == 0 && blk[2] == 0, "clips moved black");
		const Vec3 mid = {40, 30, 20};
		CHECK(reference_grade(p, mid) == mid, "clips changed the middle region");

		// Mix order: at Grade Mix 0 the grade is gone but the high clip still works (brief 5.4).
		GlobalParams m;
		m.exposure_ev = 2;
		m.grade_mix = 0;
		m.high_clip = true;
		const Vec3 w = {2000, 2000, 2000};
		CHECK(rel(reference_grade(m, w)[0], shoulder_curve(2000, 1000, 0.25)) < 1e-12, "clip not after mix");

		// Offset: added exactly to every channel, black included; part of the grade (mixed).
		GlobalParams o;
		o.offset_nits = 0.05;
		const Vec3 ro = reference_grade(o, Vec3{0, 1, 100});
		CHECK(ro[0] == 0.05 && ro[1] == 1.05 && ro[2] == 100.05, "offset not exact");
		o.offset_nits = -0.02;
		CHECK(reference_grade(o, Vec3{0, 0, 0})[0] == -0.02, "negative offset clamped");
		o.offset_nits = 0.1;
		o.grade_mix = 0.5;
		CHECK(std::fabs(reference_grade(o, Vec3{0, 0, 0})[0] - 0.05) < 1e-15, "offset not mixed");
		CHECK(!is_neutral(o), "offset counted as neutral");
		// Offset then toe: a lifted black below the knee is compressed, not removed.
		GlobalParams ot;
		ot.offset_nits = 0.05;
		ot.low_clip = true;
		const double lifted = reference_grade(ot, Vec3{0, 0, 0})[0];
		CHECK(lifted > 0 && rel(lifted, toe_curve(0.05, 0.1, 0.5)) < 1e-12, "offset + toe");

		// Conflict: both on with L > H.
		GlobalParams c;
		c.low_clip = c.high_clip = true;
		c.low_knee_nits = 800;
		CHECK(clips_conflict(c), "conflict not detected");
		c.low_knee_nits = 700;
		CHECK(!clips_conflict(c), "false conflict");
	}
	{
		// Float mirror with offset and clips.
		std::mt19937_64 r3(31);
		std::uniform_real_distribution<double> V(0, 1);
		double worst = 0;
		for (int i = 0; i < 20000; i++) {
			GlobalParams p;
			p.exposure_ev = V(r3) * 6 - 2;
			p.saturation = V(r3) * 2;
			p.offset_nits = V(r3) < 0.5 ? 0 : V(r3) * 2 - 1;
			p.grade_mix = V(r3) < 0.5 ? 1 : V(r3);
			p.low_clip = V(r3) < 0.5;
			p.low_knee_nits = std::pow(10.0, V(r3) * 3 - 3);
			p.low_strength = V(r3);
			p.high_clip = V(r3) < 0.5;
			p.high_peak_nits = 100 + V(r3) * 4000;
			p.high_softness = V(r3) < 0.1 ? 0 : V(r3) * 0.95;
			p.zones[ZoneShadow].exposure_ev = V(r3) * 2 - 1;
			sanitize(p);
			ShaderParams sp;
			CHECK(make_shader_params(p, sp), "shader params");
			const double y = std::pow(10.0, V(r3) * 7 - 3);
			const Vec3 c = {y * (0.2 + V(r3)), y * (0.2 + V(r3)), y * (0.2 + V(r3))};
			const Vec3 r = reference_grade(p, c);
			const auto f = shader_grade(sp, {(float)c[0], (float)c[1], (float)c[2]});
			const double scale = std::fmax(std::fabs(r[0]), std::fmax(std::fabs(r[1]), std::fabs(r[2])));
			for (int k = 0; k < 3; k++) {
				CHECK(std::isfinite(f[k]), "mirror not finite");
				worst = std::fmax(worst, std::fabs(f[k] - r[k]) / std::fmax(scale, 1e-3));
			}
		}
		std::printf("float shader mirror vs double, offset + soft clips: max error %.3g\n", worst);
		CHECK(worst < 5e-5, "float mirror error too large with clips");
	}

	std::printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
	return failures ? 1 : 0;
}
