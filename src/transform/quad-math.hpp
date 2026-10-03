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
 * Corner-pin geometry (brief section 4). Pure CPU code, no libobs, so it is
 * unit-tested in tests/cpu/test_quad.cpp. Written from the brief's equations;
 * no StreamFX code is used.
 *
 * Conventions:
 *   normalised output rectangle, x right, y down, identity A=(0,0) B=(1,0) C=(1,1) D=(0,1)
 *   corner order A=TL, B=TR, C=BR, D=BL (perimeter order)
 *   source square (u,v) in [0,1]^2 maps to the corresponding corners
 */

#include <array>
#include <string>

namespace hdrtk {
namespace quad {

struct Vec2 {
	double x = 0.0, y = 0.0;
};

struct Quad {
	Vec2 a{0, 0}, b{1, 0}, c{1, 1}, d{0, 1}; // TL, TR, BR, BL
};

using Mat3 = std::array<std::array<double, 3>, 3>;

// Documented coordinate limits: -400% .. 500% of the output rectangle.
constexpr double kCoordMin = -4.0;
constexpr double kCoordMax = 5.0;

bool is_identity(const Quad &q);

// Forward bilinear map P(u,v) (StreamFX-style warp model).
Vec2 forward_bilinear(const Quad &q, double u, double v);

// Brief 4.3: inverse of the bilinear map for destination point p, double
// precision CPU reference. Returns false when p is outside the warped quad.
bool inverse_bilinear(const Quad &q, Vec2 p, double &u, double &v);

// Brief 4.4: homography H mapping source (u,v,1) to destination (x_h,y_h,w_h),
// h33 = 1, solved by pivoted Gaussian elimination. Returns false if singular.
bool solve_homography(const Quad &q, Mat3 &h);
bool invert3(const Mat3 &m, Mat3 &inv);
Vec2 project(const Mat3 &h, double u, double v);

struct Validation {
	bool ok = false;
	std::string reason; // empty when ok
	double area = 0.0;  // signed (positive = clockwise in y-down coordinates)
};

// Brief 4.6. `projective` additionally checks the homography (residual,
// homogeneous denominator sign over the source square, invertibility).
Validation validate(const Quad &q, bool projective);

// Uniforms for the shader, single precision as uploaded.
struct BilinearUniforms {
	float a[2], e[2], f[2], g[2];
};
struct ProjectiveUniforms {
	float row0[3], row1[3], row2[3]; // H^-1 rows, normalised
	float w_sign;                    // sign of the homogeneous denominator inside the quad
};
BilinearUniforms bilinear_uniforms(const Quad &q);
bool projective_uniforms(const Quad &q, ProjectiveUniforms &out);

// Float mirrors of the shader functions in data/effects/hdr-transform.effect.
// Kept in lock-step with the shader so tests measure GPU-like precision.
// Return value: (u, v, valid) with valid = 1 or 0.
std::array<float, 3> shader_inverse_bilinear(const BilinearUniforms &uni, float px, float py);
std::array<float, 3> shader_inverse_projective(const ProjectiveUniforms &uni, float px, float py);

} // namespace quad
} // namespace hdrtk
