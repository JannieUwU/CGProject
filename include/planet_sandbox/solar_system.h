#pragma once

#include "planet_sandbox/project.h"

#include <cstddef>
#include <string>

namespace planet_sandbox {

Project make_solar_system_project();
Project make_observed_year_project();
// Time is measured in TDB days for the observed-year preset.
double simulation_time_limit(const Project& project);
std::string simulation_calendar_date(const Project& project, double time);
BodyKind solar_body_kind(std::size_t index);
const char* solar_body_name(std::size_t index);
const char* body_kind_id(BodyKind kind);
BodyKind parse_body_kind(const std::string& id);
DesignOverrides baseline_design(BodyKind kind);

} // namespace planet_sandbox
