// CPU tests for HDR Color global stages (brief 5-7, 11.6). No libobs needed.
// Build: g++ -std=c++17 -O2 -Isrc tests/cpu/test_color.cpp src/color/color-math.cpp -o test_color
// Optional: ./test_color --dump-wb  prints WB matrices for tests/cpu/check_wb_vs_brief.py
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

	std::printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
	return failures ? 1 : 0;
}
