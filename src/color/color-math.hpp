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

#pragma once

/*
 * HDR Color maths (brief sections 5-7), CPU side. No libobs, unit-tested in
 * tests/cpu/test_color.cpp. Written from the brief's equations.
 *
 * Working units: straight linear Rec.709-primary RGB in nominal nits.
 * P2 implements the global stages of the fixed v1 order (brief 5.3):
 *
 *   unpremultiply -> nits -> C0 -> white balance -> global exposure
 *   -> [zone masks + zone exposure: P3] -> contrast around pivot
 *   -> global wheel -> global saturation -> grade mix
 *   -> [low / high soft clip, gamut containment: P3/P5] -> working units -> premultiply
 */

#include <array>
#include <string>

namespace hdrtk {
namespace color {

using Vec3 = std::array<double, 3>;
using Mat3 = std::array<Vec3, 3>; // row-major, applied to column vectors

// Rec.709 / D65 RGB -> XYZ (brief 5.1, section 15). Y row is the luminance row.
extern const Mat3 kRgbToXyz;
extern const Vec3 kLuma;
extern const Mat3 kBradford;

Mat3 identity3();
Mat3 mul(const Mat3 &a, const Mat3 &b);
Vec3 mul(const Mat3 &m, const Vec3 &v);
bool invert(const Mat3 &m, Mat3 &out);
double dot(const Vec3 &a, const Vec3 &b);

// ---- White balance (brief 6) ----------------------------------------------

// CIE daylight locus x,y for 4000..25000 K (Colour's CCT_to_xy_CIE_D). Returns
// false outside the domain.
bool daylight_xy(double cct, double &x, double &y);
// CIE 1960 UCS
void xy_to_uv(double x, double y, double &u, double &v);
void uv_to_xy(double u, double v, double &x, double &y);

// Target white XYZ (Y = 1) for a mired shift and a tint in -1..1 (UI value / 100).
bool target_white(double mired_shift, double tint, Vec3 &xyz, std::string *error = nullptr);

// Bradford adaptation from D65 to the target white, expressed in linear
// Rec.709 RGB. Exactly the identity matrix at (0, 0).
bool white_balance_matrix(double mired_shift, double tint, Mat3 &rgb, std::string *error = nullptr);

// ---- Colour wheel (brief 7.4) ---------------------------------------------

// Luminance-orthogonal unit hue direction for angle theta (radians); red at
// 0, yellow 60 deg, green 120, cyan 180, blue 240, magenta 300.
Vec3 hue_direction(double theta);
// Wheel point (x, y) in the unit disk (radius clamped to 1) times the
// sensitivity k -> RGB balance delta. Zero radius -> exactly zero.
Vec3 wheel_delta(double x, double y, double k = 0.5);

// ---- Parameters -------------------------------------------------------------

struct GlobalParams {
	double exposure_ev = 0.0; // +-6
	double contrast = 1.0;    // 0.5 .. 2
	double pivot_nits = 18.0; // > 0
	double gray_nits = 18.0;  // zone/stop reference (brief 5.2)
	double saturation = 1.0;  // 0 .. 2
	double wb_mired = 0.0;    // -90 .. 90
	double wb_tint = 0.0;     // -100 .. 100 (UI units)
	double wheel_x = 0.0;     // unit disk
	double wheel_y = 0.0;
	double grade_mix = 1.0; // 0 .. 1
};

// Clamp to documented ranges, replace non-finite values with defaults.
// Returns a list of problems (empty when clean).
std::string sanitize(GlobalParams &p);

bool is_neutral(const GlobalParams &p);

// Values uploaded to the shader (precomputed on the CPU, brief 10.6).
struct ShaderParams {
	float wb_row[3][3];
	float exposure_gain;
	float contrast_minus_one;   // (contrast - 1)
	float log2_pivot_over_gray; // p
	float gray_nits;
	float wheel_delta[3];
	float saturation;
	float grade_mix;
	// Stage enables (uniform branches give exact identity for neutral stages).
	float use_wb, use_contrast, use_wheel, use_saturation, use_mix;
};

bool make_shader_params(const GlobalParams &p, ShaderParams &out, std::string *error = nullptr);

// Reference pixel pipeline on straight linear RGB in nits (double), and the
// float mirror of data/effects/hdr-color.effect (PSGrade, nits domain).
Vec3 reference_grade(const GlobalParams &p, const Vec3 &c_nits);
std::array<float, 3> shader_grade(const ShaderParams &s, const std::array<float, 3> &c_nits);

constexpr double kYEpsilonNits = 1e-6;

} // namespace color
} // namespace hdrtk
