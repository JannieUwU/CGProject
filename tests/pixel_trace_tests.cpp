#include "pixel_trace_checks.h"
#include "planet_sandbox/solar_system.h"
#include <iostream>

#ifdef NDEBUG
#error Pixel trace tests require assertions
#endif
using namespace planet_sandbox;

static Project simple_project() {
    Project p;
    p.view_extent = 2.0;
    p.display_scale = 1.0;
    p.bodies = {{"front", "Front", 1.0, .4, {1,0}, {}, {1,"#ffbb62","#d65032",4,.3,0,0}},
                {"back", "Back", 1.0, 1.0, {-1,0}, {}, {2,"#3cbdc2","#1d5c78",8,.7,0,0}}};
    return p;
}

static void equivalent(const RenderFrame& a, const RenderFrame& b) {
    assert(a.image.rgb == b.image.rgb && a.body_index == b.body_index);
    assert(a.sample_surface_variation == b.sample_surface_variation);
    assert(a.shade_evaluations == b.shade_evaluations);
    assert(a.surface_uv.size() == b.surface_uv.size());
    for (std::size_t i = 0; i < a.surface_uv.size(); ++i)
        assert(a.surface_uv[i].x == b.surface_uv[i].x && a.surface_uv[i].y == b.surface_uv[i].y);
}

static void test_modes_and_comparison() {
    const Project p = simple_project();
    SceneSnapshot scene;
    scene.bodies = {{"front", {1,0}, {}, 1,.4}, {"back", {-1,0}, {}, 1,1}};
    RenderConfig c;
    c.width = 96; c.height = 72; c.camera.yaw = 1.5707963267948966;
    bool rejected = false, shared = false, partial = false;
    for (auto mode : {SamplingMode::Single, SamplingMode::SSAA4, SamplingMode::MSAA4, SamplingMode::SSAA16}) {
        c.sampling = mode; c.trace_x = c.trace_y = -1;
        const auto baseline = render_scene(p, scene, c);
        for (const auto xy : {Vec2{48,36}, Vec2{52,36}, Vec2{53,37}, Vec2{50,34}, Vec2{0,0}}) {
            c.trace_x = int(xy.x); c.trace_y = int(xy.y); c.trace_process = true;
            const auto traced = render_scene(p, scene, c);
            equivalent(baseline, traced);
            check_pixel_process(*traced.trace);
            if (xy.x == 0) assert(traced.trace->events.empty());
            for (const auto& e : traced.trace->events) {
                int writes = 0, covered = 0;
                for (const auto& s : e.samples) {
                    rejected |= s.action == TraceAction::DepthRejected;
                    writes += s.action == TraceAction::Written;
                    covered += s.covered;
                }
                partial |= covered > 0 && covered < int(e.samples.size());
                shared |= mode == SamplingMode::MSAA4 && writes > 1 && e.shading.size() == 1;
            }
            c.trace_process = false;
            const auto final_only = render_scene(p, scene, c);
            equivalent(traced, final_only);
            assert(final_only.trace && !final_only.trace->process_recorded);
            assert(final_only.trace->events.empty() && final_only.trace->initial_samples.empty());
        }
    }
    assert(rejected && shared && partial);
    c.trace_process = true; c.trace_x = 48; c.trace_y = 36;
    auto left = render_comparison_scene(p, scene, c, SamplingMode::SSAA4, SamplingMode::MSAA4);
    check_pixel_process(*left.trace);
    assert(left.trace->sampling == SamplingMode::SSAA4 && left.trace->x == 48);
    c.trace_x += c.width;
    auto right = render_comparison_scene(p, scene, c, SamplingMode::SSAA4, SamplingMode::MSAA4, true);
    equivalent(left, right);
    check_pixel_process(*right.trace);
    assert(right.trace->sampling == SamplingMode::MSAA4 && right.trace->x == 144);
}

static void test_layers() {
    Project p = make_solar_system_project();
    RenderConfig c;
    c.width = 64; c.height = 48; c.sampling = SamplingMode::MSAA4;
    bool cloud = false, ring = false, atmosphere = false;
    for (int body : {2,3,6,7}) {
        p.view_extent = p.bodies[body].display_radius * 1.1;
        p.bodies[body].design.cloud_opacity = .7;
        p.bodies[body].design.atmosphere_density = 1.0;
        SceneSnapshot scene;
        scene.bodies = {{p.bodies[body].id, {0,0}, {}, p.bodies[body].mass, p.bodies[body].radius}};
        c.trace_x = c.trace_y = -1;
        const auto baseline = render_scene(p, scene, c);
        for (int y : {16,20,24,28,32}) for (int x : {24,28,32,36,40}) {
            c.trace_x = x; c.trace_y = y;
            const auto f = render_scene(p, scene, c);
            equivalent(baseline, f);
            check_pixel_process(*f.trace);
            for (const auto& e : f.trace->events) for (const auto& s : e.samples) {
                if (s.action != TraceAction::Written) continue;
                cloud |= e.surface == TraceSurface::Cloud && s.blend == TraceBlend::CloudBase;
                ring |= e.surface == TraceSurface::Ring && s.blend == TraceBlend::Alpha;
                atmosphere |= e.primitive == TracePrimitive::Atmosphere;
            }
        }
        // Rings can lie between the coarse grid points: select an actually
        // visible ring sample using the renderer's picking output.
        if (body == 6 || body == 7) {
            std::size_t index = 0;
            while (index < baseline.body_index.size() &&
                   !(baseline.body_index[index] == body && baseline.surface_uv[index].x < 0.0)) ++index;
            assert(index < baseline.body_index.size());
            c.trace_x = int(index % c.width); c.trace_y = int(index / c.width);
            const auto traced = render_scene(p, scene, c);
            equivalent(baseline, traced);
            check_pixel_process(*traced.trace);
            for (const auto& event : traced.trace->events) for (const auto& step : event.samples)
                ring |= event.surface == TraceSurface::Ring && step.action == TraceAction::Written &&
                        step.blend == TraceBlend::Alpha;
        }
    }
    if (!(cloud && ring && atmosphere))
        std::cerr << "Layer coverage: cloud=" << cloud << " ring=" << ring << " atmosphere=" << atmosphere << '\n';
    assert(cloud && ring && atmosphere);
}

static void test_trails_and_limit() {
    const auto p = simple_project();
    SceneSnapshot scene;
    scene.trails = {{"front", {{-1,0},{1,0}}}};
    scene.reference_trails = {{"back", {{0,-1},{0,1}}}};
    RenderConfig c;
    c.width = 16; c.height = 16; c.trace_x = 8; c.trace_y = 8; c.sampling = SamplingMode::SSAA4;
    const auto f = render_scene(p, scene, c);
    check_pixel_process(*f.trace);
    assert(f.trace->events.size() == 2);
    assert(f.shade_evaluations == 0);
    for (const auto& e : f.trace->events) assert(e.primitive == TracePrimitive::TrailSegment && e.shading.empty());
    scene.reference_trails.clear();
    auto& points = scene.trails.front().points;
    points.clear();
    for (std::size_t n = 0; n < PixelTrace::max_events + 4; ++n) points.push_back({n%2 ? 1.0 : -1.0,0});
    const auto limited = render_scene(p, scene, c);
    assert(limited.trace->process_truncated && limited.trace->events.size() == PixelTrace::max_events);
    c.trace_process = false;
    equivalent(limited, render_scene(p, scene, c));
}

int main() {
    test_modes_and_comparison();
    test_layers();
    test_trails_and_limit();
    std::cout << "Pixel process replay, sampling, layering, timing switch and limits passed\n";
}
