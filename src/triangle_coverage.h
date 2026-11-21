#pragma once

#include "planet_sandbox/renderer.h"
#include <array>
#include <cstdint>
#include <memory>

namespace planet_sandbox::raster_detail {

// 16 fractional bits reduce skinny-triangle interpolation perturbations while
// retaining the exact same half-open ownership rule in both arithmetic paths.
constexpr int subpixel_bits = 16;
static_assert(subpixel_bits >= 3 && subpixel_bits <= 20);
constexpr std::int64_t subpixel_scale = std::int64_t{1} << subpixel_bits;
constexpr double subpixel_step = 1.0 / subpixel_scale;
// Above this magnitude, binary64 spacing is already at least one grid step.
constexpr double grid_aligned_min = double(std::uint64_t{1} << (52 - subpixel_bits));
constexpr std::int64_t fast_subpixel_limit = std::int64_t{1} << 29;

inline Vec2 sample_offset(int count, int sample) {
    if (count == 1) return {.5, .5};
    const int side = count == 16 ? 4 : 2;
    return {(sample % side + .5) / side, (sample / side + .5) / side};
}

// Screen x right, y down; E(a,b,p) = (p-a).x*(b-a).y - (p-a).y*(b-a).x.
// Round vertices to subpixel_step (ties away from zero). Normalize edge signs,
// not vertex attributes. Positive-area top/left edges have dy>0 or dy==0, dx<0.
// Interpolation uses the SAME snapped geometry, with unbiased edge values.
class TriangleCoverage {
public:
    explicit TriangleCoverage(const std::array<Vec2, 3>& vertices);
    ~TriangleCoverage();
    bool valid() const { return valid_; }
    bool uses_wide_arithmetic() const { return bool(wide_); }
    Vec2 minimum() const { return minimum_; }
    Vec2 maximum() const { return maximum_; }
    // Sample coordinates are exact integers in units of subpixel_step.
    bool weights(std::int64_t x, std::int64_t y, std::array<double, 3>& w) const {
        if (!valid_) return false;
        if (wide_) return wide_weights(x,y,w);
        // Also bound arbitrary caller-supplied samples before multiplication.
        // A fast triangle cannot contain a sample beyond this guard range.
        if (x < -fast_subpixel_limit || x > fast_subpixel_limit ||
            y < -fast_subpixel_limit || y > fast_subpixel_limit) return false;
        std::array<std::int64_t,3> values;
        for (int i=0;i<3;++i) {
            const auto& e=edges_[i];
            values[i]=x*e.x+y*e.y+e.constant;
            if (values[i] < e.threshold) return false;
        }
        for (int i=0;i<3;++i) w[i]=double(values[i])*inverse_area_;
        return true;
    }

private:
    struct WideTriangle;
    std::unique_ptr<WideTriangle> wide_;
    struct Edge { std::int64_t x, y, constant, threshold; };
    std::array<Edge,3> edges_{};
    std::array<bool, 3> inclusive_{};
    double inverse_area_ = 0;
    int orientation_ = 1;
    bool valid_ = false;
    Vec2 minimum_{}, maximum_{};
    bool wide_weights(std::int64_t x, std::int64_t y, std::array<double,3>& w) const;
};

} // namespace planet_sandbox::raster_detail
