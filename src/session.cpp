#include "planet_sandbox/session.h"
#include "planet_sandbox/solar_system.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace planet_sandbox {

SandboxSession::SandboxSession(Project project) : project_(std::move(project)) {
    validate_project(project_);
    simulation_.reset(new Simulation(project_));
}

void SandboxSession::cache_checkpoint() {
    if(mode_!=SessionMode::Simulate) return;
    const double time=simulation_->snapshot().time;
    for(const auto& checkpoint:checkpoints_) if(std::abs(checkpoint.time-time)<.5) return;
    Checkpoint checkpoint{time,simulation_->clone(),reference_simulation_?reference_simulation_->clone():nullptr};
    // Keep the initial state, and bound both integrator and trail-history memory.
    if(checkpoints_.size()>=16) checkpoints_.erase(checkpoints_.begin()+1);
    checkpoints_.push_back(std::move(checkpoint));
}

std::optional<SceneSnapshot> SandboxSession::reference_snapshot() const {
    if (!reference_simulation_) return std::nullopt;
    return reference_simulation_->snapshot();
}

void SandboxSession::record_events() {
    for (const CollisionEvent& event : simulation_->snapshot().events) {
        const auto found = std::find_if(observed_events_.begin(), observed_events_.end(),
            [&](const CollisionEvent& seen) {
                return seen.survivor == event.survivor && seen.absorbed == event.absorbed;
            });
        if (found == observed_events_.end()) observed_events_.push_back(event);
    }
}

void SandboxSession::replace_project(Project project) {
    if (mode_ != SessionMode::Design) throw std::logic_error("Return to Design before editing");
    validate_project(project);
    auto next = std::make_unique<Simulation>(project);
    project_ = std::move(project);
    simulation_ = std::move(next);
    checkpoints_.clear();last_seek_origin_=0;
    ++project_revision_;
}

void SandboxSession::add_planet() {
    if (mode_ != SessionMode::Design) throw std::logic_error("Return to Design before editing");
    if (project_.solar_system) throw std::logic_error("The solar-system catalog is fixed");
    if (project_.bodies.size() >= 32) throw std::invalid_argument("Project is full");

    Project next = project_;
    int number = 1;
    std::string id;
    do {
        id = "planet-" + std::to_string(number++);
    } while (std::any_of(next.bodies.begin(), next.bodies.end(),
                         [&](const BodySpec& existing) { return existing.id == id; }));

    BodySpec body;
    body.id = id;
    body.name = "Planet " + std::to_string(number - 1);
    body.appearance.seed = static_cast<std::uint32_t>(number * 41);
    const BodySpec& anchor = next.bodies.front();
    const double base_radius = std::max({next.view_extent * 0.5,
                                         anchor.radius + body.radius + 0.4, 0.6});
    constexpr double golden_angle = 2.399963229728653;
    bool placed = false;
    for (int attempt = 0; attempt < 256; ++attempt) {
        const double radius = base_radius + (attempt / 32) * std::max(0.4, base_radius * 0.35);
        const double angle = attempt * golden_angle;
        const WorldVector candidate{anchor.position.x + radius * std::cos(angle),
                             anchor.position.y + radius * std::sin(angle)};
        const bool overlaps = std::any_of(next.bodies.begin(), next.bodies.end(),
                                          [&](const BodySpec& existing) {
            const double dx = candidate.x - existing.position.x;
            const double dy = candidate.y - existing.position.y;
            const double clearance = body.radius + existing.radius + 0.05;
            return dx * dx + dy * dy <= clearance * clearance;
        });
        if (overlaps) continue;
        body.position = candidate;
        const double speed = std::sqrt(anchor.mass / radius);
        body.velocity = {anchor.velocity.x - speed * std::sin(angle),
                         anchor.velocity.y + speed * std::cos(angle)};
        next.view_extent = std::max(next.view_extent,
                                    std::max(std::abs(candidate.x), std::abs(candidate.y)) +
                                        body.radius * next.display_scale + 0.3);
        placed = true;
        break;
    }
    if (!placed) throw std::runtime_error("Could not place a new planet");
    next.bodies.push_back(std::move(body));
    replace_project(std::move(next));
}

void SandboxSession::reset_body_design(BodyKind kind) {
    if (mode_ != SessionMode::Design) throw std::logic_error("Return to Design before editing");
    if (!project_.solar_system || kind == BodyKind::Custom) {
        throw std::invalid_argument("Reset requires a solar-system body");
    }
    Project next = project_;
    auto found = std::find_if(next.bodies.begin(), next.bodies.end(),
                              [&](const BodySpec& body) { return body.kind == kind; });
    if (found == next.bodies.end()) throw std::invalid_argument("Body is not in the project");
    found->design = baseline_design(kind);
    replace_project(std::move(next));
}

void SandboxSession::begin_simulation() {
    simulation_ = std::make_unique<Simulation>(project_);
    reference_simulation_ = project_.solar_system
        ? std::make_unique<Simulation>(project_.epoch_jd > 0 ? make_observed_year_project() : make_solar_system_project()) : nullptr;
    observed_events_.clear();
    record_events();
    mode_ = SessionMode::Simulate;
    playing_ = true;
    checkpoints_.clear();last_seek_origin_=0;cache_checkpoint();
}

void SandboxSession::return_to_design() {
    simulation_ = std::make_unique<Simulation>(project_);
    reference_simulation_.reset();
    observed_events_.clear();
    mode_ = SessionMode::Design;
    playing_ = false;
    checkpoints_.clear();last_seek_origin_=0;
}

void SandboxSession::reset_simulation() {
    simulation_ = std::make_unique<Simulation>(project_);
    reference_simulation_ = mode_ == SessionMode::Simulate && project_.solar_system
        ? std::make_unique<Simulation>(project_.epoch_jd > 0 ? make_observed_year_project() : make_solar_system_project()) : nullptr;
    observed_events_.clear();
    record_events();
    playing_ = false;
    checkpoints_.clear();last_seek_origin_=0;cache_checkpoint();
}

void SandboxSession::set_playing(bool playing) {
    if (mode_ != SessionMode::Simulate && playing) {
        throw std::logic_error("Start simulation before playing");
    }
    playing_ = playing;
}

void SandboxSession::set_speed(double speed) {
    if (!std::isfinite(speed) || speed < 0.1 || speed > 20.0) {
        throw std::invalid_argument("Speed must be between 0.1 and 20");
    }
    speed_ = speed;
}

void SandboxSession::advance(double real_seconds) {
    if (!std::isfinite(real_seconds) || real_seconds < 0.0) {
        throw std::invalid_argument("Elapsed time must be nonnegative and finite");
    }
    if (!playing_) return;
    const double time = std::min(simulation_time_limit(project_), simulation_->snapshot().time + std::min(real_seconds, 0.1) * speed_);
    if (time >= simulation_time_limit(project_)) playing_ = false;
    simulation_->advance_to(time);
    if (reference_simulation_) reference_simulation_->advance_to(time);
    record_events();
    cache_checkpoint();
}

void SandboxSession::step(double simulated_seconds) {
    if (mode_ != SessionMode::Simulate) throw std::logic_error("Start simulation before stepping");
    if (!std::isfinite(simulated_seconds) || simulated_seconds <= 0.0) {
        throw std::invalid_argument("Step must be positive and finite");
    }
    const double time = std::min(simulation_time_limit(project_), simulation_->snapshot().time + simulated_seconds);
    simulation_->advance_to(time);
    if (reference_simulation_) reference_simulation_->advance_to(time);
    record_events();
    cache_checkpoint();
}

void SandboxSession::seek(double simulated_time) {
    if (mode_ != SessionMode::Simulate) throw std::logic_error("Start simulation before seeking");
    if (!std::isfinite(simulated_time) || simulated_time < 0.0 || simulated_time > simulation_time_limit(project_)) {
        throw std::invalid_argument("Seek time must be finite and between 0 and 100000");
    }
    playing_ = false;
    const double current=simulation_->snapshot().time;
    const Checkpoint* nearest=nullptr;
    for(const auto& checkpoint:checkpoints_)
        if(checkpoint.time<=simulated_time && (!nearest || checkpoint.time>nearest->time)) nearest=&checkpoint;
    if(nearest && (simulated_time<current || nearest->time>current)) {
        auto restored=nearest->simulation->clone();
        auto reference=nearest->reference?nearest->reference->clone():nullptr;
        simulation_=std::move(restored);reference_simulation_=std::move(reference);
    } else if(simulated_time<current) {
        simulation_=std::make_unique<Simulation>(project_);
        if(reference_simulation_) reference_simulation_=std::make_unique<Simulation>(project_.epoch_jd > 0 ? make_observed_year_project() : make_solar_system_project());
    }
    last_seek_origin_=simulation_->snapshot().time;
    simulation_->advance_to(simulated_time);
    if (reference_simulation_) reference_simulation_->advance_to(simulated_time);
    record_events();
    cache_checkpoint();
}

} // namespace planet_sandbox
