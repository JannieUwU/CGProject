#include "planet_sandbox/project.h"
#include "planet_sandbox/solar_system.h"
#include "planet_sandbox/sculpt.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cmath>
#include <set>
#include <stdexcept>

namespace planet_sandbox {
namespace {

WorldVector parse_vec2(const nlohmann::json& value) {
    if (!value.is_array() || (value.size() != 2 && value.size() != 3)) {
        throw std::invalid_argument("Position and velocity must be [x, y] or [x, y, z] arrays");
    }
    return {value.at(0).get<double>(), value.at(1).get<double>(), value.size() == 3 ? value.at(2).get<double>() : 0.0};
}

bool valid_color(const std::string& color) {
    if (color.size() != 7 || color[0] != '#') return false;
    for (std::size_t i = 1; i < color.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(color[i]))) return false;
    }
    return true;
}

} // namespace

Project read_project(std::istream& input) {
    nlohmann::json document;
    input >> document;
    Project project;
    const std::string type = document.value("type", "sandbox");
    if (type != "sandbox" && type != "solar-system") {
        throw std::invalid_argument("Unknown project type: " + type);
    }
    project.solar_system = type == "solar-system";
    project.epoch_jd = document.value("epoch_jd", 0.0);
    project.duration_days = document.value("duration_days", 0.0);
    project.gravitational_constant = document.value("gravitational_constant", 1.0);
    project.view_extent = document.value("view_extent", 5.0);
    project.display_scale = document.value("display_scale", 2.0);
    for (const auto& item : document.at("bodies")) {
        BodySpec body;
        body.id = item.at("id").get<std::string>();
        body.name = item.value("name", body.id);
        body.mass = item.at("mass").get<double>();
        body.radius = item.at("radius").get<double>();
        body.position = parse_vec2(item.at("position"));
        body.velocity = parse_vec2(item.at("velocity"));
        if (project.solar_system) {
            body.kind = parse_body_kind(item.at("kind").get<std::string>());
            body.design = baseline_design(body.kind);
            body.display_radius = item.at("display_radius").get<double>();
            if (item.contains("overrides")) {
                const auto& design = item.at("overrides");
                if (design.contains("sculpt_visible")) {
                    body.design.sculpt_visible = design.at("sculpt_visible").get<std::vector<float>>();
                }
                if (design.contains("sculpt_radar")) {
                    body.design.sculpt_radar = design.at("sculpt_radar").get<std::vector<float>>();
                }
                body.design.relief = design.value("relief", body.design.relief);
                body.design.crater_depth = design.value("crater_depth", body.design.crater_depth);
                body.design.radar_relief = design.value("radar_relief", body.design.radar_relief);
                body.design.belt_relief = design.value("belt_relief", body.design.belt_relief);
                body.design.storm_relief = design.value("storm_relief", body.design.storm_relief);
                body.design.color_shift = design.value("color_shift", body.design.color_shift);
                body.design.cloud_opacity = design.value("cloud_opacity", body.design.cloud_opacity);
                body.design.dust_opacity = design.value("dust_opacity", body.design.dust_opacity);
                body.design.ice_extent = design.value("ice_extent", body.design.ice_extent);
                body.design.band_contrast = design.value("band_contrast", body.design.band_contrast);
                body.design.storm_scale = design.value("storm_scale", body.design.storm_scale);
                body.design.ring_width = design.value("ring_width", body.design.ring_width);
                body.design.granulation = design.value("granulation", body.design.granulation);
                body.design.sunspot_strength = design.value("sunspot_strength", body.design.sunspot_strength);
                body.design.cassini_gap = design.value("cassini_gap", body.design.cassini_gap);
                body.design.axis_tilt_offset = design.value("axis_tilt_offset", body.design.axis_tilt_offset);
                body.design.granule_scale = design.value("granule_scale", body.design.granule_scale);
                body.design.cloud_motion = design.value("cloud_motion", body.design.cloud_motion);
                body.design.ring_tilt_offset = design.value("ring_tilt_offset", body.design.ring_tilt_offset);
                body.design.surface_detail = design.value("surface_detail", body.design.surface_detail);
                body.design.surface_roughness = design.value("surface_roughness", body.design.surface_roughness);
                body.design.atmosphere_density = design.value("atmosphere_density", body.design.atmosphere_density);
                body.design.cloud_altitude = design.value("cloud_altitude", body.design.cloud_altitude);
                body.design.cloud_shadow = design.value("cloud_shadow", body.design.cloud_shadow);
                body.design.band_warp = design.value("band_warp", body.design.band_warp);
                body.design.storm_longitude = design.value("storm_longitude", body.design.storm_longitude);
                body.design.storm_latitude = design.value("storm_latitude", body.design.storm_latitude);
                body.design.storm_aspect = design.value("storm_aspect", body.design.storm_aspect);
            }
        }
        if (item.contains("appearance")) {
            const auto& appearance = item.at("appearance");
            body.appearance.seed = appearance.value("seed", 1u);
            body.appearance.primary = appearance.value("primary", body.appearance.primary);
            body.appearance.secondary = appearance.value("secondary", body.appearance.secondary);
            body.appearance.bands = appearance.value("bands", body.appearance.bands);
            body.appearance.terrain = appearance.value("terrain", body.appearance.terrain);
            body.appearance.clouds = appearance.value("clouds", body.appearance.clouds);
            body.appearance.ring_width = appearance.value("ring_width", body.appearance.ring_width);
        }
        project.bodies.push_back(std::move(body));
    }
    validate_project(project);
    return project;
}

void write_project(std::ostream& output, const Project& project) {
    validate_project(project);
    nlohmann::json document = {
        {"type", project.solar_system ? "solar-system" : "sandbox"},
        {"view_extent", project.view_extent},
        {"display_scale", project.display_scale},
        {"epoch_jd", project.epoch_jd},
        {"duration_days", project.duration_days},
        {"gravitational_constant", project.gravitational_constant},
        {"bodies", nlohmann::json::array()}
    };
    for (const auto& body : project.bodies) {
        const auto& a = body.appearance;
        nlohmann::json item = {
            {"id", body.id}, {"name", body.name}, {"mass", body.mass}, {"radius", body.radius},
            {"position", {body.position.x, body.position.y, body.position.z}},
            {"velocity", {body.velocity.x, body.velocity.y, body.velocity.z}}
        };
        if (!project.solar_system) {
            item["appearance"] = {
                {"seed", a.seed}, {"primary", a.primary}, {"secondary", a.secondary},
                {"bands", a.bands}, {"terrain", a.terrain}, {"clouds", a.clouds},
                {"ring_width", a.ring_width}
            };
        }
        if (project.solar_system) {
            item["kind"] = body_kind_id(body.kind);
            item["display_radius"] = body.display_radius;
            item["overrides"] = {
                {"relief", body.design.relief}, {"crater_depth", body.design.crater_depth},
                {"radar_relief", body.design.radar_relief},
                {"belt_relief", body.design.belt_relief},
                {"storm_relief", body.design.storm_relief},
                {"color_shift", body.design.color_shift},
                {"cloud_opacity", body.design.cloud_opacity}, {"dust_opacity", body.design.dust_opacity},
                {"ice_extent", body.design.ice_extent}, {"band_contrast", body.design.band_contrast},
                {"storm_scale", body.design.storm_scale}, {"ring_width", body.design.ring_width},
                {"granulation", body.design.granulation},
                {"sunspot_strength", body.design.sunspot_strength},
                {"cassini_gap", body.design.cassini_gap},
                {"axis_tilt_offset", body.design.axis_tilt_offset},
                {"granule_scale", body.design.granule_scale},
                {"cloud_motion", body.design.cloud_motion},
                {"ring_tilt_offset", body.design.ring_tilt_offset},
                {"surface_detail", body.design.surface_detail},
                {"surface_roughness", body.design.surface_roughness},
                {"atmosphere_density", body.design.atmosphere_density},
                {"cloud_altitude", body.design.cloud_altitude},
                {"cloud_shadow", body.design.cloud_shadow},
                {"band_warp", body.design.band_warp},
                {"storm_longitude", body.design.storm_longitude},
                {"storm_latitude", body.design.storm_latitude},
                {"storm_aspect", body.design.storm_aspect}
            };
            if (!body.design.sculpt_visible.empty()) {
                item["overrides"]["sculpt_visible"] = body.design.sculpt_visible;
            }
            if (!body.design.sculpt_radar.empty()) {
                item["overrides"]["sculpt_radar"] = body.design.sculpt_radar;
            }
        }
        document["bodies"].push_back(std::move(item));
    }
    output << document.dump(2) << '\n';
    if (!output) throw std::runtime_error("Could not write project JSON");
}

void validate_project(const Project& project) {
    if (project.bodies.empty() || project.bodies.size() > 32) {
        throw std::invalid_argument("Project must contain between 1 and 32 bodies");
    }
    if (!std::isfinite(project.view_extent) || project.view_extent <= 0 ||
        !std::isfinite(project.display_scale) || project.display_scale <= 0) {
        throw std::invalid_argument("View extent and display scale must be positive finite numbers");
    }
    if (!std::isfinite(project.epoch_jd) || !std::isfinite(project.duration_days) ||
        !std::isfinite(project.gravitational_constant) || project.gravitational_constant <= 0 ||
        project.epoch_jd < 0 || project.duration_days < 0 || project.duration_days > 100000 ||
        ((project.epoch_jd > 0) != (project.duration_days > 0)))
        throw std::invalid_argument("Invalid epoch, duration or gravitational constant");
    if (project.duration_days > 365.0)
        throw std::invalid_argument("Dated projects support at most 365 TDB days");
    std::set<std::string> ids;
    if (project.solar_system && project.bodies.size() != 9) {
        throw std::invalid_argument("Solar system must contain the Sun and eight planets");
    }
    for (std::size_t index = 0; index < project.bodies.size(); ++index) {
        const auto& body = project.bodies[index];
        if (body.id.empty() || body.name.empty() || !ids.insert(body.id).second) {
            throw std::invalid_argument("Body IDs must be unique and names cannot be empty");
        }
        if (!std::isfinite(body.mass) || body.mass <= 0 || !std::isfinite(body.radius) || body.radius <= 0 ||
            !std::isfinite(body.position.x) || !std::isfinite(body.position.y) ||
            !std::isfinite(body.position.z) || !std::isfinite(body.velocity.z) ||
            !std::isfinite(body.velocity.x) || !std::isfinite(body.velocity.y)) {
            throw std::invalid_argument("Body mass and radius must be positive; state must be finite");
        }
        if (project.solar_system) {
            const BodyKind expected = solar_body_kind(index);
            const DesignOverrides& d = body.design;
            const auto valid_sculpt_map = [](const std::vector<float>& map) {
                if (map.empty()) return true;
                if (map.size() != static_cast<std::size_t>(sculpt_columns * sculpt_rows)) return false;
                for (float value : map) {
                    if (!std::isfinite(value) || value < -0.08f || value > 0.08f) return false;
                }
                for (int col = 1; col < sculpt_columns; ++col) {
                    if (map[col] != map[0] ||
                        map[(sculpt_rows - 1) * sculpt_columns + col] !=
                            map[(sculpt_rows - 1) * sculpt_columns]) return false;
                }
                return true;
            };
            if (body.kind != expected || body.id != body_kind_id(expected) ||
                body.name != solar_body_name(index) ||
                !std::isfinite(body.display_radius) || body.display_radius <= 0.0 ||
                !valid_sculpt_map(d.sculpt_visible) || !valid_sculpt_map(d.sculpt_radar) ||
                (body.kind != BodyKind::Venus && !d.sculpt_radar.empty()) ||
                !std::isfinite(d.relief) || d.relief < 0.0 || d.relief > 4.0 ||
                !std::isfinite(d.crater_depth) || d.crater_depth < 0.0 || d.crater_depth > 3.0 ||
                !std::isfinite(d.radar_relief) || d.radar_relief < 0.0 || d.radar_relief > 4.0 ||
                !std::isfinite(d.belt_relief) || d.belt_relief < 0.0 || d.belt_relief > 3.0 ||
                !std::isfinite(d.storm_relief) || d.storm_relief < 0.0 || d.storm_relief > 3.0 ||
                !std::isfinite(d.color_shift) || d.color_shift < -1.0 || d.color_shift > 1.0 ||
                !std::isfinite(d.cloud_opacity) || d.cloud_opacity < 0.0 || d.cloud_opacity > 1.0 ||
                !std::isfinite(d.dust_opacity) || d.dust_opacity < 0.0 || d.dust_opacity > 1.0 ||
                !std::isfinite(d.ice_extent) || d.ice_extent < -1.0 || d.ice_extent > 1.0 ||
                !std::isfinite(d.band_contrast) || d.band_contrast < 0.0 || d.band_contrast > 3.0 ||
                !std::isfinite(d.storm_scale) || d.storm_scale < 0.0 || d.storm_scale > 3.0 ||
                !std::isfinite(d.ring_width) || d.ring_width < 0.0 || d.ring_width > 2.0 ||
                !std::isfinite(d.granulation) || d.granulation < 0.0 || d.granulation > 3.0 ||
                !std::isfinite(d.sunspot_strength) || d.sunspot_strength < 0.0 || d.sunspot_strength > 3.0 ||
                !std::isfinite(d.cassini_gap) || d.cassini_gap < 0.0 || d.cassini_gap > 3.0 ||
                !std::isfinite(d.axis_tilt_offset) || d.axis_tilt_offset < -30.0 ||
                d.axis_tilt_offset > 30.0 ||
                !std::isfinite(d.granule_scale) || d.granule_scale < 0.2 || d.granule_scale > 3.0 ||
                !std::isfinite(d.cloud_motion) || d.cloud_motion < 0.0 || d.cloud_motion > 3.0 ||
                !std::isfinite(d.ring_tilt_offset) || d.ring_tilt_offset < -30.0 ||
                d.ring_tilt_offset > 30.0 ||
                !std::isfinite(d.surface_detail) || d.surface_detail < 0.0 || d.surface_detail > 2.0 ||
                !std::isfinite(d.surface_roughness) || d.surface_roughness < 0.0 || d.surface_roughness > 1.0 ||
                !std::isfinite(d.atmosphere_density) || d.atmosphere_density < 0.0 || d.atmosphere_density > 2.0 ||
                !std::isfinite(d.cloud_altitude) || d.cloud_altitude < 0.005 || d.cloud_altitude > 0.06 ||
                !std::isfinite(d.cloud_shadow) || d.cloud_shadow < 0.0 || d.cloud_shadow > 1.0 ||
                !std::isfinite(d.band_warp) || d.band_warp < 0.0 || d.band_warp > 1.0 ||
                !std::isfinite(d.storm_longitude) || d.storm_longitude < 0.0 || d.storm_longitude > 1.0 ||
                !std::isfinite(d.storm_latitude) || d.storm_latitude < 0.08 || d.storm_latitude > 0.92 ||
                !std::isfinite(d.storm_aspect) || d.storm_aspect < 0.5 || d.storm_aspect > 2.0) {
                throw std::invalid_argument("Invalid fixed body identity or design override");
            }
        }
        const auto& a = body.appearance;
        if (!valid_color(a.primary) || !valid_color(a.secondary) ||
            !std::isfinite(a.bands) || a.bands < 0 || a.bands > 40 ||
            !std::isfinite(a.terrain) || a.terrain < 0 || a.terrain > 1 ||
            !std::isfinite(a.clouds) || a.clouds < 0 || a.clouds > 1 ||
            !std::isfinite(a.ring_width) || a.ring_width < 0 || a.ring_width > 2) {
            throw std::invalid_argument("Appearance values are outside supported ranges");
        }
    }
}

} // namespace planet_sandbox
