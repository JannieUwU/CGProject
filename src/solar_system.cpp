#include "planet_sandbox/solar_system.h"

#include <array>
#include <cmath>
#include <stdexcept>
#include <ctime>
#include <algorithm>

namespace planet_sandbox {
namespace {

#include "observed_year_data.inc"

struct CatalogEntry {
    BodyKind kind;
    const char* id;
    const char* name;
    double mass;
    double orbit;
    double phase;
    double physical_radius;
    double display_radius;
};

constexpr std::array<CatalogEntry, 9> catalog{{
    {BodyKind::Sun, "sun", "Sun", 1.0, 0.0, 0.0, 0.10, 0.75},
    {BodyKind::Mercury, "mercury", "Mercury", 1.66e-7, 2.0, 0.2, 0.012, 0.20},
    {BodyKind::Venus, "venus", "Venus", 2.45e-6, 3.0, 1.0, 0.020, 0.28},
    {BodyKind::Earth, "earth", "Earth", 3.00e-6, 4.0, 2.2, 0.021, 0.31},
    {BodyKind::Mars, "mars", "Mars", 3.23e-7, 5.2, 3.3, 0.016, 0.26},
    {BodyKind::Jupiter, "jupiter", "Jupiter", 9.54e-4, 8.0, 4.6, 0.050, 0.68},
    {BodyKind::Saturn, "saturn", "Saturn", 2.86e-4, 11.0, 5.4, 0.044, 0.60},
    {BodyKind::Uranus, "uranus", "Uranus", 4.37e-5, 14.0, 0.65, 0.030, 0.40},
    {BodyKind::Neptune, "neptune", "Neptune", 5.15e-5, 17.0, 2.75, 0.030, 0.39}
}};

} // namespace

Project make_solar_system_project() {
    Project project;
    project.solar_system = true;
    project.view_extent = 19.0;
    project.display_scale = 1.0;
    for (const auto& entry : catalog) {
        BodySpec body;
        body.id = entry.id;
        body.name = entry.name;
        body.mass = entry.mass;
        body.radius = entry.physical_radius;
        body.display_radius = entry.display_radius;
        body.kind = entry.kind;
        body.design = baseline_design(entry.kind);
        body.position = {entry.orbit * std::cos(entry.phase),
                         entry.orbit * std::sin(entry.phase)};
        const double speed = entry.orbit > 0.0 ? std::sqrt(1.0 / entry.orbit) : 0.0;
        body.velocity = {-speed * std::sin(entry.phase), speed * std::cos(entry.phase)};
        project.bodies.push_back(body);
    }
    return project;
}

Project make_observed_year_project() {
    Project project = make_solar_system_project();
    project.epoch_jd = observed_epoch_jd;
    project.duration_days = 365.0;
    constexpr double au_km = 149597870.7;
    project.gravitational_constant = 132712440041.279419 * 86400.0 * 86400.0 / (au_km * au_km * au_km);
    project.view_extent = 34.0;
    const double display_radii[] = {.12, .055, .065, .07, .06, .18, .16, .11, .105};
    for (std::size_t i = 0; i < project.bodies.size(); ++i) {
        auto& body = project.bodies[i];
        const auto& values = observed_states[i];
        body.mass = values[0]; body.radius = values[1];
        body.position = {values[2], values[3], values[4]};
        body.velocity = {values[5], values[6], values[7]};
        body.display_radius = display_radii[i];
    }
    return project;
}

double simulation_time_limit(const Project& project) {
    return project.duration_days > 0 ? project.duration_days : 100000.0;
}

std::string simulation_calendar_date(const Project& project, double time) {
    if (project.epoch_jd <= 0) return {};
    // UTC calendar formatting of a numerical TDB day count; no time-scale conversion.
    const auto seconds = static_cast<std::time_t>(std::floor(project.epoch_jd + time - 2440587.5) * 86400.0);
    const auto* calendar = std::gmtime(&seconds);
    if (!calendar) return "Date unavailable";
    char text[32]{};
    std::strftime(text, sizeof(text), "%Y-%m-%d", calendar);
    return text;
}

BodyKind solar_body_kind(std::size_t index) {
    if (index >= catalog.size()) throw std::out_of_range("Solar body index is out of range");
    return catalog[index].kind;
}

const char* solar_body_name(std::size_t index) {
    if (index >= catalog.size()) throw std::out_of_range("Solar body index is out of range");
    return catalog[index].name;
}

const char* body_kind_id(BodyKind kind) {
    for (const auto& entry : catalog) if (entry.kind == kind) return entry.id;
    return "custom";
}

BodyKind parse_body_kind(const std::string& id) {
    for (const auto& entry : catalog) if (id == entry.id) return entry.kind;
    if (id == "custom") return BodyKind::Custom;
    throw std::invalid_argument("Unknown body kind: " + id);
}

DesignOverrides baseline_design(BodyKind kind) {
    DesignOverrides design;
    if (kind == BodyKind::Venus) design.cloud_opacity = 1.0;
    if (kind == BodyKind::Neptune) {
        design.storm_longitude = 0.19;
        design.storm_latitude = 0.16;
    }
    return design;
}

} // namespace planet_sandbox
