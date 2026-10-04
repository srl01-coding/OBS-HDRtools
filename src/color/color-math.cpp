/*
OBS HDR Toolkit
Copyright (C) 2026 srl01-coding

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "color-math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hdrtk {
namespace color {

const Mat3 kRgbToXyz = {{
	{0.412390799265959, 0.357584339383878, 0.180480788401834},
	{0.212639005871510, 0.715168678767756, 0.072192315360734},
	{0.019330818715592, 0.119194779794626, 0.950532152249661},
}};
const Vec3 kLuma = {0.212639005871510, 0.715168678767756, 0.072192315360734};
const Mat3 kBradford = {{
	{0.8951, 0.2664, -0.1614},
	{-0.7502, 1.7135, 0.0367},
	{0.0389, -0.0685, 1.0296},
}};

static constexpr double kD65x = 0.3127, kD65y = 0.3290;
static constexpr double kBaseCct = 6504.0;
static constexpr double kPi = 3.14159265358979323846;

Mat3 identity3()
{
	return {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
}

Mat3 mul(const Mat3 &a, const Mat3 &b)
{
	Mat3 r{};
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) {
			double s = 0;
			for (int k = 0; k < 3; k++)
				s += a[i][k] * b[k][j];
			r[i][j] = s;
		}
	return r;
}

Vec3 mul(const Mat3 &m, const Vec3 &v)
{
	return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
		m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

double dot(const Vec3 &a, const Vec3 &b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

bool invert(const Mat3 &m, Mat3 &o)
{
	const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
	const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
	const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
	const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
	if (!std::isfinite(det) || std::fabs(det) < 1e-300)
		return false;
	const double id = 1.0 / det;
	o[0][0] = c00 * id;
	o[1][0] = c01 * id;
	o[2][0] = c02 * id;
	o[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * id;
	o[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * id;
	o[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * id;
	o[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * id;
	o[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * id;
	o[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * id;
	return true;
}

bool daylight_xy(double t, double &x, double &y)
{
	if (!(t >= 4000.0 && t <= 25000.0))
		return false;
	if (t <= 7000.0)
		x = -4.6070e9 / (t * t * t) + 2.9678e6 / (t * t) + 0.09911e3 / t + 0.244063;
	else
		x = -2.0064e9 / (t * t * t) + 1.9018e6 / (t * t) + 0.24748e3 / t + 0.237040;
	y = -3.0 * x * x + 2.87 * x - 0.275;
	return true;
}

void xy_to_uv(double x, double y, double &u, double &v)
{
	const double d = -2.0 * x + 12.0 * y + 3.0;
	u = 4.0 * x / d;
	v = 6.0 * y / d;
}

void uv_to_xy(double u, double v, double &x, double &y)
{
	const double d = 4.0 + 2.0 * u - 8.0 * v;
	x = 3.0 * u / d;
	y = 2.0 * v / d;
}

static bool locus_uv(double mired, double &u, double &v)
{
	if (!(mired > 0))
		return false;
	double x, y;
	if (!daylight_xy(1e6 / mired, x, y))
		return false;
	xy_to_uv(x, y, u, v);
	return true;
}

static void set_error(std::string *e, const char *msg)
{
	if (e)
		*e = msg;
}

bool target_white(double mired_shift, double tint, Vec3 &xyz, std::string *error)
{
	const double m0 = 1e6 / kBaseCct;
	const double m = m0 + mired_shift;
	double u0, v0, ul, vl, ub, vb, up, vp, um, vm;
	xy_to_uv(kD65x, kD65y, u0, v0);
	if (!locus_uv(m, ul, vl) || !locus_uv(m0, ub, vb) || !locus_uv(m + 0.1, up, vp) || !locus_uv(m - 0.1, um, vm)) {
		set_error(error, "temperature outside the 4000-25000 K daylight domain");
		return false;
	}
	double u = u0 + ul - ub;
	double v = v0 + vl - vb;
	// Perpendicular to the locus; positive tint = magenta (negative v at D65).
	double nx = vp - vm, ny = -(up - um);
	const double nl = std::sqrt(nx * nx + ny * ny);
	if (!(nl > 0)) {
		set_error(error, "degenerate daylight locus tangent");
		return false;
	}
	nx /= nl;
	ny /= nl;
	if (ny > 0) {
		nx = -nx;
		ny = -ny;
	}
	u += tint * 0.01 * nx;
	v += tint * 0.01 * ny;
	double x, y;
	uv_to_xy(u, v, x, y);
	if (!std::isfinite(x) || !std::isfinite(y) || !(y > 1e-6) || !(x > 0) || x + y >= 1.0) {
		set_error(error, "invalid target chromaticity");
		return false;
	}
	xyz = {x / y, 1.0, (1.0 - x - y) / y};
	return true;
}

bool white_balance_matrix(double mired_shift, double tint, Mat3 &rgb, std::string *error)
{
	if (mired_shift == 0.0 && tint == 0.0) {
		rgb = identity3(); // exact neutral (brief 6.3)
		return true;
	}
	Vec3 wt;
	if (!target_white(mired_shift, tint, wt, error))
		return false;
	const Vec3 w0 = {kD65x / kD65y, 1.0, (1.0 - kD65x - kD65y) / kD65y};
	const Vec3 ct = mul(kBradford, wt), c0 = mul(kBradford, w0);
	for (int i = 0; i < 3; i++)
		if (!(std::fabs(c0[i]) > 1e-6) || !std::isfinite(ct[i]) || !(ct[i] / c0[i] > 0)) {
			set_error(error, "invalid cone response");
			return false;
		}
	Mat3 d = {{{ct[0] / c0[0], 0, 0}, {0, ct[1] / c0[1], 0}, {0, 0, ct[2] / c0[2]}}};
	Mat3 b_inv, m_inv;
	if (!invert(kBradford, b_inv) || !invert(kRgbToXyz, m_inv)) {
		set_error(error, "matrix inversion failed");
		return false;
	}
	const Mat3 a_xyz = mul(mul(b_inv, d), kBradford);
	rgb = mul(mul(m_inv, a_xyz), kRgbToXyz);
	for (auto &row : rgb)
		for (double x : row)
			if (!std::isfinite(x)) {
				set_error(error, "non-finite white balance matrix");
				return false;
			}
	return true;
}

Vec3 hue_direction(double theta)
{
	Vec3 h = {std::cos(theta), std::cos(theta - 2.0 * kPi / 3.0), std::cos(theta + 2.0 * kPi / 3.0)};
	const double yl = dot(kLuma, h);
	for (double &c : h)
		c -= yl;
	const double l = std::sqrt(dot(h, h));
	for (double &c : h)
		c /= l;
	return h;
}

Vec3 wheel_delta(double x, double y, double k)
{
	double r = std::sqrt(x * x + y * y);
	if (!(r > 0))
		return {0, 0, 0}; // no atan2(0, 0)
	const double theta = std::atan2(y, x);
	r = std::min(r, 1.0);
	Vec3 d = hue_direction(theta);
	for (double &c : d)
		c *= k * r;
	return d;
}

static void clampv(double &v, double lo, double hi, double def, const char *name, std::string &log)
{
	if (!std::isfinite(v)) {
		log += std::string(name) + " not finite; ";
		v = def;
	} else if (v < lo || v > hi) {
		log += std::string(name) + " out of range; ";
		v = std::clamp(v, lo, hi);
	}
}

const char *const kZoneNames[kZoneCount] = {"black", "dark", "shadow", "light", "highlight", "specular"};

ZoneParams default_zone(int zone)
{
	// Brief 7.1 table (project defaults, stops relative to gray_nits).
	static const double e[kZoneCount][4] = {
		{kOpenLow, kOpenLow + 1, -7, -4},   {-7, -5, -3, -1}, {-4, -2, 0, 2}, {-1, 1, 2, 4}, {2, 3.5, 4.5, 6},
		{4.5, 6, kOpenHigh, kOpenHigh + 1},
	};
	ZoneParams z;
	z.a = e[zone][0];
	z.b = e[zone][1];
	z.c = e[zone][2];
	z.d = e[zone][3];
	return z;
}

std::array<ZoneParams, kZoneCount> default_zones()
{
	std::array<ZoneParams, kZoneCount> z;
	for (int i = 0; i < kZoneCount; i++)
		z[i] = default_zone(i);
	return z;
}

double smooth01(double t)
{
	t = std::clamp(t, 0.0, 1.0); // the mask coordinate, never RGB
	return t * t * (3.0 - 2.0 * t);
}

double zone_weight(double s, double a, double b, double c, double d)
{
	const double left = smooth01((s - a) / (b - a));
	const double right = 1.0 - smooth01((s - c) / (d - c));
	return left * right;
}

double tonal_stop(double y, double gray)
{
	return std::log2(std::max(y, kYEpsilonNits) / gray);
}

std::string sanitize(GlobalParams &p)
{
	std::string log;
	clampv(p.exposure_ev, -6, 6, 0, "exposure", log);
	clampv(p.contrast, 0.5, 2.0, 1, "contrast", log);
	clampv(p.pivot_nits, 0.01, 10000, 18, "pivot", log);
	clampv(p.gray_nits, 0.01, 10000, 18, "gray reference", log);
	clampv(p.saturation, 0, 2, 1, "saturation", log);
	clampv(p.wb_mired, -90, 90, 0, "temperature", log);
	clampv(p.wb_tint, -100, 100, 0, "tint", log);
	clampv(p.wheel_x, -1, 1, 0, "wheel x", log);
	clampv(p.wheel_y, -1, 1, 0, "wheel y", log);
	clampv(p.grade_mix, 0, 1, 1, "grade mix", log);
	for (int i = 0; i < kZoneCount; i++) {
		ZoneParams &z = p.zones[i];
		const ZoneParams def = default_zone(i);
		const std::string n = std::string(kZoneNames[i]) + " ";
		clampv(z.exposure_ev, -4, 4, 0, (n + "exposure").c_str(), log);
		clampv(z.saturation, 0, 2, 1, (n + "saturation").c_str(), log);
		clampv(z.wheel_x, -1, 1, 0, (n + "wheel x").c_str(), log);
		clampv(z.wheel_y, -1, 1, 0, (n + "wheel y").c_str(), log);
		if (i == ZoneBlack) {
			z.a = def.a;
			z.b = def.b;
		} else {
			clampv(z.a, -kEdgeLimit, kEdgeLimit, def.a, (n + "edge a").c_str(), log);
			clampv(z.b, -kEdgeLimit, kEdgeLimit, def.b, (n + "edge b").c_str(), log);
		}
		if (i == ZoneSpecular) {
			z.c = def.c;
			z.d = def.d;
		} else {
			clampv(z.c, -kEdgeLimit, kEdgeLimit, def.c, (n + "edge c").c_str(), log);
			clampv(z.d, -kEdgeLimit, kEdgeLimit, def.d, (n + "edge d").c_str(), log);
		}
		const ZoneParams before = z;
		if (i != ZoneBlack)
			z.b = std::max(z.b, z.a + kMinFalloff);
		if (i != ZoneBlack && i != ZoneSpecular)
			z.c = std::max(z.c, z.b);
		if (i != ZoneSpecular)
			z.d = std::max(z.d, z.c + kMinFalloff);
		if (z.b != before.b || z.c != before.c || z.d != before.d)
			log += n + "edges reordered; ";
	}
	return log;
}

bool is_neutral(const GlobalParams &p)
{
	if (!(p.exposure_ev == 0 && p.contrast == 1 && p.saturation == 1 && p.wb_mired == 0 && p.wb_tint == 0 &&
	      p.wheel_x == 0 && p.wheel_y == 0))
		return false;
	for (const ZoneParams &z : p.zones)
		if (z.active())
			return false;
	return true;
	// grade_mix is irrelevant when the grade itself is neutral.
}

struct StageUse {
	bool zones = false, wheel = false, saturation = false;
};

static StageUse stage_use(const GlobalParams &p)
{
	StageUse u;
	u.wheel = p.wheel_x != 0 || p.wheel_y != 0;
	u.saturation = p.saturation != 1.0;
	for (const ZoneParams &z : p.zones) {
		if (!z.active())
			continue;
		u.zones = true;
		u.wheel = u.wheel || z.wheel_x != 0 || z.wheel_y != 0;
		u.saturation = u.saturation || z.saturation != 1.0;
	}
	return u;
}

bool make_shader_params(const GlobalParams &p, ShaderParams &s, std::string *error)
{
	Mat3 wb;
	if (!white_balance_matrix(p.wb_mired, p.wb_tint / 100.0, wb, error))
		return false;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			s.wb_row[i][j] = (float)wb[i][j];
	s.exposure_gain = (float)std::exp2(p.exposure_ev);
	s.contrast_minus_one = (float)(p.contrast - 1.0);
	s.log2_pivot_over_gray = (float)std::log2(p.pivot_nits / p.gray_nits);
	s.gray_nits = (float)p.gray_nits;
	const Vec3 wd = wheel_delta(p.wheel_x, p.wheel_y);
	for (int i = 0; i < 3; i++)
		s.wheel_delta[i] = (float)wd[i];
	s.saturation = (float)p.saturation;
	s.grade_mix = (float)p.grade_mix;
	for (int i = 0; i < kZoneCount; i++) {
		const ZoneParams &z = p.zones[i];
		s.zone_edges[i][0] = (float)z.a;
		s.zone_edges[i][1] = (float)z.b;
		s.zone_edges[i][2] = (float)z.c;
		s.zone_edges[i][3] = (float)z.d;
		const bool on = z.active();
		s.zone_ev[i] = on ? (float)z.exposure_ev : 0.0f;
		s.zone_sat[i] = on ? (float)z.saturation : 1.0f;
		const Vec3 zd = on ? wheel_delta(z.wheel_x, z.wheel_y) : Vec3{0, 0, 0};
		for (int k = 0; k < 3; k++)
			s.zone_wheel[i][k] = (float)zd[k];
	}
	const StageUse u = stage_use(p);
	s.use_wb = (p.wb_mired != 0 || p.wb_tint != 0) ? 1.0f : 0.0f;
	s.use_contrast = p.contrast != 1.0 ? 1.0f : 0.0f;
	s.use_zones = u.zones ? 1.0f : 0.0f;
	s.use_wheel = u.wheel ? 1.0f : 0.0f;
	s.use_saturation = u.saturation ? 1.0f : 0.0f;
	s.use_mix = p.grade_mix != 1.0 ? 1.0f : 0.0f;
	return true;
}

static Vec3 wb_and_exposure(const GlobalParams &p, const Vec3 &in)
{
	Vec3 c = in;
	if (p.wb_mired != 0 || p.wb_tint != 0) {
		Mat3 wb;
		if (white_balance_matrix(p.wb_mired, p.wb_tint / 100.0, wb))
			c = mul(wb, c);
	}
	const double g = std::exp2(p.exposure_ev);
	for (double &x : c)
		x *= g;
	return c;
}

static std::array<double, kZoneCount> weights_at(const GlobalParams &p, const Vec3 &c)
{
	std::array<double, kZoneCount> w;
	const double s = tonal_stop(dot(kLuma, c), p.gray_nits);
	for (int i = 0; i < kZoneCount; i++) {
		const ZoneParams &z = p.zones[i];
		w[i] = zone_weight(s, z.a, z.b, z.c, z.d);
	}
	return w;
}

std::array<double, kZoneCount> reference_weights(const GlobalParams &p, const Vec3 &in)
{
	return weights_at(p, wb_and_exposure(p, in));
}

// Written directly from brief 5.3 / 7.1-7.5 (independently of the shader).
Vec3 reference_grade(const GlobalParams &p, const Vec3 &in)
{
	const Vec3 c0 = in;
	const StageUse u = stage_use(p);
	Vec3 c = wb_and_exposure(p, in);

	// Masks frozen here: nothing below recomputes membership.
	std::array<double, kZoneCount> w{};
	if (u.zones) {
		w = weights_at(p, c);
		double zone_ev = 0;
		for (int i = 0; i < kZoneCount; i++)
			if (p.zones[i].active())
				zone_ev += w[i] * p.zones[i].exposure_ev;
		const double g = std::exp2(zone_ev);
		for (double &x : c)
			x *= g;
	}
	if (p.contrast != 1.0) {
		const double y = dot(kLuma, c);
		const double s = std::log2(std::max(std::fabs(y), kYEpsilonNits) / p.gray_nits);
		const double pv = std::log2(p.pivot_nits / p.gray_nits);
		const double k = std::exp2((p.contrast - 1.0) * (s - pv));
		for (double &x : c)
			x *= k;
	}
	if (u.wheel) {
		Vec3 delta = wheel_delta(p.wheel_x, p.wheel_y);
		for (int i = 0; i < kZoneCount; i++) {
			if (!p.zones[i].active())
				continue;
			const Vec3 zd = wheel_delta(p.zones[i].wheel_x, p.zones[i].wheel_y);
			for (int k = 0; k < 3; k++)
				delta[k] += w[i] * zd[k];
		}
		const double y = std::max(dot(kLuma, c), 0.0);
		for (int k = 0; k < 3; k++)
			c[k] += y * delta[k];
	}
	if (u.saturation) {
		double st = p.saturation;
		for (int i = 0; i < kZoneCount; i++)
			if (p.zones[i].active())
				st *= 1.0 + w[i] * (p.zones[i].saturation - 1.0);
		const double y = dot(kLuma, c);
		for (double &x : c)
			x = y + st * (x - y);
	}
	if (p.grade_mix != 1.0)
		for (int i = 0; i < 3; i++)
			c[i] = (1.0 - p.grade_mix) * c0[i] + p.grade_mix * c[i];
	return c;
}

// Float mirror of PSGrade's nits-domain body in data/effects/hdr-color.effect.
// Keep in lock-step with the effect, line for line.
static float smooth01f(float t)
{
	t = std::clamp(t, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

static float zone_weightf(float s, const float e[4])
{
	const float left = smooth01f((s - e[0]) / (e[1] - e[0]));
	const float right = 1.0f - smooth01f((s - e[2]) / (e[3] - e[2]));
	return left * right;
}

std::array<float, 3> shader_grade(const ShaderParams &s, const std::array<float, 3> &in)
{
	const float lr = (float)kLuma[0], lg = (float)kLuma[1], lb = (float)kLuma[2];
	auto luma = [&](const std::array<float, 3> &v) {
		return lr * v[0] + lg * v[1] + lb * v[2];
	};
	const std::array<float, 3> c0 = in;
	std::array<float, 3> c = in;
	if (s.use_wb > 0.5f) {
		std::array<float, 3> t;
		for (int i = 0; i < 3; i++)
			t[i] = s.wb_row[i][0] * c[0] + s.wb_row[i][1] * c[1] + s.wb_row[i][2] * c[2];
		c = t;
	}
	for (float &x : c)
		x *= s.exposure_gain;

	float w[kZoneCount] = {0, 0, 0, 0, 0, 0};
	if (s.use_zones > 0.5f) {
		const float sm = std::log2(std::max(luma(c), 0.000001f) / s.gray_nits);
		float zev = 0.0f;
		for (int i = 0; i < kZoneCount; i++) {
			w[i] = zone_weightf(sm, s.zone_edges[i]);
			zev += w[i] * s.zone_ev[i];
		}
		const float g = std::exp2(zev);
		for (float &x : c)
			x *= g;
	}
	if (s.use_contrast > 0.5f) {
		const float y = luma(c);
		const float st = std::log2(std::max(std::fabs(y), 0.000001f) / s.gray_nits);
		const float k = std::exp2(s.contrast_minus_one * (st - s.log2_pivot_over_gray));
		for (float &x : c)
			x *= k;
	}
	if (s.use_wheel > 0.5f) {
		float d[3] = {s.wheel_delta[0], s.wheel_delta[1], s.wheel_delta[2]};
		for (int i = 0; i < kZoneCount; i++)
			for (int k = 0; k < 3; k++)
				d[k] += w[i] * s.zone_wheel[i][k];
		const float y = std::max(luma(c), 0.0f);
		for (int k = 0; k < 3; k++)
			c[k] += d[k] * y;
	}
	if (s.use_saturation > 0.5f) {
		float st = s.saturation;
		for (int i = 0; i < kZoneCount; i++)
			st *= 1.0f + w[i] * (s.zone_sat[i] - 1.0f);
		const float y = luma(c);
		for (float &x : c)
			x = y + st * (x - y);
	}
	if (s.use_mix > 0.5f)
		for (int i = 0; i < 3; i++)
			c[i] = (1.0f - s.grade_mix) * c0[i] + s.grade_mix * c[i];
	return c;
}

} // namespace color
} // namespace hdrtk
