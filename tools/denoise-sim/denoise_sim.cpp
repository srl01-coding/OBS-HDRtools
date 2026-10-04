// Synthetic premium-temporal comparison (docs/denoise/MOTION_AWARE_TEMPORAL.md).
//
// CPU only. Compares, at matched static noise reduction:
//   HQDN3D temporal (P1), HQDN3D spatial B + temporal, spatial NLMeans,
//   Temporal NLMeans (policies A / B / H), and motion-compensated temporal with
//   *oracle* flow (exact, and quantised to a 4x4 grid with/without flow error) as a
//   stand-in for NVOFA. The oracle variants bound what hardware flow could achieve;
//   they are not an optical-flow implementation.
//
// Build: g++ -std=c++17 -O2 -Isrc tools/denoise-sim/denoise_sim.cpp src/denoise/*.cpp
//        -o denoise_sim       (d3d11-compute.cpp / program-denoise.cpp excluded)
// Run:   denoise_sim [--noise enc|white2] [--quick]
//        denoise_sim --process METHOD S in.raw out.raw W H N   (float32 RGB nits)
#include "denoise/hqdn3d-math.hpp"
#include "denoise/nlm-math.hpp"
#include "denoise/spatial-math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace hdrtk::denoise;

namespace {

constexpr double kK = 0.1;

double luma(const Rgba &c)
{
	return kLumaR * c.r + kLumaG * c.g + kLumaB * c.b;
}

Image blank(int w, int h)
{
	Image im;
	im.w = w;
	im.h = h;
	im.px.assign((size_t)w * h, Rgba{});
	return im;
}

Rgba &px(Image &im, int x, int y)
{
	return im.px[(size_t)y * im.w + x];
}

// ---- procedural clean content ------------------------------------------------------

uint32_t hash2(int x, int y, uint32_t s)
{
	uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + s * 2246822519u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

double vnoise(double x, double y, uint32_t s)
{
	const int ix = (int)std::floor(x), iy = (int)std::floor(y);
	const double fx = x - ix, fy = y - iy;
	auto v = [&](int a, int b) {
		return (hash2(ix + a, iy + b, s) & 0xffffff) / 16777216.0;
	};
	const double ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
	const double a = v(0, 0) + ux * (v(1, 0) - v(0, 0));
	const double b = v(0, 1) + ux * (v(1, 1) - v(0, 1));
	return a + uy * (b - a) - 0.5;
}

enum Zone { ZoneWall = 0, ZoneFabric = 1, ZoneText = 2, ZoneSkin = 3, ZoneDark = 4, ZoneObject = 5, ZoneTrail = 6 };

// world coordinates -> zone (48-px columns; an 8-row dark band every 64 rows)
int zone_of(int xw, int yw)
{
	return (yw % 64 > 56) ? ZoneDark : (xw / 48) % 4;
}

// The background world: wall with near-noise-level texture, fabric, text strokes,
// dark band and highlights. Values in nits.
Image make_world(int w, int h, uint32_t seed)
{
	Image im = blank(w, h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			const int zone = zone_of(x, y);
			double v;
			double cr = 1.0, cb = 1.0;
			if (zone == 0) { // wall: 140 nits, texture ~2%
				v = 140 * (1 + 0.04 * vnoise(x / 3.0, y / 3.0, seed) +
					   0.02 * vnoise(x / 1.3, y / 1.3, seed + 1));
				cr = 1.08;
				cb = 0.9;
			} else if (zone == 1) { // fabric: 60 nits, texture 10% + weave
				v = 60 *
				    (1 + 0.2 * vnoise(x / 2.0, y / 2.0, seed + 2) + 0.06 * std::sin(x * 1.9 + y * 0.4));
				cr = 0.8;
				cb = 1.15;
			} else if (zone == 2) { // graphics/text: 250 nits with 1-2 px dark strokes
				v = 250;
				const bool stroke = ((x % 11) == 3 && (y % 23) < 15) ||
						    ((y % 23) == 4 && (x % 37) < 20) ||
						    (((x + y) % 29) == 0 && (y % 41) < 25);
				if (stroke)
					v = 20;
			} else { // skin-like: 110 nits, low-contrast texture 3% + soft features
				v = 110 * (1 + 0.06 * vnoise(x / 2.5, y / 2.5, seed + 3) +
					   0.08 * std::sin(x * 0.21) * std::sin(y * 0.17));
				cr = 1.25;
				cb = 0.85;
			}
			if (zone == ZoneDark) // dark band (5 nits)
				v = 5 * (1 + 0.2 * vnoise(x / 2.0, y / 2.0, seed + 4));
			if ((hash2(x / 16, y / 16, seed + 5) & 63) == 0 && x % 16 > 5 && x % 16 < 9 && y % 16 > 5 &&
			    y % 16 < 9)
				v = 700; // small highlights
			const double g = v;
			px(im, x, y) = Rgba{g * cr, g, g * cb, 1};
		}
	return im;
}

// Foreground object texture (a different surface: warm, mid-contrast, a few edges).
Image make_object(int s, uint32_t seed)
{
	Image im = blank(s, s);
	for (int y = 0; y < s; y++)
		for (int x = 0; x < s; x++) {
			double v = 90 * (1 + 0.1 * vnoise(x / 2.0, y / 2.0, seed) +
					 0.05 * vnoise(x / 1.2, y / 1.2, seed + 9));
			if ((y > s / 3 && y < s / 3 + 3) || (x > s / 2 && x < s / 2 + 2))
				v = 25; // dark lines
			px(im, x, y) = Rgba{v * 1.3, v, v * 0.75, 1};
		}
	return im;
}

double cubic(double t, double a, double b, double c, double d)
{ // Catmull-Rom
	return b + 0.5 * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
}

Rgba sample_cubic(const Image &im, double x, double y)
{
	const int ix = (int)std::floor(x), iy = (int)std::floor(y);
	const double fx = x - ix, fy = y - iy;
	Rgba row[4];
	for (int j = 0; j < 4; j++) {
		const int yy = std::clamp(iy - 1 + j, 0, im.h - 1);
		const Rgba *p[4];
		for (int i = 0; i < 4; i++)
			p[i] = &im.at(std::clamp(ix - 1 + i, 0, im.w - 1), yy);
		row[j].r = cubic(fx, p[0]->r, p[1]->r, p[2]->r, p[3]->r);
		row[j].g = cubic(fx, p[0]->g, p[1]->g, p[2]->g, p[3]->g);
		row[j].b = cubic(fx, p[0]->b, p[1]->b, p[2]->b, p[3]->b);
	}
	Rgba o;
	o.r = cubic(fy, row[0].r, row[1].r, row[2].r, row[3].r);
	o.g = cubic(fy, row[0].g, row[1].g, row[2].g, row[3].g);
	o.b = cubic(fy, row[0].b, row[1].b, row[2].b, row[3].b);
	o.a = 1;
	return o;
}

Rgba sample_bilinear(const Image &im, double x, double y)
{
	const int ix = (int)std::floor(x), iy = (int)std::floor(y);
	const double fx = x - ix, fy = y - iy;
	auto at = [&](int a, int b) -> const Rgba & {
		return im.at(std::clamp(ix + a, 0, im.w - 1), std::clamp(iy + b, 0, im.h - 1));
	};
	const Rgba &p00 = at(0, 0), &p10 = at(1, 0), &p01 = at(0, 1), &p11 = at(1, 1);
	Rgba o;
	o.r = (1 - fy) * ((1 - fx) * p00.r + fx * p10.r) + fy * ((1 - fx) * p01.r + fx * p11.r);
	o.g = (1 - fy) * ((1 - fx) * p00.g + fx * p10.g) + fy * ((1 - fx) * p01.g + fx * p11.g);
	o.b = (1 - fy) * ((1 - fx) * p00.b + fx * p10.b) + fy * ((1 - fx) * p01.b + fx * p11.b);
	o.a = (1 - fy) * ((1 - fx) * p00.a + fx * p10.a) + fy * ((1 - fx) * p01.a + fx * p11.a);
	return o;
}

double lanczos3(double x)
{
	x = std::fabs(x);
	if (x < 1e-9)
		return 1.0;
	if (x >= 3.0)
		return 0.0;
	const double px_ = M_PI * x;
	return 3.0 * std::sin(px_) * std::sin(px_ / 3.0) / (px_ * px_);
}

Rgba sample_lanczos3(const Image &im, double x, double y)
{
	const int ix = (int)std::floor(x), iy = (int)std::floor(y);
	double wx[6], wy[6], sx = 0, sy = 0;
	for (int i = 0; i < 6; i++) {
		wx[i] = lanczos3(x - (ix - 2 + i));
		wy[i] = lanczos3(y - (iy - 2 + i));
		sx += wx[i];
		sy += wy[i];
	}
	Rgba o{0, 0, 0, 0};
	for (int j = 0; j < 6; j++) {
		const int yy = std::clamp(iy - 2 + j, 0, im.h - 1);
		for (int i = 0; i < 6; i++) {
			const Rgba &p = im.at(std::clamp(ix - 2 + i, 0, im.w - 1), yy);
			const double w = wx[i] * wy[j] / (sx * sy);
			o.r += w * p.r;
			o.g += w * p.g;
			o.b += w * p.b;
			o.a += w * p.a;
		}
	}
	return o;
}

// ---- fixtures ----------------------------------------------------------------------

enum FixtureKind { FixStatic, FixPan, FixObject, FixCut };

struct Frame {
	Image clean;
	std::vector<float> fx, fy;          // true flow: current pixel x came from x + f in the previous frame
	std::vector<uint8_t> object, trail; // masks (object fixture)
	std::vector<uint8_t> zone;          // Zone per pixel (object / trail override)
};

struct Fixture {
	FixtureKind kind;
	double speed; // px/frame
	int w, h, frames;
	Image world, world2, obj;
	int obj_size = 64;
	// object enters from the left and wraps, so every speed has an object in view
	double obj_x(int t) const { return std::fmod(60 + speed * t, (double)(w + obj_size)) - obj_size / 2.0; }
	Frame frame(int t) const
	{
		Frame f;
		f.clean = blank(w, h);
		f.fx.assign((size_t)w * h, 0.f);
		f.fy.assign((size_t)w * h, 0.f);
		f.object.assign((size_t)w * h, 0);
		f.trail.assign((size_t)w * h, 0);
		f.zone.assign((size_t)w * h, 0);
		const double ox = kind == FixPan ? 40 + speed * t : 40;
		const Image &wd = (kind == FixCut && t >= frames / 2) ? world2 : world;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				px(f.clean, x, y) = (kind == FixPan && std::fabs(speed - std::round(speed)) > 1e-9)
							    ? sample_cubic(wd, x + ox, y + 30)
							    : wd.at(x + (int)std::lround(ox), y + 30);
				if (kind == FixPan)
					f.fx[(size_t)y * w + x] = (float)speed;
				f.zone[(size_t)y * w + x] = (uint8_t)zone_of(x + (int)std::lround(ox), y + 30);
			}
		if (kind == FixObject) {
			const double x0 = obj_x(t);
			const int oy = (h - obj_size) / 2;
			for (int y = oy; y < oy + obj_size; y++)
				for (int x = 0; x < w; x++) {
					const double u = x - x0;
					if (u < 0 || u >= obj_size)
						continue;
					px(f.clean, x, y) = std::fabs(speed - std::round(speed)) > 1e-9
								    ? sample_cubic(obj, u, y - oy)
								    : obj.at((int)std::lround(u), y - oy);
					f.object[(size_t)y * w + x] = 1;
					f.zone[(size_t)y * w + x] = ZoneObject;
					f.fx[(size_t)y * w + x] = (float)-speed;
				}
			// trail: background pixels covered by the object in any of the last 6 frames
			for (int k = 1; k <= 6 && t - k >= 0; k++) {
				const double xp = obj_x(t - k);
				for (int y = oy; y < oy + obj_size; y++)
					for (int x = std::max(0, (int)std::floor(xp));
					     x < std::min(w, (int)std::ceil(xp + obj_size)); x++)
						if (!f.object[(size_t)y * w + x]) {
							f.trail[(size_t)y * w + x] = 1;
							f.zone[(size_t)y * w + x] = ZoneTrail;
						}
			}
		}
		return f;
	}
};

// ---- noise -------------------------------------------------------------------------

// Measured (NOISE_MODEL.md) luma noise relative to level; log-linear interpolation.
double rel_sigma(double Y)
{
	static const double lv[][2] = {{5, 0.04},     {23, 0.023},   {114, 0.0089},
				       {280, 0.0072}, {378, 0.0051}, {1000, 0.004}};
	const int n = 6;
	if (Y <= lv[0][0])
		return lv[0][1];
	for (int i = 1; i < n; i++)
		if (Y <= lv[i][0]) {
			const double t = std::log(Y / lv[i - 1][0]) / std::log(lv[i][0] / lv[i - 1][0]);
			return lv[i - 1][1] + t * (lv[i][1] - lv[i - 1][1]);
		}
	return lv[n - 1][1];
}

// Fitted to the measured autocorrelation (lags 1-8): horizontal and vertical half-kernels.
const double kKh[] = {1.0,    0.7877, 0.3021, 0.1354, 0.2693, 0.0516, 0.2231,
		      0.1243, 0.1318, 0.0914, 0.0117, 0.0104, 0.1297};
const double kKv[] = {1.0, 0.3474, 0.0659, 0.094, 0.0489, 0.0348};

enum NoiseModel { NoiseEnc, NoiseWhite2 };

std::vector<double> noise_plane(int w, int h, NoiseModel m, std::mt19937_64 &rng)
{
	std::normal_distribution<double> N(0, 1);
	const int pad = 16;
	const int W = w + 2 * pad, H = h + 2 * pad;
	std::vector<double> a((size_t)W * H);
	for (auto &v : a)
		v = N(rng);
	if (m == NoiseWhite2) {
		std::vector<double> o((size_t)w * h);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				o[(size_t)y * w + x] = a[(size_t)(y + pad) * W + x + pad];
		return o;
	}
	const int nh = (int)(sizeof(kKh) / sizeof(kKh[0])), nv = (int)(sizeof(kKv) / sizeof(kKv[0]));
	double sh = 0, sv = 0;
	for (int i = -(nh - 1); i < nh; i++)
		sh += kKh[std::abs(i)] * kKh[std::abs(i)];
	for (int i = -(nv - 1); i < nv; i++)
		sv += kKv[std::abs(i)] * kKv[std::abs(i)];
	std::vector<double> t((size_t)W * H, 0.0), o((size_t)w * h);
	for (int y = 0; y < H; y++)
		for (int x = nh; x < W - nh; x++) {
			double s = 0;
			for (int i = -(nh - 1); i < nh; i++)
				s += kKh[std::abs(i)] * a[(size_t)y * W + x + i];
			t[(size_t)y * W + x] = s / std::sqrt(sh);
		}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			double s = 0;
			for (int j = -(nv - 1); j < nv; j++)
				s += kKv[std::abs(j)] * t[(size_t)(y + pad + j) * W + x + pad];
			o[(size_t)y * w + x] = s / std::sqrt(sv);
		}
	return o;
}

Image add_noise(const Image &clean, NoiseModel m, uint64_t seed)
{
	std::mt19937_64 rng(seed);
	const auto n0 = noise_plane(clean.w, clean.h, m, rng);
	const auto n1 = noise_plane(clean.w, clean.h, m, rng);
	const auto n2 = noise_plane(clean.w, clean.h, m, rng);
	// chroma basis: two orthonormal-ish directions with zero luma
	const double u[3] = {1.0, -kLumaR / kLumaG, 0.0}, v[3] = {0.0, -kLumaB / kLumaG, 1.0};
	const double amp = m == NoiseWhite2 ? 2.0 : 1.0;
	Image o = clean;
	for (size_t i = 0; i < o.px.size(); i++) {
		const double Y = luma(clean.px[i]);
		const double sy = amp * rel_sigma(std::max(Y, 0.5)) * std::max(Y, 0.5);
		const double sc = 0.6 * sy;
		const double dY = sy * n0[i];
		const double dc[3] = {sc * (n1[i] * u[0] + n2[i] * v[0]), sc * (n1[i] * u[1] + n2[i] * v[1]),
				      sc * (n1[i] * u[2] + n2[i] * v[2])};
		o.px[i].r += dY + dc[0];
		o.px[i].g += dY + dc[1];
		o.px[i].b += dY + dc[2];
	}
	return o;
}

// ---- methods -----------------------------------------------------------------------

struct Method {
	std::string name;
	virtual ~Method() = default;
	virtual void reset() = 0;
	// strength S applies to luma; chroma uses 1.5 S
	virtual Image step(const Image &noisy, const Frame &truth, double S) = 0;
};

// P1 global factor machinery, shared.
struct Gate {
	Params p; // cut/protection defaults
	double f = 0;
	bool f_valid = false;
	double factor(const Image &cur, const Image *ref)
	{
		if (!ref) {
			f_valid = false;
			return 0.0;
		}
		const double m = frame_metric(cur, *ref, kK);
		f = floor_update(m, f, f_valid);
		f_valid = true;
		return global_factor(p, m, f, false);
	}
};

struct NoneMethod : Method {
	NoneMethod() { name = "none"; }
	void reset() override {}
	Image step(const Image &n, const Frame &, double) override { return n; }
};

struct Hqdn3dMethod : Method {
	bool spatial;
	double spatial_ratio; // spatial strength relative to S
	Image hist;
	bool valid = false;
	Gate gate;
	Hqdn3dMethod(bool sp, double ratio) : spatial(sp), spatial_ratio(ratio)
	{
		name = sp ? "HQ spatial+temporal" : "HQ temporal";
	}
	void reset() override { valid = false, gate = Gate(); }
	Image step(const Image &n, const Frame &, double S) override
	{
		Image src = n;
		if (spatial) {
			SpatialParams sp;
			sp.luma = S * spatial_ratio;
			sp.chroma = 1.5 * S * spatial_ratio;
			sp.radius = 8;
			spatial_b(sp, n, src);
		}
		Params p = gate.p;
		p.temporal_luma = S;
		p.temporal_chroma = 1.5 * S;
		const double g = gate.factor(src, valid ? &hist : nullptr);
		Image out = src;
		if (valid)
			for (size_t i = 0; i < out.px.size(); i++)
				out.px[i] = temporal_pixel(p, src.px[i], hist.px[i], g);
		hist = out;
		valid = true;
		return out;
	}
};

struct NlmMethod : Method {
	NlmParams base;
	Image prev_in, prev_out;
	bool valid = false;
	Gate gate;
	NlmMethod(const std::string &n, const NlmParams &b) : base(b) { name = n; }
	void reset() override { valid = false, gate = Gate(); }
	Image step(const Image &n, const Frame &, double S) override
	{
		NlmParams p = base;
		p.luma = S;
		p.chroma = 1.5 * S;
		double g = 0;
		if (p.temporal)
			g = gate.factor(n, valid ? (p.policy == NlmPolicyB ? &prev_out : &prev_in) : nullptr);
		NlmFrame f{&n, valid ? &prev_in : nullptr, valid ? &prev_out : nullptr, g};
		Image out;
		nlm_offset(p, f, out);
		prev_in = n;
		prev_out = out;
		valid = true;
		return out;
	}
};

// Motion-compensated HQDN3D-style temporal: warp D(t-1) with (oracle) flow, then the P1
// recurrence against the warped history. Flow models: exact, 4x4 grid, 4x4 grid + error.
enum Interp { InterpBilinear, InterpCubic, InterpLanczos3 };

struct McMethod : Method {
	int grid;        // 1 = per pixel, 4 = 4x4 blocks
	double flow_err; // px, Gaussian per block
	int interp;
	bool zero_fallback = false; // also try the unwarped history; keep the better 3x3 match
	Image hist;
	bool valid = false;
	Gate gate;
	uint64_t frame_no = 0;
	McMethod(const std::string &n, int g, double err, int ip, bool zf = false)
		: grid(g),
		  flow_err(err),
		  interp(ip),
		  zero_fallback(zf)
	{
		name = n;
	}
	void reset() override { valid = false, gate = Gate(), frame_no = 0; }
	Image step(const Image &n, const Frame &truth, double S) override
	{
		frame_no++;
		Params p = gate.p;
		p.temporal_luma = S;
		p.temporal_chroma = 1.5 * S;
		Image out = n;
		if (valid) {
			Image warped = n;
			std::mt19937_64 rng(1000 + frame_no);
			std::normal_distribution<double> N(0, 1);
			const int bw = (n.w + grid - 1) / grid, bh = (n.h + grid - 1) / grid;
			std::vector<double> ex((size_t)bw * bh), ey((size_t)bw * bh);
			for (size_t i = 0; i < ex.size(); i++) {
				ex[i] = flow_err * N(rng);
				ey[i] = flow_err * N(rng);
			}
			for (int y = 0; y < n.h; y++)
				for (int x = 0; x < n.w; x++) {
					// grid: the block's flow is the true flow at the block centre
					const int bx = x / grid, by = y / grid;
					const int cx = std::min(n.w - 1, bx * grid + grid / 2),
						  cy = std::min(n.h - 1, by * grid + grid / 2);
					const size_t ci = grid == 1 ? (size_t)y * n.w + x : (size_t)cy * n.w + cx;
					const double fx = truth.fx[ci] + ex[(size_t)by * bw + bx],
						     fy = truth.fy[ci] + ey[(size_t)by * bw + bx];
					const double sx = x + fx, sy = y + fy;
					if (sx < 0 || sy < 0 || sx > n.w - 1 || sy > n.h - 1)
						continue; // no history: warped = current -> identity there
					px(warped, x, y) = interp == InterpCubic      ? sample_cubic(hist, sx, sy)
							   : interp == InterpLanczos3 ? sample_lanczos3(hist, sx, sy)
										      : sample_bilinear(hist, sx, sy);
				}
			if (zero_fallback) {
				// per pixel: keep whichever history (warped or same-coordinate) matches the
				// current frame better over a 3x3 patch in the comparison domain
				std::vector<double> fc(n.px.size()), fw(n.px.size()), fh(n.px.size());
				for (size_t i = 0; i < n.px.size(); i++) {
					fc[i] = comp(kLumaR * n.px[i].r + kLumaG * n.px[i].g + kLumaB * n.px[i].b, 0.1);
					fw[i] = comp(kLumaR * warped.px[i].r + kLumaG * warped.px[i].g +
							     kLumaB * warped.px[i].b,
						     0.1);
					fh[i] = comp(kLumaR * hist.px[i].r + kLumaG * hist.px[i].g +
							     kLumaB * hist.px[i].b,
						     0.1);
				}
				Image chosen = warped;
				for (int y = 0; y < n.h; y++)
					for (int x = 0; x < n.w; x++) {
						double dw = 0, dh = 0;
						for (int j = -1; j <= 1; j++)
							for (int i = -1; i <= 1; i++) {
								const size_t k =
									(size_t)std::clamp(y + j, 0, n.h - 1) * n.w +
									std::clamp(x + i, 0, n.w - 1);
								dw += std::fabs(fc[k] - fw[k]);
								dh += std::fabs(fc[k] - fh[k]);
							}
						if (dh < dw)
							px(chosen, x, y) = hist.at(x, y);
					}
				warped = chosen;
			}
			const double g = gate.factor(n, &warped);
			for (size_t i = 0; i < out.px.size(); i++)
				out.px[i] = temporal_pixel(p, n.px[i], warped.px[i], g);
		} else {
			gate.factor(n, nullptr);
		}
		hist = out;
		valid = true;
		return out;
	}
};

// ---- metrics -----------------------------------------------------------------------

double Fy(const Rgba &c)
{
	return comp(luma(c), kK);
}

struct Stats {
	double sum2 = 0;
	long n = 0;
	void add(double e)
	{
		sum2 += e * e;
		n++;
	}
	double rms() const { return n ? std::sqrt(sum2 / n) : 0; }
};

// texture gain: <HP(out), HP(clean)> / <HP(clean), HP(clean)> over a mask (HP = x - 3x3 mean)
struct TexGain {
	double num = 0, den = 0;
	double value() const { return den > 0 ? num / den : 0; }
};

double hp(const Image &im, int x, int y)
{
	double s = 0;
	for (int j = -1; j <= 1; j++)
		for (int i = -1; i <= 1; i++)
			s += Fy(im.at(std::clamp(x + i, 0, im.w - 1), std::clamp(y + j, 0, im.h - 1)));
	return Fy(im.at(x, y)) - s / 9.0;
}

struct Result {
	double ratio[7] = {0};  // error rms / input noise rms per zone (luma, F units)
	double tex[7] = {0};    // texture gain per zone
	double chroma_wall = 0; // chroma error / input on the wall
	double first_cut = 0;   // first frame after a cut: error / input
	double tflicker = 0;    // wall: rms of the frame-to-frame change of the error (temporal noise)
	double tflicker_in = 0;
};

struct Runner {
	NoiseModel noise;
	int w, h;
	Result run(Method &m, const Fixture &fx, double S, int warm)
	{
		m.reset();
		Stats eo_z[7], ei_z[7], ch_o, ch_i, fl_o, fl_i;
		TexGain tg[7];
		Result r;
		std::vector<double> prev_eo, prev_ei;
		for (int t = 0; t < fx.frames; t++) {
			const Frame f = fx.frame(t);
			const Image noisy = add_noise(f.clean, noise, 7919ull * (uint64_t)t + 17);
			const Image out = m.step(noisy, f, S);
			if (fx.kind == FixCut && t == fx.frames / 2) {
				Stats a, b;
				for (size_t i = 0; i < out.px.size(); i++) {
					a.add(Fy(out.px[i]) - Fy(f.clean.px[i]));
					b.add(Fy(noisy.px[i]) - Fy(f.clean.px[i]));
				}
				r.first_cut = a.rms() / std::max(b.rms(), 1e-12);
			}
			if (t < warm)
				continue;
			std::vector<double> ceo(out.px.size(), 0), cei(out.px.size(), 0);
			for (int y = 8; y < h - 8; y++)
				for (int x = 8; x < w - 8; x++) {
					const size_t i = (size_t)y * w + x;
					const int z = f.zone[i];
					const double eo = Fy(out.px[i]) - Fy(f.clean.px[i]);
					const double ei = Fy(noisy.px[i]) - Fy(f.clean.px[i]);
					eo_z[z].add(eo);
					ei_z[z].add(ei);
					const double hc = hp(f.clean, x, y);
					tg[z].num += hp(out, x, y) * hc;
					tg[z].den += hc * hc;
					if (z == ZoneWall) {
						const Rgba &o = out.px[i], &c = f.clean.px[i], &nn = noisy.px[i];
						const double yo = luma(o), yc = luma(c), yn = luma(nn);
						const double co[3] = {o.r - yo, o.g - yo, o.b - yo},
							     cc[3] = {c.r - yc, c.g - yc, c.b - yc},
							     cn[3] = {nn.r - yn, nn.g - yn, nn.b - yn};
						ch_o.add(chroma_distance(co, cc, yo, yc, kK));
						ch_i.add(chroma_distance(cn, cc, yn, yc, kK));
						ceo[i] = eo;
						cei[i] = ei;
						if (!prev_eo.empty() && fx.kind == FixStatic) {
							fl_o.add(eo - prev_eo[i]);
							fl_i.add(ei - prev_ei[i]);
						}
					}
				}
			prev_eo.swap(ceo);
			prev_ei.swap(cei);
		}
		for (int z = 0; z < 7; z++) {
			r.ratio[z] = ei_z[z].n ? eo_z[z].rms() / std::max(ei_z[z].rms(), 1e-12) : NAN;
			r.tex[z] = tg[z].den > 0 ? tg[z].value() : NAN;
		}
		r.chroma_wall = ch_o.rms() / std::max(ch_i.rms(), 1e-12);
		r.tflicker = fl_o.rms();
		r.tflicker_in = fl_i.rms();
		return r;
	}
};

// Smallest S on a log grid with wall ratio <= target; if none reaches it, the S with the
// lowest wall ratio (flagged). Spatial filters are not monotone in S on correlated noise:
// texture loss makes the error rise again at high S.
double match(Runner &R, Method &m, const Fixture &st, double target, int warm, bool *reached)
{
	double best_s = 0, best_r = 1e9;
	double lo_ok = -1;
	const double grid[] = {0.25, 0.5, 0.75, 1, 1.5, 2, 3, 4, 5, 6, 8, 10, 13, 16, 20};
	double prev_s = 0;
	for (double S : grid) {
		const double r = R.run(m, st, S, warm).ratio[ZoneWall];
		if (r < best_r) {
			best_r = r;
			best_s = S;
		}
		if (r <= target) {
			// refine between the previous grid point and this one
			double lo = prev_s, hi = S;
			for (int i = 0; i < 6; i++) {
				const double mid = 0.5 * (lo + hi);
				if (R.run(m, st, mid, warm).ratio[ZoneWall] <= target)
					hi = mid;
				else
					lo = mid;
			}
			lo_ok = hi;
			break;
		}
		prev_s = S;
	}
	*reached = lo_ok >= 0;
	return *reached ? lo_ok : best_s;
}

std::vector<std::unique_ptr<Method>> make_methods(bool quick)
{
	std::vector<std::unique_ptr<Method>> v;
	v.emplace_back(new NoneMethod());
	v.emplace_back(new Hqdn3dMethod(false, 0));
	v.emplace_back(new Hqdn3dMethod(true, 0.75));
	NlmParams sp;
	sp.patch = 1;
	sp.search = 3;
	v.emplace_back(new NlmMethod("NLM spatial 3x3/7x7", sp));
	auto tnlm = [&](const char *name, int P, int Rs, int Rt, int policy, double tgain) {
		NlmParams tp;
		tp.patch = P;
		tp.search = Rs;
		tp.tsearch = Rt;
		tp.temporal = true;
		tp.policy = policy;
		tp.tgain = tgain;
		v.emplace_back(new NlmMethod(name, tp));
	};
	tnlm("TNLM A 3x3 s7 t7", 1, 3, 3, NlmPolicyA, 1);
	if (!quick)
		tnlm("TNLM H 5x5 s7 t7", 2, 3, 3, NlmPolicyH, 1);
	tnlm("TNLM B 3x3 s7 t3 g9", 1, 3, 1, NlmPolicyB, 9);
	tnlm("TNLM B 3x3 t1 only g9", 1, 0, 0, NlmPolicyB, 9);
	v.emplace_back(new McMethod("MC oracle exact, bilinear", 1, 0.0, InterpBilinear));
	v.emplace_back(new McMethod("MC oracle exact, bicubic", 1, 0.0, InterpCubic));
	v.emplace_back(new McMethod("MC oracle exact, lanczos3", 1, 0.0, InterpLanczos3));
	v.emplace_back(new McMethod("MC oracle 4x4 grid", 4, 0.0, InterpCubic));
	v.emplace_back(new McMethod("MC oracle 4x4 + 0.3px err", 4, 0.3, InterpCubic));
	v.emplace_back(new McMethod("MC 4x4 + 0.3px err + zero", 4, 0.3, InterpCubic, true));
	return v;
}

Image read_frame(FILE *f, int w, int h)
{
	Image im = blank(w, h);
	std::vector<float> buf((size_t)w * h * 3);
	if (fread(buf.data(), sizeof(float), buf.size(), f) != buf.size())
		im.w = 0;
	for (size_t i = 0; i < im.px.size(); i++)
		im.px[i] = Rgba{buf[3 * i], buf[3 * i + 1], buf[3 * i + 2], 1};
	return im;
}

void write_frame(FILE *f, const Image &im)
{
	std::vector<float> buf(im.px.size() * 3);
	for (size_t i = 0; i < im.px.size(); i++) {
		buf[3 * i] = (float)im.px[i].r;
		buf[3 * i + 1] = (float)im.px[i].g;
		buf[3 * i + 2] = (float)im.px[i].b;
	}
	fwrite(buf.data(), sizeof(float), buf.size(), f);
}

} // namespace

int main(int argc, char **argv)
{
	NoiseModel noise = NoiseEnc;
	bool quick = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--noise") && i + 1 < argc)
			noise = !strcmp(argv[++i], "white2") ? NoiseWhite2 : NoiseEnc;
		else if (!strcmp(argv[i], "--quick"))
			quick = true;
		else if (!strcmp(argv[i], "--process") && i + 7 < argc) {
			// real footage: METHOD-INDEX S in out W H N
			const int mi = atoi(argv[i + 1]);
			const double S = atof(argv[i + 2]);
			const int w = atoi(argv[i + 5]), h = atoi(argv[i + 6]), n = atoi(argv[i + 7]);
			auto methods = make_methods(false);
			if (mi < 0 || mi >= (int)methods.size() || dynamic_cast<McMethod *>(methods[mi].get())) {
				fprintf(stderr, "method index 0..%zu (MC needs flow; not for real footage)\n",
					methods.size() - 1);
				for (size_t k = 0; k < methods.size(); k++)
					fprintf(stderr, "  %zu %s\n", k, methods[k]->name.c_str());
				return 2;
			}
			FILE *in = fopen(argv[i + 3], "rb"), *out = fopen(argv[i + 4], "wb");
			if (!in || !out)
				return 3;
			Method &m = *methods[mi];
			m.reset();
			Frame dummy;
			for (int t = 0; t < n; t++) {
				const Image im = read_frame(in, w, h);
				if (!im.w)
					break;
				write_frame(out, m.step(im, dummy, S));
			}
			fclose(in);
			fclose(out);
			return 0;
		}
	}

	const int W = 192, H = 144, frames = quick ? 24 : 36, warm = 12;
	const Image world = make_world(W + 16 * 40 + 120, H + 60, 11);
	// the second shot of the cut fixture: different content, layout and exposure
	Image world2 = make_world(W + 16 * 40 + 120, H + 60, 99);
	{
		const Image w2 = world2;
		for (int y = 0; y < w2.h; y++)
			for (int x = 0; x < w2.w; x++) {
				Rgba p = w2.at((x + 24) % w2.w, y);
				p.r *= 0.45;
				p.g *= 0.45;
				p.b *= 0.45;
				px(world2, x, y) = p;
			}
	}
	const Image obj = make_object(64, 5);
	Runner R{noise, W, H};
	Fixture st{FixStatic, 0, W, H, frames, world, world2, obj};
	const double target = 0.5;
	printf("noise model: %s; frame %dx%d, %d frames (first %d excluded)\n",
	       noise == NoiseEnc ? "measured encoded (correlated, ~0.9%% at 100-300 nits)"
				 : "white, 2x measured amplitude",
	       W, H, frames, warm);
	printf("strength matched so the WALL luma error is %.2f x the input noise (* = not reachable: best S shown)\n",
	       target);
	printf("ratios = output error rms / input noise rms vs the clean frame, luma in F = log2(1+Y/0.1) units;\n"
	       "texture gain = <HP(out),HP(clean)>/<HP(clean),HP(clean)> (1 kept, < 1 smoothed)\n\n");

	auto methods = make_methods(quick);
	std::vector<double> speeds = quick ? std::vector<double>{0.5, 2, 8} : std::vector<double>{0.5, 1, 2, 4, 8, 16};
	struct Row {
		std::string name;
		double S;
		bool reached;
		Result st, pan05, pan3, cut;
		std::vector<Result> obj;
	};
	std::vector<Row> rows;
	for (auto &mp : methods) {
		Method &m = *mp;
		Row row;
		row.name = m.name;
		row.reached = true;
		row.S = m.name == "none" ? 0 : match(R, m, st, target, warm, &row.reached);
		row.st = R.run(m, st, row.S, warm);
		for (double v : speeds) {
			Fixture fo{FixObject, v, W, H, frames, world, world2, obj};
			row.obj.push_back(R.run(m, fo, row.S, warm));
		}
		Fixture p05{FixPan, 0.5, W, H, frames, world, world2, obj},
			p3{FixPan, 3, W, H, frames, world, world2, obj};
		row.pan05 = R.run(m, p05, row.S, warm);
		row.pan3 = R.run(m, p3, row.S, warm);
		Fixture fc{FixCut, 0, W, H, frames, world, world2, obj};
		row.cut = R.run(m, fc, row.S, warm);
		rows.push_back(row);
		fprintf(stderr, "done %s\n", m.name.c_str());
	}

	printf("1. Static scene (error ratio per zone; texture gain on fabric / skin / text; wall chroma; wall temporal "
	       "flicker out/in)\n");
	printf("%-28s %7s | %5s %6s %5s %5s %5s | %5s %5s %5s | %5s | %5s\n", "method", "S", "wall", "fabric", "text",
	       "skin", "dark", "texF", "texS", "texT", "chrom", "flick");
	for (const Row &r : rows)
		printf("%-28s %6.2f%s | %5.2f %6.2f %5.2f %5.2f %5.2f | %5.2f %5.2f %5.2f | %5.2f | %5.2f\n",
		       r.name.c_str(), r.S, r.reached ? " " : "*", r.st.ratio[ZoneWall], r.st.ratio[ZoneFabric],
		       r.st.ratio[ZoneText], r.st.ratio[ZoneSkin], r.st.ratio[ZoneDark], r.st.tex[ZoneFabric],
		       r.st.tex[ZoneSkin], r.st.tex[ZoneText], r.st.chroma_wall,
		       r.st.tflicker / std::max(r.st.tflicker_in, 1e-12));

	printf("\n2. Moving textured object (64x64) over the static background, speed sweep (px/frame)\n");
	printf("   object error ratio / object texture gain\n%-28s", "method");
	for (double v : speeds)
		printf(" | %-10g", v);
	printf("\n");
	for (const Row &r : rows) {
		printf("%-28s", r.name.c_str());
		for (const Result &o : r.obj)
			printf(" | %5.2f/%4.2f", o.ratio[ZoneObject], o.tex[ZoneObject]);
		printf("\n");
	}
	printf("\n3. Disocclusion trail (background uncovered in the last 6 frames): error ratio (> 1 = ghosting)\n%-28s",
	       "method");
	for (double v : speeds)
		printf(" | %-5g", v);
	printf("\n");
	for (const Row &r : rows) {
		printf("%-28s", r.name.c_str());
		for (const Result &o : r.obj)
			printf(" | %5.2f", o.ratio[ZoneTrail]);
		printf("\n");
	}
	printf("\n4. Camera pan (wall ratio / fabric texture gain) and hard cut (first frame after the cut)\n");
	printf("%-28s | %-11s | %-11s | %s\n", "method", "pan 0.5", "pan 3", "cut");
	for (const Row &r : rows)
		printf("%-28s | %5.2f/%4.2f | %5.2f/%4.2f | %5.2f\n", r.name.c_str(), r.pan05.ratio[ZoneWall],
		       r.pan05.tex[ZoneFabric], r.pan3.ratio[ZoneWall], r.pan3.tex[ZoneFabric], r.cut.first_cut);
	return 0;
}
