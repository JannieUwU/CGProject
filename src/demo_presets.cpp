#include "planet_sandbox/demo_presets.h"
#include "planet_sandbox/solar_system.h"
#include "planet_sandbox/impact_visual.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace planet_sandbox {

void DemoPresets::activate(DemoPreset preset, const Project& source) {
    if (preset == DemoPreset::None) { clear(); return; }
    if (!source.solar_system) throw std::invalid_argument("Demos require the fixed solar system");
    validate_project(source);
    if (preset == DemoPreset::Collision) {
        Project copy = source.epoch_jd > 0 ? make_solar_system_project() : source;
        if (source.epoch_jd > 0) {
            // Collision demo uses its own teaching units, not dated ephemeris units.
            for (std::size_t i = 0; i < copy.bodies.size(); ++i)
                copy.bodies[i].design = source.bodies[i].design;
        }
        BodySpec& earth = copy.bodies[3];
        BodySpec& mars = copy.bodies[4];
        earth.position = {4.0, 0.0};
        earth.velocity = {0.0, 0.0};
        mars.position = {5.2, 0.0};
        mars.velocity = {-0.85, 0.0};
        copy.view_extent = 1.5;
        Project physics = copy;
        physics.solar_system = false;
        physics.bodies = {earth, mars};
        auto simulation = std::make_unique<Simulation>(physics);
        collision_project_ = std::move(copy);
        collision_physics_ = std::move(physics);
        collision_simulation_ = std::move(simulation);
        playing_ = true;
    } else {
        collision_simulation_.reset();
        collision_project_ = {};
        collision_physics_ = {};
        playing_ = false;
    }
    active_ = preset;
}

void DemoPresets::clear() {
    active_ = DemoPreset::None;
    playing_ = false;
    collision_simulation_.reset();
    collision_project_ = {};
    collision_physics_ = {};
}

const Project& DemoPresets::project(const Project& source) const {
    return active_ == DemoPreset::Collision ? collision_project_ : source;
}

SceneSnapshot DemoPresets::snapshot(const SceneSnapshot& normal) const {
    return collision_simulation_ ? collision_simulation_->snapshot() : normal;
}

void DemoPresets::set_playing(bool playing) {
    if (active_ != DemoPreset::Collision && playing) {
        throw std::logic_error("Start the collision demo before playing");
    }
    playing_ = playing;
}

void DemoPresets::advance(double real_seconds, double speed) {
    if (!std::isfinite(real_seconds) || real_seconds < 0.0 ||
        !std::isfinite(speed) || speed <= 0.0) {
        throw std::invalid_argument("Demo time and speed must be finite and positive");
    }
    if (!playing_) return;
    if (real_seconds == 0.0) return;
    step(std::min(real_seconds, 0.1) * speed);
}

void DemoPresets::step(double simulated_seconds) {
    if (!collision_simulation_) throw std::logic_error("Start the collision demo before stepping");
    if (!std::isfinite(simulated_seconds) || simulated_seconds <= 0.0) {
        throw std::invalid_argument("Demo step must be finite and positive");
    }
    const auto before=collision_simulation_->snapshot();
    double target=before.time+simulated_seconds;
    if(!before.events.empty()) target=std::min(target,before.events.front().time+impact_visual_lifetime);
    if(target>before.time) collision_simulation_->advance_to(target);
    const auto after=collision_simulation_->snapshot();
    if(!after.events.empty() && after.time>=after.events.front().time+impact_visual_lifetime) playing_=false;
}

void DemoPresets::reset() {
    if (!collision_simulation_) throw std::logic_error("Start the collision demo before resetting");
    collision_simulation_ = std::make_unique<Simulation>(collision_physics_);
    playing_ = false;
}

} // namespace planet_sandbox
