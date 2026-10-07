// Equal-strength comparison of temporal vs spatial B on a static flat patch
// (docs/denoise/STRENGTH_SCALES.md). Build from the repo root:
//   g++ -O2 -std=c++17 [-DMEASURED] -Isrc/denoise tools/denoise-sim/strength_sim.cpp \
//       src/denoise/hqdn3d-math.cpp src/denoise/spatial-math.cpp src/denoise/noise-profile.cpp
#include "hqdn3d-math.hpp"
#include "spatial-math.hpp"
#include <cmath>
#include <cstdio>
#include <random>
using namespace hdrtk::denoise;
#ifdef MEASURED
#define PROF builtin_profile(ProfileMeasured20261005, 3.0)
#else
#define PROF NoiseProfile()
#endif
static const int W = 128, H = 128;
static Image noisy(double Y, double rel, double rho, std::mt19937 &rng)
{
	std::normal_distribution<double> n(0, 1);
	// spatially correlated: AR(1) along rows and columns (separable), unit variance
	std::vector<double> a((size_t)W * H);
	for (auto &v : a)
		v = n(rng);
	if (rho > 0) {
		double s = std::sqrt(1 - rho * rho);
		for (int y = 0; y < H; y++)
			for (int x = 1; x < W; x++)
				a[y * W + x] = rho * a[y * W + x - 1] + s * a[y * W + x];
		for (int x = 0; x < W; x++)
			for (int y = 1; y < H; y++)
				a[y * W + x] = rho * a[(y - 1) * W + x] + s * a[y * W + x];
	}
	Image im;
	im.w = W;
	im.h = H;
	im.px.resize(a.size());
	for (size_t i = 0; i < a.size(); i++) {
		double v = Y * (1 + rel * a[i]);
		im.px[i] = {v, v, v, 1};
	}
	return im;
}
static double resid(const Image &im, double Y, double rel)
{
	double s = 0;
	int n = 0;
	for (int y = 16; y < H - 16; y++)
		for (int x = 16; x < W - 16; x++) {
			double d = im.at(x, y).g - Y;
			s += d * d;
			n++;
		}
	return std::sqrt(s / n) / (Y * rel);
}
int main()
{
	const double Ys[2] = {50.0, 2.27}, rels[2] = {0.011, 0.051};
	const double rhos[2] = {0.0, 0.6};
	const double S[] = {1, 2, 4, 6, 8, 12, 20};
	for (int zi = 0; zi < 2; zi++)
		for (double rho : rhos) {
			printf("\nY=%.2f nits, rel noise %.1f%%, spatial lag-1 rho=%.1f (temporal iid)\n", Ys[zi],
			       rels[zi] * 100, rho);
			printf("  S   | temporal: mean W/0.9  residual | spatial B: residual\n");
			for (double s : S) {
				std::mt19937 rng(1);
				Params p;
				p.temporal_luma = s;
				p.temporal_chroma = s;
				p.profile = PROF;
				Image hist = noisy(Ys[zi], rels[zi], rho, rng);
				double wsum = 0;
				int wn = 0;
				for (int f = 0; f < 120; f++) {
					Image cur = noisy(Ys[zi], rels[zi], rho, rng), out = cur;
					for (size_t i = 0; i < cur.px.size(); i++) {
						out.px[i] = temporal_pixel(p, cur.px[i], hist.px[i], 1.0);
						if (f >= 60) {
							double yc = cur.px[i].g, yo = out.px[i].g, yh = hist.px[i].g;
							if (std::fabs(yh - yc) > 1e-12) {
								wsum += (yo - yc) / (yh - yc);
								wn++;
							}
						}
					}
					hist = out;
				}
				double rt = resid(hist, Ys[zi], rels[zi]);
				SpatialParams sp;
				sp.luma = s;
				sp.chroma = s;
				sp.profile = PROF;
				std::mt19937 rng2(2);
				Image in = noisy(Ys[zi], rels[zi], rho, rng2), o;
				spatial_b(sp, in, o);
				printf("  %4.0f | %8.2f            %5.2f    | %5.2f\n", s, wn ? wsum / wn / 0.9 : 0, rt,
				       resid(o, Ys[zi], rels[zi]));
			}
		}
}
