// CPU tests for the NLMeans references (docs/denoise/NLMEANS_DESIGN.md section 11).
// Build: g++ -std=c++17 -O2 -Isrc tests/cpu/test_nlm.cpp src/denoise/nlm-math.cpp
//        src/denoise/hqdn3d-math.cpp -o test_nlm
#include "denoise/nlm-math.hpp"

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

static double luma(const Rgba &c)
{
	return kLumaR * c.r + kLumaG * c.g + kLumaB * c.b;
}

static bool same(const Rgba &a, const Rgba &b)
{
	return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static Image blank(int w, int h)
{
	Image im;
	im.w = w;
	im.h = h;
	im.px.assign((size_t)w * h, Rgba{});
	return im;
}

// Structured noisy test image: blocks of different levels and colours, a ramp, noise.
static Image scene(int w, int h, uint64_t seed, double noise = 0.02)
{
	std::mt19937_64 rng(seed);
	std::normal_distribution<double> N(0, 1);
	Image im = blank(w, h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			double v = ((x / 7 + y / 5) % 3 == 0) ? 18 : ((x * 3 / w) == 1 ? 203 : 900);
			const double t = (double)x / w;
			Rgba p{v * (0.7 + 0.6 * t), v, v * (1.2 - 0.4 * t), 1};
			p.r *= 1 + noise * N(rng);
			p.g *= 1 + noise * N(rng);
			p.b *= 1 + noise * N(rng);
			im.px[(size_t)y * w + x] = p;
		}
	return im;
}

static double max_rel_diff(const Image &a, const Image &b)
{
	double m = 0;
	for (size_t i = 0; i < a.px.size(); i++) {
		const double s = std::max(1.0, std::fabs(luma(a.px[i])));
		m = std::max(m, std::fabs(a.px[i].r - b.px[i].r) / s);
		m = std::max(m, std::fabs(a.px[i].g - b.px[i].g) / s);
		m = std::max(m, std::fabs(a.px[i].b - b.px[i].b) / s);
	}
	return m;
}

int main()
{
	// 1. naive == offset-major, spatial and temporal, all policies, odd sizes, chroma term
	{
		int cases = 0;
		double worst = 0;
		for (int policy = 0; policy < 3; policy++)
			for (int P : {0, 1, 2})
				for (int lam = 0; lam < 2; lam++) {
					NlmParams p;
					p.luma = 4;
					p.chroma = 6;
					p.patch = P;
					p.search = 2;
					p.tsearch = 3;
					p.temporal = true;
					p.policy = policy;
					p.lambda = lam ? 0.5 : 0.0;
					const Image cur = scene(23, 17, 1 + P), pin = scene(23, 17, 9 + P),
						    pout = scene(23, 17, 5, 0.005);
					NlmFrame f{&cur, &pin, &pout, 0.7};
					Image a, b;
					nlm_naive(p, f, a);
					nlm_offset(p, f, b);
					worst = std::max(worst, max_rel_diff(a, b));
					cases++;
				}
		std::printf("naive vs offset-major: %d cases, max rel diff %.2e\n", cases, worst);
		CHECK(worst < 1e-9, "offset-major differs from naive");
	}

	// 2. strength 0 is an exact identity; far candidates leave the pixel exact
	{
		NlmParams p;
		p.temporal = true;
		const Image cur = scene(31, 19, 3), prev = scene(31, 19, 4);
		NlmFrame f{&cur, &prev, &prev, 1.0};
		Image o;
		nlm_offset(p, f, o);
		bool ok = true;
		for (size_t i = 0; i < cur.px.size(); i++)
			ok = ok && same(o.px[i], cur.px[i]);
		CHECK(ok, "S = 0 not identity");
		// isolated pixel far from all neighbours (in a flat field of a very different level)
		Image iso = blank(15, 15);
		for (auto &q : iso.px)
			q = Rgba{10, 10, 10, 1};
		iso.px[7 * 15 + 7] = Rgba{900, 850, 950, 1};
		p.luma = p.chroma = 6;
		p.temporal = false;
		NlmFrame fi{&iso, nullptr, nullptr, 1.0};
		nlm_offset(p, fi, o);
		CHECK(same(o.px[7 * 15 + 7], iso.px[7 * 15 + 7]), "isolated pixel changed (impulse)");
	}

	// 3. constant, black, neutral; convex bounds (no overshoot)
	{
		NlmParams p;
		p.luma = 8;
		p.chroma = 8;
		p.temporal = true;
		p.policy = NlmPolicyH;
		Image c = blank(20, 14), z = blank(20, 14);
		for (auto &q : c.px)
			q = Rgba{150, 40, 7, 1};
		NlmFrame f{&c, &c, &c, 1.0};
		Image o;
		nlm_offset(p, f, o);
		CHECK(max_rel_diff(o, c) < 1e-12, "constant changed");
		NlmFrame fz{&z, &z, &z, 1.0};
		nlm_offset(p, fz, o);
		bool black = true;
		for (auto &q : o.px)
			black = black && q.r == 0 && q.g == 0 && q.b == 0;
		CHECK(black, "black moved");
		std::mt19937_64 rng(2);
		std::uniform_real_distribution<double> U(5, 50);
		Image n = blank(20, 14), n2 = blank(20, 14);
		for (size_t i = 0; i < n.px.size(); i++) {
			const double v = U(rng), v2 = U(rng);
			n.px[i] = Rgba{v, v, v, 1};
			n2.px[i] = Rgba{v2, v2, v2, 1};
		}
		NlmFrame fn{&n, &n2, &n2, 1.0};
		nlm_offset(p, fn, o);
		bool neutral = true;
		for (auto &q : o.px)
			neutral = neutral && q.r == q.g && q.g == q.b;
		CHECK(neutral, "neutral gained colour");

		// bounds: Y and chroma components within the min/max over all candidate values
		const Image cur = scene(29, 21, 12, 0.05), prev = scene(29, 21, 13, 0.05);
		p.policy = NlmPolicyA;
		NlmFrame fb{&cur, &prev, &prev, 1.0};
		nlm_offset(p, fb, o);
		int bad = 0;
		for (int y = 0; y < cur.h; y++)
			for (int x = 0; x < cur.w; x++) {
				double lo[4] = {1e300, 1e300, 1e300, 1e300}, hi[4] = {-1e300, -1e300, -1e300, -1e300};
				const int R = std::max(p.search, p.tsearch);
				for (const Image *im : {&cur, &prev})
					for (int dy = -R; dy <= R; dy++)
						for (int dx = -R; dx <= R; dx++) {
							const int cx = x + dx, cy = y + dy;
							if (cx < 0 || cy < 0 || cx >= cur.w || cy >= cur.h)
								continue;
							const Rgba &q = im->at(cx, cy);
							const double Y = luma(q), v[4] = {Y, q.r - Y, q.g - Y, q.b - Y};
							for (int i = 0; i < 4; i++) {
								lo[i] = std::min(lo[i], v[i]);
								hi[i] = std::max(hi[i], v[i]);
							}
						}
				const Rgba &q = o.at(x, y);
				const double Y = luma(q), v[4] = {Y, q.r - Y, q.g - Y, q.b - Y};
				for (int i = 0; i < 4; i++)
					if (v[i] < lo[i] - 1e-9 * (std::fabs(lo[i]) + 1) ||
					    v[i] > hi[i] + 1e-9 * (std::fabs(hi[i]) + 1))
						bad++;
			}
		CHECK(bad == 0, "%d values outside the candidate range", bad);
	}

	// 4. HDR stress (brief N4): negative components, SDR white, nominal peak, above peak
	{
		Image im = blank(16, 16);
		const double lv[4] = {203, 1000, 4000, 12000};
		std::mt19937_64 rng(8);
		std::normal_distribution<double> N(0, 1);
		for (int y = 0; y < 16; y++)
			for (int x = 0; x < 16; x++) {
				const double v = lv[(x / 4) % 4];
				im.px[(size_t)y * 16 + x] =
					Rgba{v * (1 + 0.02 * N(rng)) - (y < 4 ? 1.5 * v : 0), v, v * 1.1, 1};
			}
		NlmParams p;
		p.luma = 10;
		p.chroma = 10;
		p.temporal = true;
		p.policy = NlmPolicyB;
		NlmFrame f{&im, &im, &im, 1.0};
		Image o;
		nlm_offset(p, f, o);
		bool finite = true;
		double maxv = 0, minr = 0;
		for (auto &q : o.px) {
			finite = finite && std::isfinite(q.r) && std::isfinite(q.g) && std::isfinite(q.b);
			maxv = std::max(maxv, q.b);
			minr = std::min(minr, q.r);
		}
		CHECK(finite, "non-finite output");
		CHECK(maxv > 12000 && minr < -100, "HDR range was clamped (max %.0f, min r %.0f)", maxv, minr);
	}

	// 5. cut (g = 0): no previous-frame contribution; temporal share cap for B and H
	{
		NlmParams p;
		p.luma = 6;
		p.chroma = 6;
		p.temporal = true;
		const Image cur = scene(25, 19, 21), prev = scene(25, 19, 22);
		Image spatial_only, cut;
		NlmParams ps = p;
		ps.temporal = false;
		NlmFrame f0{&cur, nullptr, nullptr, 1.0};
		nlm_offset(ps, f0, spatial_only);
		for (int policy = 0; policy < 3; policy++) {
			p.policy = policy;
			NlmFrame fc{&cur, &prev, &prev, 0.0};
			nlm_offset(p, fc, cut);
			CHECK(max_rel_diff(cut, spatial_only) == 0, "cut frame used temporal candidates (policy %d)",
			      policy);
		}
		// identical history: temporal share would be large; recursive policies cap it
		std::vector<double> share;
		Image o;
		p.tsearch = 4;
		p.search = 0; // only self + 81 temporal candidates: uncapped share would be ~0.98
		for (int policy = 1; policy < 3; policy++) {
			p.policy = policy;
			NlmFrame ft{&cur, &cur, &cur, 1.0};
			nlm_offset_share(p, ft, o, &share);
			const double mx = *std::max_element(share.begin(), share.end());
			std::printf("policy %d: max temporal share %.4f (cap %.2f)\n", policy, mx, p.cap);
			CHECK(mx <= p.cap + 1e-12, "temporal share above cap");
			CHECK(mx > p.cap - 1e-6, "cap never engaged: test does not exercise it");
		}
	}

	// 6. Spatial mirror symmetry (x and y), to rounding
	{
		NlmParams p;
		p.luma = 7;
		p.chroma = 7;
		p.patch = 2;
		p.search = 3;
		const Image im = scene(27, 21, 31);
		Image fx = im, fy = im;
		for (int y = 0; y < im.h; y++)
			for (int x = 0; x < im.w; x++) {
				fx.px[(size_t)y * im.w + x] = im.at(im.w - 1 - x, y);
				fy.px[(size_t)y * im.w + x] = im.at(x, im.h - 1 - y);
			}
		Image o, ox, oy;
		NlmFrame f{&im, nullptr, nullptr, 1}, f1{&fx, nullptr, nullptr, 1}, f2{&fy, nullptr, nullptr, 1};
		nlm_offset(p, f, o);
		nlm_offset(p, f1, ox);
		nlm_offset(p, f2, oy);
		double dx = 0, dy = 0;
		for (int y = 0; y < im.h; y++)
			for (int x = 0; x < im.w; x++) {
				const double s = std::max(1.0, luma(o.at(x, y)));
				dx = std::max(dx, std::fabs(o.at(x, y).g - ox.at(im.w - 1 - x, y).g) / s);
				dy = std::max(dy, std::fabs(o.at(x, y).g - oy.at(x, im.h - 1 - y).g) / s);
			}
		std::printf("spatial mirror symmetry: max rel diff x %.2e, y %.2e\n", dx, dy);
		CHECK(dx < 1e-9 && dy < 1e-9, "mirror asymmetry");
	}

	// 7. Sanitize
	{
		NlmParams q;
		q.luma = NAN;
		q.patch = 9;
		q.policy = 7;
		q.cap = 3;
		sanitize(q);
		CHECK(q.luma == 0 && q.patch == 4 && q.policy == NlmPolicyA && q.cap == 1, "sanitize");
	}

	std::printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
	return failures ? 1 : 0;
}
