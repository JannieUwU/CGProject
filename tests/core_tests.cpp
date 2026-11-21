#include "planet_sandbox/project.h"
#include "planet_sandbox/aa_benchmark.h"
#include "planet_sandbox/demo_presets.h"
#ifdef NDEBUG
#error Core tests require enabled assertions
#endif
#include "planet_sandbox/renderer.h"
#include "planet_sandbox/sculpt.h"
#include "planet_sandbox/session.h"
#include "planet_sandbox/simulation.h"
#include "planet_sandbox/solar_system.h"
#include "planet_sandbox/texture.h"
#include "pixel_trace_checks.h"

#include <nlohmann/json.hpp>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

using namespace planet_sandbox;

static Project example_project() {
    Project project;
    project.bodies = {
        {"sun", "Sun", 100.0, 0.25, {0.0, 0.0}, {0.0, 0.0}, {1, "#ffbb62", "#d65032", 4.0, 0.3, 0.0, 0.0}},
        {"a", "A", 1.0, 0.08, {2.0, 0.0}, {0.0, 6.5}, {2, "#3cbdc2", "#1d5c78", 8.0, 0.7, 0.2, 0.0}},
        {"b", "B", 1.0, 0.08, {-2.0, 0.0}, {0.0, -6.5}, {3, "#e8906e", "#7655a3", 5.0, 0.4, 0.1, 0.0}}
    };
    return project;
}

static void test_project_validation() {
    Project project = example_project();
    validate_project(project);
    std::istringstream valid_json(R"({"bodies":[{"id":"sample","name":"Sample","mass":1,"radius":0.2,"position":[1,0],"velocity":[0,1]}]})");
    assert(read_project(valid_json).bodies.size() == 1);
    std::istringstream invalid_json("{invalid");
    bool json_rejected = false;
    try { read_project(invalid_json); } catch (const std::exception&) { json_rejected = true; }
    assert(json_rejected);
    project.bodies[2].id = "a";
    bool duplicate_rejected = false;
    try { validate_project(project); } catch (const std::invalid_argument&) { duplicate_rejected = true; }
    assert(duplicate_rejected);
    project.bodies[2].id = "b";
    project.bodies[2].mass = 0;
    bool mass_rejected = false;
    try { validate_project(project); } catch (const std::invalid_argument&) { mass_rejected = true; }
    assert(mass_rejected);
}

static void test_project_round_trip() {
    const Project original = example_project();
    std::ostringstream output;
    write_project(output, original);
    std::istringstream input(output.str());
    const Project restored = read_project(input);
    assert(restored.bodies.size() == original.bodies.size());
    assert(restored.view_extent == original.view_extent);
    assert(restored.display_scale == original.display_scale);
    for (std::size_t i = 0; i < original.bodies.size(); ++i) {
        const auto& a = original.bodies[i];
        const auto& b = restored.bodies[i];
        assert(a.id == b.id && a.name == b.name);
        assert(a.mass == b.mass && a.radius == b.radius);
        assert(a.position.x == b.position.x && a.position.y == b.position.y);
        assert(a.velocity.x == b.velocity.x && a.velocity.y == b.velocity.y);
        assert(a.appearance.seed == b.appearance.seed);
        assert(a.appearance.primary == b.appearance.primary);
        assert(a.appearance.secondary == b.appearance.secondary);
        assert(a.appearance.bands == b.appearance.bands);
        assert(a.appearance.terrain == b.appearance.terrain);
        assert(a.appearance.clouds == b.appearance.clouds);
        assert(a.appearance.ring_width == b.appearance.ring_width);
    }
}

static void test_solar_system_catalog() {
    const Project project = make_solar_system_project();
    const std::vector<std::string> ids = {"sun", "mercury", "venus", "earth", "mars",
                                          "jupiter", "saturn", "uranus", "neptune"};
    assert(project.solar_system);
    assert(project.bodies.size() == ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        assert(project.bodies[i].id == ids[i]);
        assert(project.bodies[i].display_radius > project.bodies[i].radius);
        if (i > 1) {
            const auto& a = project.bodies[i - 1].position;
            const auto& b = project.bodies[i].position;
            assert(std::hypot(b.x, b.y) > std::hypot(a.x, a.y));
        }
    }
    assert(std::abs(project.bodies[2].position.y) > 1.0);
    assert(std::abs(project.bodies[3].position.y) > 1.0);
    Project changed = project;
    changed.bodies[3].id = "new-earth";
    bool rejected = false;
    try { validate_project(changed); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    changed = project;
    changed.bodies[3].name = "My Earth";
    rejected = false;
    try { validate_project(changed); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    changed = project;
    changed.bodies.erase(changed.bodies.begin() + 3);
    rejected = false;
    try { validate_project(changed); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}

static void test_solar_design_round_trip() {
    Project project = make_solar_system_project();
    project.bodies[3].design.relief = 2.4;
    project.bodies[3].design.cloud_opacity = 0.75;
    project.bodies[0].design.granulation = 2.0;
    project.bodies[6].design.cassini_gap = 1.8;
    project.bodies[7].design.axis_tilt_offset = 15.0;
    project.bodies[0].design.granule_scale = 1.8;
    project.bodies[3].design.cloud_motion = 2.0;
    project.bodies[6].design.ring_tilt_offset = 12.0;
    std::ostringstream output;
    write_project(output, project);
    assert(output.str().find("\"appearance\"") == std::string::npos);
    std::istringstream input(output.str());
    const Project restored = read_project(input);
    assert(restored.solar_system);
    assert(restored.bodies[3].kind == BodyKind::Earth);
    assert(restored.bodies[3].display_radius == project.bodies[3].display_radius);
    assert(restored.bodies[3].design.relief == 2.4);
    assert(restored.bodies[3].design.cloud_opacity == 0.75);
    assert(restored.bodies[0].design.granulation == 2.0);
    assert(restored.bodies[6].design.cassini_gap == 1.8);
    assert(restored.bodies[7].design.axis_tilt_offset == 15.0);
    assert(restored.bodies[0].design.granule_scale == 1.8);
    assert(restored.bodies[3].design.cloud_motion == 2.0);
    assert(restored.bodies[6].design.ring_tilt_offset == 12.0);
    std::istringstream legacy(R"({"bodies":[{"id":"sample","mass":1,"radius":0.2,"position":[1,0],"velocity":[0,1]}]})");
    assert(!read_project(legacy).solar_system);
}

static void test_anchored_shape_controls_persist_and_reset() {
    Project project = make_solar_system_project();
    project.bodies[1].design.crater_depth = 2.5;
    project.bodies[2].design.radar_relief = 3.0;
    project.bodies[5].design.belt_relief = 2.2;
    project.bodies[5].design.storm_relief = 0.5;
    std::ostringstream output;
    write_project(output, project);
    std::istringstream input(output.str());
    const Project restored = read_project(input);
    assert(restored.bodies[1].design.crater_depth == 2.5);
    assert(restored.bodies[2].design.radar_relief == 3.0);
    assert(restored.bodies[5].design.belt_relief == 2.2);
    assert(restored.bodies[5].design.storm_relief == 0.5);

    nlohmann::json old_document = nlohmann::json::parse(output.str());
    for (auto& body : old_document["bodies"]) {
        auto& overrides = body["overrides"];
        overrides.erase("crater_depth");
        overrides.erase("radar_relief");
        overrides.erase("belt_relief");
        overrides.erase("storm_relief");
    }
    std::istringstream old_input(old_document.dump());
    const Project old_project = read_project(old_input);
    assert(old_project.bodies[1].design.crater_depth == 1.0);
    assert(old_project.bodies[2].design.radar_relief == 1.0);
    assert(old_project.bodies[5].design.belt_relief == 1.0);
    assert(old_project.bodies[5].design.storm_relief == 1.0);

    SandboxSession session(project);
    session.reset_body_design(BodyKind::Venus);
    assert(session.project().bodies[2].design.radar_relief == 1.0);
    assert(session.project().bodies[1].design.crater_depth == 2.5);
    session.reset_body_design(BodyKind::Mercury);
    session.reset_body_design(BodyKind::Jupiter);
    assert(session.project().bodies[1].design.crater_depth == 1.0);
    assert(session.project().bodies[5].design.belt_relief == 1.0);
    assert(session.project().bodies[5].design.storm_relief == 1.0);

    for (int index : {1, 2, 5}) {
        Project invalid = project;
        if (index == 1) invalid.bodies[index].design.crater_depth = 3.1;
        if (index == 2) invalid.bodies[index].design.radar_relief = 4.1;
        if (index == 5) invalid.bodies[index].design.belt_relief = 3.1;
        bool rejected = false;
        try { validate_project(invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    Project invalid_storm = project;
    invalid_storm.bodies[5].design.storm_relief = 3.1;
    bool rejected = false;
    try { validate_project(invalid_storm); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}

static void test_solar_texture_and_relief() {
    bool missing_rejected = false;
    try { load_texture("missing-solar-map.jpg"); }
    catch (const std::runtime_error&) { missing_rejected = true; }
    assert(missing_rejected);
    bool corrupt_rejected = false;
    try { load_texture("CMakeLists.txt"); }
    catch (const std::runtime_error&) { corrupt_rejected = true; }
    assert(corrupt_rejected);
    const Texture earth = load_texture(solar_asset_path(BodyKind::Earth, AssetLayer::Color));
    const Texture mars_height = load_texture(solar_asset_path(BodyKind::Mars, AssetLayer::Height));
    assert(earth.width == 2048 && earth.height == 1024);
    assert(mars_height.width == 1000 && mars_height.height == 500);
    assert(sample_texture(earth, 0.25, 0.5) != sample_texture(earth, 0.75, 0.5));
    assert(solar_relief_multiplier(BodyKind::Earth, 0.9, 0.0, 4.0) == 1.0);
    assert(solar_relief_multiplier(BodyKind::Mars, 0.9, 1.0, 4.0) == 1.0);
    assert(solar_relief_multiplier(BodyKind::Earth, 0.0, 0.5, 4.0) == 1.0);
    assert(solar_relief_multiplier(BodyKind::Earth, 0.8, 0.5, 4.0) > 1.0);
    assert(solar_height_sample(BodyKind::Mars, 0.125, 0.40) >
           solar_height_sample(BodyKind::Mars, 0.625, 0.40) + 0.15);

    Project project = make_solar_system_project();
    const SceneSnapshot snapshot = Simulation(project).snapshot();
    RenderConfig config;
    config.width = 240;
    config.height = 180;
    config.camera.target = project.bodies[3].position;
    config.camera.zoom = 0.035;
    const RenderFrame baseline = render_scene(project, snapshot, config);
    assert(render_scene(project, snapshot, config).image.rgb == baseline.image.rgb);
    Project changed = project;
    changed.bodies[3].design.relief = 4.0;
    const RenderFrame raised = render_scene(changed, snapshot, config);
    assert(raised.image.rgb != baseline.image.rgb);
    changed = project;
    changed.bodies[3].design.cloud_opacity = 0.0;
    assert(render_scene(changed, snapshot, config).image.rgb != baseline.image.rgb);
    changed = project;
    changed.bodies[3].appearance.seed += 999;
    assert(render_scene(changed, snapshot, config).image.rgb == baseline.image.rgb);

    config.camera.target = project.bodies[4].position;
    const RenderFrame mars = render_scene(project, snapshot, config);
    changed = project;
    changed.bodies[4].design.relief = 4.0;
    assert(render_scene(changed, snapshot, config).image.rgb != mars.image.rgb);
    changed = project;
    changed.bodies[4].design.dust_opacity = 0.8;
    assert(render_scene(changed, snapshot, config).image.rgb != mars.image.rgb);
}

static void test_every_solar_body_has_editable_shape() {
    for (std::size_t index = 0; index < 9; ++index) {
        const BodyKind kind = solar_body_kind(index);
        double minimum = 1.0;
        double maximum = 0.0;
        bool displaced = false;
        for (int row = 1; row < 8; ++row) {
            const double v = row / 8.0;
            for (int col = 0; col < 12; ++col) {
                const double u = col / 12.0;
                const double height = solar_height_sample(kind, u, v);
                assert(std::isfinite(height) && height >= 0.0 && height <= 1.0);
                assert(solar_height_sample(kind, u, v) == height);
                assert(solar_relief_multiplier(kind, height, v, 0.0) == 1.0);
                const double radius = solar_relief_multiplier(kind, height, v, 1.0);
                assert(std::isfinite(radius) && radius > 0.0);
                if (std::abs(radius - 1.0) > 1e-5) {
                    displaced = true;
                    assert(std::abs(solar_relief_multiplier(kind, height, v, 4.0) - 1.0) >
                           std::abs(radius - 1.0) * 3.9);
                }
                minimum = std::min(minimum, height);
                maximum = std::max(maximum, height);
            }
        }
        assert(maximum - minimum > 0.02);
        assert(displaced);
    }
}

static void test_anchored_feature_shape_sampling() {
    DesignOverrides mercury;
    mercury.crater_depth = 0.0;
    DesignOverrides deep_craters = mercury;
    deep_craters.crater_depth = 3.0;
    double strongest_crater = 0.0;
    double weakest_crater = 1.0;
    for (int row = 4; row < 44; ++row) {
        const double v = row / 48.0;
        for (int col = 0; col < 96; ++col) {
            const double u = col / 96.0;
            const double difference =
                solar_shape_multiplier(BodyKind::Mercury, u, v, mercury) -
                solar_shape_multiplier(BodyKind::Mercury, u, v, deep_craters);
            assert(difference >= -1e-10);
            strongest_crater = std::max(strongest_crater, difference);
            weakest_crater = std::min(weakest_crater, difference);
        }
    }
    assert(strongest_crater > 0.008);
    assert(weakest_crater < 0.0001);
    assert(std::abs(solar_shape_multiplier(BodyKind::Mercury, 0.0, 0.5, deep_craters) -
                    solar_shape_multiplier(BodyKind::Mercury, 1.0, 0.5, deep_craters)) < 1e-9);
    assert(solar_shape_multiplier(BodyKind::Mercury, 0.3, 0.0, deep_craters) == 1.0);

    DesignOverrides venus;
    DesignOverrides cloud_raised = venus;
    cloud_raised.relief = 4.0;
    DesignOverrides radar_raised = venus;
    radar_raised.radar_relief = 4.0;
    double radar_change = 0.0;
    double cloud_change = 0.0;
    for (int row = 1; row < 24; ++row) {
        const double v = row / 24.0;
        for (int col = 0; col < 48; ++col) {
            const double u = col / 48.0;
            const double cloud = solar_shape_multiplier(BodyKind::Venus, u, v, venus);
            const double radar = solar_shape_multiplier(BodyKind::Venus, u, v, venus,
                                                         SolarShapeLayer::VenusRadar);
            assert(cloud > radar);
            assert(solar_shape_multiplier(BodyKind::Venus, u, v, radar_raised) == cloud);
            assert(solar_shape_multiplier(BodyKind::Venus, u, v, cloud_raised,
                                          SolarShapeLayer::VenusRadar) == radar);
            radar_change = std::max(radar_change,
                std::abs(solar_shape_multiplier(BodyKind::Venus, u, v, radar_raised,
                                                SolarShapeLayer::VenusRadar) - radar));
            cloud_change = std::max(cloud_change,
                std::abs(solar_shape_multiplier(BodyKind::Venus, u, v, cloud_raised) - cloud));
            assert(solar_shape_multiplier(BodyKind::Venus, u, v, cloud_raised) >
                   solar_shape_multiplier(BodyKind::Venus, u, v, radar_raised,
                                          SolarShapeLayer::VenusRadar));
        }
    }
    assert(radar_change > 0.008);
    assert(cloud_change > 0.008);

    DesignOverrides jupiter;
    DesignOverrides flat_belts = jupiter;
    flat_belts.belt_relief = 0.0;
    DesignOverrides raised_belts = jupiter;
    raised_belts.belt_relief = 3.0;
    double belt_change = 0.0;
    for (int row = 4; row < 44; ++row) {
        const double v = row / 48.0;
        belt_change = std::max(belt_change,
            std::abs(solar_shape_multiplier(BodyKind::Jupiter, 0.1, v, raised_belts) -
                     solar_shape_multiplier(BodyKind::Jupiter, 0.1, v, flat_belts)));
    }
    assert(belt_change > 0.006);
    DesignOverrides flat_spot = jupiter;
    flat_spot.storm_relief = 0.0;
    DesignOverrides raised_spot = jupiter;
    raised_spot.storm_relief = 3.0;
    const auto spot_difference = [&](double u, double v) {
        return solar_shape_multiplier(BodyKind::Jupiter, u, v, raised_spot) -
               solar_shape_multiplier(BodyKind::Jupiter, u, v, flat_spot);
    };
    assert(spot_difference(0.37, 0.61) > 0.02);
    assert(std::abs(spot_difference(0.75, 0.61)) < 0.001);
    assert(std::abs(spot_difference(0.37, 0.15)) < 0.001);
    double minimum_radius = 2.0;
    double maximum_radius = 0.0;
    for (int row = 1; row < 48; ++row) {
        for (int col = 0; col < 96; ++col) {
            const double radius = solar_shape_multiplier(BodyKind::Jupiter,
                                                          col / 96.0, row / 48.0, jupiter);
            minimum_radius = std::min(minimum_radius, radius);
            maximum_radius = std::max(maximum_radius, radius);
        }
    }
    if (minimum_radius <= 0.97 || maximum_radius >= 1.025) {
        std::fprintf(stderr, "Jupiter default radius range: %.5f .. %.5f\n",
                     minimum_radius, maximum_radius);
    }
    assert(minimum_radius > 0.97 && maximum_radius < 1.025);
}

static void test_local_sculpt_shape_and_persistence() {
    Project project = make_solar_system_project();
    auto& earth = project.bodies[3].design;
    const double base = solar_shape_multiplier(BodyKind::Earth, 0.25, 0.5, earth);
    const double far = solar_shape_multiplier(BodyKind::Earth, 0.75, 0.5, earth);
    apply_sculpt_brush(earth, BodyKind::Earth, SolarShapeLayer::Visible,
                       SculptMode::Raise, 0.25, 0.5, 0.2, 1.0);
    const double raised = solar_shape_multiplier(BodyKind::Earth, 0.25, 0.5, earth);
    assert(raised > base + 0.001);
    assert(solar_shape_multiplier(BodyKind::Earth, 0.75, 0.5, earth) == far);
    apply_sculpt_brush(earth, BodyKind::Earth, SolarShapeLayer::Visible,
                       SculptMode::Smooth, 0.25, 0.5, 0.2, 1.0);
    assert(solar_shape_multiplier(BodyKind::Earth, 0.25, 0.5, earth) < raised);
    apply_sculpt_brush(earth, BodyKind::Earth, SolarShapeLayer::Visible,
                       SculptMode::Lower, 0.25, 0.5, 0.2, 1.0);
    assert(solar_shape_multiplier(BodyKind::Earth, 0.25, 0.5, earth) < raised);

    auto& mercury = project.bodies[1].design;
    apply_sculpt_brush(mercury, BodyKind::Mercury, SolarShapeLayer::Visible,
                       SculptMode::Raise, 0.0, 0.5, 0.2, 1.0);
    assert(std::abs(sculpt_offset(mercury, SolarShapeLayer::Visible, 0.001, 0.5) -
                    sculpt_offset(mercury, SolarShapeLayer::Visible, 0.999, 0.5)) < 0.001);
    apply_sculpt_brush(mercury, BodyKind::Mercury, SolarShapeLayer::Visible,
                       SculptMode::Raise, 0.3, 0.0, 0.2, 1.0);
    assert(std::abs(sculpt_offset(mercury, SolarShapeLayer::Visible, 0.1, 0.0) -
                    sculpt_offset(mercury, SolarShapeLayer::Visible, 0.8, 0.0)) < 1e-6);

    auto& venus = project.bodies[2].design;
    const double cloud = solar_shape_multiplier(BodyKind::Venus, 0.25, 0.5, venus);
    const double radar = solar_shape_multiplier(BodyKind::Venus, 0.25, 0.5, venus,
                                                SolarShapeLayer::VenusRadar);
    apply_sculpt_brush(venus, BodyKind::Venus, SolarShapeLayer::VenusRadar,
                       SculptMode::Raise, 0.25, 0.5, 0.2, 1.0);
    assert(solar_shape_multiplier(BodyKind::Venus, 0.25, 0.5, venus) == cloud);
    assert(solar_shape_multiplier(BodyKind::Venus, 0.25, 0.5, venus,
                                  SolarShapeLayer::VenusRadar) > radar);

    std::ostringstream output;
    write_project(output, project);
    std::istringstream input(output.str());
    const Project restored = read_project(input);
    assert(restored.bodies[3].design.sculpt_visible == earth.sculpt_visible);
    assert(restored.bodies[2].design.sculpt_radar == venus.sculpt_radar);
    nlohmann::json old = nlohmann::json::parse(output.str());
    for (auto& body : old["bodies"]) {
        body["overrides"].erase("sculpt_visible");
        body["overrides"].erase("sculpt_radar");
    }
    std::istringstream old_input(old.dump());
    assert(read_project(old_input).bodies[3].design.sculpt_visible.empty());
    Project invalid = project;
    invalid.bodies[3].design.sculpt_visible.pop_back();
    bool rejected = false;
    try { validate_project(invalid); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    SandboxSession session(project);
    session.reset_body_design(BodyKind::Earth);
    assert(session.project().bodies[3].design.sculpt_visible.empty());
    assert(!session.project().bodies[1].design.sculpt_visible.empty());
}

static void test_local_sculpt_undo_is_per_body() {
    Project project = make_solar_system_project();
    SculptHistory history;
    history.begin_stroke(BodyKind::Earth, SolarShapeLayer::Visible, project.bodies[3].design);
    apply_sculpt_brush(project.bodies[3].design, BodyKind::Earth, SolarShapeLayer::Visible,
                       SculptMode::Raise, 0.25, 0.5, 0.2, 1.0);
    history.begin_stroke(BodyKind::Mars, SolarShapeLayer::Visible, project.bodies[4].design);
    apply_sculpt_brush(project.bodies[4].design, BodyKind::Mars, SolarShapeLayer::Visible,
                       SculptMode::Raise, 0.25, 0.5, 0.2, 1.0);
    assert(history.undo(BodyKind::Earth, project.bodies[3].design));
    assert(project.bodies[3].design.sculpt_visible.empty());
    assert(!project.bodies[4].design.sculpt_visible.empty());
    assert(!history.undo(BodyKind::Earth, project.bodies[3].design));
    assert(history.undo(BodyKind::Mars, project.bodies[4].design));
}

static void test_sculpt_pick_uses_visible_sphere_only() {
    Project project = make_solar_system_project();
    const SceneSnapshot all = Simulation(project).snapshot();
    RenderConfig config;
    config.width = 240;
    config.height = 180;
    SceneSnapshot one = all;
    one.bodies = {all.bodies[3]};
    one.trails.clear();
    project.view_extent = project.bodies[3].display_radius * 1.1;
    config.camera.target = one.bodies[0].position;
    const RenderFrame earth = render_scene(project, one, config);
    const std::size_t center = 90u * 240u + 120u;
    assert(earth.body_index[center] == 3);
    assert(std::abs(earth.surface_uv[center].x - 0.25) < 0.04);
    assert(std::abs(earth.surface_uv[center].y - 0.5) < 0.04);
    assert(earth.surface_uv[0].x < 0.0);

    Project raised = project;
    apply_sculpt_brush(raised.bodies[3].design, BodyKind::Earth, SolarShapeLayer::Visible,
                       SculptMode::Raise, earth.surface_uv[center].x,
                       earth.surface_uv[center].y, 0.35, 1.0);
    assert(render_scene(raised, one, config).image.rgb != earth.image.rgb);

    one.bodies = {all.bodies[6]};
    project.view_extent = project.bodies[6].display_radius * 2.0;
    config.camera.target = one.bodies[0].position;
    const RenderFrame saturn = render_scene(project, one, config);
    int ring_pixels = 0;
    for (std::size_t pixel = 0; pixel < saturn.body_index.size(); ++pixel) {
        if (saturn.body_index[pixel] == 6 && saturn.surface_uv[pixel].x < 0.0) ++ring_pixels;
    }
    assert(ring_pixels > 10);
}

static void test_sculpt_stroke_requires_surface_press_and_fills_drag_path() {
    RenderFrame frame;
    frame.image.width = 41;
    frame.image.height = 1;
    frame.body_index.assign(41, -1);
    frame.surface_uv.assign(41, {-1.0, -1.0});
    for (int x = 5; x <= 35; ++x) {
        frame.body_index[x] = 3;
        frame.surface_uv[x] = {x / 40.0, 0.5};
    }
    SculptStroke stroke;
    assert(!stroke.begin(frame, 3, 0, 0));
    assert(stroke.move(frame, 3, 20, 0).empty());
    assert(stroke.begin(frame, 3, 8, 0));
    const auto dabs = stroke.move(frame, 3, 32, 0);
    assert(dabs.size() >= 4);
    assert(dabs.front().x < dabs.back().x);
    assert(stroke.move(frame, 3, 32, 0).empty());
    stroke.end();
    assert(stroke.move(frame, 3, 25, 0).empty());
    frame.surface_uv[10] = {-1.0, -1.0};
    assert(!stroke.begin(frame, 3, 10, 0));
}

static void test_earth_clouds_cover_sculpted_surface() {
    Project project = make_solar_system_project();
    project.view_extent = project.bodies[3].display_radius * 1.1;
    SceneSnapshot scene = Simulation(project).snapshot();
    scene.bodies = {scene.bodies[3]};
    scene.trails.clear();
    RenderConfig config;
    config.width = 240;
    config.height = 180;
    config.camera.target = scene.bodies[0].position;
    for (int i = 0; i < 10; ++i) {
        apply_sculpt_brush(project.bodies[3].design, BodyKind::Earth, SolarShapeLayer::Visible,
                           SculptMode::Raise, 0.25, 0.5, 0.6, 1.0);
    }
    Project clear = project;
    clear.bodies[3].design.cloud_opacity = 0.0;
    Project cloudy = project;
    cloudy.bodies[3].design.cloud_opacity = 1.0;
    const RenderFrame clear_frame = render_scene(clear, scene, config);
    const RenderFrame cloudy_frame = render_scene(cloudy, scene, config);
    const std::size_t center = 90u * 240u + 120u;
    assert(clear_frame.body_index[center] == 3);
    bool changed = false;
    for (int channel = 0; channel < 3; ++channel) {
        changed |= clear_frame.image.rgb[center * 3 + channel] !=
                   cloudy_frame.image.rgb[center * 3 + channel];
    }
    assert(changed);
}

static void test_venus_sculpt_layers_do_not_cross() {
    DesignOverrides design;
    for (int i = 0; i < 25; ++i) {
        apply_sculpt_brush(design, BodyKind::Venus, SolarShapeLayer::VenusRadar,
                           SculptMode::Raise, 0.25, 0.5, 0.5, 1.0);
        apply_sculpt_brush(design, BodyKind::Venus, SolarShapeLayer::Visible,
                           SculptMode::Lower, 0.25, 0.5, 0.5, 1.0);
    }
    for (int row = 8; row <= 40; ++row) {
        const double v = row / 48.0;
        for (int col = 0; col < 96; ++col) {
            const double u = col / 96.0;
            assert(solar_shape_multiplier(BodyKind::Venus, u, v, design) >
                   solar_shape_multiplier(BodyKind::Venus, u, v, design,
                                          SolarShapeLayer::VenusRadar));
        }
    }
}

static void test_anchored_feature_controls_change_rendered_layers() {
    const Project baseline = make_solar_system_project();
    SceneSnapshot all = Simulation(baseline).snapshot();
    all.time = 0.0;
    const auto focused = [&](const Project& project, std::size_t index) {
        Project view = project;
        view.view_extent = view.bodies[index].display_radius * 1.2;
        SceneSnapshot one = all;
        one.bodies = {all.bodies[index]};
        one.trails.clear();
        RenderConfig config;
        config.width = 224;
        config.height = 168;
        config.camera.target = one.bodies[0].position;
        return render_scene(view, one, config);
    };

    Project mercury = baseline;
    mercury.bodies[1].design.crater_depth = 3.0;
    assert(focused(mercury, 1).image.rgb != focused(baseline, 1).image.rgb);

    Project radar = baseline;
    radar.bodies[2].design.cloud_opacity = 0.0;
    Project radar_raised = radar;
    radar_raised.bodies[2].design.radar_relief = 4.0;
    assert(focused(radar_raised, 2).image.rgb != focused(radar, 2).image.rgb);
    Project hidden_cloud_change = radar;
    hidden_cloud_change.bodies[2].design.relief = 4.0;
    assert(focused(hidden_cloud_change, 2).image.rgb == focused(radar, 2).image.rgb);

    Project opaque_cloud = baseline;
    opaque_cloud.bodies[2].design.cloud_opacity = 1.0;
    Project hidden_radar_change = opaque_cloud;
    hidden_radar_change.bodies[2].design.radar_relief = 4.0;
    assert(focused(hidden_radar_change, 2).image.rgb == focused(opaque_cloud, 2).image.rgb);
    Project cloud_raised = opaque_cloud;
    cloud_raised.bodies[2].design.relief = 4.0;
    assert(focused(cloud_raised, 2).image.rgb != focused(opaque_cloud, 2).image.rgb);

    Project belts = baseline;
    belts.bodies[5].design.belt_relief = 3.0;
    assert(focused(belts, 5).image.rgb != focused(baseline, 5).image.rgb);
    Project spot = baseline;
    spot.bodies[5].design.storm_relief = 3.0;
    assert(focused(spot, 5).image.rgb != focused(baseline, 5).image.rgb);
}

static void test_venus_transparency_is_order_independent() {
    Project project = make_solar_system_project();
    project.view_extent = 0.5;
    project.bodies[2].design.cloud_opacity = 0.5;
    project.bodies[3].display_radius = 0.03;
    SceneSnapshot ordered;
    ordered.bodies = {
        {"venus", {0.0, 0.0}, {}, project.bodies[2].mass, project.bodies[2].radius},
        {"earth", {0.26, 0.0}, {}, project.bodies[3].mass, project.bodies[3].radius}
    };
    SceneSnapshot reversed = ordered;
    std::reverse(reversed.bodies.begin(), reversed.bodies.end());
    RenderConfig config;
    config.width = 256;
    config.height = 192;
    config.camera.yaw = 1.5707963267948966;
    assert(render_scene(project, ordered, config).image.rgb ==
           render_scene(project, reversed, config).image.rgb);
}

static void test_venus_cloud_rim_has_single_opacity() {
    Project project = make_solar_system_project();
    project.view_extent = project.bodies[2].display_radius * 1.2;
    SceneSnapshot snapshot;
    snapshot.bodies = {{"venus", {0.0, 0.0}, {},
                        project.bodies[2].mass, project.bodies[2].radius}};
    RenderConfig config;
    config.width = 256;
    config.height = 192;
    const auto render_opacity = [&](double opacity) {
        Project view = project;
        view.bodies[2].design.cloud_opacity = opacity;
        return render_scene(view, snapshot, config);
    };
    for (double yaw : {0.0, 0.7, 1.4}) {
        config.camera.yaw = yaw;
        const RenderFrame clear = render_opacity(0.0);
        const RenderFrame mixed = render_opacity(0.5);
        const RenderFrame opaque = render_opacity(1.0);
        int rim_pixels = 0;
        for (std::size_t pixel = 0; pixel < clear.body_index.size(); ++pixel) {
            if (clear.body_index[pixel] != -1 || opaque.body_index[pixel] != 2) continue;
            ++rim_pixels;
            for (int channel = 0; channel < 3; ++channel) {
                const auto linear = [&](const RenderFrame& frame) {
                    return std::pow(frame.image.rgb[pixel * 3 + channel] / 255.0, 2.2);
                };
                const double expected = 0.5 * (linear(clear) + linear(opaque));
                if (std::abs(linear(mixed) - expected) >= 0.035) {
                    std::fprintf(stderr, "Venus rim yaw %.2f pixel %zu channel %d: clear %.3f mixed %.3f opaque %.3f\n",
                                 yaw, pixel, channel, linear(clear), linear(mixed), linear(opaque));
                }
                assert(std::abs(linear(mixed) - expected) < 0.035);
            }
        }
        assert(rim_pixels > 20);
    }
}

static void test_venus_cloud_blends_over_trail() {
    Project project = make_solar_system_project();
    project.view_extent = 0.4;
    project.bodies[2].design.cloud_opacity = 0.5;
    SceneSnapshot scene;
    scene.bodies = {{"venus", {0.0, 0.0}, {},
                     project.bodies[2].mass, project.bodies[2].radius}};
    scene.trails = {{"venus", {{0.29, -0.2}, {0.29, 0.2}}}};
    RenderConfig config;
    config.width = 256;
    config.height = 192;
    const RenderFrame with_cloud_trail = render_scene(project, scene, config);
    scene.trails.clear();
    const RenderFrame cloud_only = render_scene(project, scene, config);
    project.bodies[2].design.cloud_opacity = 0.0;
    const RenderFrame clear_only = render_scene(project, scene, config);
    scene.trails = {{"venus", {{0.29, -0.2}, {0.29, 0.2}}}};
    const RenderFrame clear_trail = render_scene(project, scene, config);
    int intersections = 0;
    int visible_through_cloud = 0;
    for (std::size_t pixel = 0; pixel < clear_only.body_index.size(); ++pixel) {
        if (clear_only.body_index[pixel] != -1 || cloud_only.body_index[pixel] != 2) continue;
        const auto changed = [&](const RenderFrame& a, const RenderFrame& b) {
            for (int channel = 0; channel < 3; ++channel) {
                if (a.image.rgb[pixel * 3 + channel] != b.image.rgb[pixel * 3 + channel]) {
                    return true;
                }
            }
            return false;
        };
        if (!changed(clear_trail, clear_only)) continue;
        ++intersections;
        if (changed(with_cloud_trail, cloud_only)) ++visible_through_cloud;
    }
    assert(intersections > 0);
    assert(visible_through_cloud > 0);
}

static void test_every_solar_body_relief_changes_rendered_geometry() {
    const Project baseline = make_solar_system_project();
    const SceneSnapshot all = Simulation(baseline).snapshot();
    for (std::size_t index = 0; index < baseline.bodies.size(); ++index) {
        Project flat = baseline;
        Project sculpted = baseline;
        flat.bodies[index].design.relief = 0.0;
        sculpted.bodies[index].design.relief = 4.0;
        if (index == 3) {
            flat.bodies[index].design.cloud_opacity = 0.0;
            sculpted.bodies[index].design.cloud_opacity = 0.0;
        }
        if (index == 6 || index == 7) {
            flat.bodies[index].design.ring_width = 0.0;
            sculpted.bodies[index].design.ring_width = 0.0;
        }
        const double framing = baseline.bodies[index].display_radius * 1.15;
        flat.view_extent = framing;
        sculpted.view_extent = framing;
        SceneSnapshot one = all;
        one.bodies = {all.bodies[index]};
        one.trails.clear();
        RenderConfig config;
        config.width = 256;
        config.height = 192;
        config.camera.target = one.bodies[0].position;
        const RenderFrame flat_frame = render_scene(flat, one, config);
        const RenderFrame sculpted_frame = render_scene(sculpted, one, config);
        assert(flat_frame.image.rgb != sculpted_frame.image.rgb);
        assert(flat_frame.body_index != sculpted_frame.body_index);
    }
}

static void test_specialized_solar_overrides() {
    const Project baseline_project = make_solar_system_project();
    const SceneSnapshot all = Simulation(baseline_project).snapshot();
    const auto focused = [&](const Project& project, int index) {
        SceneSnapshot snapshot = all;
        snapshot.bodies = {all.bodies[static_cast<std::size_t>(index)]};
        snapshot.trails.clear();
        RenderConfig config;
        config.width = 180;
        config.height = 140;
        config.camera.target = snapshot.bodies[0].position;
        config.camera.zoom = 0.8;
        Project view = project;
        view.view_extent = view.bodies[static_cast<std::size_t>(index)].display_radius * 1.35;
        return render_scene(view, snapshot, config).image.rgb;
    };
    for (int index : {0, 1, 2, 5, 6, 7, 8}) {
        Project changed = baseline_project;
        switch (index) {
        case 0: changed.bodies[index].design.granulation = 0.0; break;
        case 1: changed.bodies[index].design.band_contrast = 2.5; break;
        case 2: changed.bodies[index].design.cloud_opacity = 0.0; break;
        case 5: changed.bodies[index].design.storm_scale = 2.5; break;
        case 6: changed.bodies[index].design.cassini_gap = 2.5; break;
        case 7: changed.bodies[index].design.axis_tilt_offset = 25.0; break;
        case 8: changed.bodies[index].design.storm_scale = 2.5; break;
        }
        assert(focused(changed, index) != focused(baseline_project, index));
    }
    Project ringless = baseline_project;
    ringless.bodies[7].design.ring_width = 0.0;
    assert(focused(ringless, 7) != focused(baseline_project, 7));
    for (int index : {5, 8}) {
        Project muted = baseline_project;
        muted.bodies[index].design.storm_scale = 0.0;
        assert(focused(muted, index) != focused(baseline_project, index));
    }
}

static void test_exposed_design_controls_change_render() {
    const Project baseline = make_solar_system_project();
    SceneSnapshot snapshot = Simulation(baseline).snapshot();
    snapshot.time = 1.0;
    const auto focused = [&](const Project& project, int index) {
        SceneSnapshot one = snapshot;
        one.bodies = {snapshot.bodies[static_cast<std::size_t>(index)]};
        one.trails.clear();
        Project view = project;
        view.view_extent = view.bodies[static_cast<std::size_t>(index)].display_radius * 1.35;
        RenderConfig config;
        config.width = 160;
        config.height = 120;
        config.camera.target = one.bodies[0].position;
        return render_scene(view, one, config).image.rgb;
    };
    const auto changed = [&](int index, double DesignOverrides::*field, double value) {
        Project project = baseline;
        project.bodies[static_cast<std::size_t>(index)].design.*field = value;
        const bool visible = focused(project, index) != focused(baseline, index);
        if (!visible) {
            std::fprintf(stderr, "Unchanged design render: body %d, value %.2f\n", index, value);
        }
        assert(visible);
    };
    changed(0, &DesignOverrides::sunspot_strength, 2.0);
    changed(0, &DesignOverrides::granule_scale, 2.0);
    changed(3, &DesignOverrides::ice_extent, 1.0);
    changed(3, &DesignOverrides::cloud_motion, 2.0);
    changed(6, &DesignOverrides::ring_tilt_offset, 25.0);
}

static void test_editor_session() {
    SandboxSession session(example_project());
    assert(session.mode() == SessionMode::Design);
    assert(session.snapshot().time == 0.0);
    Project edited = session.project();
    edited.bodies[1].mass = 2.0;
    edited.bodies[1].appearance.primary = "#e1ab61";
    session.replace_project(edited);
    assert(session.snapshot().bodies[1].mass == 2.0);
    session.begin_simulation();
    assert(session.mode() == SessionMode::Simulate);
    assert(session.playing());
    session.advance(0.05);
    assert(session.snapshot().time > 0.0);
    bool edit_rejected = false;
    try { session.replace_project(example_project()); } catch (const std::logic_error&) { edit_rejected = true; }
    assert(edit_rejected);
    session.set_playing(false);
    const double paused_time = session.snapshot().time;
    session.advance(0.05);
    assert(session.snapshot().time == paused_time);
    session.step(0.02);
    assert(session.snapshot().time > paused_time);
    session.reset_simulation();
    assert(session.snapshot().time == 0.0);
    assert(!session.playing());
    session.return_to_design();
    assert(session.mode() == SessionMode::Design);
    assert(session.project().bodies[1].appearance.primary == "#e1ab61");
    Project invalid = session.project();
    invalid.bodies[1].mass = -1.0;
    bool invalid_rejected = false;
    try { session.replace_project(invalid); } catch (const std::invalid_argument&) { invalid_rejected = true; }
    assert(invalid_rejected);
    assert(session.project().bodies[1].mass == 2.0);
}

static void test_orbit_reference_and_seek() {
    Project edited = make_solar_system_project();
    edited.bodies[3].velocity.x += 0.2;
    edited.bodies[3].design.relief = 2.0;
    std::ostringstream original;
    write_project(original, edited);
    SandboxSession session(edited);
    assert(!session.reference_snapshot().has_value());
    session.begin_simulation();
    assert(session.reference_snapshot().has_value());
    assert(session.reference_snapshot()->bodies.size() == 9);
    session.seek(0.5);
    assert(!session.playing());
    assert(std::abs(session.snapshot().time - 0.5) < 1e-12);
    assert(std::abs(session.reference_snapshot()->time - 0.5) < 1e-12);
    const auto edited_earth = session.snapshot().bodies[3].position;
    const auto original_earth = session.reference_snapshot()->bodies[3].position;
    assert(std::hypot(edited_earth.x - original_earth.x,
                      edited_earth.y - original_earth.y) > 0.05);
    session.seek(0.1);
    assert(std::abs(session.snapshot().time - 0.1) < 1e-12);
    assert(std::abs(session.reference_snapshot()->time - 0.1) < 1e-12);
    session.seek(0.5);
    assert(std::hypot(session.snapshot().bodies[3].position.x - edited_earth.x,
                      session.snapshot().bodies[3].position.y - edited_earth.y) < 1e-8);
    session.step(0.05);
    assert(std::abs(session.reference_snapshot()->time - session.snapshot().time) < 1e-12);
    session.reset_simulation();
    assert(session.snapshot().time == 0.0);
    assert(session.reference_snapshot()->time == 0.0);
    std::ostringstream after;
    write_project(after, session.project());
    assert(after.str() == original.str());
    session.return_to_design();
    assert(!session.reference_snapshot().has_value());
}

static void test_orbit_collision_bookmarks_survive_rewind() {
    Project project = make_solar_system_project();
    project.bodies[3].position = {4.0, 0.0};
    project.bodies[3].velocity = {0.0, 0.0};
    project.bodies[4].position = {4.5, 0.0};
    project.bodies[4].velocity = {-1.0, 0.0};
    SandboxSession session(project);
    session.begin_simulation();
    assert(session.reference_snapshot().has_value());
    session.seek(0.8);
    assert(session.observed_events().size() == 1);
    const double event_time = session.observed_events().front().time;
    assert(event_time > 0.0 && event_time < 0.8);
    session.seek(0.0);
    assert(session.snapshot().events.empty());
    assert(session.observed_events().size() == 1);
    assert(session.reference_snapshot()->time == 0.0);
    session.seek(std::min(0.8, event_time + 0.05));
    assert(session.snapshot().events.size() == 1);
    session.reset_simulation();
    assert(session.observed_events().empty());
}

static void test_reference_trail_is_rendered_and_identified() {
    Project project = example_project();
    SceneSnapshot scene;
    scene.reference_trails = {{project.bodies[0].id, {{-1.0, 0.0}, {1.0, 0.0}}}};
    RenderConfig config;
    config.width = 120;
    config.height = 90;
    config.trace_x = 60;
    config.trace_y = 45;
    const RenderFrame reference = render_scene(project, scene, config);
    assert(reference.trace.has_value());
    bool marked = false;
    for (const TraceSample& sample : reference.trace->samples) {
        marked |= sample.surface == TraceSurface::ReferenceTrail;
    }
    assert(marked);
    scene.reference_trails.clear();
    const RenderFrame empty = render_scene(project, scene, config);
    assert(reference.image.rgb != empty.image.rgb);
}

static void test_solar_session_reset() {
    SandboxSession session(make_solar_system_project());
    Project edited = session.project();
    edited.bodies[3].design.relief = 3.0;
    session.replace_project(edited);
    session.reset_body_design(BodyKind::Earth);
    assert(session.project().bodies[3].design.relief == 1.0);
    assert(session.project().bodies[4].design.relief == 1.0);
    Project venus_edit = session.project();
    venus_edit.bodies[2].design.cloud_opacity = 0.0;
    session.replace_project(venus_edit);
    session.reset_body_design(BodyKind::Venus);
    assert(session.project().bodies[2].design.cloud_opacity == 1.0);
    bool add_rejected = false;
    try { session.add_planet(); } catch (const std::logic_error&) { add_rejected = true; }
    assert(add_rejected);
    edited = session.project();
    edited.bodies[3].design.relief = 2.0;
    session.replace_project(edited);
    session.begin_simulation();
    session.reset_simulation();
    assert(session.snapshot().bodies.size() == 9);
    session.return_to_design();
    assert(session.project().bodies[3].design.relief == 2.0);
}

static void test_added_planets_stay_distinct() {
    SandboxSession session(example_project());
    session.add_planet();
    session.add_planet();
    assert(session.project().bodies.size() == 5);
    assert(session.snapshot().bodies.size() == 5);
    assert(session.snapshot().events.empty());
    assert(session.project().bodies[3].id != session.project().bodies[4].id);
    session.begin_simulation();
    bool edit_rejected = false;
    try { session.add_planet(); } catch (const std::logic_error&) { edit_rejected = true; }
    assert(edit_rejected);
    assert(session.project().bodies.size() == 5);
}

static void test_simulation() {
    Project project = example_project();
    Simulation first(project);
    Simulation second(project);
    const SceneSnapshot frozen = first.snapshot();
    first.advance_to(0.4);
    second.advance_to(0.4);
    const auto a = first.snapshot();
    const auto b = second.snapshot();
    assert(a.bodies.size() == 3);
    assert(std::abs(a.bodies[1].position.x - b.bodies[1].position.x) < 1e-12);
    assert(std::abs(frozen.bodies[1].position.x - 2.0) < 1e-12);
    assert(std::abs(a.bodies[1].position.x - frozen.bodies[1].position.x) > 1e-4);
    assert(!a.trails.empty());
    assert(a.trails[1].points.size() > 2);
    assert(frozen.trails[1].points.size() == 1);
}

static void test_demo_collision_preserves_saved_design() {
    Project source = make_solar_system_project();
    source.bodies[3].design.relief = 2.5;
    source.bodies[4].design.dust_opacity = 0.8;
    source.bodies[3].position = {-3.0, 1.0};
    std::ostringstream original;
    write_project(original, source);

    DemoPresets demos;
    demos.activate(DemoPreset::Collision, source);
    assert(demos.active() == DemoPreset::Collision);
    assert(demos.playing());
    assert(demos.project(source).bodies[3].design.relief == 2.5);
    assert(demos.project(source).bodies[4].design.dust_opacity == 0.8);
    assert(demos.snapshot({}).bodies.size() == 2);
    assert(demos.snapshot({}).events.empty());
    demos.advance(0.0);
    assert(demos.snapshot({}).time == 0.0);
    for (int i = 0; i < 60 && demos.snapshot({}).events.empty(); ++i) demos.advance(0.05);
    const SceneSnapshot impact = demos.snapshot({});
    assert(impact.events.size() == 1);
    assert(impact.events[0].survivor == "earth");
    assert(impact.events[0].absorbed == "mars");
    assert(impact.bodies.size() == 1);
    assert(!demos.playing());
    std::ostringstream after;
    write_project(after, source);
    assert(after.str() == original.str());
    demos.reset();
    assert(demos.snapshot({}).bodies.size() == 2);
    assert(demos.snapshot({}).events.empty());
    demos.set_playing(true);
    for (int i = 0; i < 60 && demos.snapshot({}).events.empty(); ++i) demos.advance(0.05);
    assert(demos.snapshot({}).events.size() == 1);
    assert(demos.snapshot({}).events[0].survivor == "earth");
    demos.clear();
    assert(demos.active() == DemoPreset::None);
}

static void test_demo_collision_ignores_unrelated_edited_orbits() {
    Project project = make_solar_system_project();
    project.bodies[1].position = {4.0, 0.0};
    project.bodies[1].velocity = {};
    DemoPresets demos;
    demos.activate(DemoPreset::Collision, project);
    assert(demos.snapshot({}).events.empty());
    for (int i = 0; i < 60 && demos.snapshot({}).events.empty(); ++i) demos.advance(0.05);
    const SceneSnapshot impact = demos.snapshot({});
    assert(impact.events.size() == 1);
    assert(impact.events.front().survivor == "earth");
    assert(impact.events.front().absorbed == "mars");
    assert(project.bodies[1].position.x == 4.0);
}

static void test_demo_presets_require_fixed_solar_system() {
    DemoPresets demos;
    Project project = example_project();
    bool rejected = false;
    try { demos.activate(DemoPreset::EarthTerrain, project); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    assert(demos.active() == DemoPreset::None);
    const SceneSnapshot original = Simulation(project).snapshot();
    assert(demos.snapshot(original).bodies.size() == original.bodies.size());
}

static void test_initial_overlap() {
    Project project = example_project();
    project.bodies[2].position = {2.05, 0.0};
    Simulation simulation(project);
    const auto frame = simulation.snapshot();
    assert(frame.time == 0.0);
    assert(frame.bodies.size() == 2);
    assert(frame.events.size() == 1);
    assert(std::abs(frame.bodies[1].mass - 2.0) < 1e-9);
    assert(std::abs(frame.bodies[1].velocity.y) < 1e-9);
    assert(std::abs(frame.bodies[1].radius - std::cbrt(2.0 * std::pow(0.08, 3))) < 1e-9);
}

static void test_collision() {
    Project project = example_project();
    project.bodies = {
        {"sun", "Sun", 10.0, 0.1, {0.0, 0.0}, {0.0, 0.0}, {1, "#ffffff", "#ff9900", 2.0, 0.0, 0.0, 0.0}},
        {"a", "A", 1.0, 0.1, {2.0, 0.0}, {-0.5, 0.0}, {2, "#ff0000", "#990000", 2.0, 0.0, 0.0, 0.0}},
        {"b", "B", 2.0, 0.1, {2.5, 0.0}, {-1.0, 0.0}, {3, "#0000ff", "#000099", 2.0, 0.0, 0.0, 0.0}}
    };
    Simulation simulation(project);
    simulation.advance_to(0.3);
    const auto frame = simulation.snapshot();
    assert(frame.bodies.size() == 2);
    assert(frame.events.size() == 1);
    assert(frame.events[0].survivor == "b");
    assert(frame.events[0].absorbed == "a");
    assert(frame.bodies[1].id == "b");
    double total_mass = 0;
    for (const auto& body : frame.bodies) total_mass += body.mass;
    assert(std::abs(total_mass - 13.0) < 1e-9);
    assert(std::abs(frame.bodies[1].mass - 3.0) < 1e-9);
}

static void test_equal_mass_collision_uses_catalog_order() {
    Project project = example_project();
    project.bodies = {
        {"sun", "Sun", 10.0, 0.1, {0, 0}, {0, 0}, {}},
        {"mars", "Mars", 1.0, 0.1, {2.0, 0}, {0, 0}, {}},
        {"earth", "Earth", 1.0, 0.1, {2.1, 0}, {0, 0}, {}}
    };
    const auto snapshot = Simulation(project).snapshot();
    assert(snapshot.bodies.size() == 2);
    assert(snapshot.bodies[1].id == "earth");
    assert(snapshot.events[0].survivor == "earth");
    assert(snapshot.events[0].absorbed == "mars");
}

static void test_collision_preserves_momentum() {
    Project project = example_project();
    project.bodies = {
        {"a", "A", 1.0, 0.1, {0, 0}, {2, 0}, {}},
        {"b", "B", 2.0, 0.1, {0.1, 0}, {-1, 0}, {}}
    };
    const auto snapshot = Simulation(project).snapshot();
    assert(snapshot.bodies.size() == 1);
    assert(snapshot.bodies[0].id == "b");
    assert(std::abs(snapshot.bodies[0].velocity.x) < 1e-12);
    assert(std::abs(snapshot.bodies[0].mass - 3.0) < 1e-12);
}

static void test_render() {
    Project project = example_project();
    Simulation simulation(project);
    const auto frame = simulation.snapshot();
    const Image first = render_preview(project, frame, 160, 120);
    const Image second = render_preview(project, frame, 160, 120);
    assert(first.rgb == second.rgb);
    assert(first.rgb.size() == 160u * 120u * 3u);
    const auto center = (60u * 160u + 80u) * 3u;
    assert(first.rgb[center] != first.rgb[0] || first.rgb[center + 1] != first.rgb[1]);
    project.bodies[0].appearance.primary = "#00ff00";
    const Image recolored = render_preview(project, frame, 160, 120);
    assert(recolored.rgb != first.rgb);
    simulation.advance_to(0.3);
    const auto moving = simulation.snapshot();
    SceneSnapshot without_trails = moving;
    without_trails.trails.clear();
    assert(render_preview(project, moving, 160, 120).rgb !=
           render_preview(project, without_trails, 160, 120).rgb);
    std::ostringstream ppm;
    write_ppm(ppm, first);
    assert(ppm.str().substr(0, 11) == "P6\n160 120\n");
}

static void test_perspective_render_contract() {
    Project project = example_project();
    project.bodies.resize(1);
    project.bodies[0].radius = 0.6;
    project.view_extent = 2.0;
    const SceneSnapshot snapshot = Simulation(project).snapshot();
    RenderConfig config;
    config.width = 160;
    config.height = 120;
    const RenderFrame first = render_scene(project, snapshot, config);
    const RenderFrame second = render_scene(project, snapshot, config);
    assert(first.image.rgb == second.image.rgb);
    assert(first.image.rgb.size() == 160u * 120u * 3u);
    assert(first.body_index.size() == 160u * 120u);
    assert(first.body_index[60u * 160u + 80u] == 0);
    config.camera.yaw = 0.8;
    assert(render_scene(project, snapshot, config).image.rgb != first.image.rgb);
    config.width = 0;
    bool rejected = false;
    try { render_scene(project, snapshot, config); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}

static void test_perspective_depth() {
    Project project = example_project();
    project.bodies.resize(2);
    project.bodies[0].id = "front";
    project.bodies[1].id = "back";
    project.bodies[0].radius = 0.8;
    project.bodies[1].radius = 0.8;
    project.display_scale = 1.5;
    project.view_extent = 3.0;
    SceneSnapshot both;
    both.bodies = {{"front", {1.0, 0.0}, {}, 1.0, 0.8},
                   {"back", {-1.0, 0.0}, {}, 1.0, 0.8}};
    SceneSnapshot front = both;
    front.bodies.resize(1);
    SceneSnapshot back = both;
    back.bodies.erase(back.bodies.begin());
    RenderConfig config;
    config.width = 160;
    config.height = 120;
    config.camera.yaw = 1.3;
    const RenderFrame combined_frame = render_scene(project, both, config);
    const RenderFrame front_frame = render_scene(project, front, config);
    const RenderFrame back_frame = render_scene(project, back, config);
    bool found_overlap = false;
    for (std::size_t pixel = 0; pixel < combined_frame.body_index.size(); ++pixel) {
        if (front_frame.body_index[pixel] != 0 || back_frame.body_index[pixel] != 1) continue;
        found_overlap = true;
        assert(combined_frame.body_index[pixel] == 0);
        for (int channel = 0; channel < 3; ++channel) {
            assert(combined_frame.image.rgb[pixel * 3 + channel] ==
                   front_frame.image.rgb[pixel * 3 + channel]);
        }
    }
    assert(found_overlap);
}

static void test_camera_can_focus_a_planet() {
    const Project project = example_project();
    const SceneSnapshot snapshot = Simulation(project).snapshot();
    RenderConfig config;
    config.width = 160;
    config.height = 120;
    config.camera.target = {2.0, 0.0};
    config.camera.zoom = 0.25;
    const RenderFrame focused = render_scene(project, snapshot, config);
    assert(focused.body_index[60u * 160u + 80u] == 1);
}

static void test_near_trail_overlays_far_planet() {
    Project project = example_project();
    project.bodies.resize(1);
    project.bodies[0].radius = 0.8;
    project.view_extent = 3.0;
    SceneSnapshot snapshot;
    snapshot.bodies = {{"sun", {-1.0, 0.0}, {}, 1.0, 0.8}};
    snapshot.trails = {{"sun", {{1.0, -1.0}, {1.0, 1.0}}}};
    RenderConfig config;
    config.width = 160;
    config.height = 120;
    config.camera.yaw = 1.5707963267948966;
    const auto with_trail = render_scene(project, snapshot, config);
    snapshot.trails.clear();
    const auto without_trail = render_scene(project, snapshot, config);
    const std::size_t center = (60u * 160u + 80u) * 3u;
    assert(with_trail.image.rgb[center] != without_trail.image.rgb[center] ||
           with_trail.image.rgb[center + 1] != without_trail.image.rgb[center + 1]);
}

static void test_far_offscreen_trail_is_ignored() {
    Project project = example_project();
    project.bodies.resize(1);
    SceneSnapshot snapshot;
    snapshot.trails = {{"sun", {{1.0e12, 0.0}, {1.0e12 + 0.1, 0.0}}}};
    RenderConfig config;
    config.width = 160;
    config.height = 120;
    const auto with_trail = render_scene(project, snapshot, config);
    snapshot.trails.clear();
    assert(with_trail.image.rgb == render_scene(project, snapshot, config).image.rgb);
}

static void test_procedural_scene_and_sampling() {
    Project project = example_project();
    project.bodies.resize(1);
    project.bodies[0].radius = 0.6;
    project.view_extent = 2.0;
    SceneSnapshot snapshot = Simulation(project).snapshot();
    RenderConfig config;
    config.width = 144;
    config.height = 112;
    const RenderFrame original = render_scene(project, snapshot, config);
    Project tagged = project;
    tagged.bodies[0].kind = BodyKind::Venus;
    assert(render_scene(tagged, snapshot, config).image.rgb == original.image.rgb);

    Project recolored = project;
    recolored.bodies[0].appearance.primary = "#00ff00";
    assert(render_scene(recolored, snapshot, config).image.rgb != original.image.rgb);
    Project reseeded = project;
    reseeded.bodies[0].appearance.seed += 100;
    assert(render_scene(reseeded, snapshot, config).image.rgb != original.image.rgb);
    snapshot.time = 1.0;
    assert(render_scene(project, snapshot, config).image.rgb != original.image.rgb);

    snapshot.time = 0.0;
    Project ringed = project;
    ringed.bodies[0].appearance.ring_width = 0.8;
    assert(render_scene(ringed, snapshot, config).image.rgb != original.image.rgb);
    config.sampling = SamplingMode::SSAA4;
    const RenderFrame antialiased = render_scene(project, snapshot, config);
    assert(antialiased.image.rgb != original.image.rgb);
    assert(antialiased.body_index[56u * 144u + 72u] == 0);
    bool found_partial_edge = false;
    for (std::size_t pixel = 0; pixel < original.body_index.size(); ++pixel) {
        if (original.body_index[pixel] == -1 && antialiased.body_index[pixel] == 0) {
            found_partial_edge = true;
            break;
        }
    }
    assert(found_partial_edge);

    SceneSnapshot moving = Simulation(example_project()).snapshot();
    Simulation simulation(example_project());
    simulation.advance_to(0.3);
    moving = simulation.snapshot();
    SceneSnapshot no_trails = moving;
    no_trails.trails.clear();
    config.sampling = SamplingMode::Single;
    assert(render_scene(example_project(), moving, config).image.rgb !=
           render_scene(example_project(), no_trails, config).image.rgb);
}

static void test_msaa_trace_explains_partial_edge() {
    Project project = example_project();
    project.bodies.resize(1);
    project.bodies[0].radius = 0.6;
    project.view_extent = 2.0;
    const SceneSnapshot snapshot = Simulation(project).snapshot();
    RenderConfig config;
    config.width = 144;
    config.height = 112;
    config.sampling = SamplingMode::SSAA4;
    const RenderFrame ssaa = render_scene(project, snapshot, config);
    config.sampling = SamplingMode::MSAA4;
    const RenderFrame msaa = render_scene(project, snapshot, config);
    assert(msaa.shade_evaluations > 0);
    assert(msaa.shade_evaluations < ssaa.shade_evaluations);
    int edge_x = -1, edge_y = -1;
    for (int y = 0; y < config.height && edge_x < 0; ++y) {
        for (int x = 0; x < config.width; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * config.width + x;
            if (ssaa.body_index[pixel] == 0 && ssaa.image.rgb[pixel * 3] !=
                                                msaa.image.rgb[pixel * 3]) {
                edge_x = x;
                edge_y = y;
                break;
            }
        }
    }
    assert(edge_x >= 0);
    config.trace_x = edge_x;
    config.trace_y = edge_y;
    const RenderFrame inspected = render_scene(project, snapshot, config);
    check_pixel_process(*inspected.trace);
    assert(inspected.image.rgb == msaa.image.rgb);
    assert(inspected.trace.has_value());
    const auto& trace = *inspected.trace;
    assert(trace.samples.size() == 4);
    assert(trace.samples[0].position.x == 0.25 && trace.samples[0].position.y == 0.25);
    assert(trace.samples[1].position.x == 0.75 && trace.samples[1].position.y == 0.25);
    assert(trace.samples[2].position.x == 0.25 && trace.samples[2].position.y == 0.75);
    assert(trace.samples[3].position.x == 0.75 && trace.samples[3].position.y == 0.75);
    int covered = 0;
    for (const auto& sample : trace.samples) covered += sample.body_index == 0;
    assert(covered > 0 && covered < 4);
    for (int channel = 0; channel < 3; ++channel) {
        double linear = 0.0;
        for (const auto& sample : trace.samples) linear += sample.linear_rgb[channel] / 4.0;
        const auto resolved = static_cast<std::uint8_t>(std::round(
            std::pow(std::clamp(linear, 0.0, 1.0), 1.0 / 2.2) * 255.0));
        assert(resolved == trace.display_rgb[channel]);
        assert(resolved == inspected.image.rgb[
            (static_cast<std::size_t>(edge_y) * config.width + edge_x) * 3 + channel]);
    }
}

static void test_msaa_keeps_independent_sample_depth() {
    Project project = example_project();
    project.bodies.resize(2);
    project.bodies[0].id = "front";
    project.bodies[1].id = "back";
    project.bodies[0].radius = 0.4;
    project.bodies[1].radius = 1.0;
    project.view_extent = 3.0;
    SceneSnapshot scene;
    scene.bodies = {{"front", {1.0, 0.0}, {}, 1.0, 0.4},
                    {"back", {-1.0, 0.0}, {}, 1.0, 1.0}};
    RenderConfig config;
    config.width = 160;
    config.height = 120;
    config.camera.yaw = 1.5707963267948966;
    config.sampling = SamplingMode::MSAA4;
    const RenderFrame overview = render_scene(project, scene, config);
    bool mixed = false;
    for (int y = 52; y <= 68 && !mixed; ++y) {
        int edge = -1;
        for (int x = config.width / 2; x + 1 < config.width; ++x) {
            if (overview.body_index[static_cast<std::size_t>(y) * config.width + x] == 0 &&
                overview.body_index[static_cast<std::size_t>(y) * config.width + x + 1] == 1) {
                edge = x;
                break;
            }
        }
        if (edge < 0) continue;
        for (int x = edge; x <= edge + 1 && !mixed; ++x) {
            config.trace_x = x;
            config.trace_y = y;
            const RenderFrame frame = render_scene(project, scene, config);
            int front = 0, back = 0;
            for (const auto& sample : frame.trace->samples) {
                front += sample.body_index == 0;
                back += sample.body_index == 1;
            }
            mixed = front > 0 && back > 0;
        }
    }
    assert(mixed);
}

static void test_trace_identifies_trail_owner() {
    Project project = example_project();
    project.bodies.resize(1);
    project.view_extent = 2.0;
    SceneSnapshot scene;
    scene.bodies = {{"sun", {1.5, 1.5}, {}, 100.0, 0.25}};
    scene.trails = {{"sun", {{-0.8, 0.0}, {0.8, 0.0}}}};
    RenderConfig config;
    config.width = 144;
    config.height = 112;
    config.sampling = SamplingMode::MSAA4;
    const RenderFrame with_trail = render_scene(project, scene, config);
    scene.trails.clear();
    const RenderFrame without_trail = render_scene(project, scene, config);
    int trail_x = -1, trail_y = -1;
    for (int y = 0; y < config.height && trail_x < 0; ++y) {
        for (int x = 0; x < config.width; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * config.width + x;
            if (with_trail.image.rgb[pixel * 3] != without_trail.image.rgb[pixel * 3]) {
                trail_x = x;
                trail_y = y;
                break;
            }
        }
    }
    assert(trail_x >= 0);
    config.trace_x = trail_x;
    config.trace_y = trail_y;
    const RenderFrame inspected = render_scene(project, scene, config);
    assert(inspected.image.rgb == without_trail.image.rgb);
    scene.trails = {{"sun", {{-0.8, 0.0}, {0.8, 0.0}}}};
    const RenderFrame traced = render_scene(project, scene, config);
    assert(traced.trace.has_value());
    bool owned_trail = false;
    for (const TraceSample& sample : traced.trace->samples) {
        if (sample.surface == TraceSurface::Trail) {
            assert(sample.body_index == 0);
            assert(std::isfinite(sample.depth));
            owned_trail = true;
        }
    }
    assert(owned_trail);
}

static void test_translucent_ring_blends_over_planet_per_sample() {
    Project ringed = make_solar_system_project();
    ringed.view_extent = 1.5;
    ringed.bodies[6].design.ring_width = 0.1;
    Project ringless = ringed;
    ringless.bodies[6].design.ring_width = 0.0;
    SceneSnapshot scene;
    scene.bodies = {{"saturn", {0.0, 0.0}, {},
                     ringed.bodies[6].mass, ringed.bodies[6].radius}};
    RenderConfig config;
    config.width = 240;
    config.height = 180;
    config.sampling = SamplingMode::MSAA4;
    const RenderFrame ring_frame = render_scene(ringed, scene, config);
    const RenderFrame body_frame = render_scene(ringless, scene, config);
    int x_found = -1, y_found = -1;
    for (int y = 0; y < config.height && x_found < 0; ++y) {
        for (int x = 0; x < config.width; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * config.width + x;
            if (ring_frame.body_index[pixel] == 6 && ring_frame.surface_uv[pixel].x < 0.0 &&
                body_frame.body_index[pixel] == 6 && body_frame.surface_uv[pixel].x >= 0.0) {
                x_found = x;
                y_found = y;
                break;
            }
        }
    }
    assert(x_found >= 0);
    config.trace_x = x_found;
    config.trace_y = y_found;
    const auto ring_trace = *render_scene(ringed, scene, config).trace;
    const auto body_trace = *render_scene(ringless, scene, config).trace;
    int overlap = 0;
    for (std::size_t sample = 0; sample < ring_trace.samples.size(); ++sample) {
        if (ring_trace.samples[sample].surface != TraceSurface::Ring ||
            body_trace.samples[sample].surface != TraceSurface::Body) continue;
        ++overlap;
        for (int channel = 0; channel < 3; ++channel) {
            assert(std::abs(ring_trace.samples[sample].linear_rgb[channel] -
                            body_trace.samples[sample].linear_rgb[channel]) <= 0.12);
        }
    }
    assert(overlap > 0);
}

static void test_near_clipped_ring_keeps_visible_arc() {
    Project project = example_project();
    project.bodies.resize(1);
    project.bodies[0].radius = 0.02;
    project.bodies[0].appearance.ring_width = 2.0;
    project.view_extent = 0.1;
    project.display_scale = 1.0;
    SceneSnapshot scene;
    scene.bodies = {{"sun", {0.0, 0.0}, {}, 100.0, 0.02}};
    RenderConfig config;
    config.width = 120;
    config.height = 100;
    config.camera.zoom = 0.245;
    for (const SamplingMode mode : {SamplingMode::Single, SamplingMode::SSAA4,
                                    SamplingMode::MSAA4}) {
        config.sampling = mode;
        const RenderFrame frame = render_scene(project, scene, config);
        int ring_x = -1, ring_y = -1;
        for (int y = 16; y <= 27 && ring_x < 0; ++y) {
            for (int x = 55; x <= 64; ++x) {
                const std::size_t pixel = static_cast<std::size_t>(y) * config.width + x;
                if (frame.body_index[pixel] == 0 && frame.surface_uv[pixel].x < 0.0) {
                    ring_x = x;
                    ring_y = y;
                    break;
                }
            }
        }
        assert(ring_x >= 0);
        config.trace_x = ring_x;
        config.trace_y = ring_y;
        const RenderFrame traced = render_scene(project, scene, config);
        assert(traced.image.rgb == frame.image.rgb);
        check_pixel_process(*traced.trace);
        int ring_samples = 0;
        for (const TraceSample& sample : traced.trace->samples) {
            if (sample.surface != TraceSurface::Ring) continue;
            assert(sample.body_index == 0);
            assert(std::isfinite(sample.depth) && sample.depth >= 0.05 - 1e-9);
            ++ring_samples;
        }
        assert(ring_samples > 0);
        config.trace_x = config.trace_y = -1;
    }
}

static void test_near_clipped_trail_keeps_visible_segment() {
    Project project = example_project();
    project.bodies.resize(1);
    project.view_extent = 0.1;
    project.display_scale = 1.0;
    SceneSnapshot scene;
    scene.trails = {{"sun", {{0.0, 0.0}, {0.08, 0.0}}}};
    RenderConfig config;
    config.width = 400;
    config.height = 100;
    config.camera.yaw = 1.5707963267948966;
    config.camera.zoom = 0.245;
    config.sampling = SamplingMode::MSAA4;
    const RenderFrame frame = render_scene(project, scene, config);
    int trail_x = -1;
    for (int y = 48; y <= 51 && trail_x < 0; ++y) {
        for (int x = 270; x <= 325; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * config.width + x;
            if (frame.body_index[pixel] == 0) {
                trail_x = x;
                break;
            }
        }
    }
    assert(trail_x >= 0);
    assert(frame.body_index[50u * 400u + 300u] == 0);
    config.trace_x = 300;
    config.trace_y = 50;
    const RenderFrame traced = render_scene(project, scene, config);
    bool visible = false;
    constexpr double half_fov = 0.41421356237309503;
    const double camera_distance = project.view_extent * 1.35 / half_fov * config.camera.zoom;
    const double image_scale = config.height / (2.0 * half_fov);
    const double start_x = config.width * 0.5 + 0.05 * image_scale / camera_distance;
    const double end_x = config.width * 0.5 + 0.05 * image_scale / 0.05;
    for (const TraceSample& sample : traced.trace->samples) {
        if (sample.surface != TraceSurface::Trail) continue;
        assert(sample.body_index == 0);
        assert(std::isfinite(sample.depth) && sample.depth >= 0.05 - 1e-9);
        const double screen_x = config.trace_x + sample.position.x;
        const double t = std::clamp((screen_x - start_x) / (end_x - start_x), 0.0, 1.0);
        const double expected_depth = 1.0 / ((1.0 - t) / camera_distance + t / 0.05);
        assert(std::abs(sample.depth - expected_depth) < 1e-6);
        visible = true;
    }
    assert(visible);
    scene.trails = {{"sun", {{0.05, 0.0}, {0.08, 0.0}}}};
    const RenderFrame hidden = render_scene(project, scene, config);
    scene.trails.clear();
    assert(hidden.image.rgb == render_scene(project, scene, config).image.rgb);

    Project occlusion_project = project;
    occlusion_project.bodies.push_back(occlusion_project.bodies[0]);
    occlusion_project.bodies[1].id = "occluder";
    occlusion_project.bodies[1].name = "Occluder";
    occlusion_project.bodies[1].radius = 0.225;
    SceneSnapshot occlusion_scene;
    occlusion_scene.bodies = {{"occluder", {-0.2, 0.0}, {}, 1.0, 0.225}};
    const RenderFrame surface = render_scene(occlusion_project, occlusion_scene, config);
    occlusion_scene.trails = {{"sun", {{0.0, 0.0}, {0.08, 0.0}}}};
    const RenderFrame combined = render_scene(occlusion_project, occlusion_scene, config);
    bool front_trail = false;
    for (std::size_t index = 0; index < combined.trace->samples.size(); ++index) {
        const TraceSample& body_sample = surface.trace->samples[index];
        const TraceSample& trail_sample = combined.trace->samples[index];
        if (traced.trace->samples[index].surface != TraceSurface::Trail) continue;
        if (body_sample.surface != TraceSurface::Body) continue;
        if (body_sample.depth <= 0.0603 || body_sample.depth >= 0.0634) continue;
        assert(trail_sample.surface == TraceSurface::Trail);
        front_trail = true;
    }
    assert(front_trail);
}

static void test_comparison_uses_matching_scene_and_trace_coordinates() {
    Project project = example_project();
    SceneSnapshot scene = Simulation(project).snapshot();
    RenderConfig config;
    config.width = 96;
    config.height = 72;
    config.trace_x = 96 + 48;
    config.trace_y = 36;
    const RenderFrame comparison = render_comparison_scene(
        project, scene, config, SamplingMode::Single, SamplingMode::MSAA4);
    assert(comparison.image.width == 192 && comparison.image.height == 72);
    assert(comparison.trace.has_value());
    assert(comparison.trace->x == 144 && comparison.trace->y == 36);
    assert(comparison.trace->samples.size() == 4);
    RenderConfig one = config;
    one.trace_x = -1;
    one.trace_y = -1;
    one.sampling = SamplingMode::Single;
    const RenderFrame left = render_scene(project, scene, one);
    one.sampling = SamplingMode::MSAA4;
    const RenderFrame right = render_scene(project, scene, one);
    assert(comparison.sample_surface_variation.size() ==
           static_cast<std::size_t>(comparison.image.width) * comparison.image.height);
    for (int y = 0; y < config.height; ++y) {
        for (int x = 0; x < config.width; ++x) {
            const std::size_t source = static_cast<std::size_t>(y) * config.width + x;
            const std::size_t output = static_cast<std::size_t>(y) * comparison.image.width + x;
            for (int channel = 0; channel < 3; ++channel) {
                assert(comparison.image.rgb[output * 3 + channel] == left.image.rgb[source * 3 + channel]);
                assert(comparison.image.rgb[(output + config.width) * 3 + channel] ==
                       right.image.rgb[source * 3 + channel]);
            }
            assert(comparison.body_index[output] == left.body_index[source]);
            assert(comparison.body_index[output + config.width] == right.body_index[source]);
            assert(comparison.sample_surface_variation[output] ==
                   left.sample_surface_variation[source]);
            assert(comparison.sample_surface_variation[output + config.width] ==
                   right.sample_surface_variation[source]);
        }
    }
    assert(comparison.shade_evaluations == left.shade_evaluations + right.shade_evaluations);
    assert(comparison.panel_shade_evaluations[0] == left.shade_evaluations);
    assert(comparison.panel_shade_evaluations[1] == right.shade_evaluations);
}

static void test_16x_reference_uses_independent_grid_samples() {
    Project project = example_project();
    project.bodies.resize(1);
    project.bodies[0].radius = 0.6;
    project.view_extent = 2.0;
    const SceneSnapshot scene = Simulation(project).snapshot();
    RenderConfig config;
    config.width = 96;
    config.height = 72;
    config.sampling = SamplingMode::SSAA16;
    const RenderFrame overview = render_scene(project, scene, config);
    assert(overview.sample_surface_variation.size() == 96u * 72u);
    int edge_x = -1, edge_y = -1;
    for (int y = 0; y < config.height && edge_x < 0; ++y) {
        for (int x = 0; x < config.width; ++x) {
            if (overview.sample_surface_variation[static_cast<std::size_t>(y) * config.width + x]) {
                edge_x = x;
                edge_y = y;
                break;
            }
        }
    }
    assert(edge_x >= 0);
    config.trace_x = edge_x;
    config.trace_y = edge_y;
    const RenderFrame traced = render_scene(project, scene, config);
    assert(traced.image.rgb == overview.image.rgb);
    assert(traced.trace->samples.size() == 16);
    int covered = 0;
    for (int sample = 0; sample < 16; ++sample) {
        const TraceSample& item = traced.trace->samples[sample];
        assert(item.position.x == (sample % 4 + 0.5) / 4.0);
        assert(item.position.y == (sample / 4 + 0.5) / 4.0);
        covered += item.body_index == 0;
    }
    assert(covered > 0 && covered < 16);
    for (int channel = 0; channel < 3; ++channel) {
        double linear = 0.0;
        for (const TraceSample& item : traced.trace->samples) {
            linear += item.linear_rgb[channel] / 16.0;
        }
        const auto expected = static_cast<std::uint8_t>(std::round(
            std::pow(std::clamp(linear, 0.0, 1.0), 1.0 / 2.2) * 255.0));
        assert(traced.trace->display_rgb[channel] == expected);
    }
    config.trace_x = config.trace_y = -1;
    config.width = 1024;
    config.height = 512;
    bool rejected = false;
    try { render_scene(project, scene, config); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);

    Project layered = example_project();
    layered.bodies.resize(2);
    layered.bodies[0].id = "front";
    layered.bodies[1].id = "back";
    layered.bodies[0].radius = 0.4;
    layered.bodies[1].radius = 1.0;
    layered.view_extent = 3.0;
    SceneSnapshot overlap;
    overlap.bodies = {{"front", {1.0, 0.0}, {}, 1.0, 0.4},
                      {"back", {-1.0, 0.0}, {}, 1.0, 1.0}};
    config.width = 160;
    config.height = 120;
    config.camera.yaw = 1.5707963267948966;
    const RenderFrame layered_overview = render_scene(layered, overlap, config);
    bool mixed_depth = false;
    for (int y = 52; y <= 68 && !mixed_depth; ++y) {
        for (int x = 60; x < 100 && !mixed_depth; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * config.width + x;
            if (layered_overview.body_index[pixel] ==
                layered_overview.body_index[pixel + 1]) continue;
            for (int offset = 0; offset <= 1 && !mixed_depth; ++offset) {
                config.trace_x = x + offset;
                config.trace_y = y;
                const RenderFrame frame = render_scene(layered, overlap, config);
                int front = 0, back = 0;
                for (const TraceSample& sample : frame.trace->samples) {
                    front += sample.body_index == 0;
                    back += sample.body_index == 1;
                }
                mixed_depth = front > 0 && back > 0;
            }
        }
    }
    assert(mixed_depth);
}

static void test_benchmark_scenes_and_linear_error() {
    const Project catalog = make_solar_system_project();
    const AABenchmarkScene earth = make_aa_benchmark_scene(catalog, AAScene::Earth, 96, 72);
    const AABenchmarkScene saturn = make_aa_benchmark_scene(catalog, AAScene::Saturn, 96, 72);
    const AABenchmarkScene orbit = make_aa_benchmark_scene(catalog, AAScene::Orbit, 96, 72);
    assert(earth.name == "earth" && earth.snapshot.bodies.size() == 1);
    assert(earth.snapshot.bodies[0].id == "earth" && earth.snapshot.trails.empty());
    assert(saturn.name == "saturn" && saturn.snapshot.bodies.size() == 1);
    assert(saturn.snapshot.bodies[0].id == "saturn");
    assert(orbit.name == "orbit" && orbit.snapshot.bodies.empty());
    assert(orbit.snapshot.reference_trails.size() == 1);
    assert(orbit.snapshot.reference_trails[0].points.size() > 64);
    for (const AABenchmarkScene* scene : {&earth, &saturn, &orbit}) {
        RenderConfig config = scene->config;
        config.sampling = SamplingMode::SSAA16;
        const RenderFrame reference = render_scene(scene->project, scene->snapshot, config);
        const auto mask = make_aa_edge_mask(reference);
        assert(std::count(mask.begin(), mask.end(), 1) > 0);
        assert(measure_aa_error(reference.image, reference.image, mask).edge_mae_linear == 0.0);
    }
    RenderConfig earth_config = earth.config;
    earth_config.sampling = SamplingMode::SSAA16;
    const auto cloudy_mask = make_aa_edge_mask(
        render_scene(earth.project, earth.snapshot, earth_config));
    Project clear_earth = earth.project;
    clear_earth.bodies[3].design.cloud_opacity = 0.0;
    const auto clear_mask = make_aa_edge_mask(
        render_scene(clear_earth, earth.snapshot, earth_config));
    assert(std::count(cloudy_mask.begin(), cloudy_mask.end(), 1) <=
           2 * std::count(clear_mask.begin(), clear_mask.end(), 1));

    RenderFrame reference;
    reference.image = {3, 3, std::vector<std::uint8_t>(27, 0)};
    reference.sample_surface_variation.assign(9, 0);
    reference.sample_surface_variation[4] = 1;
    const auto mask = make_aa_edge_mask(reference);
    assert(std::count(mask.begin(), mask.end(), 1) == 9);
    Image candidate = reference.image;
    candidate.rgb[0] = candidate.rgb[1] = candidate.rgb[2] = 255;
    const AAError error = measure_aa_error(candidate, reference.image, mask);
    assert(std::abs(error.edge_mae_linear - 1.0 / 9.0) < 1e-12);
    assert(std::abs(error.full_mae_linear - 1.0 / 9.0) < 1e-12);
    assert(error.edge_pixels == 9);
    bool rejected = false;
    try { measure_aa_error(candidate, reference.image, std::vector<std::uint8_t>(9, 0)); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    candidate.width = 2;
    rejected = false;
    try { measure_aa_error(candidate, reference.image, mask); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}

static void test_png_writer_creates_valid_rgb_dimensions() {
    const Image image{2, 1, {255, 0, 0, 0, 255, 0}};
    const auto path = std::filesystem::temp_directory_path() / "planet-aa-png-test.png";
    write_png(path.string(), image);
    std::ifstream input(path, std::ios::binary);
    assert(input.good());
    unsigned char header[24]{};
    input.read(reinterpret_cast<char*>(header), sizeof(header));
    assert(input.gcount() == sizeof(header));
    const unsigned char signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    assert(std::equal(std::begin(signature), std::end(signature), header));
    assert(header[16] == 0 && header[17] == 0 && header[18] == 0 && header[19] == 2);
    assert(header[20] == 0 && header[21] == 0 && header[22] == 0 && header[23] == 1);
    input.close();
    std::filesystem::remove(path);
    bool rejected = false;
    try { write_png(path.string(), {2, 1, {255}}); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { write_png((path / "missing.png").string(), image); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    const auto unicode_path = std::filesystem::temp_directory_path() /
        std::filesystem::u8path("planet-aa-\xE6\xB5\x8B\xE8\xAF\x95.png");
    write_png(unicode_path.u8string(), image);
    assert(std::filesystem::exists(unicode_path));
    std::filesystem::remove(unicode_path);
}

int main() {
    test_16x_reference_uses_independent_grid_samples();
    test_benchmark_scenes_and_linear_error();
    test_png_writer_creates_valid_rgb_dimensions();
    test_project_validation();
    test_project_round_trip();
    test_solar_system_catalog();
    test_solar_design_round_trip();
    test_anchored_shape_controls_persist_and_reset();
    test_solar_texture_and_relief();
    test_every_solar_body_has_editable_shape();
    test_anchored_feature_shape_sampling();
    test_local_sculpt_shape_and_persistence();
    test_local_sculpt_undo_is_per_body();
    test_sculpt_pick_uses_visible_sphere_only();
    test_sculpt_stroke_requires_surface_press_and_fills_drag_path();
    test_earth_clouds_cover_sculpted_surface();
    test_venus_sculpt_layers_do_not_cross();
    test_anchored_feature_controls_change_rendered_layers();
    test_venus_cloud_rim_has_single_opacity();
    test_venus_transparency_is_order_independent();
    test_venus_cloud_blends_over_trail();
    test_every_solar_body_relief_changes_rendered_geometry();
    test_specialized_solar_overrides();
    test_exposed_design_controls_change_render();
    test_editor_session();
    test_orbit_reference_and_seek();
    test_orbit_collision_bookmarks_survive_rewind();
    test_reference_trail_is_rendered_and_identified();
    test_solar_session_reset();
    test_added_planets_stay_distinct();
    test_simulation();
    test_demo_collision_preserves_saved_design();
    test_demo_collision_ignores_unrelated_edited_orbits();
    test_demo_presets_require_fixed_solar_system();
    test_initial_overlap();
    test_collision();
    test_equal_mass_collision_uses_catalog_order();
    test_collision_preserves_momentum();
    test_render();
    test_perspective_render_contract();
    test_perspective_depth();
    test_camera_can_focus_a_planet();
    test_near_trail_overlays_far_planet();
    test_far_offscreen_trail_is_ignored();
    test_procedural_scene_and_sampling();
    test_msaa_trace_explains_partial_edge();
    test_msaa_keeps_independent_sample_depth();
    test_trace_identifies_trail_owner();
    test_translucent_ring_blends_over_planet_per_sample();
    test_near_clipped_ring_keeps_visible_arc();
    test_near_clipped_trail_keeps_visible_segment();
    test_comparison_uses_matching_scene_and_trace_coordinates();
}
