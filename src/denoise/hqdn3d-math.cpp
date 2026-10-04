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

// Implemented from docs/HQDN3D_DESIGN.md (independent of FFmpeg/MPlayer code).

#include "hqdn3d-math.hpp"

#include <algorithm>
#include <cmath>

namespace hdrtk {
namespace denoise {

static void clampv(double &v, double lo, double hi, double def)
{
	if (!std::isfinite(v))
		v = def;
	else
		v = std::clamp(v, lo, hi);
}

void sanitize(Params &p)
{
	clampv(p.temporal_luma, 0, 20, 0);
	clampv(p.temporal_chroma, 0, 20, 0);
	clampv(p.k_nits, 0.001, 10, 0.1);
	clampv(p.cut_sensitivity, 0, 100, 50);
	clampv(p.protection_amount, 0, 1, 0.8);
}

double comp(double y, double k)
{
	const double a = std::log2(1.0 + std::fabs(y) / k);
	return y < 0 ? -a : a;
}

double response(double x)
{
	if (!(x < 1.0))
		return 0.0;
	const double u = 1.0 - x * x;
	return u * u;
}

double cut_threshold(double s)
{
	return 1.2 * std::exp2(-s / 25.0);
}

double smoothstep(double e0, double e1, double x)
{
	double t = (x - e0) / (e1 - e0);
	t = std::clamp(t, 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

double global_factor(const Params &p, double m, double f, bool reset, bool *cut)
{
	if (cut)
		*cut = false;
	if (reset)
		return 0.0;
	if (p.cut_reset && m >= cut_threshold(p.cut_sensitivity)) {
		if (cut)
			*cut = true;
		return 0.0;
	}
	if (!p.protection)
		return 1.0;
	const double prot = smoothstep(0.0, kProtectRamp, m - kProtectFloorMul * f);
	return 1.0 - p.protection_amount * prot;
}

Rgba temporal_pixel(const Params &p, const Rgba &cur, const Rgba &hist, double g)
{
	const double tl = kThresholdPerUnit * p.temporal_luma;
	const double tc = kThresholdPerUnit * p.temporal_chroma;
	const double yc = kLumaR * cur.r + kLumaG * cur.g + kLumaB * cur.b;
	const double yh = kLumaR * hist.r + kLumaG * hist.g + kLumaB * hist.b;
	const double cc[3] = {cur.r - yc, cur.g - yc, cur.b - yc};
	const double ch[3] = {hist.r - yh, hist.g - yh, hist.b - yh};

	const double dl = std::fabs(comp(yh, p.k_nits) - comp(yc, p.k_nits));
	double dd = 0;
	for (int i = 0; i < 3; i++)
		dd += (ch[i] - cc[i]) * (ch[i] - cc[i]);
	const double dc = std::sqrt(dd) / (0.5 * (std::fabs(yh) + std::fabs(yc)) + p.k_nits) / std::log(2.0);

	const double wl = tl > 0 ? kBeta * response(dl / tl) * g : 0.0;
	const double wc = tc > 0 ? kBeta * response(dc / tc) * g : 0.0;
	if (wl == 0.0 && wc == 0.0)
		return cur; // exact identity (S = 0, cut, reset, or all differences above threshold)

	const double yo = yc + wl * (yh - yc);
	Rgba o;
	o.r = yo + cc[0] + wc * (ch[0] - cc[0]);
	o.g = yo + cc[1] + wc * (ch[1] - cc[1]);
	o.b = yo + cc[2] + wc * (ch[2] - cc[2]);
	o.a = cur.a;
	return o;
}

static double luma(const Rgba &c)
{
	return kLumaR * c.r + kLumaG * c.g + kLumaB * c.b;
}

double frame_metric(const Image &cur, const Image &hist, double k)
{
	// level 1: w/16 x h/16, 4 x 4 samples per 16 x 16 block at offsets 2, 6, 10, 14
	const int w1 = (cur.w + kBlock - 1) / kBlock, h1 = (cur.h + kBlock - 1) / kBlock;
	std::vector<double> l1((size_t)w1 * (size_t)h1);
	for (int by = 0; by < h1; by++)
		for (int bx = 0; bx < w1; bx++) {
			double sum = 0;
			int n = 0;
			for (int j = 0; j < kSub; j++)
				for (int i = 0; i < kSub; i++) {
					const int x = bx * kBlock + 2 + 4 * i, y = by * kBlock + 2 + 4 * j;
					if (x >= cur.w || y >= cur.h)
						continue;
					sum += std::fabs(comp(luma(cur.at(x, y)), k) - comp(luma(hist.at(x, y)), k));
					n++;
				}
			l1[(size_t)by * w1 + bx] = n ? sum / n : 0.0;
		}
	// level 2: 16 x 16 blocks of level 1
	const int w2 = (w1 + kBlock - 1) / kBlock, h2 = (h1 + kBlock - 1) / kBlock;
	std::vector<double> l2((size_t)w2 * (size_t)h2);
	for (int by = 0; by < h2; by++)
		for (int bx = 0; bx < w2; bx++) {
			double sum = 0;
			int n = 0;
			for (int j = 0; j < kBlock; j++)
				for (int i = 0; i < kBlock; i++) {
					const int x = bx * kBlock + i, y = by * kBlock + j;
					if (x >= w1 || y >= h1)
						continue;
					sum += l1[(size_t)y * w1 + x];
					n++;
				}
			l2[(size_t)by * w2 + bx] = n ? sum / n : 0.0;
		}
	// level 3: everything
	double sum = 0;
	for (double v : l2)
		sum += v;
	return l2.empty() ? 0.0 : sum / (double)l2.size();
}

double floor_update(double m, double f_prev, bool f_valid)
{
	return f_valid ? std::min(m, f_prev * kFloorRise + kFloorAdd) : m;
}

// ---- float mirror ------------------------------------------------------------

ShaderParams make_shader_params(const Params &p)
{
	ShaderParams s;
	s.t_luma = (float)(kThresholdPerUnit * p.temporal_luma);
	s.t_chroma = (float)(kThresholdPerUnit * p.temporal_chroma);
	s.k = (float)p.k_nits;
	s.m_cut = (float)cut_threshold(p.cut_sensitivity);
	s.protect_amount = p.protection ? (float)p.protection_amount : 0.0f;
	s.cut_enabled = p.cut_reset ? 1.0f : 0.0f;
	return s;
}

static float comp_f(float y, float k)
{
	const float a = std::log2(1.0f + std::fabs(y) / k);
	return y < 0.0f ? -a : a;
}

static float smoothstep_f(float e0, float e1, float x)
{
	float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

float global_factor_f(const ShaderParams &s, float m, float f, bool reset)
{
	if (reset)
		return 0.0f;
	if (s.cut_enabled > 0.5f && m >= s.m_cut)
		return 0.0f;
	const float prot = smoothstep_f(0.0f, (float)kProtectRamp, m - (float)kProtectFloorMul * f);
	return 1.0f - s.protect_amount * prot;
}

static float response_f(float x)
{
	if (!(x < 1.0f))
		return 0.0f;
	const float u = 1.0f - x * x;
	return u * u;
}

void temporal_pixel_f(const ShaderParams &s, const float cur[4], const float hist[4], float g, float out[4])
{
	const float lr = (float)kLumaR, lg = (float)kLumaG, lb = (float)kLumaB;
	const float yc = lr * cur[0] + lg * cur[1] + lb * cur[2];
	const float yh = lr * hist[0] + lg * hist[1] + lb * hist[2];
	float cc[3], ch[3];
	for (int i = 0; i < 3; i++) {
		cc[i] = cur[i] - yc;
		ch[i] = hist[i] - yh;
	}
	const float dl = std::fabs(comp_f(yh, s.k) - comp_f(yc, s.k));
	float dd = 0.0f;
	for (int i = 0; i < 3; i++)
		dd += (ch[i] - cc[i]) * (ch[i] - cc[i]);
	const float dc = std::sqrt(dd) / (0.5f * (std::fabs(yh) + std::fabs(yc)) + s.k) * 1.442695041f;
	const float wl = s.t_luma > 0.0f ? (float)kBeta * response_f(dl / s.t_luma) * g : 0.0f;
	const float wc = s.t_chroma > 0.0f ? (float)kBeta * response_f(dc / s.t_chroma) * g : 0.0f;
	if (wl == 0.0f && wc == 0.0f) {
		for (int i = 0; i < 4; i++)
			out[i] = cur[i];
		return;
	}
	const float yo = yc + wl * (yh - yc);
	for (int i = 0; i < 3; i++)
		out[i] = yo + cc[i] + wc * (ch[i] - cc[i]);
	out[3] = cur[3];
}

float frame_metric_f(const Image &cur, const Image &hist, float k)
{
	auto lum = [](const Rgba &c) {
		return (float)kLumaR * (float)c.r + (float)kLumaG * (float)c.g + (float)kLumaB * (float)c.b;
	};
	const int w1 = (cur.w + kBlock - 1) / kBlock, h1 = (cur.h + kBlock - 1) / kBlock;
	std::vector<float> l1((size_t)w1 * (size_t)h1);
	for (int by = 0; by < h1; by++)
		for (int bx = 0; bx < w1; bx++) {
			float sum = 0.0f, n = 0.0f;
			for (int j = 0; j < kSub; j++)
				for (int i = 0; i < kSub; i++) {
					const int x = bx * kBlock + 2 + 4 * i, y = by * kBlock + 2 + 4 * j;
					if (x < cur.w && y < cur.h) {
						sum += std::fabs(comp_f(lum(cur.at(x, y)), k) -
								 comp_f(lum(hist.at(x, y)), k));
						n += 1.0f;
					}
				}
			l1[(size_t)by * w1 + bx] = sum / std::max(n, 1.0f);
		}
	const int w2 = (w1 + kBlock - 1) / kBlock, h2 = (h1 + kBlock - 1) / kBlock;
	std::vector<float> l2((size_t)w2 * (size_t)h2);
	for (int by = 0; by < h2; by++)
		for (int bx = 0; bx < w2; bx++) {
			float sum = 0.0f, n = 0.0f;
			for (int j = 0; j < kBlock; j++)
				for (int i = 0; i < kBlock; i++) {
					const int x = bx * kBlock + i, y = by * kBlock + j;
					if (x < w1 && y < h1) {
						sum += l1[(size_t)y * w1 + x];
						n += 1.0f;
					}
				}
			l2[(size_t)by * w2 + bx] = sum / std::max(n, 1.0f);
		}
	float sum = 0.0f;
	for (float v : l2)
		sum += v;
	return sum / std::max((float)l2.size(), 1.0f);
}

} // namespace denoise
} // namespace hdrtk
