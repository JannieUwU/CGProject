#include "planet_sandbox/solar_system.h"
#include "planet_sandbox/session.h"
#include "planet_sandbox/renderer.h"
#include "planet_sandbox/demo_presets.h"
#include "planet_sandbox/impact_visual.h"
#include <nlohmann/json.hpp>
#ifdef NDEBUG
#error Assertions must be enabled
#endif
#include <cassert>
#include <cmath>
#include <iostream>
#include <sstream>
#include <limits>

using namespace planet_sandbox;
namespace {
#include "../src/observed_year_data.inc"
}

int main() {
    const auto project = make_observed_year_project();
    validate_project(project);
    // Dated files must be rejected before a reference simulation can overrun its year.
    std::ostringstream valid_document;
    write_project(valid_document, project);
    for (double duration : {365.5, 366.0, 100000.0}) {
        auto document = nlohmann::json::parse(valid_document.str());
        document["duration_days"] = duration;
        std::istringstream file(document.dump());
        bool duration_rejected = false;
        try { read_project(file); }
        catch (const std::invalid_argument&) { duration_rejected = true; }
        assert(duration_rejected);
    }
    assert(simulation_calendar_date(project, 0) == "2025-11-28");
    assert(simulation_calendar_date(project, 365) == "2026-11-28");
    assert(simulation_calendar_date(project, 92) == "2026-02-28");
    assert(project.bodies[1].position.z != 0 && project.bodies[1].velocity.z != 0);
    std::ostringstream saved;
    write_project(saved, project);
    std::istringstream input(saved.str());
    const auto restored = read_project(input);
    assert(restored.epoch_jd == project.epoch_jd && restored.duration_days == 365);
    assert(restored.gravitational_constant == project.gravitational_constant);
    assert(restored.bodies[1].position.z == project.bodies[1].position.z);
    auto invalid = project;
    invalid.bodies[1].velocity.z = std::numeric_limits<double>::quiet_NaN();
    bool rejected = false;
    try { validate_project(invalid); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    // Old two-component files remain readable and old scenes remain planar.
    std::istringstream legacy(R"({"bodies":[{"id":"a","mass":1,"radius":0.1,"position":[0,0],"velocity":[0,0]}]})");
    const auto old = read_project(legacy);
    assert(old.bodies[0].position.z == 0 && old.gravitational_constant == 1);

    Simulation simulation(project);
    double max_error_km = 0;
    for (int sample = 0; sample < 5; ++sample) {
        simulation.advance_to(observed_reference_days[sample]);
        const auto snapshot = simulation.snapshot();
        assert(snapshot.bodies.size() == 9 && snapshot.events.empty());
        for (int body = 0; body < 9; ++body) {
            const auto actual = snapshot.bodies[body].position;
            const auto& expected = observed_reference_positions[sample][body];
            const double dx = actual.x - expected[0], dy = actual.y - expected[1], dz = actual.z - expected[2];
            const double error = std::sqrt(dx*dx + dy*dy + dz*dz) * 149597870.7;
            max_error_km = std::max(max_error_km, error);
            std::cout << observed_reference_days[sample] << "," << snapshot.bodies[body].id << "," << error << " km\n";
            // A nine-body Newtonian model omits moons, asteroids and relativity.
            // This is a visualization accuracy bound, not an ephemeris claim.
            assert(error < 1000.0);
        }
    }
    assert(simulation.snapshot().trails[1].points.size() > 500);
    std::cout << "Maximum quarterly position error: " << max_error_km << " km\n";
    SandboxSession session(project);
    session.begin_simulation();
    session.seek(300);
    const auto first = session.snapshot().bodies[1].position;
    session.seek(100);
    session.seek(300);
    const auto again = session.snapshot().bodies[1].position;
    assert(std::abs(first.z - again.z) < 1e-10);
    session.seek(364.9);
    session.set_speed(20);
    session.set_playing(true);
    session.advance(.1);
    assert(session.snapshot().time == 365 && !session.playing());
    session.step(1);
    assert(session.snapshot().time == 365);
    rejected = false;
    try { session.seek(365.01); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);

    // Bodies at identical x/y but different z must not collide at initialization.
    Project separated;
    separated.bodies = {{"a","A",1,.1,{0,0,0},{0,0,0},{}},
                        {"b","B",1,.1,{0,0,1},{0,0,0},{}}};
    assert(Simulation(separated).snapshot().bodies.size() == 2);
    separated.bodies[1].position.z = .15;
    const auto merged = Simulation(separated).snapshot();
    assert(merged.bodies.size() == 1 && merged.events.size() == 1);
    assert(merged.events[0].contact && std::abs(merged.events[0].contact->normal.z) > .99);
    const auto sprites = impact_sprites(separated, merged);
    assert(!sprites.empty() && std::abs(sprites.front().z) > .01);
    DemoPresets demos;
    demos.activate(DemoPreset::Collision, project);
    assert(demos.project(project).epoch_jd == 0);
    assert(project.epoch_jd == observed_epoch_jd);
}
