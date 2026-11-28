#pragma once

#include "planet_sandbox/project.h"
#include "planet_sandbox/simulation.h"

#include <cstdint>
#include <array>
#include <optional>
#include <ostream>
#include <vector>

namespace planet_sandbox {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
};

enum class SamplingMode { Single, SSAA4, MSAA4, SSAA16 };
enum class TraceSurface { Background, Body, Ring, Cloud, Trail, ReferenceTrail, Atmosphere, Impact };

struct TraceSample {
    Vec2 position;
    int body_index = -1;
    double depth = 0.0;
    std::array<double, 3> linear_rgb{};
    TraceSurface surface = TraceSurface::Background;
};

enum class TracePrimitive { Triangle, TrailSegment, Atmosphere, Impact };
enum class TraceDepthTest { NotTested, Passed, Rejected };
enum class TraceAction { Uncovered, DepthRejected, BackfaceRejected, Transparent, Written, Occluded, InvalidAttributes };
enum class TraceBlend { Replace, Alpha, CloudBase };
enum class TraceShadingLocation { Sample, PixelCenter, CoverageCentroid, CoveredSample };
enum class TracePointIssue { None, OutsideCoverage, InvalidInterpolation, BackFacing, NoValidCoveredPoint };

struct TraceShading {
    Vec2 position; // Pixel-local coordinates, as for TraceSample::position.
    std::array<double, 3> linear_rgb{};
    double alpha = 1.0;
    TraceShadingLocation location = TraceShadingLocation::Sample;
    TracePointIssue center_issue = TracePointIssue::None;
    TracePointIssue centroid_issue = TracePointIssue::None;
};

struct TraceSampleStep {
    bool covered = false;
    TraceDepthTest depth_test = TraceDepthTest::NotTested;
    std::optional<double> candidate_depth;
    TraceAction action = TraceAction::Uncovered;
    int shading_index = -1; // Index in this event; -1 for constant-color trails/no shading.
    TraceBlend blend = TraceBlend::Replace;
    std::array<double, 3> source_rgb{}, blend_base_rgb{};
    double alpha = 0.0;
    // Atmosphere clips a ray interval against scene depth; it does not write depth.
    std::optional<double> ray_entry, ray_exit;
    TraceSample before, after;
};

struct TraceEvent {
    TracePrimitive primitive = TracePrimitive::Triangle;
    std::uint64_t primitive_id = 0; // Frame-local source triangle/segment/pass identifier.
    int clipped_part = 0;
    int body_index = -1;
    TraceSurface surface = TraceSurface::Background;
    std::vector<TraceSampleStep> samples;
    std::vector<TraceShading> shading;
    TracePointIssue shading_failure = TracePointIssue::None; // No actual shader invocation.
};

struct PixelTrace {
    int x = -1;
    int y = -1;
    std::vector<TraceSample> samples;
    std::array<std::uint8_t, 3> display_rgb{};
    SamplingMode sampling = SamplingMode::Single;
    bool process_recorded = false;
    bool process_truncated = false;
    static constexpr std::size_t max_events = 4096;
    std::vector<TraceSample> initial_samples;
    std::vector<TraceEvent> events;
    std::array<double, 3> resolved_linear_rgb{};
};

struct RenderCamera {
    double yaw = 0.0;
    double pitch = 0.0;
    double zoom = 1.0;
    WorldVector target;
};

struct RenderConfig {
    int width = 640;
    int height = 480;
    RenderCamera camera;
    SamplingMode sampling = SamplingMode::Single;
    int trace_x = -1;
    int trace_y = -1;
    bool trace_process = true; // Only applies to a selected pixel; disable for timing.
    bool impact_effects = true;
    bool texture_filtering = true;
    bool adaptive_geometry = true;
    bool preview_light = false;
    double light_azimuth = -.66,light_elevation = .67;
};

struct RenderFrame {
    Image image;
    std::vector<int> body_index;
    std::vector<Vec2> surface_uv;
    std::vector<std::uint8_t> sample_surface_variation;
    std::optional<PixelTrace> trace;
    std::uint64_t shade_evaluations = 0;
    std::array<std::uint64_t, 2> panel_shade_evaluations{};
    // CPU render time excludes final trace extraction/upload/UI/stitching. Detailed
    // in-render recording is included when enabled; timed measurements disable it.
    double render_ms = 0.0;
    std::array<double, 2> panel_render_ms{};
    int panel_count = 1;
    std::array<SamplingMode, 2> panel_sampling{SamplingMode::Single, SamplingMode::Single};
};

RenderFrame render_scene(const Project& project, const SceneSnapshot& snapshot,
                         const RenderConfig& config);
RenderFrame render_comparison_scene(const Project& project, const SceneSnapshot& snapshot,
                                    const RenderConfig& config, SamplingMode left,
                                    SamplingMode right, bool right_first = false,const Project* right_project=nullptr);

Image render_preview(const Project& project, const SceneSnapshot& snapshot, int width, int height);
void write_ppm(std::ostream& output, const Image& image);

} // namespace planet_sandbox
