// CPU tests for the live noise measurement analysis (src/denoise/noise-meter-math.cpp).
// Build from the repo root:
//   g++ -O2 -std=c++17 -Isrc/denoise tests/cpu/test_noise_meter.cpp src/denoise/noise-meter-math.cpp

#include "noise-meter-math.hpp"

#include <cmath>
#include <cstdio>
#include <functional>
#include <random>

using namespace hdrtk::denoise;

static int failures = 0;
#define CHECK(c, msg)                                          \
	do {                                                   \
		if (!(c)) {                                    \
			std::printf("FAIL: %s (%s)\n", msg, #c); \
			failures++;                            \
		}                                              \
	} while (0)

// A static scene: pixels log-spread over [lo, hi] nits; 60 frames with noise sigma(Y),
// optional multiplicative flicker, optional moving pixels.
static NoiseFit run(const char *name, double lo, double hi, const std::function<double(double)> &sigma, double flicker,
		    double moving, double *gain_range = nullptr, double *outliers = nullptr)
{
	const size_t n = 200000;
	std::mt19937 rng(7);
	std::uniform_real_distribution<double> U(0, 1);
	std::normal_distribution<double> N(0, 1);
	std::vector<double> clean(n);
	std::vector<uint8_t> mov(n);
	for (size_t i = 0; i < n; i++) {
		clean[i] = lo * std::pow(hi / lo, U(rng));
		mov[i] = U(rng) < moving;
	}
	NoiseAnalysis a;
	a.reset(n);
	std::vector<float> f(n);
	for (int t = 0; t < 60; t++) {
		const double g = 1.0 + flicker * std::sin(t * 1.7);
		for (size_t i = 0; i < n; i++) {
			double y = clean[i];
			if (mov[i])
				y *= 1.0 + 0.5 * U(rng); // a moving edge: large frame-to-frame changes
			f[i] = (float)(g * (y + sigma(y) * N(rng)));
		}
		a.add_frame(f.data(), n);
	}
	const auto bands = a.bands();
	const NoiseFit fit = fit_knee(bands);
	std::printf(
		"--- %s\n%s\n", name,
		noise_report(bands, fit, a.gain_std(), a.gain_range(), a.outlier_fraction(), a.frames(), n).c_str());
	if (gain_range)
		*gain_range = a.gain_range();
	if (outliers)
		*outliers = a.outlier_fraction();
	return fit;
}

int main()
{
	// 1. Affine noise sigma = a (Y + 8): the fit recovers the floor.
	{
		const NoiseFit f = run("affine K=8", 0.3, 400, [](double y) { return 0.011 * (y + 8.0); }, 0, 0);
		CHECK(f.ok, "affine fit");
		CHECK(f.knee > 6.5 && f.knee < 9.5, "affine knee");
		CHECK(f.spread_fit < 1.15 && f.spread_default > 5, "affine spreads");
	}
	// 2. Purely proportional noise: floor near zero.
	{
		const NoiseFit f = run("proportional", 0.3, 400, [](double y) { return 0.01 * y; }, 0, 0);
		CHECK(f.ok && f.knee < 0.05, "proportional knee");
	}
	// 3. Flicker (3% global) is removed before the per-pixel statistics.
	{
		double gr = 0;
		const NoiseFit f = run(
			"affine K=8 + 3% flicker", 0.3, 400, [](double y) { return 0.011 * (y + 8.0); }, 0.03, 0, &gr);
		CHECK(f.ok && f.knee > 6.0 && f.knee < 10.0, "flicker knee");
		CHECK(gr > 0.04, "flicker reported");
	}
	// 4. 10% moving pixels: medians hold; motion is reported.
	{
		double out = 0;
		const NoiseFit f = run(
			"affine K=8 + 10% moving", 0.3, 400, [](double y) { return 0.011 * (y + 8.0); }, 0, 0.10,
			nullptr, &out);
		CHECK(f.ok && f.knee > 5.5 && f.knee < 11.0 && f.spread_fit < 1.5, "moving knee");
		CHECK(out > 0.05, "motion reported");
	}
	// 5. Shot + read noise sigma^2 = 0.1^2 + 0.004 Y: a fit exists and improves uniformity.
	{
		const NoiseFit f =
			run("shot + read", 0.3, 400, [](double y) { return std::sqrt(0.01 + 0.004 * y); }, 0, 0);
		CHECK(f.ok && f.spread_fit < f.spread_default, "shot+read improves");
	}
	// 6. Too narrow a range: no fit.
	{
		const NoiseFit f = run("narrow", 50, 120, [](double y) { return 0.011 * (y + 8.0); }, 0, 0);
		CHECK(!f.ok, "narrow range rejected");
	}
	// 7. Non-finite pixels are dropped, not propagated.
	{
		NoiseAnalysis a;
		a.reset(1000);
		std::vector<float> v(1000, 10.0f);
		for (int t = 0; t < 10; t++) {
			v[3] = t == 4 ? NAN : 10.0f;
			for (size_t i = 0; i < v.size(); i++)
				if (i != 3)
					v[i] = 10.0f + 0.1f * (float)((i * 7 + t * 13) % 5);
			a.add_frame(v.data(), v.size());
		}
		const auto b = a.bands(10);
		CHECK(!b.empty() && std::isfinite(b[0].sigma) && b[0].count == 999, "NaN pixel dropped");
	}
	std::printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
	return failures ? 1 : 0;
}
