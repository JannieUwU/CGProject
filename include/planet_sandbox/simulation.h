#pragma once

#include "planet_sandbox/project.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace planet_sandbox {

struct BodyState {
    std::string id;
    WorldVector position;
    WorldVector velocity;
    double mass = 0.0;
    double radius = 0.0;
};

struct CollisionContact {
    WorldVector position, normal, velocity, survivor_position;
    double relative_speed = 0.0;
    double survivor_radius = 0.0;
    double absorbed_radius = 0.0;
};

struct CollisionEvent {
    double time = 0.0;
    std::string survivor;
    std::string absorbed;
    std::optional<CollisionContact> contact;
};

struct BodyTrail {
    std::string id;
    std::vector<WorldVector> points;
};

struct SceneSnapshot {
    double time = 0.0;
    std::vector<BodyState> bodies;
    std::vector<CollisionEvent> events;
    std::vector<BodyTrail> trails;
    std::vector<BodyTrail> reference_trails;
};

class Simulation {
public:
    explicit Simulation(const Project& project);
    ~Simulation();
    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;

    void advance_to(double time);
    SceneSnapshot snapshot() const;
    std::unique_ptr<Simulation> clone() const;

private:
    struct Impl;
    explicit Simulation(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace planet_sandbox
