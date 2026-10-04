// CPU tests for the HQDN3D-style temporal denoise (docs/HQDN3D_DESIGN.md section 6).
// Build: g++ -std=c++17 -O2 -Isrc tests/cpu/test_hqdn3d.cpp src/denoise/hqdn3d-math.cpp -o test_hqdn3d
#include "denoise/hqdn3d-math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

static Image make_image(int w, int h, std::mt19937_64 &rng, double scale)
{
	std::uniform_real_distribution<double> U(0.2, 1.0);
	Image im;
	im.w = w;
	im.h = h;
	im.px.resize((size_t)w * h);
	for (auto &p : im.px) {
		const double y = scale * U(rng);
		p.r = y * U(rng);
		p.g = y * U(rng);
		p.b = y * U(rng);
	}
	return im;
}

int main()
{
	std::mt19937_64 rng(5);
	std::uniform_real_distribution<double> U(0, 1);

	// 1. Strength 0 is an exact identity, whatever the history.
	{
		Params p;
		for (int i = 0; i < 2000; i++) {
			const Rgba c{U(rng) * 2000 - 5, U(rng) * 2000, U(rng) * 2000, 1};
			const Rgba h{U(rng) * 2000, U(rng) * 2000 - 5, U(rng) * 2000, 1};
			CHECK(same(temporal_pixel(p, c, h, 1.0), c), "S=0 not identity");
			const ShaderParams s = make_shader_params(p);
			const float cf[4] = {(float)c.r, (float)c.g, (float)c.b, 1},
				    hf[4] = {(float)h.r, (float)h.g, (float)h.b, 1};
			float o[4];
			temporal_pixel_f(s, cf, hf, 1.0f, o);
			CHECK(o[0] == cf[0] && o[1] == cf[1] && o[2] == cf[2] && o[3] == cf[3],
			      "S=0 mirror not identity");
		}
	}

	Params p;
	p.temporal_luma = 6;
	p.temporal_chroma = 6;

	// 2. Black stays black; constant stays constant; neutral stays neutral; g = 0 is identity.
	{
		const Rgba z{0, 0, 0, 1};
		const Rgba o = temporal_pixel(p, z, z, 1.0);
		CHECK(o.r == 0 && o.g == 0 && o.b == 0, "black moved");
		for (int i = 0; i < 1000; i++) {
			const Rgba c{U(rng) * 1000, U(rng) * 1000, U(rng) * 1000, 1};
			const Rgba r = temporal_pixel(p, c, c, 1.0);
			const double m = std::max({std::fabs(c.r), std::fabs(c.g), std::fabs(c.b), 1e-9});
			CHECK(std::fabs(r.r - c.r) / m < 1e-12 && std::fabs(r.g - c.g) / m < 1e-12 &&
				      std::fabs(r.b - c.b) / m < 1e-12,
			      "constant not constant");
			const double y1 = U(rng) * 500, y2 = y1 * (1 + 0.02 * (U(rng) - 0.5));
			const Rgba n = temporal_pixel(p, Rgba{y1, y1, y1, 1}, Rgba{y2, y2, y2, 1}, 1.0);
			CHECK(std::fabs(n.r - n.g) < 1e-9 * y1 && std::fabs(n.g - n.b) < 1e-9 * y1,
			      "neutral not neutral");
			const Rgba h{U(rng) * 1000, U(rng) * 1000, U(rng) * 1000, 1};
			CHECK(same(temporal_pixel(p, c, h, 0.0), c), "g=0 not identity");
		}
	}

	// 3. No overshoot: luma and each chroma component lie between current and history.
	//    Finite for negative components and > 10,000 nits.
	{
		for (int i = 0; i < 20000; i++) {
			const double y = std::pow(10.0, U(rng) * 6 - 2);
			const Rgba c{y * (U(rng) * 1.4 - 0.2), y * U(rng), y * U(rng), 1};
			const double f = 1 + 0.05 * (U(rng) - 0.5);
			const Rgba h{c.r * f + 0.01 * y * (U(rng) - 0.5), c.g * f, c.b * f, 1};
			const Rgba o = temporal_pixel(p, c, h, U(rng));
			CHECK(std::isfinite(o.r) && std::isfinite(o.g) && std::isfinite(o.b), "not finite");
			const double yc = luma(c), yh = luma(h), yo = luma(o);
			const double tol = 1e-9 * (std::fabs(yc) + std::fabs(yh) + 1);
			CHECK(yo >= std::min(yc, yh) - tol && yo <= std::max(yc, yh) + tol, "luma overshoot");
			const double cc[3] = {c.r - yc, c.g - yc, c.b - yc}, ch[3] = {h.r - yh, h.g - yh, h.b - yh},
				     co[3] = {o.r - yo, o.g - yo, o.b - yo};
			for (int k = 0; k < 3; k++)
				CHECK(co[k] >= std::min(cc[k], ch[k]) - tol && co[k] <= std::max(cc[k], ch[k]) + tol,
				      "chroma overshoot");
		}
		const Rgba big{60000, 50000, 70000, 1}, neg{-3, 5, 2, 1};
		const Rgba o1 = temporal_pixel(p, big, Rgba{59000, 50500, 69000, 1}, 1.0);
		const Rgba o2 = temporal_pixel(p, neg, Rgba{-2.9, 5.1, 2, 1}, 1.0);
		CHECK(std::isfinite(o1.r + o1.g + o1.b + o2.r + o2.g + o2.b), "extremes not finite");
	}

	// 4. Noise reduction on a static noisy field vs the first-order IIR prediction.
	{
		std::normal_distribution<double> N(0, 1);
		const double y0 = 18.0, sigma = 0.004; // relative noise ~0.006 log2 units, well below T = 0.06
		double in_var = 0, out_var = 0;
		int n = 0;
		Rgba hist{y0, y0, y0, 1};
		for (int f = 0; f < 20000; f++) {
			const double v = y0 * (1 + sigma * N(rng));
			const Rgba c{v, v, v, 1};
			const Rgba o = temporal_pixel(p, c, hist, 1.0);
			hist = o;
			if (f > 200) {
				in_var += (v - y0) * (v - y0);
				out_var += (o.r - y0) * (o.r - y0);
				n++;
			}
		}
		const double ratio = std::sqrt(out_var / in_var);
		const double ideal = std::sqrt((1 - kBeta) / (1 + kBeta));
		std::printf("static noise: std out/in = %.3f (IIR limit for beta %.1f: %.3f)\n", ratio, kBeta, ideal);
		CHECK(ratio > ideal * 0.95 && ratio < ideal * 1.6, "noise reduction off model");
	}

	// 5. A real change (motion edge, 3 stops) passes through untouched.
	{
		const Rgba c{100, 100, 100, 1}, h{12, 12, 12, 1};
		CHECK(same(temporal_pixel(p, c, h, 1.0), c), "moving edge dragged");
	}

	// 6. Frame metric: float mirror vs double on odd sizes, and a uniform-difference fixture.
	{
		for (auto wh : {std::pair<int, int>{100, 37}, {1000, 563}, {17, 1}, {3840, 270}}) {
			Image a = make_image(wh.first, wh.second, rng, 300),
			      b = make_image(wh.first, wh.second, rng, 300);
			const double md = frame_metric(a, b, 0.1);
			const double mf = frame_metric_f(a, b, 0.1f);
			CHECK(std::fabs(md - mf) / std::max(md, 1e-9) < 1e-5, "metric mirror %dx%d: %g vs %g", wh.first,
			      wh.second, md, mf);
		}
		Image a = make_image(333, 211, rng, 200), b = a;
		for (auto &px : b.px) {
			px.r *= 2;
			px.g *= 2;
			px.b *= 2;
		}
		// k << luminance: one stop everywhere -> m close to 1
		const double m = frame_metric(a, b, 0.0001);
		CHECK(std::fabs(m - 1.0) < 1e-3, "one-stop metric %g", m);
		CHECK(frame_metric(a, a, 0.1) == 0.0, "identical frames metric not 0");
	}

	// 7. Global factor, floor and float mirror.
	{
		Params q = p;
		const ShaderParams s = make_shader_params(q);
		for (int i = 0; i < 5000; i++) {
			const double m = U(rng) * 0.6, f = U(rng) * 0.1;
			const double gd = global_factor(q, m, f, false);
			const float gf = global_factor_f(s, (float)m, (float)f, false);
			CHECK(std::fabs(gd - gf) < 1e-5, "global factor mirror");
			CHECK(gd >= 0 && gd <= 1, "g out of range");
		}
		bool cut = false;
		CHECK(global_factor(q, 0.31, 0.01, false, &cut) == 0 && cut, "cut at m >= m_cut (s=50: 0.3)");
		CHECK(global_factor(q, 0.29, 0.29, false, &cut) == 1.0 && !cut, "no protection at the floor");
		CHECK(global_factor(q, 0.1, 0.1, true) == 0, "reset not identity");
		CHECK(std::fabs(cut_threshold(50) - 0.3) < 1e-12 && std::fabs(cut_threshold(100) - 0.075) < 1e-12,
		      "cut thresholds");
		CHECK(floor_update(0.05, 0.0, false) == 0.05 && floor_update(0.01, 0.05, true) == 0.01 &&
			      std::fabs(floor_update(0.5, 0.05, true) - (0.05 * kFloorRise + kFloorAdd)) < 1e-15,
		      "floor update");
		q.cut_reset = false;
		CHECK(global_factor(q, 5.0, 0.0, false, &cut) < 1 && !cut, "cut reset off");
	}

	// 8. Float temporal mirror vs double, random strengths, pixels and g.
	{
		double worst = 0;
		for (int i = 0; i < 50000; i++) {
			Params q;
			q.temporal_luma = U(rng) * 20;
			q.temporal_chroma = U(rng) * 20;
			q.k_nits = std::pow(10.0, U(rng) * 2 - 2);
			const double y = std::pow(10.0, U(rng) * 6 - 2);
			const Rgba c{y * U(rng), y * U(rng), y * U(rng), 1};
			const double f = 1 + 0.1 * (U(rng) - 0.5);
			const Rgba h{c.r * f * (1 + 0.02 * (U(rng) - 0.5)), c.g * f, c.b * f, 1};
			const double g = U(rng);
			const Rgba r = temporal_pixel(q, c, h, g);
			const ShaderParams s = make_shader_params(q);
			const float cf[4] = {(float)c.r, (float)c.g, (float)c.b, 1},
				    hf[4] = {(float)h.r, (float)h.g, (float)h.b, 1};
			float o[4];
			temporal_pixel_f(s, cf, hf, (float)g, o);
			const double scale = std::max({std::fabs(r.r), std::fabs(r.g), std::fabs(r.b), 1e-6});
			worst = std::max({worst, std::fabs(o[0] - r.r) / scale, std::fabs(o[1] - r.g) / scale,
					  std::fabs(o[2] - r.b) / scale});
		}
		std::printf("float temporal mirror vs double: max error %.3g of the pixel's largest channel\n", worst);
		CHECK(worst < 2e-5, "temporal mirror error");
	}

	// 9. Sequences on small images: cut and dissolve.
	{
		const int W = 96, H = 54;
		std::normal_distribution<double> N(0, 1);
		Image A = make_image(W, H, rng, 150), B = make_image(W, H, rng, 400);
		auto noisy = [&](const Image &im) {
			Image o = im;
			for (auto &px : o.px) {
				const double f = 1 + 0.004 * N(rng);
				px.r *= f;
				px.g *= f;
				px.b *= f;
			}
			return o;
		};
		auto run = [&](const Params &q, const std::vector<Image> &frames, int *cuts, double *final_err) {
			Image hist = frames[0];
			double floor = 0;
			bool floor_valid = false;
			*cuts = 0;
			*final_err = 0;
			for (size_t f = 1; f < frames.size(); f++) {
				const Image &cur = frames[f];
				const double m = frame_metric(cur, hist, q.k_nits);
				floor = floor_update(m, floor, floor_valid);
				floor_valid = true;
				bool cut = false;
				const double g = global_factor(q, m, floor, false, &cut);
				*cuts += cut;
				Image out = cur;
				for (size_t i = 0; i < cur.px.size(); i++)
					out.px[i] = temporal_pixel(q, cur.px[i], hist.px[i], g);
				hist = out;
				// mean relative luma lag behind the input over the whole sequence
				double e = 0;
				for (size_t i = 0; i < out.px.size(); i++)
					e += std::fabs(luma(out.px[i]) - luma(cur.px[i])) / luma(cur.px[i]);
				*final_err += e / out.px.size() / (double)(frames.size() - 1);
				if (getenv("HQDN3D_TRACE"))
					std::printf("  frame %2zu m %.4f floor %.4f g %.3f\n", f, m, floor, g);
			}
		};
		// cut: 30 frames of A, then B. The first B frame must be B exactly.
		{
			std::vector<Image> seq;
			for (int i = 0; i < 30; i++)
				seq.push_back(noisy(A));
			seq.push_back(noisy(B));
			Image hist = seq[0];
			double floor = 0;
			bool fv = false;
			int cuts = 0;
			bool b_exact = false;
			for (size_t f = 1; f < seq.size(); f++) {
				const double m = frame_metric(seq[f], hist, p.k_nits);
				floor = floor_update(m, floor, fv);
				fv = true;
				bool cut = false;
				const double g = global_factor(p, m, floor, false, &cut);
				cuts += cut;
				Image out = seq[f];
				for (size_t i = 0; i < out.px.size(); i++)
					out.px[i] = temporal_pixel(p, seq[f].px[i], hist.px[i], g);
				if (f == seq.size() - 1) {
					b_exact = true;
					for (size_t i = 0; i < out.px.size(); i++)
						b_exact = b_exact && same(out.px[i], seq[f].px[i]);
					std::printf("cut frame metric %.3f (threshold %.3f)\n", m, cut_threshold(50));
				}
				hist = out;
			}
			CHECK(cuts == 1 && b_exact, "cut: %d resets, first B frame exact %d", cuts, (int)b_exact);
		}
		// dissolve A -> C over 30 frames (linear blend), then one settled frame of C. C is A
		// 0.6 stop brighter with mild texture change, so each dissolve step (~0.02 log2) is
		// well below the threshold: without protection the filter lags behind the dissolve.
		{
			Image C = A;
			for (auto &px : C.px) {
				const double f = 1.5 * (1 + 0.05 * (U(rng) - 0.5));
				px.r *= f;
				px.g *= f;
				px.b *= f;
			}
			const Image Bsave = B;
			B = C;
			std::vector<Image> seq;
			for (int i = 0; i < 10; i++)
				seq.push_back(noisy(A));
			for (int i = 1; i <= 30; i++) {
				Image mix = A;
				const double t = i / 30.0;
				for (size_t k = 0; k < mix.px.size(); k++) {
					mix.px[k].r = (1 - t) * A.px[k].r + t * B.px[k].r;
					mix.px[k].g = (1 - t) * A.px[k].g + t * B.px[k].g;
					mix.px[k].b = (1 - t) * A.px[k].b + t * B.px[k].b;
				}
				seq.push_back(noisy(mix));
			}
			seq.push_back(noisy(B));
			Params on = p, off = p;
			off.protection = false;
			int cuts_on = 0, cuts_off = 0;
			double err_on = 0, err_off = 0;
			run(on, seq, &cuts_on, &err_on);
			run(off, seq, &cuts_off, &err_off);
			std::printf("dissolve: mean luma lag %.4f with protection, %.4f without; resets %d\n", err_on,
				    err_off, cuts_on);
			CHECK(cuts_on == 0, "dissolve triggered a cut reset");
			CHECK(err_on < 0.5 * err_off, "protection did not clearly reduce dissolve lag");
			B = Bsave;
		}
	}

	// 10. Sanitize.
	{
		Params q;
		q.temporal_luma = NAN;
		q.cut_sensitivity = 500;
		q.k_nits = -1;
		sanitize(q);
		CHECK(q.temporal_luma == 0 && q.cut_sensitivity == 100 && q.k_nits == 0.001, "sanitize");
	}

	std::printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
	return failures ? 1 : 0;
}
