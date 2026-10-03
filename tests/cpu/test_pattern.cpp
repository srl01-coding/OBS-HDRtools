// CPU tests for the developer pattern source math (no libobs needed).
// Build: g++ -std=c++17 -O2 -Isrc tests/cpu/test_pattern.cpp src/pattern/pattern-math.cpp -o test_pattern
#include "pattern/pattern-math.hpp"

#include <cmath>
#include <cstdio>

using namespace hdrtk::pattern;

static int failures = 0;
#define CHECK(cond, ...)                                  \
	do {                                              \
		if (!(cond)) {                            \
			std::printf("FAIL: " __VA_ARGS__); \
			std::printf("\n");                \
			failures++;                       \
		}                                         \
	} while (0)

int main()
{
	// 1. HLG OETF / inverse round trip over the full signal range incl. >100%.
	double max_rt = 0;
	for (int i = 0; i <= 11000; i++) {
		const double e = i / 10000.0;
		max_rt = std::fmax(max_rt, std::fabs(hlg_oetf(hlg_inverse_oetf(e)) - e));
	}
	CHECK(max_rt < 1e-12, "HLG round trip error %g", max_rt);
	std::printf("hlg_round_trip_max_abs_error %.3g\n", max_rt);

	// 2. HLG level -> nits -> OBS encoder model returns the same level.
	double max_obs = 0;
	for (int i = 0; i < kHlgCount; i++) {
		const double e = kHlgLevels[i];
		const double back = obs_nits_to_hlg_level(hlg_level_to_obs_nits(e));
		max_obs = std::fmax(max_obs, std::fabs(back - e));
		std::printf("hlg %6.2f%% -> %10.4f nits -> code %7.2f\n", e * 100, hlg_level_to_obs_nits(e),
			    64 + 876 * e);
	}
	CHECK(max_obs < 1e-12, "OBS HLG model round trip error %g", max_obs);

	// 3. Known anchors: 75% HLG = 203 nits (BT.2408), 100% = 1000 nits.
	CHECK(std::fabs(hlg_level_to_obs_nits(0.75) - 203.0) < 0.5, "75%% HLG = %g nits", hlg_level_to_obs_nits(0.75));
	CHECK(std::fabs(hlg_level_to_obs_nits(1.0) - 1000.0) < 1e-3, "100%% HLG = %g nits", hlg_level_to_obs_nits(1.0));

	// 4. Chart generation: exact neutral values, colour rows contain negative
	//    Rec.709 components (Rec.2020 primaries), luminance of colour patches = 100.
	Settings s;
	std::vector<Rgba> px;
	CHECK(generate(s, px), "generate chart");
	const uint32_t w = s.width, h = s.height;
	for (int i = 0; i < kNeutralCount; i++) {
		const uint32_t x = (uint32_t)((i + 0.5) * w / kNeutralCount);
		const Rgba &p = px[(size_t)(h * 0.09) * w + x];
		CHECK(p.r == (float)kNeutralNits[i] && p.g == p.r && p.b == p.r && p.a == 1.0f,
		      "neutral patch %d = %g (want %g)", i, p.r, kNeutralNits[i]);
	}
	bool saw_negative = false;
	const uint32_t cy = (uint32_t)(h * 0.72);
	for (int i = 0; i < 9; i++) {
		const uint32_t x = (uint32_t)((i + 0.5) * w / 9);
		const Rgba &p = px[(size_t)cy * w + x];
		const double y = kLuma[0] * p.r + kLuma[1] * p.g + kLuma[2] * p.b;
		CHECK(std::fabs(y - 100.0) < 1e-3, "colour patch %d luminance %g", i, y);
		if (p.r < 0 || p.g < 0 || p.b < 0)
			saw_negative = true;
	}
	CHECK(saw_negative, "no negative Rec.709 components in colour row");

	// 5. Alpha row levels.
	const uint32_t ay = (uint32_t)(h * 0.9);
	for (int i = 0; i < kAlphaCount * 2; i++) {
		const uint32_t x = (uint32_t)((i + 0.5) * w / (kAlphaCount * 2));
		const Rgba &p = px[(size_t)ay * w + x];
		CHECK(p.a == (float)kAlphaLevels[i / 2], "alpha patch %d = %g", i, p.a);
	}

	// 6. Invalid settings rejected.
	Settings bad = s;
	bad.flat_nits = NAN;
	bad.mode = Mode::FlatField;
	CHECK(!generate(bad, px), "NaN flat level accepted");

	std::printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
	return failures ? 1 : 0;
}
