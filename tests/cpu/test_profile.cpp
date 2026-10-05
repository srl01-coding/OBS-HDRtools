// CPU tests for the luminance-dependent threshold profile (docs/denoise/NOISE_PROFILE.md).
// Build: g++ -std=c++17 -O2 -Isrc tests/cpu/test_profile.cpp src/denoise/noise-profile.cpp
//        src/denoise/hqdn3d-math.cpp src/denoise/spatial-math.cpp src/denoise/nlm-math.cpp -o test_profile
#include "denoise/nlm-math.hpp"
#include "denoise/noise-profile.hpp"
#include "denoise/spatial-math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using namespace hdrtk::denoise;

static int failures = 0;
#define CHECK(cond, ...)                                   \
	do {                                               \
		if (!(cond)) {                             \
			std::printf("FAIL: " __VA_ARGS__); \
			std::printf("\n");                 \
			failures++;                        \
		}                                          \
	} while (0)

static Image noisy_image(int w, int h, double level, double rel, uint64_t seed)
{
	std::mt19937_64 rng(seed);
	std::normal_distribution<double> N(0, 1);
	Image im;
	im.w = w;
	im.h = h;
	im.px.resize((size_t)w * h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const double v = x < w / 2 ? level : 40.0 * level;
			im.px[(size_t)y * w + x] =
				Rgba{v * (1 + rel * N(rng)), v * (1 + rel * N(rng)), v * (1 + rel * N(rng)), 1};
		}
	return im;
}

int main()
{
	const double ys[] = {-50, -1e-6, 0,  1e-6, 1e-3, 0.01, 0.5, 2.27, 5,
			     18,  35.6,  50, 64.4, 70,   89.4, 203, 1000, 1e5};

	// 1. identity: exactly 1 everywhere, including n > 0 with all multipliers 1
	{
		NoiseProfile id;
		NoiseProfile ones = builtin_profile(ProfileMeasured20261005, 3.0);
		for (int i = 0; i < ones.n; i++)
			ones.m[i] = 1.0;
		const ShaderProfile sid = make_shader_profile(id), sones = make_shader_profile(ones);
		bool ok = true;
		for (double y : ys)
			ok = ok && profile_multiplier(id, y) == 1.0 && profile_multiplier(ones, y) == 1.0 &&
			     profile_multiplier_f(sid, (float)y) == 1.0f &&
			     profile_multiplier_f(sones, (float)y) == 1.0f;
		CHECK(ok, "identity profile is not exactly 1");
		CHECK(std::isfinite(profile_multiplier(builtin_profile(1, 3.0), NAN)), "NaN luminance not finite");
	}

	// 2. measured profile: anchors, bounds, constant outside, monotone segments
	{
		const NoiseProfile p = builtin_profile(ProfileMeasured20261005, 3.0);
		CHECK(p.n == 4, "measured profile anchors");
		CHECK(std::fabs(profile_multiplier(p, 2.27) - 3.0) < 1e-12, "dark anchor not clamped to max 3");
		CHECK(std::fabs(profile_multiplier(p, 35.6) - 1.30) < 1e-12, "anchor 35.6");
		CHECK(std::fabs(profile_multiplier(p, 64.4) - 1.00) < 1e-12, "anchor 64.4 (reference)");
		CHECK(std::fabs(profile_multiplier(p, 89.4) - 0.84) < 1e-12, "anchor 89.4");
		CHECK(profile_multiplier(p, 0.0) == 3.0 && profile_multiplier(p, -1e-4) == 3.0 &&
			      profile_multiplier(p, -20) == profile_multiplier(p, 20),
		      "below first anchor / uses |Y|");
		CHECK(std::fabs(profile_multiplier(p, 1e4) - 0.84) < 1e-12, "above last anchor");
		const NoiseProfile p8 = builtin_profile(ProfileMeasured20261005, 8.0);
		CHECK(std::fabs(profile_multiplier(p8, 2.27) - 4.67) < 1e-12, "max 8 keeps the measured 4.67");
		double prev = 1e9;
		bool mono = true;
		for (double l = -3; l < 8; l += 0.01) {
			const double m = profile_multiplier(p8, std::exp2(l));
			mono = mono && m <= prev + 1e-12; // this profile falls with luminance everywhere
			prev = m;
		}
		CHECK(mono, "measured profile not monotone");
		// half-way between two anchors in log2
		const double mid = std::exp2(0.5 * (std::log2(35.6) + std::log2(64.4)));
		CHECK(std::fabs(profile_multiplier(p, mid) - 1.15) < 1e-12, "log-linear interpolation");
	}

	// 3. float mirror of the shader function vs double
	{
		const NoiseProfile p = builtin_profile(ProfileMeasured20261005, 3.0);
		const ShaderProfile s = make_shader_profile(p);
		double worst = 0;
		for (double l = -12; l < 16; l += 0.003) {
			const double y = std::exp2(l);
			worst = std::max(worst,
					 std::fabs(profile_multiplier(p, y) - profile_multiplier_f(s, (float)y)));
		}
		std::printf("profile float mirror vs double: max abs error %.2e\n", worst);
		CHECK(worst < 1e-5, "float mirror diverges");
	}

	// 4. sanitize: unsorted, duplicates, NaN, bounds
	{
		NoiseProfile q;
		q.n = 3;
		q.l[0] = 6;
		q.m[0] = 1;
		q.l[1] = 1;
		q.m[1] = 4;
		q.l[2] = 6;
		q.m[2] = 0.8;
		q.max_mult = 99;
		q.min_mult = -1;
		sanitize(q);
		CHECK(q.l[0] == 1 && q.m[0] == 4 && q.l[1] < q.l[2] && q.max_mult == 16 && q.min_mult == 0.1,
		      "sanitize");
		NoiseProfile bad = q;
		bad.m[1] = NAN;
		sanitize(bad);
		CHECK(bad.n == 0, "NaN anchor did not fall back to identity");
	}

	// 5. temporal: identity profile bit-identical; measured profile lets dark noise through
	{
		Params a, b;
		a.temporal_luma = b.temporal_luma = 4;
		a.temporal_chroma = b.temporal_chroma = 6;
		b.profile = builtin_profile(ProfileMeasured20261005, 3.0);
		std::mt19937_64 rng(4);
		std::normal_distribution<double> N(0, 1);
		Params ones = b;
		for (int i = 0; i < ones.profile.n; i++)
			ones.profile.m[i] = 1.0;
		bool same = true;
		int dark_more = 0, dark_n = 0, mid_same = 0, mid_n = 0;
		for (int i = 0; i < 4000; i++) {
			const double lv = i % 2 ? 2.27 : 64.4;
			const double rel = i % 2 ? 0.05 : 0.01;
			const Rgba c{lv * (1 + rel * N(rng)), lv * (1 + rel * N(rng)), lv * (1 + rel * N(rng)), 1};
			const Rgba h{lv * (1 + rel * N(rng)), lv * (1 + rel * N(rng)), lv * (1 + rel * N(rng)), 1};
			const Rgba o0 = temporal_pixel(a, c, h, 1.0), o1 = temporal_pixel(ones, c, h, 1.0);
			same = same && o0.r == o1.r && o0.g == o1.g && o0.b == o1.b;
			const Rgba ob = temporal_pixel(b, c, h, 1.0);
			const double moved0 = std::fabs(o0.g - c.g), moved1 = std::fabs(ob.g - c.g);
			if (i % 2) {
				dark_n++;
				dark_more += moved1 > moved0;
			} else {
				mid_n++;
				// at the reference level the multiplier is ~1 (interpolated between anchors)
				mid_same += std::fabs(moved1 - moved0) <= 0.05 * moved0 + 1e-9;
			}
		}
		std::printf(
			"temporal at 2.27 nits (5%% noise): history used more in %d / %d pixels; at 64.4 nits unchanged "
			"(within 5%%) in %d / %d\n",
			dark_more, dark_n, mid_same, mid_n);
		CHECK(same, "profile of ones is not bit-identical to identity");
		CHECK(dark_more > 0.8 * dark_n, "measured profile did not open the threshold in the darks");
		CHECK(mid_same > 0.95 * mid_n, "measured profile changed the reference level");

		// float mirror of the temporal pixel with the profile
		const ShaderParams s = make_shader_params(b);
		double worst = 0;
		for (int i = 0; i < 4000; i++) {
			const double lv = std::exp2(-2 + 0.004 * i);
			const float c[4] = {(float)(lv * (1 + 0.03 * N(rng))), (float)(lv * (1 + 0.03 * N(rng))),
					    (float)(lv * (1 + 0.03 * N(rng))), 1};
			const float h[4] = {(float)(lv * (1 + 0.03 * N(rng))), (float)(lv * (1 + 0.03 * N(rng))),
					    (float)(lv * (1 + 0.03 * N(rng))), 1};
			float o[4];
			temporal_pixel_f(s, c, h, 1.0f, o);
			const Rgba od = temporal_pixel(b, Rgba{c[0], c[1], c[2], 1}, Rgba{h[0], h[1], h[2], 1}, 1.0);
			worst = std::max(worst, std::fabs(o[1] - od.g) / std::max(1.0, lv));
		}
		std::printf("temporal float mirror with profile: max rel error %.2e\n", worst);
		CHECK(worst < 1e-4, "temporal mirror with profile diverges");
	}

	// 6. spatial B mirror with the profile; NLM naive == offset with the profile
	{
		SpatialParams sp;
		sp.luma = 5;
		sp.chroma = 7;
		sp.profile = builtin_profile(ProfileMeasured20261005, 3.0);
		const Image im = noisy_image(41, 23, 2.27, 0.05, 9);
		Image od, of;
		spatial_b(sp, im, od);
		spatial_b_f(make_spatial_shader_params(sp), im, of);
		double worst = 0;
		for (size_t i = 0; i < od.px.size(); i++)
			worst = std::max(worst, std::fabs(od.px[i].g - of.px[i].g) / std::max(1.0, od.px[i].g));
		std::printf("spatial B float mirror with profile: max rel error %.2e\n", worst);
		CHECK(worst < 1e-4, "spatial mirror with profile diverges");

		NlmParams np;
		np.luma = 4;
		np.chroma = 6;
		np.temporal = true;
		np.policy = NlmPolicyH;
		np.profile = sp.profile;
		const Image prev = noisy_image(41, 23, 2.27, 0.05, 10);
		NlmFrame f{&im, &prev, &prev, 0.8};
		Image a, b;
		nlm_naive(np, f, a);
		nlm_offset(np, f, b);
		double d = 0;
		for (size_t i = 0; i < a.px.size(); i++)
			d = std::max(d, std::fabs(a.px[i].g - b.px[i].g) / std::max(1.0, a.px[i].g));
		std::printf("NLM naive vs offset with profile: %.2e\n", d);
		CHECK(d < 1e-9, "NLM with profile: offset-major differs from naive");
	}

	std::printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
	return failures ? 1 : 0;
}
