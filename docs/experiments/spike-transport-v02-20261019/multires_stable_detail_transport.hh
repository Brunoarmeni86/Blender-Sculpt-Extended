/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cmath>

/* Matrices use Blender's column-major indexing. The experimental transport
 * carries the complete object-space residual by a rotation only: changes in
 * tangent skew cannot amplify it. Degenerate frames use the object axes as a
 * deterministic reference; residual amplitude is retained, orientation at a
 * collapsing frame remains an experimental policy requiring visual review.
 * This deliberately differs from affine native transport under shear. */
namespace blender::multires_stable_detail {
inline double dot(const double a[3], const double b[3])
{
  return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
inline bool normalize(double a[3])
{
  const double length = std::hypot(a[0], a[1], a[2]);
  if (!std::isfinite(length) || length == 0.0) {
    return false;
  }
  for (int i = 0; i < 3; i++) {
    a[i] /= length;
  }
  return true;
}
template<class Matrix> bool frame(const Matrix &matrix, double q[3][3])
{
  for (int c = 0; c < 3; c++) {
    for (int r = 0; r < 3; r++) {
      q[c][r] = matrix[c][r];
      if (!std::isfinite(q[c][r])) {
        return false;
      }
    }
    if (!normalize(q[c])) {
      return false;
    }
  }
  const double projection = dot(q[0], q[1]);
  for (int i = 0; i < 3; i++) {
    q[1][i] -= projection * q[0][i];
  }
  if (std::hypot(q[1][0], q[1][1], q[1][2]) <= 1.0e-6 || !normalize(q[1])) {
    return false;
  }
  double normal[3] = {q[0][1]*q[1][2]-q[0][2]*q[1][1],
                      q[0][2]*q[1][0]-q[0][0]*q[1][2],
                      q[0][0]*q[1][1]-q[0][1]*q[1][0]};
  const double orientation = dot(normal, q[2]);
  if (std::abs(orientation) <= 1.0e-6) {
    return false;
  }
  if (orientation < 0.0) {
    for (int i = 0; i < 3; i++) {
      q[1][i] = -q[1][i];
      normal[i] = -normal[i];
    }
  }
  for (int i = 0; i < 3; i++) {
    q[2][i] = normal[i];
  }
  return true;
}
template<class Matrix, class Vector>
Vector encode(const Matrix &old_frame, const Vector &detail)
{
  double old_q[3][3];
  if (!frame(old_frame, old_q)) {
    return detail;
  }
  Vector components = detail;
  for (int c = 0; c < 3; c++) {
    double value = 0;
    for (int r = 0; r < 3; r++) {
      value += old_q[c][r] * double(detail[r]);
    }
    components[c] = float(value);
  }
  return components;
}
template<class Matrix, class Vector>
Vector decode(const Matrix &new_frame, const Vector &detail)
{
  double new_q[3][3];
  if (!frame(new_frame, new_q)) {
    return detail;
  }
  Vector result = detail;
  for (int r = 0; r < 3; r++) {
    double value = 0;
    for (int c = 0; c < 3; c++) {
      value += new_q[c][r] * double(detail[c]);
    }
    result[r] = float(value);
    if (!std::isfinite(result[r])) {
      return detail;
    }
  }
  return result;
}
template<class Matrix, class Vector>
Vector transport(const Matrix &old_frame, const Matrix &new_frame, const Vector &detail)
{
  return decode(new_frame, encode(old_frame, detail));
}
} // namespace blender::multires_stable_detail
