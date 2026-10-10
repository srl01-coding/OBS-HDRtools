// CPU tests and synthetic gates for P2 spatial denoise (docs/HQDN3D_DESIGN.md section 8).
// Build: g++ -std=c++17 -O2 -Isrc tests/cpu/test_spatial.cpp src/denoise/spatial-math.cpp
//        src/denoise/hqdn3d-math.cpp -o test_spatial
#include "denoise/spatial-math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <string>

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

using Filter = std::function<void(const SpatialParams &, const Image &, Image &)>;

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

static Rgba &px(Image &im, int x, int y)
{
	return im.px[(size_t)y * im.w + x];
}

// Per-channel Gaussian noise, sigma = rel * the clean value of the channel.
static void add_noise(Image &im, double rel, uint64_t seed)
{
	std::mt19937_64 rng(seed);
	std::normal_distribution<double> N(0.0, 1.0);
	for (auto &p : im.px) {
		p.r += rel * p.r * N(rng);
		p.g += rel * p.g * N(rng);
		p.b += rel * p.b * N(rng);
	}
}

static Image flat(int w, int h, double nits)
{
	Image im = blank(w, h);
	for (auto &p : im.px)
		p = Rgba{nits, nits, nits, 1};
	return im;
}

// Luma and chroma noise std over the interior (border b excluded).
static void sigmas(const Image &im, int b, double *sl, double *sc)
{
	double sy = 0, syy = 0, s[3] = {0, 0, 0}, ss[3] = {0, 0, 0};
	double n = 0;
	for (int y = b; y < im.h - b; y++)
		for (int x = b; x < im.w - b; x++) {
			const Rgba &p = im.at(x, y);
			const double Y = luma(p);
			const double c[3] = {p.r - Y, p.g - Y, p.b - Y};
			sy += Y;
			syy += Y * Y;
			for (int i = 0; i < 3; i++) {
				s[i] += c[i];
				ss[i] += c[i] * c[i];
			}
			n++;
		}
	*sl = std::sqrt(std::max(0.0, syy / n - (sy / n) * (sy / n)));
	double v = 0;
	for (int i = 0; i < 3; i++)
		v += std::max(0.0, ss[i] / n - (s[i] / n) * (s[i] / n));
	*sc = std::sqrt(v / 3.0);
}

// Smallest strength in [0, 20] with ratio(S) <= target (bisection; ratio falls with S).
static double solve(const std::function<double(double)> &ratio, double target, double *got)
{
	if (ratio(20.0) > target) {
		*got = ratio(20.0);
		return -1;
	}
	double lo = 0, hi = 20;
	for (int i = 0; i < 26; i++) {
		const double mid = 0.5 * (lo + hi);
		if (ratio(mid) <= target)
			hi = mid;
		else
			lo = mid;
	}
	*got = ratio(hi);
	return hi;
}

struct EdgeResult {
	double width_in, width_out, over, under, over_in, under_in;
};

// 10-90 % width of a rising profile around the step (linear interpolation).
static double width1090(const std::vector<double> &pr, double lo, double hi)
{
	const double a = lo + 0.1 * (hi - lo), b = lo + 0.9 * (hi - lo);
	auto cross = [&](double level) {
		for (size_t i = 1; i < pr.size(); i++)
			if (pr[i - 1] < level && pr[i] >= level)
				return (double)(i - 1) + (level - pr[i - 1]) / (pr[i] - pr[i - 1]);
		return -1.0;
	};
	return cross(b) - cross(a);
}

// Noisy step lo -> hi (rising = step up in +axis), filtered; profile averaged along the
// edge. vertical_edge: the step is along x (edge line is vertical).
static EdgeResult edge_test(const Filter &f, const SpatialParams &p, double lo, double hi, bool rising,
			    bool vertical_edge, uint64_t seed)
{
	const int L = 128, M = 384, step = 64; // L across the edge, M along it
	const int w = vertical_edge ? L : M, h = vertical_edge ? M : L;
	Image im = blank(w, h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const int a = vertical_edge ? x : y;
			const bool upper = rising ? a >= step : a < step;
			const double v = upper ? hi : lo;
			px(im, x, y) = Rgba{v, v, v, 1};
		}
	add_noise(im, 0.02, seed);
	Image out;
	f(p, im, out);
	auto profile = [&](const Image &m) {
		std::vector<double> pr((size_t)L, 0.0);
		for (int a = 0; a < L; a++) {
			double s = 0;
			for (int b = 16; b < M - 16; b++)
				s += luma(vertical_edge ? m.at(a, b) : m.at(b, a));
			pr[(size_t)a] = s / (M - 32);
		}
		if (!rising)
			std::reverse(pr.begin(), pr.end()); // measure as a rising step
		return pr;
	};
	auto measure = [&](const std::vector<double> &pr, double *width, double *over, double *under) {
		double l = 0, u = 0;
		for (int a = 8; a < 40; a++)
			l += pr[(size_t)a];
		for (int a = 88; a < 120; a++)
			u += pr[(size_t)a];
		l /= 32;
		u /= 32;
		const int c = rising ? step : L - step;
		double mo = 0, mu = 0;
		for (int a = c - 20; a < c + 20; a++) {
			mo = std::max(mo, pr[(size_t)a] - u);
			mu = std::max(mu, l - pr[(size_t)a]);
		}
		*width = width1090(pr, l, u);
		*over = mo / (u - l);
		*under = mu / (u - l);
	};
	EdgeResult r;
	measure(profile(im), &r.width_in, &r.over_in, &r.under_in);
	measure(profile(out), &r.width_out, &r.over, &r.under);
	return r;
}

static Image flip_x(const Image &im)
{
	Image o = im;
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
			px(o, x, y) = im.at(im.w - 1 - x, y);
	return o;
}

static Image flip_y(const Image &im)
{
	Image o = im;
	for (int y = 0; y < im.h; y++)
		for (int x = 0; x < im.w; x++)
			px(o, x, y) = im.at(x, im.h - 1 - y);
	return o;
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

// A test image with structure: noisy flat areas, hard and soft edges, colour, darks.
static Image scene(int w, int h, uint64_t seed)
{
	Image im = blank(w, h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			double v = x < w / 3 ? 18 : (x < 2 * w / 3 ? 203 : 1000);
			if (y > h / 2)
				v *= 1.2;
			const double t = (double)(x + y) / (w + h);
			px(im, x, y) = Rgba{v * (0.8 + 0.4 * t), v, v * (1.1 - 0.3 * t), 1};
			if ((x / 9 + y / 7) % 5 == 0)
				px(im, x, y) = Rgba{0.02, 0.01, 0.03, 1}; // near black
		}
	add_noise(im, 0.03, seed);
	return im;
}

int main()
{
	const Filter B = [](const SpatialParams &p, const Image &in, Image &out) {
		spatial_b(p, in, out);
	};
	const Filter A = [](const SpatialParams &p, const Image &in, Image &out) {
		spatial_a(p, in, out);
	};
	const Filter Bf = [](const SpatialParams &p, const Image &in, Image &out) {
		spatial_b_f(make_spatial_shader_params(p), in, out);
	};

	// 1. Exact identity at S = 0 (A, B, B float mirror), including negative and huge values.
	{
		Image im = scene(61, 47, 1);
		px(im, 3, 3) = Rgba{-5, 2, 60000, 1};
		px(im, 4, 3) = Rgba{1e-9, -1e-6, 0, 0.5};
		SpatialParams p;
		for (const Filter *f : {&B, &A, &Bf}) {
			Image o;
			(*f)(p, im, o);
			bool ok = true;
			for (size_t i = 0; i < im.px.size(); i++) {
				Rgba e = im.px[i];
				if (f == &Bf) // the mirror stores float values
					e = Rgba{(float)e.r, (float)e.g, (float)e.b, (float)e.a};
				ok = ok && same(o.px[i], e);
			}
			CHECK(ok, "S = 0 is not an exact identity");
		}
	}

	// 2. Black stays black; constant stays constant; neutral stays neutral (exactly).
	{
		SpatialParams p;
		p.luma = 8;
		p.chroma = 8;
		for (const Filter *f : {&B, &A}) {
			Image z = flat(40, 30, 0.0), o;
			(*f)(p, z, o);
			bool black = true;
			for (auto &q : o.px)
				black = black && q.r == 0 && q.g == 0 && q.b == 0;
			CHECK(black, "black moved");
			Image c = blank(40, 30);
			for (auto &q : c.px)
				q = Rgba{150, 40, 7, 1};
			(*f)(p, c, o);
			CHECK(max_rel_diff(c, o) < 1e-12, "constant field changed");
			Image n = flat(40, 30, 100);
			add_noise(n, 0.0, 1);
			std::mt19937_64 rng(3);
			std::uniform_real_distribution<double> U(10, 30);
			for (auto &q : n.px) {
				const double v = U(rng);
				q = Rgba{v, v, v, 1};
			}
			(*f)(p, n, o);
			bool neutral = true;
			for (auto &q : o.px)
				neutral = neutral && q.r == q.g && q.g == q.b;
			CHECK(neutral, "neutral input gained colour");
		}
	}

	// 3. No overshoot: Y and each chroma component of a single pass stay inside the input
	//    range of the window (B: radius R along the axis; A: the whole line).
	{
		SpatialParams p;
		p.luma = 12;
		p.chroma = 12;
		const Image im = scene(97, 31, 7);
		for (int which = 0; which < 2; which++) {
			Image o;
			if (which == 0)
				spatial_b_pass(p, im, o, false);
			else
				spatial_a_pass(p, im, o, false);
			const int r = which == 0 ? p.radius : im.w;
			int bad = 0;
			for (int y = 0; y < im.h; y++)
				for (int x = 0; x < im.w; x++) {
					double lo[4] = {1e300, 1e300, 1e300, 1e300},
					       hi[4] = {-1e300, -1e300, -1e300, -1e300};
					for (int j = std::max(0, x - r); j <= std::min(im.w - 1, x + r); j++) {
						const Rgba &q = im.at(j, y);
						const double Y = luma(q), v[4] = {Y, q.r - Y, q.g - Y, q.b - Y};
						for (int i = 0; i < 4; i++) {
							lo[i] = std::min(lo[i], v[i]);
							hi[i] = std::max(hi[i], v[i]);
						}
					}
					const Rgba &q = o.at(x, y);
					const double Y = luma(q), v[4] = {Y, q.r - Y, q.g - Y, q.b - Y};
					for (int i = 0; i < 4; i++) {
						const double tol =
							1e-9 * std::max(1.0, std::fabs(hi[i]) + std::fabs(lo[i]));
						if (v[i] < lo[i] - tol || v[i] > hi[i] + tol)
							bad++;
					}
				}
			CHECK(bad == 0, "%s: %d values outside the input window range", which ? "A" : "B", bad);
		}
	}

	// 4. Mirror symmetry (no directional bias): f(flip(x)) == flip(f(x)).
	{
		SpatialParams p;
		p.luma = 10;
		p.chroma = 10;
		const Image im = scene(83, 57, 11);
		for (const Filter *f : {&B, &A}) {
			Image o, ox, oy;
			(*f)(p, im, o);
			(*f)(p, flip_x(im), ox);
			(*f)(p, flip_y(im), oy);
			const double dx = max_rel_diff(flip_x(o), ox), dy = max_rel_diff(flip_y(o), oy);
			std::printf("mirror symmetry %s: max rel diff x %.2e, y %.2e\n", f == &B ? "B" : "A", dx, dy);
			CHECK(dx < 1e-9 && dy < 1e-9, "mirror asymmetry");
		}
	}

	// 5. Float mirror of the shader vs the double reference (B).
	{
		SpatialParams p;
		p.luma = 6;
		p.chroma = 8;
		for (int r : {6, 8, 12}) {
			p.radius = r;
			const Image im = scene(73, 41, 19);
			Image od, of;
			spatial_b(p, im, od);
			spatial_b_f(make_spatial_shader_params(p), im, of);
			const double e = max_rel_diff(od, of);
			std::printf("B float mirror vs double, R=%d: max rel error %.2e\n", r, e);
			CHECK(e < 1e-4, "float mirror diverges");
		}
	}

	// 6. Synthetic gates at matched noise reduction (decision section 14).
	struct Cand {
		const char *name;
		Filter f;
		int radius;
	};
	const Cand cands[] = {{"B R=6", B, 6}, {"B R=8", B, 8}, {"B R=12", B, 12}, {"A", A, 0}};
	std::printf("\nmatched-noise gates: per-channel noise 2%%; gated target sigma_out/sigma_in <= 0.70, "
		    "0.50 informative\n");
	std::printf("edge: worst of rising/falling x vertical/horizontal; dW = 10-90%% width increase (px); "
		    "spread = max-min output width over the 4 orientations\n");
	std::printf("low-contrast steps (informative): dW at +2%%, +5%%, +10%%\n\n");
	std::printf("%-7s %4s %5s | %5s %5s | %5s %5s | %-9s %5s %5s %5s %6s | %5s %5s %5s\n", "filter", "nits",
		    "target", "S_L", "S_C", "luma", "chrom", "edge", "dW", "over%", "undr%", "spread", "+2%", "+5%",
		    "+10%");
	for (const Cand &cd : cands) {
		for (double level : {18.0, 203.0}) {
			for (double target : {0.70, 0.50}) {
				const int N = 128, border = 16;
				Image noisy = flat(N, N, level);
				add_noise(noisy, 0.02, 1000 + (uint64_t)level);
				double sl_in, sc_in;
				sigmas(noisy, border, &sl_in, &sc_in);
				SpatialParams p;
				p.radius = cd.radius ? cd.radius : 8;
				auto ratios = [&](const SpatialParams &q, double *rl, double *rc) {
					Image o;
					cd.f(q, noisy, o);
					double a, b;
					sigmas(o, border, &a, &b);
					*rl = a / sl_in;
					*rc = b / sc_in;
				};
				double rl, rc;
				const double SL = solve(
					[&](double S) {
						SpatialParams q = p;
						q.luma = S;
						double a, b;
						ratios(q, &a, &b);
						return a;
					},
					target, &rl);
				p.luma = std::max(SL, 0.0);
				const double SC = solve(
					[&](double S) {
						SpatialParams q = p;
						q.chroma = S;
						double a, b;
						ratios(q, &a, &b);
						return b;
					},
					target, &rc);
				p.chroma = std::max(SC, 0.0);
				ratios(p, &rl, &rc);
				const bool gated = target > 0.6;

				// gated edges from this level
				const double hi = level == 18.0 ? 203.0 : 1000.0;
				double worst_dw = -1e9, worst_over = 0, worst_under = 0, dmin = 1e9, dmax = -1e9;
				for (int rising = 0; rising < 2; rising++)
					for (int vert = 0; vert < 2; vert++) {
						const EdgeResult e = edge_test(cd.f, p, level, hi, rising, vert,
									       77 + rising + 2 * vert);
						worst_dw = std::max(worst_dw, e.width_out - e.width_in);
						dmin = std::min(dmin, e.width_out);
						dmax = std::max(dmax, e.width_out);
						worst_over = std::max(worst_over, e.over);
						worst_under = std::max(worst_under, e.under);
					}
				// informative low-contrast steps (worst over the four orientations)
				double lc_dw[3];
				const double mult[3] = {1.02, 1.05, 1.10};
				for (int m = 0; m < 3; m++) {
					lc_dw[m] = -1e9;
					for (int rising = 0; rising < 2; rising++)
						for (int vert = 0; vert < 2; vert++) {
							const EdgeResult e = edge_test(cd.f, p, level, level * mult[m],
										       rising, vert,
										       91 + rising + 2 * vert);
							lc_dw[m] = std::max(lc_dw[m], e.width_out - e.width_in);
						}
				}
				char edge[32];
				std::snprintf(edge, sizeof(edge), "%.0f>%.0f", level, hi);
				std::printf(
					"%-7s %4.0f %5.2f%s | %5.2f %5.2f | %5.3f %5.3f | %-9s %5.2f %5.2f %5.2f %6.2f | %5.2f "
					"%5.2f %5.2f\n",
					cd.name, level, target, gated ? "*" : " ", SL, SC, rl, rc, edge, worst_dw,
					100 * worst_over, 100 * worst_under, dmax - dmin, lc_dw[0], lc_dw[1], lc_dw[2]);
				CHECK(SL > 0 && SC > 0, "%s at %.0f nits: target %.2f not reachable", cd.name, level,
				      target);
				if (!gated)
					continue;
				CHECK(rl <= 0.70 + 1e-9 && rc <= 0.70 + 1e-9, "%s: matched point not reached", cd.name);
				CHECK(worst_dw <= 1.5, "%s %.0f nits: edge widened %.2f px", cd.name, level, worst_dw);
				CHECK(worst_over <= 0.02 && worst_under <= 0.02, "%s %.0f nits: overshoot", cd.name,
				      level);
				CHECK(dmax - dmin <= 0.1, "%s %.0f nits: directional edge-width spread %.2f px",
				      cd.name, level, dmax - dmin);
			}
		}
	}
	std::printf("(* = gated)\n\n");

	// 7. Sanitize.
	{
		SpatialParams q;
		q.luma = NAN;
		q.chroma = 400;
		q.radius = 40;
		sanitize(q);
		CHECK(q.luma == 0 && q.chroma == kStrengthMax && q.radius == kSpatialRadiusMax, "sanitize");
	}

	std::printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
	return failures ? 1 : 0;
}
