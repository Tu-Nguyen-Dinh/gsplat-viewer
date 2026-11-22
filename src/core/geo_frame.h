#pragma once
// Double-precision similarity transforms (Sim(3)) and the "local origin" trick for rendering
// real-world coordinates with float GPUs.
//
// Why: survey data (LAS, UTM, VN-2000) has coordinates around 10^5..10^7 m. A float has a 24-bit
// mantissa, so at 1.2e6 m its spacing is 0.125 m: points visibly snap and jitter. Every layer
// therefore keeps a GeoFrame (layer -> world, in double), and rendering subtracts a common origin
// in double before converting to float (relative-to-origin rendering). No OpenGL here.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace gsv {

// p_world = scale * rotation * p_local + translation
struct GeoFrame {
  double scale = 1.0;
  glm::dmat3 rotation{1.0};     // orthonormal, det = +1
  glm::dvec3 translation{0.0};

  static GeoFrame identity() { return {}; }
  static GeoFrame fromTranslation(const glm::dvec3& t) {
    GeoFrame f;
    f.translation = t;
    return f;
  }
  static GeoFrame fromQuaternion(const glm::dquat& q, const glm::dvec3& t, double s = 1.0) {
    GeoFrame f;
    f.scale = s;
    f.rotation = glm::mat3_cast(glm::normalize(q));
    f.translation = t;
    return f;
  }

  glm::dvec3 apply(const glm::dvec3& p) const { return scale * (rotation * p) + translation; }
  glm::dvec3 applyVector(const glm::dvec3& v) const { return scale * (rotation * v); }

  // (a * b)(p) = a(b(p))
  GeoFrame operator*(const GeoFrame& b) const {
    GeoFrame r;
    r.scale = scale * b.scale;
    r.rotation = rotation * b.rotation;
    r.translation = apply(b.translation);
    return r;
  }
  GeoFrame inverse() const;

  glm::dmat4 matrix() const;  // homogeneous 4x4 (column-major, like glm)
  // Float matrix mapping layer-local coordinates to world coordinates *minus* `origin`.
  // The subtraction happens in double, so the float result only holds small numbers.
  glm::mat4 localMatrix(const glm::dvec3& origin) const;

  // Re-orthonormalizes the rotation (Gram-Schmidt on columns). Use after accumulating many
  // compositions (e.g. ICP iterations).
  void orthonormalize();
};

// World point -> float offset from `origin` (exact up to float rounding of the *offset*).
inline glm::vec3 toLocal(const glm::dvec3& world, const glm::dvec3& origin) { return glm::vec3(world - origin); }
inline glm::dvec3 fromLocal(const glm::vec3& local, const glm::dvec3& origin) { return glm::dvec3(local) + origin; }

// Angle of the relative rotation between two frames, in radians, and distance of translations.
double rotationAngleBetween(const glm::dmat3& a, const glm::dmat3& b);

}  // namespace gsv
