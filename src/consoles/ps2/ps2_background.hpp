// SPDX-License-Identifier: MIT
// Original procedural scene. Visual reference only: Sony's PS2 startup image at
// https://www.playstation.com/en-us/playstation-history/2000-ps2-psp/
// No firmware images, logos, scene data or renderer implementation are used.
#pragma once

#include "imgui.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace ps2_background_detail {
namespace {

inline float noise(unsigned int seed) {
    seed ^= seed >> 16;
    seed *= 0x7feb352du;
    seed ^= seed >> 15;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16;
    return static_cast<float>(seed & 0xffffu) / 65535.0f;
}

struct Camera {
    ImVec2 origin, size;
    float sine, cosine, distance, elevation_sine, elevation_cosine, focal;

    float depth(float x, float y, float z) const {
        return distance - elevation_cosine * (sine * x + cosine * z)
                        - elevation_sine * y;
    }

    ImVec2 project(float x, float y, float z) const {
        const float forward = sine * x + cosine * z;
        const float reciprocal = focal / std::max(2.0f, depth(x, y, z));
        return ImVec2(origin.x + size.x * 0.50f
                                  + (cosine * x - sine * z) * reciprocal,
                      origin.y + size.y * 0.52f
                                  - (elevation_cosine * y - elevation_sine * forward)
                                        * reciprocal);
    }
};

struct Column {
    float x, z, width, length, base, height, depth;
    unsigned int seed;
};

inline void quad(ImDrawList* draw, const std::array<ImVec2, 8>& points,
                 int a, int b, int c, int d, ImU32 color) {
    const float winding = (points[b].x - points[a].x) * (points[c].y - points[a].y)
                        - (points[b].y - points[a].y) * (points[c].x - points[a].x);
    // ImGui's anti-aliased convex fills require clockwise screen-space winding.
    if (winding < 0.0f)
        draw->AddQuadFilled(points[a], points[d], points[c], points[b], color);
    else
        draw->AddQuadFilled(points[a], points[b], points[c], points[d], color);
}

inline void floor_fog(ImDrawList* draw, const Camera& camera) {
    constexpr float tau = 6.28318530718f;
    // These are projected patches on the ground plane, not a horizon or a
    // screen-space blue band. Very low overlapping alpha softens their edges.
    std::array<ImVec2, 24> ring{};
    for (int layer = 0; layer < 7; ++layer) {
        const float radius = 10.8f - static_cast<float>(layer) * 1.02f;
        for (unsigned int i = 0; i < ring.size(); ++i) {
            const float angle = tau * static_cast<float>(i) / static_cast<float>(ring.size());
            ring[i] = camera.project(-0.7f + std::cos(angle) * radius,
                                     -0.04f, -1.0f + std::sin(angle) * radius * 0.76f);
        }
        const float winding = (ring[1].x - ring[0].x) * (ring[2].y - ring[0].y)
                            - (ring[1].y - ring[0].y) * (ring[2].x - ring[0].x);
        if (winding < 0.0f) std::reverse(ring.begin(), ring.end());
        draw->AddConvexPolyFilled(ring.data(), static_cast<int>(ring.size()),
                                 IM_COL32(20, 43, 100, 14 + layer));
    }
}

} // namespace
} // namespace ps2_background_detail

// Fixed work: 36 grounded columns, 4 floating cubes, 7 fog patches and 12 lights,
// at most 312 drawing
// primitives. All scene/sort/projection storage is on the stack. DrawList's
// normal reusable vertex/index buffers are the only rendering storage.
static inline void draw_ps2_startup_background(ImDrawList* draw, ImVec2 origin,
                                        ImVec2 size, float time) {
    using namespace ps2_background_detail;
    if (!draw || size.x <= 0.0f || size.y <= 0.0f) return;
    if (!std::isfinite(time)) time = 0.0f;

    constexpr float tau = 6.28318530718f;
    // A steep overhead camera makes the square caps lead the scene, as in the
    // startup field, rather than presenting a side-on skyline of tall walls.
    constexpr float radius = 8.5f, camera_height = 27.0f;
    const float angle = 0.53f + std::fmod(time * 0.012f, tau);
    const float distance = std::sqrt(radius * radius + camera_height * camera_height);
    const Camera camera{origin, size, std::sin(angle), std::cos(angle), distance,
                        camera_height / distance, radius / distance,
                        std::min(size.x * 1.10f, size.y * 1.45f)};
    const float camera_x = camera.sine * radius;
    const float camera_z = camera.cosine * radius;

    draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                        IM_COL32(0, 1, 5, 255));
    floor_fog(draw, camera);

    std::array<Column, 40> columns{};
    for (unsigned int i = 0; i < columns.size(); ++i) {
        const unsigned int seed = i * 31u + 117u;
        auto& column = columns[i];
        // Jittered strata leave open space between columns; each base is an
        // independent point in X/Z, with no common screen-space baseline.
        column.x = (static_cast<float>(i % 6) - 2.5f) * 2.75f
                 + (noise(seed) - 0.5f) * 1.65f;
        column.z = (static_cast<float>(i / 6) - 2.5f) * 2.60f
                 + (noise(seed + 1u) - 0.5f) * 1.65f;
        column.width = 0.46f + noise(seed + 2u) * 0.39f;
        column.length = 0.48f + noise(seed + 3u) * 0.42f;
        const float height = noise(seed + 4u);
        column.base = 0.0f;
        column.height = 1.35f + height * height * 4.1f;
        if (i >= 36) {
            constexpr float cube_x[4] = {-5.8f, -0.8f, 4.9f, 2.8f};
            constexpr float cube_z[4] = {-3.8f, 5.2f, -0.6f, -5.4f};
            column.x = cube_x[i - 36];
            column.z = cube_z[i - 36];
            column.width = column.length = column.height = 0.62f + height * 0.28f;
            column.base = 2.6f + noise(seed + 5u) * 1.2f;
        }
        column.depth = camera.depth(column.x, column.base + column.height * 0.5f, column.z);
        column.seed = seed;
    }
    std::sort(columns.begin(), columns.end(), [](const Column& a, const Column& b) {
        return a.depth > b.depth || (a.depth == b.depth && a.seed < b.seed);
    });

    for (const auto& column : columns) {
        std::array<ImVec2, 8> points{};
        const float x0 = column.x - column.width * 0.5f;
        const float x1 = column.x + column.width * 0.5f;
        const float z0 = column.z - column.length * 0.5f;
        const float z1 = column.z + column.length * 0.5f;
        points[0] = camera.project(x0, column.base, z0);
        points[1] = camera.project(x1, column.base, z0);
        points[2] = camera.project(x1, column.base, z1);
        points[3] = camera.project(x0, column.base, z1);
        points[4] = camera.project(x0, column.base + column.height, z0);
        points[5] = camera.project(x1, column.base + column.height, z0);
        points[6] = camera.project(x1, column.base + column.height, z1);
        points[7] = camera.project(x0, column.base + column.height, z1);

        const float fade = std::clamp((39.0f - column.depth) / 27.0f, 0.18f, 0.93f);
        const int alpha = static_cast<int>(38.0f + fade * 112.0f);
        const ImU32 back = IM_COL32(100, 134, 173, alpha / 5);
        quad(draw, points, 0, 1, 2, 3, IM_COL32(24, 49, 92, 18));

        // Faint far walls show through the brighter near walls. Faces and the
        // cap share the same perspective projection, so their edges converge.
        const bool positive_x = camera_x > column.x;
        const bool positive_z = camera_z > column.z;
        if (positive_x) quad(draw, points, 0, 3, 7, 4, back);
        else            quad(draw, points, 1, 2, 6, 5, back);
        if (positive_z) quad(draw, points, 0, 1, 5, 4, back);
        else            quad(draw, points, 3, 2, 6, 7, back);
        if (positive_x) quad(draw, points, 1, 2, 6, 5, IM_COL32(112, 144, 179, alpha));
        else            quad(draw, points, 0, 3, 7, 4, IM_COL32(112, 144, 179, alpha));
        if (positive_z) quad(draw, points, 3, 2, 6, 7, IM_COL32(148, 173, 196, alpha));
        else            quad(draw, points, 0, 1, 5, 4, IM_COL32(148, 173, 196, alpha));
        quad(draw, points, 4, 5, 6, 7, IM_COL32(188, 207, 220, alpha + 12));
        const int edge = positive_x ? (positive_z ? 2 : 1) : (positive_z ? 3 : 0);
        draw->AddLine(points[edge], points[edge + 4],
                      IM_COL32(178, 205, 233, static_cast<int>(52.0f * fade)), 1.0f);
    }

    for (unsigned int i = 0; i < 12; ++i) {
        const unsigned int seed = i * 47u + 809u;
        const float phase = noise(seed + 2u) * tau;
        const float x = (noise(seed) - 0.5f) * 19.0f
                      + std::sin(time * 0.055f + phase) * 0.25f;
        const float z = (noise(seed + 1u) - 0.5f) * 17.0f;
        const float y = 0.6f + noise(seed + 3u) * 2.4f
                      + std::sin(time * 0.08f + phase) * 0.22f;
        const ImVec2 point = camera.project(x, y, z);
        const float glow = 0.55f + std::sin(time * 0.23f + phase) * 0.25f;
        const float light_radius = std::clamp(camera.focal / camera.depth(x, y, z)
                                                * 0.018f, 0.55f, 1.65f);
        draw->AddCircleFilled(point, light_radius * 3.2f,
                              IM_COL32(45, 94, 176, 7), 8);
        draw->AddCircleFilled(point, light_radius,
                              IM_COL32(139, 184, 235, static_cast<int>(60.0f * glow)), 8);
    }
    draw->PopClipRect();
}
