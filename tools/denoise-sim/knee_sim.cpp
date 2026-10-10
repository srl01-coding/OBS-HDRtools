// Temporal residual vs comparison knee K and luminance (docs/denoise/STRENGTH_SCALES.md,
// section "Darks"). Build from the repo root:
//   g++ -O2 -std=c++17 -Isrc/denoise tools/denoise-sim/knee_sim.cpp src/denoise/hqdn3d-math.cpp \
//       src/denoise/spatial-math.cpp src/denoise/noise-profile.cpp
#include "hqdn3d-math.hpp"
#include "spatial-math.hpp"
#include <cmath>
#include <cstdio>
#include <random>
using namespace hdrtk::denoise;
static const int W = 96, H = 96;
static Image noisy(double Y, double sig, std::mt19937 &rng)
{
	std::normal_distribution<double> n(0, 1);
	Image im;
	im.w = W;
	im.h = H;
	im.px.resize(W * H);
	for (auto &p : im.px) {
		double v = Y + sig * n(rng);
		p = {v, v, v, 1};
	}
	return im;
}
static double resid(const Image &im, double Y, double sig)
{
	double s = 0;
	int n = 0;
	for (int y = 12; y < H - 12; y++)
		for (int x = 12; x < W - 12; x++) {
			double d = im.at(x, y).g - Y;
			s += d * d;
			n++;
		}
	return std::sqrt(s / n) / sig;
}
int main()
{
	// sigma in nits: measured (2.27, 35.6, 64.4, 89.4); 0.5 nits extrapolated with sigma = 0.0114 (Y + 8)
	const double Y[5] = {0.5, 2.27, 35.6, 64.4, 89.4}, sg[5] = {0.097, 0.117, 0.488, 0.683, 0.796};
	const double Ks[3] = {0.1, 4, 8};
	const double Ss[3] = {4, 8, 12};
	for (double K : Ks)
		for (double S : Ss) {
			printf("K=%4.1f S=%4.0f  temporal residual:", K, S);
			for (int i = 0; i < 5; i++) {
				std::mt19937 rng(1);
				Params p;
				p.temporal_luma = S;
				p.temporal_chroma = S;
				p.k_nits = K;
				Image hist = noisy(Y[i], sg[i], rng);
				for (int f = 0; f < 100; f++) {
					Image c = noisy(Y[i], sg[i], rng), o = c;
					for (size_t j = 0; j < c.px.size(); j++)
						o.px[j] = temporal_pixel(p, c.px[j], hist.px[j], 1.0);
					hist = o;
				}
				printf("  %5.2fn %.2f", Y[i], resid(hist, Y[i], sg[i]));
			}
			printf("\n");
		}
}
