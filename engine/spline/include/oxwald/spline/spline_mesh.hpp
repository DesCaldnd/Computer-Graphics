#pragma once

#include <oxwald/core/types.hpp>
#include <oxwald/spline/spline.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <vector>

namespace ox::spline {

// 2D cross-section swept along a spline. x maps to the frame binormal ("right"), y to the normal ("up").
// Face normals point to the right of the walking direction, i.e. wind closed profiles counter-clockwise
// for outward-facing surfaces; an open strip walked right → left faces up (see makeStripProfile).
// Duplicate a point to get a hard edge (zero-length edges are ignored for normal smoothing).
struct ExtrusionProfile {
    std::vector<glm::vec2> points;
    // Optional texture u per point (closed: may have one extra entry for the seam). Empty → normalised
    // cumulative profile length.
    std::vector<f32> u;
    bool closed = false;
};

struct ExtrusionSettings {
    f32 spacing = 1.0f;   // ring spacing along the arc length (adjusted to fit the length exactly)
    f32 vPerUnit = 1.0f;  // texture v = distance * vPerUnit
    bool capStart = false; // closed profiles on open splines only; fan from the centroid (convex profiles)
    bool capEnd = false;
};

struct SplineVertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f};
    glm::vec2 uv{0.0f};
};

struct ExtrudedMesh {
    std::vector<SplineVertex> vertices;
    std::vector<u32> indices; // triangle list, counter-clockwise front faces
};

// rings = max(1, round(length/spacing)) + 1, profile columns = points (+1 when closed for the UV seam).
// vertices = rings * columns (+ (points + 1) per cap), indices = (rings-1) * (columns-1) * 6 (+ 3 * points per cap).
// Closed splines duplicate the first ring at the end (distance = length) for the v seam.
ExtrudedMesh extrude(const Spline& spline, const ExtrusionProfile& profile, const ExtrusionSettings& settings = {});

ExtrusionProfile makeCircleProfile(f32 radius, u32 segments);
ExtrusionProfile makeRectangleProfile(f32 width, f32 height); // hard edges, centred on the curve
ExtrusionProfile makeStripProfile(f32 width);                 // flat, facing up (roads)

} // namespace ox::spline
