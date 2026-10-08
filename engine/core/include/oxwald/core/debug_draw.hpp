#pragma once

// Immediate-mode debug geometry collector (lines + 3D text). Any system on any thread submits
// shapes; the renderer owns the frame cadence:
//
//   debugDraw.flush(dt);                         // once per frame, before rendering
//   upload(debugDraw.depthTestedLines());        // line list: vertex pairs
//   upload(debugDraw.overlayLines());            // drawn without depth test
//   for (auto& t : debugDraw.texts()) ...        // projected/rendered by the UI
//
// Lifetime: an item submitted with duration d appears in the next flush and in every following
// flush until d seconds (sum of the dt passed to later flushes) have elapsed. duration 0 means
// exactly one flush. Submission is thread-safe; flush/clear and the span accessors must not race
// with each other (call them from the render/main thread). Spans stay valid until the next flush/clear.

#include <oxwald/core/math.hpp>
#include <oxwald/core/types.hpp>

#include <atomic>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace ox {

struct DebugVertex {
    glm::vec3 position{0.0f};
    u32 color = 0xffffffffu; // RGBA8 packed, r in the low byte (VK_FORMAT_R8G8B8A8_UNORM)
};
static_assert(sizeof(DebugVertex) == 16);

namespace debug_color {

[[nodiscard]] constexpr u32 rgba(u8 r, u8 g, u8 b, u8 a = 255) {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) | (static_cast<u32>(a) << 24);
}
// Components are clamped to [0,1].
[[nodiscard]] u32 pack(const glm::vec4& color);
[[nodiscard]] glm::vec4 unpack(u32 color);

inline constexpr u32 kWhite = rgba(255, 255, 255);
inline constexpr u32 kBlack = rgba(0, 0, 0);
inline constexpr u32 kGray = rgba(128, 128, 128);
inline constexpr u32 kRed = rgba(255, 0, 0);
inline constexpr u32 kGreen = rgba(0, 255, 0);
inline constexpr u32 kBlue = rgba(0, 0, 255);
inline constexpr u32 kYellow = rgba(255, 255, 0);
inline constexpr u32 kCyan = rgba(0, 255, 255);
inline constexpr u32 kMagenta = rgba(255, 0, 255);
inline constexpr u32 kOrange = rgba(255, 165, 0);

} // namespace debug_color

// Accepts both packed constants (debug_color::kRed) and float colors (glm::vec4{1,0,0,1}).
struct DebugColor {
    u32 packed = debug_color::kWhite;
    constexpr DebugColor() = default;
    constexpr DebugColor(u32 rgba8) : packed(rgba8) {} // NOLINT: implicit by design
    DebugColor(const glm::vec4& c) : packed(debug_color::pack(c)) {} // NOLINT
    DebugColor(const glm::vec3& c) : packed(debug_color::pack(glm::vec4{c, 1.0f})) {} // NOLINT
};

struct DebugText {
    glm::vec3 position{0.0f};
    std::string text;
    u32 color = debug_color::kWhite;
    bool depthTest = true;
};

class DebugDraw {
public:
    DebugDraw() = default;
    DebugDraw(const DebugDraw&) = delete;
    DebugDraw& operator=(const DebugDraw&) = delete;

    void line(glm::vec3 a, glm::vec3 b, DebugColor color = {}, f32 duration = 0.0f, bool depthTest = true);
    // From origin along normalize(direction) * length.
    void ray(glm::vec3 origin, glm::vec3 direction, f32 length, DebugColor color = {}, f32 duration = 0.0f,
             bool depthTest = true);
    void aabb(const AABB& box, DebugColor color = {}, f32 duration = 0.0f, bool depthTest = true);
    void box(glm::vec3 center, glm::vec3 halfExtents, const glm::quat& rotation, DebugColor color = {},
             f32 duration = 0.0f, bool depthTest = true);
    void obb(const OBB& box, DebugColor color = {}, f32 duration = 0.0f, bool depthTest = true);
    // Three great circles.
    void sphere(glm::vec3 center, f32 radius, DebugColor color = {}, f32 duration = 0.0f, bool depthTest = true,
                u32 segments = 24);
    void circle(glm::vec3 center, glm::vec3 normal, f32 radius, DebugColor color = {}, f32 duration = 0.0f,
                bool depthTest = true, u32 segments = 32);
    void capsule(glm::vec3 p0, glm::vec3 p1, f32 radius, DebugColor color = {}, f32 duration = 0.0f,
                 bool depthTest = true, u32 segments = 24);
    // halfAngle in radians.
    void cone(glm::vec3 apex, glm::vec3 direction, f32 length, f32 halfAngle, DebugColor color = {},
              f32 duration = 0.0f, bool depthTest = true, u32 segments = 24);
    void cylinder(glm::vec3 p0, glm::vec3 p1, f32 radius, DebugColor color = {}, f32 duration = 0.0f,
                  bool depthTest = true, u32 segments = 24);
    // 12 edges of the frustum of a [0,1]-depth view-projection inverse (finite far plane only).
    void frustum(const glm::mat4& invViewProj, bool reversedZ = true, DebugColor color = {}, f32 duration = 0.0f,
                 bool depthTest = true);
    void arrow(glm::vec3 from, glm::vec3 to, f32 headSize, DebugColor color = {}, f32 duration = 0.0f,
               bool depthTest = true);
    // X red, Y green, Z blue.
    void axes(const Transform& transform, f32 size = 1.0f, f32 duration = 0.0f, bool depthTest = true);
    void axes(const glm::mat4& transform, f32 size = 1.0f, f32 duration = 0.0f, bool depthTest = true);
    // Grid in the XZ plane (Y up) with `cells` cells per side, centered on center.
    void grid(glm::vec3 center, f32 cellSize, u32 cells, DebugColor color = debug_color::kGray, f32 duration = 0.0f,
              bool depthTest = true);
    // Three axis-aligned lines crossing at p; size is the full line length.
    void point(glm::vec3 p, f32 size = 0.1f, DebugColor color = {}, f32 duration = 0.0f, bool depthTest = true);
    void text3D(glm::vec3 position, std::string text, DebugColor color = {}, f32 duration = 0.0f,
                bool depthTest = true);

    // Advances lifetimes by dt and rebuilds the output arrays (see header comment).
    void flush(f32 dt);
    [[nodiscard]] std::span<const DebugVertex> depthTestedLines() const { return m_outDepth; }
    [[nodiscard]] std::span<const DebugVertex> overlayLines() const { return m_outOverlay; }
    [[nodiscard]] std::span<const DebugText> texts() const { return m_outTexts; }

    // Drops all pending, live and output items.
    void clear();
    // While disabled, submissions are ignored (existing items still expire normally).
    void setEnabled(bool enabled) { m_enabled.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }

private:
    struct LineItem {
        DebugVertex a;
        DebugVertex b;
        f32 remaining;
        bool depthTest;
    };
    struct TextItem {
        DebugText text;
        f32 remaining;
    };

    void submit(std::span<const glm::vec3> points, u32 color, f32 duration, bool depthTest);
    void appendCircle(std::vector<glm::vec3>& out, glm::vec3 center, glm::vec3 axisU, glm::vec3 axisV, f32 radius,
                      u32 segments, f32 startAngle = 0.0f, f32 sweep = kTwoPi) const;

    std::atomic<bool> m_enabled{true};
    std::mutex m_mutex;
    std::vector<LineItem> m_pendingLines; // submitted since the last flush (guarded by m_mutex)
    std::vector<TextItem> m_pendingTexts;
    std::vector<LineItem> m_lines; // live items (flush thread only)
    std::vector<TextItem> m_texts;
    std::vector<DebugVertex> m_outDepth;
    std::vector<DebugVertex> m_outOverlay;
    std::vector<DebugText> m_outTexts;
};

} // namespace ox
