#include "planet_sandbox/studio_ui.h"
#include "planet_sandbox/studio_render_pipeline.h"
#include "planet_sandbox/solar_system.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace planet_sandbox {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double design_zoom = 1.15;

double detail_extent(const Project& project, const BodySpec& body) {
    const double framing = body.kind == BodyKind::Saturn ? 2.0 :
                           body.kind == BodyKind::Uranus ? 1.4 : 0.95;
    return body.display_radius * project.display_scale * framing;
}

RenderCamera world_camera(const StudioState& state) {
    RenderCamera camera = state.render.camera;
    if (state.design_view && state.session.project().solar_system) {
        const Project& project = state.session.project();
        const BodySpec& body = project.bodies[state.selected];
        camera.target = body.position;
        camera.zoom *= detail_extent(project, body) / project.view_extent;
    }
    return camera;
}

RenderCamera overview_camera(const StudioState& state) {
    RenderCamera camera;
    camera.zoom = state.session.project().solar_system ? 0.55 : 1.0;
    return camera;
}

double smooth_fraction(double fraction) {
    return fraction * fraction * fraction *
           (fraction * (fraction * 6.0 - 15.0) + 10.0);
}

RenderCamera interpolate_camera(const RenderCamera& from, const RenderCamera& to,
                                const std::optional<Vec2>& anchor, double fraction) {
    const double t = smooth_fraction(fraction);
    RenderCamera camera;
    camera.yaw = from.yaw + std::remainder(to.yaw - from.yaw, 2.0 * pi) * t;
    camera.pitch = from.pitch + (to.pitch - from.pitch) * t;
    camera.target = {from.target.x + (to.target.x - from.target.x) * t,
                     from.target.y + (to.target.y - from.target.y) * t};
    camera.zoom = std::exp(std::log(from.zoom) +
                           (std::log(to.zoom) - std::log(from.zoom)) * t);
    if (anchor) {
        const double start_x = (anchor->x - from.target.x) / from.zoom;
        const double start_y = (anchor->y - from.target.y) / from.zoom;
        const double end_x = (anchor->x - to.target.x) / to.zoom;
        const double end_y = (anchor->y - to.target.y) / to.zoom;
        camera.target = {anchor->x - camera.zoom * (start_x + (end_x - start_x) * t),
                         anchor->y - camera.zoom * (start_y + (end_y - start_y) * t)};
    }
    if (fraction >= 1.0) return to;
    return camera;
}

RenderCamera camera_between(const StudioCameraTransition& flight, double fraction) {
    if (flight.via) {
        if (fraction < 0.5) {
            return interpolate_camera(flight.from, *flight.via,
                                      flight.departure_anchor, fraction * 2.0);
        }
        return interpolate_camera(*flight.via, flight.to,
                                  flight.screen_anchor, (fraction - 0.5) * 2.0);
    }
    return interpolate_camera(flight.from, flight.to, flight.screen_anchor, fraction);
}

void start_camera_transition(StudioState& state, RenderCamera from, RenderCamera to,
                             StudioCameraDestination destination,
                             RenderCamera detail = {}, bool resume = false,
                             std::optional<Vec2> anchor = std::nullopt) {
    state.sculpt_stroke.end();
    state.selected_pixel_x = state.selected_pixel_y = -1;
    state.compare_modes = false;
    state.inspect_pixel = false;
    state.render.camera = from;
    state.camera_transition = StudioCameraTransition{from, to, detail, anchor,
                                                      std::nullopt, std::nullopt, 0.0, 0.9,
                                                      destination, resume};
}

void select_body(StudioState& state, int index) {
    const int selected = std::clamp(index, 0, static_cast<int>(state.session.project().bodies.size()) - 1);
    if (selected != state.selected) {
        if (state.design_view && state.session.project().solar_system) state.render.camera.zoom = 0.55;
        state.design_view = false;
        state.compare_baseline = false;
        state.sculpt_stroke.end();
        state.sculpt_layer = SolarShapeLayer::Visible;
    }
    state.selected = selected;
    std::snprintf(state.name.data(), state.name.size(), "%s",
                  state.session.project().bodies[state.selected].name.c_str());
}

void replace_project(StudioState& state, Project project, const std::string& label = "Edit planet") {
    const auto body = state.session.project().bodies[state.selected].id;
    const auto group = ImGui::IsAnyItemActive() ? std::to_string(ImGui::GetItemID()) : std::string{};
    apply_studio_edit(state, std::move(project), label, body, group);
}

void save_project(StudioState& state) {
    save_studio_document(state);
}

void load_project(StudioState& state) {
    load_studio_document(state, std::filesystem::u8path(state.path.data()));
}

float hex_channel(const std::string& hex, int offset) {
    return static_cast<float>(std::stoi(hex.substr(offset, 2), nullptr, 16)) / 255.0f;
}

std::string to_hex(const float color[3]) {
    const int r = static_cast<int>(std::round(std::clamp(color[0], 0.0f, 1.0f) * 255.0f));
    const int g = static_cast<int>(std::round(std::clamp(color[1], 0.0f, 1.0f) * 255.0f));
    const int b = static_cast<int>(std::round(std::clamp(color[2], 0.0f, 1.0f) * 255.0f));
    char text[8]{};
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", r, g, b);
    return text;
}

const char* sampling_name(SamplingMode mode) {
    switch (mode) {
    case SamplingMode::Single: return "1x";
    case SamplingMode::SSAA4: return "4x SSAA";
    case SamplingMode::MSAA4: return "4x MSAA";
    case SamplingMode::SSAA16: return "16x SSAA";
    }
    return "Unknown";
}

const char* trace_surface_name(TraceSurface surface) {
    switch (surface) {
    case TraceSurface::Background: return "Background";
    case TraceSurface::Body: return "Surface";
    case TraceSurface::Ring: return "Ring";
    case TraceSurface::Cloud: return "Cloud";
    case TraceSurface::Atmosphere: return "Atmosphere";
    case TraceSurface::Trail: return "Trail";
    case TraceSurface::ReferenceTrail: return "Reference orbit";
    }
    return "Unknown";
}

void sampling_combo(const char* label, SamplingMode& selected) {
    ImGui::SetNextItemWidth(120.0f);
    if (!ImGui::BeginCombo(label, sampling_name(selected))) return;
    for (SamplingMode mode : {SamplingMode::Single, SamplingMode::SSAA4, SamplingMode::MSAA4}) {
        if (ImGui::Selectable(sampling_name(mode), selected == mode)) selected = mode;
    }
    ImGui::EndCombo();
}

bool playback_button(bool playing, bool comparing) {
    ImGui::BeginDisabled(comparing);
    const bool pressed = ImGui::Button(playing ? "Pause" : "Play");
    ImGui::EndDisabled();
    if (comparing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Compare pauses playback. Uncheck Compare to play.\n"
                          "Step and Reset can still change the snapshot.");
    }
    return pressed;
}

ImVec4 display_color(const std::array<double, 3>& linear) {
    const auto channel = [](double value) {
        return static_cast<float>(std::pow(std::clamp(value, 0.0, 1.0), 1.0 / 2.2));
    };
    return {channel(linear[0]), channel(linear[1]), channel(linear[2]), 1.0f};
}

void draw_pixel_inspector(StudioState& state, const RenderFrame& frame) {
    ImGui::SeparatorText("Pixel zoom");
    ImGui::SetNextItemWidth(100.0f);
    const char* region_label = state.pixel_region_size == 9 ? "9 x 9" :
                               state.pixel_region_size == 17 ? "17 x 17" : "25 x 25";
    if (ImGui::BeginCombo("Region", region_label)) {
        for (int cells : {9, 17, 25}) {
            const std::string label = std::to_string(cells) + " x " + std::to_string(cells);
            if (ImGui::Selectable(label.c_str(), state.pixel_region_size == cells)) {
                state.pixel_region_size = cells;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Number of source pixels. Both views use nearest-neighbor magnification.");
    const int panels = frame.panel_count;
    const int panel_width = frame.image.width / panels;
    const auto region = frame.trace
        ? studio_pixel_region(frame, frame.trace->x, frame.trace->y, state.pixel_region_size)
        : StudioPixelRegion{};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (region.size > 0) {
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float cell_size = std::max(1.0f, std::floor(
            (ImGui::GetContentRegionAvail().x - gap * (panels - 1)) / panels / region.size));
        const int local_x = frame.trace->x % panel_width;
        for (int panel = 0; panel < panels; ++panel) {
            if (panel > 0) ImGui::SameLine(0, gap);
            ImGui::BeginGroup();
            ImGui::Text("%s%s", panels == 2 ? (panel == 0 ? "L " : "R ") : "",
                        sampling_name(frame.panel_sampling[panel]));
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(region.size * cell_size, region.size * cell_size));
            for (int row = 0; row < region.size; ++row) {
                for (int col = 0; col < region.size; ++col) {
                    const int x = panel * panel_width + region.x + col;
                    const int y = region.y + row;
                    const std::size_t pixel = (static_cast<std::size_t>(y) * frame.image.width + x) * 3;
                    const ImU32 color = IM_COL32(frame.image.rgb[pixel], frame.image.rgb[pixel + 1],
                                                 frame.image.rgb[pixel + 2], 255);
                    const ImVec2 a(origin.x + col * cell_size, origin.y + row * cell_size);
                    draw->AddRectFilled(a, {a.x + cell_size, a.y + cell_size}, color);
                }
            }
            const ImVec2 center(origin.x + (local_x - region.x) * cell_size,
                                 origin.y + (frame.trace->y - region.y) * cell_size);
            draw->AddRect(center, {center.x + cell_size, center.y + cell_size},
                          IM_COL32(255, 255, 255, 255), 0.0f, 0, 1.0f);
            ImGui::EndGroup();
        }
    } else {
        ImGui::TextWrapped("%s", state.inspect_pixel
            ? "Click the scene to inspect a pixel."
            : "Enable Inspect to select a pixel.");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Frame cost");
    if (ImGui::BeginTable("##frame-cost", 3, ImGuiTableFlags_BordersInnerH |
                                               ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Method", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Shades", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableHeadersRow();
        for (int panel = 0; panel < panels; ++panel) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s%s", panels == 2 ? (panel == 0 ? "L " : "R ") : "",
                        sampling_name(frame.panel_sampling[panel]));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(panels == 2
                ? frame.panel_shade_evaluations[panel] : frame.shade_evaluations));
            ImGui::TableNextColumn();
            if (state.measurement.ready) ImGui::Text("%.2f", state.measurement.median_ms[panel]);
            else ImGui::TextDisabled("--");
        }
        ImGui::EndTable();
    }
    const auto& measurement = state.measurement;
    if (measurement.running) {
        ImGui::TextDisabled("Measuring... %d/%d", std::max(0, measurement.completed_runs -
            StudioRenderMeasurement::warmup_runs), StudioRenderMeasurement::sample_runs);
    } else if (measurement.waiting) {
        ImGui::TextDisabled("Waiting for a steady view...");
    } else if (!measurement.ready) {
        ImGui::TextDisabled("Pause to measure automatically.");
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Time (ms): median of %d renders (CPU only)", StudioRenderMeasurement::sample_runs);
    ImGui::PopTextWrapPos();
    ImGui::SetItemTooltip("Automatically measures a stationary view: 3 warm-up renders, then 11 timed renders. "
                         "The median stays fixed until the scene, view or sampling method changes. "
                         "Detailed process recording is disabled during measurement. "
                         "Excludes final trace extraction, image upload, comparison stitching and UI. "
                         "Moving the pixel selection or changing Region keeps the timing result.");
    ImGui::TextDisabled("%d x %d per view", panel_width, frame.image.height);
    ImGui::Spacing();
    auto& disclosure = state.pixel_disclosure;
    ImGui::SetNextItemOpen(disclosure.details, ImGuiCond_Always);
    disclosure.details = ImGui::CollapsingHeader("Details");
    if (!disclosure.details) {
        disclosure.close_details();
        return;
    }
    if (!frame.trace) return;
    const PixelTrace& trace = *frame.trace;
    ImGui::Text("Pixel (%d, %d)", trace.x % panel_width, trace.y);
    for (int panel = 0; panel < panels; ++panel) {
        const std::size_t pixel = (static_cast<std::size_t>(trace.y) * frame.image.width +
                                   panel * panel_width + trace.x % panel_width) * 3;
        ImGui::Text("%sRGB %u, %u, %u", panels == 2 ? (panel == 0 ? "Left " : "Right ") : "",
                    frame.image.rgb[pixel], frame.image.rgb[pixel + 1], frame.image.rgb[pixel + 2]);
    }
    ImGui::TextDisabled("Filter: Nearest neighbor");
    if (panels == 2) ImGui::TextDisabled("Samples: %s view (clicked side)",
                                        trace.x < panel_width ? "left" : "right");
    draw_pixel_process(state, trace);
    ImGui::SeparatorText("Samples");
    const ImVec2 diagram = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(112.0f, 112.0f));
    draw->AddRectFilled(diagram, {diagram.x + 112.0f, diagram.y + 112.0f},
                        IM_COL32(27, 32, 40, 255));
    for (std::size_t index = 0; index < trace.samples.size(); ++index) {
        const auto& sample = trace.samples[index];
        const ImVec4 color = display_color(sample.linear_rgb);
        const ImVec2 center(diagram.x + static_cast<float>(sample.position.x) * 112.0f,
                            diagram.y + static_cast<float>(sample.position.y) * 112.0f);
        const float radius = trace.samples.size() > 4 ? 9.0f : 14.0f;
        draw->AddCircleFilled(center, radius, ImGui::ColorConvertFloat4ToU32(color));
        draw->AddCircle(center, radius, IM_COL32(240, 245, 250, 255), 0, 2.0f);
        draw->AddText({center.x - 4.0f, center.y - 7.0f}, IM_COL32(0, 0, 0, 255),
                      std::to_string(index + 1).c_str());
    }
    for (std::size_t index = 0; index < trace.samples.size(); ++index) {
        const auto& sample = trace.samples[index];
        const char* body = sample.body_index >= 0 &&
                           sample.body_index < static_cast<int>(state.session.project().bodies.size())
            ? state.session.project().bodies[sample.body_index].name.c_str() : "Background";
        ImGui::Text("%zu  %s / %s", index + 1, body,
                    trace_surface_name(sample.surface));
        ImGui::SameLine(0, 8);
        if (sample.body_index < 0) ImGui::TextDisabled("depth -");
        else ImGui::TextDisabled("depth %.3f", sample.depth);
        ImGui::TextDisabled("    linear %.3f  %.3f  %.3f", sample.linear_rgb[0],
                            sample.linear_rgb[1], sample.linear_rgb[2]);
    }
}

void edit_appearance(StudioState& state) {
    const Appearance current = state.session.project().bodies[state.selected].appearance;
    auto update = [&](auto edit) {
        Project next = state.session.project();
        edit(next.bodies[state.selected].appearance);
        replace_project(state, std::move(next), "Appearance");
    };

    float primary[3] = {hex_channel(current.primary, 1), hex_channel(current.primary, 3),
                        hex_channel(current.primary, 5)};
    if (ImGui::ColorEdit3("Primary", primary)) update([&](Appearance& a) { a.primary = to_hex(primary); });
    float secondary[3] = {hex_channel(current.secondary, 1), hex_channel(current.secondary, 3),
                          hex_channel(current.secondary, 5)};
    if (ImGui::ColorEdit3("Secondary", secondary)) update([&](Appearance& a) { a.secondary = to_hex(secondary); });
    std::uint32_t seed = current.seed;
    if (ImGui::InputScalar("Seed", ImGuiDataType_U32, &seed)) {
        update([&](Appearance& a) { a.seed = seed; });
    }
    float bands = static_cast<float>(current.bands);
    if (ImGui::SliderFloat("Bands", &bands, 0.0f, 40.0f)) update([&](Appearance& a) { a.bands = bands; });
    float terrain = static_cast<float>(current.terrain);
    if (ImGui::SliderFloat("Terrain", &terrain, 0.0f, 1.0f)) update([&](Appearance& a) { a.terrain = terrain; });
    float clouds = static_cast<float>(current.clouds);
    if (ImGui::SliderFloat("Clouds", &clouds, 0.0f, 1.0f)) update([&](Appearance& a) { a.clouds = clouds; });
    float ring = static_cast<float>(current.ring_width);
    if (ImGui::SliderFloat("Ring width", &ring, 0.0f, 2.0f)) update([&](Appearance& a) { a.ring_width = ring; });
}

void edit_solar_design(StudioState& state) {
    // Editing replaces the project's bodies; keep values, not a reference into them.
    const BodyKind kind = state.session.project().bodies[state.selected].kind;
    const DesignOverrides current = state.session.project().bodies[state.selected].design;
    const auto update = [&](const char* label, auto edit) {
        Project next = state.session.project();
        edit(next.bodies[state.selected].design);
        replace_project(state, std::move(next), label);
    };
    auto slider = [&](const char* label, double value, float low, float high, auto edit) {
        ImGui::TextUnformatted(label);
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(-1.0f);
        float candidate = static_cast<float>(value);
        if (ImGui::SliderFloat("##value", &candidate, low, high)) {
            update(label, [&](DesignOverrides& d) { edit(d, candidate); });
        }
        ImGui::PopID();
    };
    switch (kind) {
    case BodyKind::Sun:
        ImGui::SeparatorText("Photosphere");
        slider("Artistic photosphere relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Color temperature", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        slider("Granulation", current.granulation, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.granulation = v; });
        slider("Granule scale", current.granule_scale, 0.2f, 3.0f,
               [](DesignOverrides& d, float v) { d.granule_scale = v; });
        slider("Sunspot intensity", current.sunspot_strength, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.sunspot_strength = v; });
        break;
    case BodyKind::Mercury:
        ImGui::SeparatorText("Cratered surface");
        slider("Image-derived crater relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Crater depth", current.crater_depth, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.crater_depth = v; });
        slider("Crater contrast", current.band_contrast, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.band_contrast = v; });
        slider("Surface tone", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        break;
    case BodyKind::Venus:
        ImGui::SeparatorText("Cloud deck");
        slider("Artistic cloud-top relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Cloud opacity", current.cloud_opacity, 0.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.cloud_opacity = v; });
        slider("Warmth", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        ImGui::SeparatorText("Radar surface");
        slider("Surface relief", current.radar_relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.radar_relief = v; });
        if (current.cloud_opacity < 0.99) ImGui::TextDisabled("Exposed surface is radar-derived");
        break;
    case BodyKind::Earth:
        ImGui::SeparatorText("Surface");
        slider("Relative terrain relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Land / ocean tone", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        slider("Polar ice", current.ice_extent, 0.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.ice_extent = v; });
        ImGui::SeparatorText("Clouds");
        slider("Cloud cover", current.cloud_opacity, 0.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.cloud_opacity = v; });
        slider("Cloud drift", current.cloud_motion, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.cloud_motion = v; });
        break;
    case BodyKind::Mars:
        ImGui::SeparatorText("Terrain");
        slider("Relative elevation relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Rust tone", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        slider("Dust", current.dust_opacity, 0.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.dust_opacity = v; });
        break;
    case BodyKind::Saturn:
        ImGui::SeparatorText("Cloud belts");
        slider("Artistic cloud-top relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Cloud tone", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        slider("Band contrast", current.band_contrast, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.band_contrast = v; });
        slider("Ring density", current.ring_width, 0.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.ring_width = v; });
        slider("Cassini gap", current.cassini_gap, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.cassini_gap = v; });
        slider("Ring tilt offset", current.ring_tilt_offset, -30.0f, 30.0f,
               [](DesignOverrides& d, float v) { d.ring_tilt_offset = v; });
        break;
    case BodyKind::Jupiter:
        ImGui::SeparatorText("Cloud belts");
        slider("Artistic cloud-top relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Cloud-belt relief", current.belt_relief, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.belt_relief = v; });
        slider("Band contrast", current.band_contrast, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.band_contrast = v; });
        slider("Great Red Spot size", current.storm_scale, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.storm_scale = v; });
        slider("Spot relief", current.storm_relief, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.storm_relief = v; });
        slider("Cloud tone", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        break;
    case BodyKind::Uranus:
        ImGui::SeparatorText("Ice giant");
        slider("Artistic cloud-top relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Cyan tone", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        slider("Cloud contrast", current.band_contrast, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.band_contrast = v; });
        slider("Axis tilt offset", current.axis_tilt_offset, -30.0f, 30.0f,
               [](DesignOverrides& d, float v) { d.axis_tilt_offset = v; });
        slider("Faint rings", current.ring_width, 0.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.ring_width = v; });
        break;
    case BodyKind::Neptune:
        ImGui::SeparatorText("Cloud deck");
        slider("Artistic cloud-top relief", current.relief, 0.0f, 4.0f,
               [](DesignOverrides& d, float v) { d.relief = v; });
        slider("Blue tone", current.color_shift, -1.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.color_shift = v; });
        slider("Cloud contrast", current.band_contrast, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.band_contrast = v; });
        slider("Storm size", current.storm_scale, 0.0f, 3.0f,
               [](DesignOverrides& d, float v) { d.storm_scale = v; });
        break;
    default:
        break;
    }
    if (kind != BodyKind::Sun && ImGui::CollapsingHeader("Light and atmosphere")) {
        slider("Fine surface detail", current.surface_detail, 0.0f, 2.0f,
               [](DesignOverrides& d, float v) { d.surface_detail = v; });
        if (kind != BodyKind::Mercury) {
            slider("Atmosphere density", current.atmosphere_density, 0.0f, 2.0f,
                   [](DesignOverrides& d, float v) { d.atmosphere_density = v; });
        }
        if (kind == BodyKind::Earth) {
            slider("Cloud altitude", current.cloud_altitude, 0.005f, 0.06f,
                   [](DesignOverrides& d, float v) { d.cloud_altitude = std::clamp(double(v), 0.005, 0.06); });
            slider("Cloud shadow", current.cloud_shadow, 0.0f, 1.0f,
                   [](DesignOverrides& d, float v) { d.cloud_shadow = v; });
        }
        ImGui::TextWrapped("Sunlight follows the Sun in simulation. Close-ups use photographic lighting.");
    }
    if ((kind == BodyKind::Jupiter || kind == BodyKind::Saturn || kind == BodyKind::Uranus ||
         kind == BodyKind::Neptune) && ImGui::CollapsingHeader("Cloud flow and storms")) {
        slider("Cloud-band warp", current.band_warp, 0.0f, 1.0f,
               [](DesignOverrides& d, float v) { d.band_warp = v; });
        if (kind == BodyKind::Jupiter || kind == BodyKind::Neptune) {
            slider("Storm longitude (degrees)", current.storm_longitude * 360.0, 0.0f, 360.0f,
                   [](DesignOverrides& d, float v) { d.storm_longitude = v / 360.0; });
            slider("Storm latitude (north positive)", 90.0 - current.storm_latitude * 180.0, -75.6f, 75.6f,
                   [](DesignOverrides& d, float v) { d.storm_latitude = std::clamp((90.0 - v) / 180.0, 0.08, 0.92); });
            slider("Storm aspect", current.storm_aspect, 0.5f, 2.0f,
                   [](DesignOverrides& d, float v) { d.storm_aspect = v; });
            if (kind == BodyKind::Neptune) {
                slider("Storm relief", current.storm_relief, 0.0f, 3.0f,
                       [](DesignOverrides& d, float v) { d.storm_relief = v; });
            }
            ImGui::TextWrapped("Moves the existing storm and surrounding cloud bands together.");
        }
    }
    if (state.design_view && ImGui::CollapsingHeader("Sculpt brush", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::RadioButton("Raise", state.sculpt_mode == SculptMode::Raise)) {
            state.sculpt_mode = SculptMode::Raise;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Lower", state.sculpt_mode == SculptMode::Lower)) {
            state.sculpt_mode = SculptMode::Lower;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Smooth", state.sculpt_mode == SculptMode::Smooth)) {
            state.sculpt_mode = SculptMode::Smooth;
        }
        if (kind == BodyKind::Venus) {
            if (ImGui::RadioButton("Cloud top", state.sculpt_layer == SolarShapeLayer::Visible)) {
                state.sculpt_layer = SolarShapeLayer::Visible;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Radar surface",
                                   state.sculpt_layer == SolarShapeLayer::VenusRadar)) {
                state.sculpt_layer = SolarShapeLayer::VenusRadar;
            }
        }
        ImGui::SliderFloat("Brush size", &state.sculpt_radius, 0.12f, 0.8f);
        ImGui::SliderFloat("Strength", &state.sculpt_strength, 0.1f, 1.0f);
        ImGui::TextWrapped("Paint with the left mouse button. Rotate with the right. Ctrl/Cmd+Z undoes a stroke.");
    }
    if (ImGui::Button("Reset this body", ImVec2(-1, 0))) {
        try {
            Project next = state.session.project();
            next.bodies[state.selected].design = baseline_design(kind);
            state.history.finish();
            apply_studio_edit(state, std::move(next), "Reset body",
                              state.session.project().bodies[state.selected].id);
            state.sculpt_history.clear_body(kind);
            state.sculpt_stroke.end();
            state.notice = "Body reset";
        }
        catch (const std::exception& error) { state.notice = error.what(); }
    }
}

void edit_motion(StudioState& state) {
    const BodySpec current = state.session.project().bodies[state.selected];
    auto update = [&](auto edit) {
        Project next = state.session.project();
        edit(next.bodies[state.selected]);
        replace_project(state, std::move(next));
    };
    double mass = current.mass;
    if (ImGui::InputDouble("Mass", &mass,
                           state.session.project().solar_system ? 0.0 : 0.1,
                           state.session.project().solar_system ? 0.0 : 1.0,
                           state.session.project().solar_system ? "%.8g" : "%.4f")) {
        update([&](BodySpec& b) { b.mass = mass; });
    }
    double radius = current.radius;
    if (ImGui::InputDouble(state.session.project().solar_system ? "Collision radius" : "Radius",
                           &radius, state.session.project().solar_system ? 0.0 : 0.01,
                           state.session.project().solar_system ? 0.0 : 0.1,
                           state.session.project().solar_system ? "%.8g" : "%.4f")) {
        update([&](BodySpec& b) { b.radius = radius; });
    }
    ImGui::SeparatorText("Initial position");
    double x = current.position.x, y = current.position.y;
    if (ImGui::InputDouble("X##position", &x, 0.1, 1.0, "%.3f")) update([&](BodySpec& b) { b.position.x = x; });
    if (ImGui::InputDouble("Y##position", &y, 0.1, 1.0, "%.3f")) update([&](BodySpec& b) { b.position.y = y; });
    ImGui::SeparatorText("Initial velocity");
    double vx = current.velocity.x, vy = current.velocity.y;
    if (ImGui::InputDouble("X##velocity", &vx, 0.1, 1.0, "%.3f")) update([&](BodySpec& b) { b.velocity.x = vx; });
    if (ImGui::InputDouble("Y##velocity", &vy, 0.1, 1.0, "%.3f")) update([&](BodySpec& b) { b.velocity.y = vy; });
}

void draw_sidebar(StudioState& state, float width) {
    ImGui::BeginChild("##sidebar", ImVec2(width, 0), ImGuiChildFlags_Borders);
    ImGui::BeginDisabled(state.camera_transition.has_value());
    ImGui::TextUnformatted(state.session.project().solar_system ? "CHOOSE A PLANET" : "BODIES");
    ImGui::Separator();
    for (std::size_t i = 0; i < state.session.project().bodies.size(); ++i) {
        const auto& body = state.session.project().bodies[i];
        const bool selected = state.selected == static_cast<int>(i);
        if (selected) ImGui::PushStyleColor(ImGuiCol_Header, {0.12f, 0.36f, 0.37f, 1.0f});
        if (ImGui::Selectable((body.name + "##" + body.id).c_str(), selected)) {
            select_studio_body(state, static_cast<int>(i));
        }
        ImGui::SetItemTooltip("%s %s", state.session.mode() == SessionMode::Design &&
            state.demos.active() != DemoPreset::Collision ? "Design" : "Select", body.name.c_str());
        if (selected) ImGui::PopStyleColor();
    }
    const bool can_edit = state.session.mode() == SessionMode::Design &&
                          state.demos.active() != DemoPreset::Collision;
    if (!state.session.project().solar_system) {
      ImGui::BeginDisabled(!can_edit || state.session.project().bodies.size() >= 32);
      if (ImGui::Button("Add planet", ImVec2(-1, 0))) {
        try {
            state.session.add_planet();
            select_body(state, static_cast<int>(state.session.project().bodies.size()) - 1);
            state.notice.clear();
        } catch (const std::exception& error) {
            state.notice = error.what();
        }
    }
      ImGui::EndDisabled();
      ImGui::BeginDisabled(!can_edit || state.selected == 0);
      if (ImGui::Button("Remove", ImVec2(-1, 0))) {
        Project next = state.session.project();
        next.bodies.erase(next.bodies.begin() + state.selected);
        replace_project(state, std::move(next));
    }
      ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Project");
    ImGui::InputText("##path", state.path.data(), state.path.size());
    if (ImGui::Button("Save", ImVec2((width - 28) * 0.5f, 0))) save_project(state);
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_edit);
    if (ImGui::Button("Load", ImVec2(-1, 0))) load_project(state);
    ImGui::EndDisabled();
    ImGui::TextColored(state.history.dirty() ? ImVec4(1.0f, 0.76f, 0.36f, 1.0f) :
                                             ImVec4(0.58f, 0.74f, 0.64f, 1.0f),
                       "%s", state.history.dirty() ? "Unsaved changes" : "No unsaved changes");
    const bool history_locked = !can_edit || state.compare_baseline;
    ImGui::BeginDisabled(history_locked || !state.history.can_undo());
    if (ImGui::Button("Undo", ImVec2((width - 28) * 0.5f, 0))) undo_studio_edit(state);
    if (state.history.can_undo()) ImGui::SetItemTooltip("Undo: %s", state.history.undo_label().c_str());
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(history_locked || !state.history.can_redo());
    if (ImGui::Button("Redo", ImVec2(-1, 0))) redo_studio_edit(state);
    if (state.history.can_redo()) ImGui::SetItemTooltip("Redo: %s", state.history.redo_label().c_str());
    ImGui::EndDisabled();
    if (state.recovery_writer) ImGui::TextDisabled("Recovery draft enabled");
    if (!state.recovery_error.empty()) {
        ImGui::TextWrapped("%s", state.recovery_error.c_str());
        if (state.recovery_blocked && ImGui::Button("Discard unreadable draft")) discard_studio_draft(state);
    }
    if (!state.notice.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, state.notice == "Saved" || state.notice == "Loaded" ||
                                              state.notice == "Body reset"
                                              ? ImVec4(0.45f, 0.8f, 0.66f, 1.0f)
                                              : ImVec4(0.94f, 0.45f, 0.41f, 1.0f));
        ImGui::TextWrapped("%s", state.notice.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::SeparatorText("View");
    ImGui::BeginDisabled(!can_edit);
    float extent = static_cast<float>(state.session.project().view_extent);
    if (ImGui::SliderFloat("Extent", &extent, 1.0f, 20.0f)) {
        Project next = state.session.project();
        next.view_extent = extent;
        replace_project(state, std::move(next));
    }
    float scale = static_cast<float>(state.session.project().display_scale);
    if (ImGui::SliderFloat("Scale", &scale, 0.5f, 8.0f)) {
        Project next = state.session.project();
        next.display_scale = scale;
        replace_project(state, std::move(next));
    }
    ImGui::EndDisabled();
    if (state.session.project().solar_system) {
        ImGui::Spacing();
        ImGui::SeparatorText("Demo presets");
        ImGui::BeginDisabled(state.session.mode() != SessionMode::Design);
        const DemoPreset active = state.demos.active();
        const char* label = active == DemoPreset::EarthTerrain ? "Earth terrain" :
                            active == DemoPreset::SaturnRing ? "Saturn ring edge" :
                            active == DemoPreset::Collision ? "Planet collision" : "Choose preset...";
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##demo-presets", label)) {
            ImGui::SeparatorText("Appearance & AA");
            if (ImGui::Selectable("Earth terrain", active == DemoPreset::EarthTerrain)) {
                activate_studio_preset(state, DemoPreset::EarthTerrain);
            }
            if (ImGui::Selectable("Saturn ring edge", active == DemoPreset::SaturnRing)) {
                activate_studio_preset(state, DemoPreset::SaturnRing);
            }
            ImGui::SeparatorText("Motion & collisions");
            if (ImGui::Selectable("Planet collision", active == DemoPreset::Collision)) {
                activate_studio_preset(state, DemoPreset::Collision);
            }
            if (active != DemoPreset::None) {
                ImGui::Separator();
                if (ImGui::Selectable("Exit demo")) exit_studio_preset(state);
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
}

std::string body_name(const Project& project, const std::string& id) {
    const auto found = std::find_if(project.bodies.begin(), project.bodies.end(),
                                   [&](const auto& body) { return body.id == id; });
    return found == project.bodies.end() ? id : found->name;
}

void draw_initial_motion(const BodySpec& body) {
    ImGui::TextWrapped("Initial conditions used to start this run. Return to Design to change them.");
    ImGui::SeparatorText("Initial state");
    ImGui::Text("Mass: %.8g", body.mass);
    ImGui::Text("Collision radius: %.8g", body.radius);
    ImGui::Text("Position: %.3f, %.3f", body.position.x, body.position.y);
    ImGui::Text("Velocity: %.3f, %.3f", body.velocity.x, body.velocity.y);
    ImGui::TextDisabled("Values use simulation units.");
}

void draw_simulation_inspector(StudioState& state) {
    const auto snapshot = state.demos.snapshot(state.session.snapshot());
    const auto& project = state.session.project();
    const auto& selected = project.bodies[state.selected];
    const bool collision_demo = state.demos.active() == DemoPreset::Collision;
    ImGui::SeparatorText(collision_demo ? "Demo orbit" : "Live orbit");
    ImGui::Text("t = %.3f   /   %zu active bodies", snapshot.time, snapshot.bodies.size());
    const auto live = std::find_if(snapshot.bodies.begin(), snapshot.bodies.end(),
                                  [&](const auto& body) { return body.id == selected.id; });
    if (live != snapshot.bodies.end()) {
        ImGui::Text("Position: %.3f, %.3f", live->position.x, live->position.y);
        ImGui::Text("Speed: %.4f", std::hypot(live->velocity.x, live->velocity.y));
        const auto sun = std::find_if(snapshot.bodies.begin(), snapshot.bodies.end(),
                                      [](const auto& body) { return body.id == "sun"; });
        if (sun != snapshot.bodies.end() && sun != live) {
            ImGui::Text("Distance to Sun: %.4f", std::hypot(live->position.x - sun->position.x,
                                                           live->position.y - sun->position.y));
        }
    } else {
        const auto impact = std::find_if(snapshot.events.rbegin(), snapshot.events.rend(),
                                        [&](const auto& event) { return event.absorbed == selected.id; });
        if (impact != snapshot.events.rend()) {
            ImGui::TextWrapped("Merged into %s at t = %.3f.",
                              body_name(project, impact->survivor).c_str(), impact->time);
        } else {
            ImGui::TextWrapped("This planet is not active in the current snapshot.");
        }
        ImGui::TextWrapped("Select an active planet to follow it. Return to Design to edit the original.");
    }
    ImGui::PushStyleColor(ImGuiCol_Text, {0.60f, 0.65f, 0.70f, 1.0f});
    ImGui::TextWrapped("Simulation units. Display sizes are enlarged.");
    ImGui::PopStyleColor();
    if (collision_demo) {
        ImGui::TextWrapped("Temporary Earth and Mars demo. Your DIY project is preserved.");
    } else if (project.solar_system) {
        ImGui::Spacing();
        ImGui::Checkbox("Reference orbit", &state.show_reference_orbit);
        ImGui::SetItemTooltip("Compare this planet's path with the original solar-system catalog at the same time.");
        if (state.show_reference_orbit) {
            ImGui::TextColored({0.95f, 0.64f, 0.24f, 1.0f}, "Catalog orbit");
            ImGui::SameLine();
            ImGui::TextColored({0.43f, 0.75f, 0.75f, 1.0f}, "DIY orbit");
        }
    }
    const auto& events = collision_demo ? snapshot.events : state.session.observed_events();
    ImGui::Spacing();
    ImGui::SeparatorText("Collisions");
    if (state.compare_modes) {
        ImGui::TextWrapped("Exit Compare to browse collision events.");
        return;
    }
    if (events.empty()) {
        ImGui::TextWrapped("No collisions yet. Change an initial orbit in Design or try the collision demo.");
        return;
    }
    ImGui::TextWrapped(collision_demo ? "Reset the demo to watch it again." :
                                      "Select an event to pause and replay its result.");
    std::optional<CollisionEvent> replay;
    ImGui::BeginChild("##collision-list", {0, std::max(85.0f, ImGui::GetContentRegionAvail().y)},
                       ImGuiChildFlags_Borders);
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        ImGui::PushID(static_cast<int>(it - events.rbegin()));
        const bool later = it->time > snapshot.time;
        char label[512]{};
        std::snprintf(label, sizeof(label), "t = %.3f%s\n%s + %s", it->time,
                      later ? "  / later in this run" : "", body_name(project, it->survivor).c_str(),
                      body_name(project, it->absorbed).c_str());
        if (ImGui::Selectable(label, false, ImGuiSelectableFlags_None,
                              {0, ImGui::GetTextLineHeight() * 2 + 8}) && !collision_demo) replay = *it;
        ImGui::SetItemTooltip("%s absorbed %s.%s", body_name(project, it->survivor).c_str(),
                              body_name(project, it->absorbed).c_str(),
                              collision_demo ? "" : " Click to view the result.");
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (replay) replay_studio_collision(state, *replay);
}

void draw_inspector(StudioState& state, const RenderFrame& frame, float width) {
    // Check the requested selection even while the Pixel tab is hidden. Using
    // the displayed trace here could reopen/reset against an older async frame.
    const std::array<int, 8> target = {state.selected, state.selected_pixel_x,
        state.selected_pixel_y, state.inspect_pixel, state.compare_modes,
        static_cast<int>(state.render.sampling), static_cast<int>(state.compare_left),
        static_cast<int>(state.compare_right)};
    if (target != state.pixel_inspector_target) {
        state.pixel_inspector_target = target;
        state.pixel_disclosure.close_details();
        state.trace_event_index = 0;
    }
    ImGui::BeginChild("##inspector", ImVec2(width, 0), ImGuiChildFlags_Borders);
    ImGui::BeginDisabled(state.camera_transition.has_value());
    const bool simulating = state.session.mode() == SessionMode::Simulate ||
                            state.demos.active() == DemoPreset::Collision;
    const bool switched_stage = simulating != state.inspector_was_simulating;
    state.inspector_was_simulating = simulating;
    ImGui::TextUnformatted(simulating ? "SIMULATION" : "PLANET DESIGN");
    ImGui::Separator();
    const auto& body = state.session.project().bodies[state.selected];
    ImGui::TextColored({0.50f, 0.88f, 0.83f, 1.0f}, "%s", body.name.c_str());
    if (!state.session.project().solar_system) {
      ImGui::BeginDisabled(state.session.mode() != SessionMode::Design);
      ImGui::InputText("Name", state.name.data(), state.name.size());
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        Project next = state.session.project();
        next.bodies[state.selected].name = state.name.data();
        replace_project(state, std::move(next));
      }
      ImGui::EndDisabled();
    }
    if (state.session.project().solar_system && state.design_view && !simulating) {
        ImGui::Checkbox("Show baseline", &state.compare_baseline);
        ImGui::SetItemTooltip("Preview this planet's default appearance without undoing your edits.\n"
                              "Uncheck to return to your design. Unedited planets look identical.");
        if (state.compare_baseline) ImGui::TextDisabled("Baseline preview - uncheck to edit");
    }
    const bool was_pixel_tab_active = state.pixel_tab_active;
    state.pixel_tab_active = false;
    state.design_tab_active = false;
    if (ImGui::BeginTabBar("##inspector-tabs")) {
        const auto stage_tab_flags = switched_stage && !state.select_pixel_tab ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(simulating ? "Simulation" :
                               state.session.project().solar_system ? "Design" : "Appearance",
                               nullptr, stage_tab_flags)) {
          if (simulating) {
            draw_simulation_inspector(state);
          } else {
            state.design_tab_active = true;
            ImGui::BeginDisabled(state.session.mode() != SessionMode::Design ||
                                 state.demos.active() == DemoPreset::Collision ||
                                 (state.design_view && state.compare_baseline));
            if (state.session.project().solar_system && !state.design_view) {
                ImGui::Spacing();
                ImGui::TextWrapped("Shape %s using its original surface, atmosphere and features.", body.name.c_str());
                ImGui::PushStyleColor(ImGuiCol_Button, {0.12f, 0.38f, 0.39f, 1.0f});
                if (ImGui::Button(("Design " + body.name).c_str(), {-1, 0})) {
                    begin_studio_design_focus(state, state.selected);
                }
                ImGui::PopStyleColor();
                ImGui::Spacing();
                ImGui::TextWrapped("1. Select a planet.\n2. Adjust parameters or sculpt in close-up.\n3. Run simulation to explore orbits and collisions.");
            } else if (state.session.project().solar_system) edit_solar_design(state);
            else edit_appearance(state);
            ImGui::EndDisabled();
          }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(simulating ? "Initial state" : "Orbit / Physics")) {
          if (simulating) draw_initial_motion(state.demos.project(state.session.project()).bodies[state.selected]);
          else {
            ImGui::BeginDisabled(state.session.mode() != SessionMode::Design ||
                                 state.demos.active() == DemoPreset::Collision);
            edit_motion(state);
            ImGui::EndDisabled();
          }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Pixel", nullptr,
                                state.select_pixel_tab ? ImGuiTabItemFlags_SetSelected : 0)) {
            state.pixel_tab_active = true;
            // Entering Pixel selects the tool its instructions describe. Do this only
            // on entry, so disabling Inspect or resetting the view remains effective.
            if (!was_pixel_tab_active && !state.camera_transition && !state.inspect_pixel) {
                state.inspect_pixel = true;
                state.selected_pixel_x = state.selected_pixel_y = -1;
                state.sculpt_stroke.end();
            }
            draw_pixel_inspector(state, frame);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    state.select_pixel_tab = false;
    ImGui::EndDisabled();
    ImGui::EndChild();
}

struct SceneNameTag {
    int body_index;
    ImVec2 min, max;
};

std::vector<SceneNameTag> draw_scene_names(StudioState& state, const StudioRenderInput& input, const RenderFrame& frame,
                                         ImVec2 origin, ImVec2 size, bool flip_y) {
    std::vector<SceneNameTag> tags;
    if (!studio_scene_annotations_visible(state) || frame.panel_count != 1) return tags;
    const auto& current = state.session.project().bodies;
    const auto& rendered = state.frame_cache.displayed_input
        ? state.frame_cache.displayed_input->project.bodies : input.project.bodies;
    if (current.size() != rendered.size() ||
        !std::equal(current.begin(), current.end(), rendered.begin(),
                     [](const auto& a, const auto& b) { return a.id == b.id; })) {
        // Loading/removing/reordering bodies can precede publication of the new image.
        state.body_markers.clear();
        state.marker_render_count = static_cast<std::uint64_t>(-1);
        state.marker_pixels = nullptr;
        return tags;
    }
    if (state.marker_render_count != state.frame_cache.render_count ||
        state.marker_pixels != frame.body_index.data()) {
        state.body_markers = studio_body_markers(frame, static_cast<int>(state.session.project().bodies.size()));
        state.marker_render_count = state.frame_cache.render_count;
        state.marker_pixels = frame.body_index.data();
    }
    auto markers = state.body_markers;
    std::stable_sort(markers.begin(), markers.end(), [&](const auto& a, const auto& b) {
        return a.body_index == state.selected && b.body_index != state.selected;
    });
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
    const float sx = size.x / frame.image.width, sy = size.y / frame.image.height;
    for (const auto& marker : markers) {
        if (marker.body_index < 0 || marker.body_index >= static_cast<int>(current.size())) continue;
        const bool selected = marker.body_index == state.selected;
        const int min_y = flip_y ? frame.image.height - 1 - marker.max_y : marker.min_y;
        const int max_y = flip_y ? frame.image.height - 1 - marker.min_y : marker.max_y;
        const ImVec2 a{origin.x + marker.min_x * sx, origin.y + min_y * sy};
        const ImVec2 b{origin.x + (marker.max_x + 1) * sx, origin.y + (max_y + 1) * sy};
        const ImU32 accent = selected ? IM_COL32(106, 220, 210, 255) : IM_COL32(190, 205, 220, 225);
        if (selected) {
            const float corner = std::min(12.0f, std::max(5.0f, (b.x - a.x) * 0.15f));
            for (int x_side : {-1, 1}) {
                for (int y_side : {-1, 1}) {
                    const ImVec2 p{x_side < 0 ? a.x - 4 : b.x + 4, y_side < 0 ? a.y - 4 : b.y + 4};
                    draw->AddLine(p, {p.x - x_side * corner, p.y}, accent, 1.5f);
                    draw->AddLine(p, {p.x, p.y - y_side * corner}, accent, 1.5f);
                }
            }
        }
        const auto& name = state.session.project().bodies[marker.body_index].name;
        const char* end = name.c_str() + name.size();
        const float max_text_width = std::max(20.0f, std::min(150.0f, size.x - 24.0f));
        while (end > name.c_str() && ImGui::CalcTextSize(name.c_str(), end).x > max_text_width) {
            --end;
            while (end > name.c_str() && (static_cast<unsigned char>(*end) & 0xc0) == 0x80) --end;
        }
        const ImVec2 text_size = ImGui::CalcTextSize(name.c_str(), end);
        const ImVec2 tag_size{text_size.x + 14.0f, text_size.y + 8.0f};
        const float center = (a.x + b.x) * 0.5f;
        for (int attempt = 0; attempt < 10; ++attempt) {
            const float candidate_y = attempt % 2 == 0
                ? a.y - tag_size.y - 10.0f - (attempt / 2) * (tag_size.y + 3.0f)
                : b.y + 10.0f + (attempt / 2) * (tag_size.y + 3.0f);
            const ImVec2 p{std::clamp(center + 8.0f, origin.x + 4.0f,
                                      origin.x + size.x - tag_size.x - 4.0f),
                           std::clamp(candidate_y, origin.y + 4.0f,
                                      origin.y + size.y - tag_size.y - 4.0f)};
            const ImVec2 q{p.x + tag_size.x, p.y + tag_size.y};
            const bool overlap = std::any_of(tags.begin(), tags.end(), [&](const auto& tag) {
                return p.x < tag.max.x + 3 && q.x + 3 > tag.min.x &&
                       p.y < tag.max.y + 3 && q.y + 3 > tag.min.y;
            });
            if (overlap) continue;
            const ImVec2 leader{center, p.y + tag_size.y <= a.y ? a.y : b.y};
            draw->AddLine(leader, {p.x, p.y + tag_size.y * 0.5f},
                          selected ? IM_COL32(106, 220, 210, 130) : IM_COL32(160, 180, 200, 75));
            draw->AddRectFilled(p, q, IM_COL32(12, 21, 31, 225), 4.0f);
            if (selected) draw->AddRect(p, q, accent, 4.0f);
            draw->AddText({p.x + 7, p.y + 4}, accent, name.c_str(), end);
            tags.push_back({marker.body_index, p, q});
            break;
        }
    }
    draw->PopClipRect();
    return tags;
}

void draw_scene(StudioState& state, ImTextureID texture, const StudioRenderInput& input,
                const RenderFrame& frame,
                bool flip_image_y, float width) {
    ImGui::BeginChild("##scene", ImVec2(width, 0));
    const SceneSnapshot& snapshot = input.snapshot;
    ImGui::BeginDisabled(state.camera_transition.has_value());
    if (ImGui::Button("Reset view")) {
        reset_studio_view(state);
    }
    ImGui::SameLine();
    if (state.session.project().solar_system && state.session.mode() == SessionMode::Design &&
        state.demos.active() != DemoPreset::Collision) {
        const bool leaving = state.design_view || state.demos.active() != DemoPreset::None;
        if (ImGui::Button(leaving ? "Back to system" : "Design selected")) {
            if (leaving) {
                return_studio_overview(state);
            } else {
                begin_studio_design_focus(state, state.selected);
            }
        }
    } else {
        const auto& selected_id = state.session.project().bodies[state.selected].id;
        const bool present = std::any_of(snapshot.bodies.begin(), snapshot.bodies.end(),
                                         [&](const auto& body) { return body.id == selected_id; });
        ImGui::BeginDisabled(!present);
        if (ImGui::Button("Focus selected")) focus_studio_selected(state);
        if (!present) ImGui::SetItemTooltip("Select a planet that is active in this snapshot.");
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Compare", &state.compare_modes)) {
        state.selected_pixel_x = state.selected_pixel_y = -1;
        state.sculpt_stroke.end();
        if (state.compare_modes) {
            state.inspect_pixel = true;
            state.select_pixel_tab = true;
            state.selected_pixel_x = std::max(1, state.render.width / 2) / 2;
            state.selected_pixel_y = state.render.height / 2;
            if (state.session.mode() == SessionMode::Simulate) state.session.set_playing(false);
            if (state.demos.active() == DemoPreset::Collision) state.demos.set_playing(false);
        }
    }
    if (!state.compare_modes) {
        ImGui::SameLine();
        if (ImGui::Checkbox("Inspect", &state.inspect_pixel)) {
            state.selected_pixel_x = state.selected_pixel_y = -1;
            state.sculpt_stroke.end();
            if (state.inspect_pixel) state.select_pixel_tab = true;
        }
    }
    if (state.compare_modes) {
        sampling_combo("Left", state.compare_left);
        ImGui::SameLine();
        sampling_combo("Right", state.compare_right);
        if (state.session.mode() == SessionMode::Simulate ||
            state.demos.active() == DemoPreset::Collision) {
            ImGui::TextDisabled("Playback paused for comparison");
        }
    } else {
        for (SamplingMode mode : {SamplingMode::Single, SamplingMode::SSAA4,
                                  SamplingMode::MSAA4}) {
            if (mode != SamplingMode::Single) ImGui::SameLine();
            if (ImGui::RadioButton(sampling_name(mode), state.render.sampling == mode)) {
                state.render.sampling = mode;
            }
        }
    }
    const bool orbit_lab = state.session.project().solar_system &&
                           state.session.mode() == SessionMode::Simulate &&
                           state.demos.active() == DemoPreset::None;
    if (orbit_lab) {
        const double current_time = snapshot.time;
        float time = static_cast<float>(current_time);
        const float max_time = static_cast<float>(studio_timeline_limit(state));
        ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - 65.0f));
        if (ImGui::SliderFloat("Time", &time, 0.0f, max_time, "%.2f",
                               ImGuiSliderFlags_AlwaysClamp)) {
            seek_studio_time(state, time);
        }
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    const int image_width = frame.image.width;
    const int image_height = frame.image.height;
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float image_space_height = std::max(100.0f, available.y - 70.0f);
    const float fit = std::min(available.x / image_width, image_space_height / image_height);
    const ImVec2 size(image_width * fit, image_height * fit);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (available.x - size.x) * 0.5f));
    const ImVec2 top_left = ImGui::GetCursorScreenPos();
    ImGui::Image(texture, size, flip_image_y ? ImVec2(0, 1) : ImVec2(0, 0),
                 flip_image_y ? ImVec2(1, 0) : ImVec2(1, 1));
    const auto name_tags = draw_scene_names(state, input, frame, top_left, size, flip_image_y);
    if (frame.panel_count == 2) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float middle = top_left.x + size.x * 0.5f;
        draw->AddLine({middle, top_left.y}, {middle, top_left.y + size.y},
                      IM_COL32(245, 245, 245, 220), 1.0f);
        draw->AddText({top_left.x + 8.0f, top_left.y + 8.0f}, IM_COL32(230, 235, 240, 255),
                      sampling_name(frame.panel_sampling[0]));
        draw->AddText({middle + 8.0f, top_left.y + 8.0f}, IM_COL32(230, 235, 240, 255),
                      sampling_name(frame.panel_sampling[1]));
    }
    if (state.inspect_pixel && frame.trace) {
        const auto region = studio_pixel_region(frame, frame.trace->x, frame.trace->y,
                                                  state.pixel_region_size);
        const int panel_width = image_width / frame.panel_count;
        for (int panel = 0; panel < frame.panel_count && region.size > 0; ++panel) {
            const int y = flip_image_y ? image_height - region.y - region.size : region.y;
            const ImVec2 a(top_left.x + (panel * panel_width + region.x) * fit,
                            top_left.y + y * fit);
            ImGui::GetWindowDrawList()->AddRect(a,
                {a.x + region.size * fit, a.y + region.size * fit},
                IM_COL32(255, 255, 255, 255), 0.0f, 0, 1.5f);
        }
    }
    ImGui::SetCursorScreenPos(top_left);
    ImGui::InvisibleButton("##scene-interaction", size, ImGuiButtonFlags_EnableNav);
    // Keyboard focus is useful for pixel nudging, but must not count as mouse hover.
    const bool mouse_over_scene = ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride);
    if (state.inspect_pixel && ImGui::IsItemFocused()) {
        // Keep arrow keys on the sampled pixel instead of navigating to another control.
        for (ImGuiKey key : {ImGuiKey_LeftArrow, ImGuiKey_RightArrow,
                             ImGuiKey_UpArrow, ImGuiKey_DownArrow}) {
            ImGui::SetItemKeyOwner(key);
        }
    }
    if (!state.camera_transition && mouse_over_scene) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const double minimum_zoom = state.design_view && state.session.project().solar_system
                ? 0.4 : 0.08;
            state.render.camera.zoom = std::clamp(
                state.render.camera.zoom * std::pow(0.88, wheel), minimum_zoom, 3.0);
        }
    }
    const bool design_canvas = state.design_view && state.session.project().solar_system &&
                               state.session.mode() == SessionMode::Design;
    const bool sculpting = design_canvas && state.design_tab_active && !state.inspect_pixel && !state.compare_modes &&
                           !state.pixel_tab_active &&
                           !state.camera_transition;
    const ImGuiMouseButton rotate_button = design_canvas || state.inspect_pixel || state.pixel_tab_active
        ? ImGuiMouseButton_Right : ImGuiMouseButton_Left;
    if (!state.camera_transition && mouse_over_scene &&
        ImGui::IsMouseDragging(rotate_button, 2.0f)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        state.render.camera.yaw += delta.x * 0.006;
        state.render.camera.pitch = std::clamp(state.render.camera.pitch + delta.y * 0.006,
                                               -1.3, 1.3);
    }
    const bool frame_interactive = studio_frame_can_interact(state);
    if (!frame_interactive) {
        ImGui::GetWindowDrawList()->AddText({top_left.x + 8.0f, top_left.y + 28.0f},
            IM_COL32(210, 220, 235, 255),
            state.frame_cache.displayed_input ? "Updating view..." : "Loading view...");
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !frame_interactive) state.sculpt_stroke.end();
    if (frame_interactive && sculpting && !state.compare_baseline && mouse_over_scene &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const int x = std::clamp(static_cast<int>((mouse.x - top_left.x) / size.x * image_width),
                                 0, image_width - 1);
        int y = std::clamp(static_cast<int>((mouse.y - top_left.y) / size.y * image_height),
                           0, image_height - 1);
        if (flip_image_y) y = image_height - 1 - y;
        const BodyKind kind = state.session.project().bodies[state.selected].kind;
        std::vector<Vec2> dabs;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            state.sculpt_stroke.begin(frame, state.selected, x, y)) {
            state.history.finish();
            dabs.push_back(frame.surface_uv[static_cast<std::size_t>(y) * image_width + x]);
        } else {
            dabs = state.sculpt_stroke.move(frame, state.selected, x, y);
        }
        if (!dabs.empty()) {
            Project next = state.session.project();
            for (const Vec2 uv : dabs) {
                apply_sculpt_brush(next.bodies[state.selected].design, kind, state.sculpt_layer,
                                   state.sculpt_mode, uv.x, uv.y, state.sculpt_radius,
                                   state.sculpt_strength);
            }
            replace_project(state, std::move(next), "Sculpt");
        }
    }
    if (frame_interactive && !state.camera_transition && mouse_over_scene &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const int x = std::clamp(static_cast<int>((mouse.x - top_left.x) / size.x * image_width),
                                 0, image_width - 1);
        int y = std::clamp(static_cast<int>((mouse.y - top_left.y) / size.y * image_height),
                           0, image_height - 1);
        if (flip_image_y) y = image_height - 1 - y;
        if (state.inspect_pixel) {
            state.selected_pixel_x = x;
            state.selected_pixel_y = y;
            state.select_pixel_tab = true;
        } else if (!state.pixel_tab_active) {
            int body = frame.body_index[static_cast<std::size_t>(y) * image_width + x];
            for (const auto& tag : name_tags) {
                if (mouse.x >= tag.min.x && mouse.x <= tag.max.x &&
                    mouse.y >= tag.min.y && mouse.y <= tag.max.y) {
                    body = tag.body_index;
                    break;
                }
            }
            if (body >= 0 && !state.design_view) select_studio_scene_body(state, body);
        }
    }
    if (frame_interactive && !state.camera_transition && state.inspect_pixel && ImGui::IsItemFocused() &&
        state.selected_pixel_x >= 0 && state.selected_pixel_y >= 0) {
        const int panel_width = image_width / frame.panel_count;
        const int panel = std::min(state.selected_pixel_x / panel_width, frame.panel_count - 1);
        int x = state.selected_pixel_x % panel_width;
        int y = state.selected_pixel_y;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) --x;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) ++x;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) y += flip_image_y ? 1 : -1;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) y += flip_image_y ? -1 : 1;
        state.selected_pixel_x = panel * panel_width + std::clamp(x, 0, panel_width - 1);
        state.selected_pixel_y = std::clamp(y, 0, image_height - 1);
    }
    const auto workflow = studio_workflow(state);
    ImGui::TextColored({0.43f, 0.83f, 0.79f, 1.0f}, "%s", workflow.title.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, {0.65f, 0.72f, 0.80f, 1.0f});
    ImGui::TextWrapped("%s", workflow.hint.c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();
}

} // namespace

StudioPixelRegion studio_pixel_region(const RenderFrame& frame, int pixel_x, int pixel_y,
                                      int cells) {
    if (frame.panel_count < 1 || frame.panel_count > 2 || frame.image.width <= 0 ||
        frame.image.height <= 0 || pixel_x < 0 || pixel_x >= frame.image.width ||
        pixel_y < 0 || pixel_y >= frame.image.height || cells <= 0) return {};
    const int width = frame.image.width / frame.panel_count;
    const int size = std::min({cells, width, frame.image.height});
    if (size <= 0) return {};
    return {std::clamp(pixel_x % width - size / 2, 0, width - size),
            std::clamp(pixel_y - size / 2, 0, frame.image.height - size), size};
}

StudioState::StudioState(Project project, const std::string& project_path)
    : session(std::move(project)), history(session.project()),
      document_path(std::filesystem::u8path(project_path)) {
    std::snprintf(path.data(), path.size(), "%s", project_path.c_str());
    if (session.project().solar_system) render.camera.zoom = 0.55;
    select_body(*this, session.project().bodies.size() > 1 ? 1 : 0);
}

void begin_studio_design_focus(StudioState& state, int body_index) {
    if (!state.session.project().solar_system || state.session.mode() != SessionMode::Design ||
        state.demos.active() == DemoPreset::Collision) return;
    if (body_index < 0 || body_index >= static_cast<int>(state.session.project().bodies.size())) return;
    const bool transferring = state.design_view && state.selected != body_index;
    const Vec2 departing = state.session.project().bodies[state.selected].position;
    const RenderCamera from = world_camera(state);
    if (state.demos.active() != DemoPreset::None) {
        exit_studio_preset(state);
        state.design_view = false;
        state.render.camera = from;
    }
    select_body(state, body_index);
    state.design_view = false;
    state.compare_baseline = false;
    const Project& project = state.session.project();
    const BodySpec& body = project.bodies[body_index];
    RenderCamera detail;
    detail.yaw = from.yaw;
    detail.pitch = from.pitch;
    detail.zoom = design_zoom;
    RenderCamera to = detail;
    to.target = body.position;
    to.zoom *= detail_extent(project, body) / project.view_extent;
    start_camera_transition(state, from, to, StudioCameraDestination::Design,
                            detail, false, body.position);
    if (transferring) {
        RenderCamera via = overview_camera(state);
        via.yaw = from.yaw;
        via.pitch = from.pitch;
        state.camera_transition->via = via;
        state.camera_transition->departure_anchor = departing;
        state.camera_transition->duration = 1.5;
    }
}

void select_studio_body(StudioState& state, int body_index) {
    if (state.session.mode() == SessionMode::Design && state.session.project().solar_system &&
        state.demos.active() != DemoPreset::Collision) {
        // Reselecting the open closeup must not restart its camera flight or
        // discard the current pixel analysis. Explicit Reset view still works.
        if (state.design_view && state.selected == body_index && !state.camera_transition) return;
        begin_studio_design_focus(state, body_index);
    } else {
        if (state.demos.active() != DemoPreset::None) exit_studio_preset(state);
        select_body(state, body_index);
    }
}

void select_studio_scene_body(StudioState& state, int body_index) {
    if (state.demos.active() == DemoPreset::Collision) {
        select_body(state, body_index);
    } else {
        select_studio_body(state, body_index);
    }
}

void return_studio_overview(StudioState& state) {
    RenderCamera from = world_camera(state);
    std::optional<Vec2> anchor = state.session.project().solar_system
        ? std::optional<Vec2>(state.session.project().bodies[state.selected].position)
        : std::nullopt;
    const DemoPreset preset = state.demos.active();
    if (preset != DemoPreset::None) {
        exit_studio_preset(state);
        if (preset == DemoPreset::Collision) {
            from = world_camera(state);
            anchor = state.session.project().bodies[state.selected].position;
        }
    }
    state.design_view = false;
    state.compare_baseline = false;
    start_camera_transition(state, from, overview_camera(state),
                            StudioCameraDestination::Overview, {}, false, anchor);
}

void begin_studio_simulation(StudioState& state) {
    if (state.session.mode() != SessionMode::Design) return;
    const DemoPreset preset = state.demos.active();
    const int focused = state.selected;
    RenderCamera from = world_camera(state);
    std::optional<Vec2> anchor = state.session.project().solar_system
        ? std::optional<Vec2>(state.session.project().bodies[state.selected].position)
        : std::nullopt;
    if (preset != DemoPreset::None) {
        exit_studio_preset(state);
        if (preset != DemoPreset::Collision) select_body(state, focused);
        else {
            from = world_camera(state);
            anchor = state.session.project().bodies[state.selected].position;
        }
    }
    state.session.begin_simulation();
    state.session.set_playing(false);
    state.design_view = false;
    state.compare_baseline = false;
    state.timeline_horizon = 80.0;
    start_camera_transition(state, from, overview_camera(state),
                            StudioCameraDestination::Simulation, {}, true, anchor);
}

void return_studio_design(StudioState& state) {
    if (state.session.mode() != SessionMode::Simulate) return;
    const RenderCamera from = world_camera(state);
    state.session.set_playing(false);
    const std::optional<Vec2> anchor = state.session.project().solar_system
        ? std::optional<Vec2>(state.session.project().bodies[state.selected].position)
        : std::nullopt;
    state.design_view = false;
    state.timeline_horizon = 80.0;
    start_camera_transition(state, from, overview_camera(state),
                            StudioCameraDestination::DesignOverview, {}, false, anchor);
}

void focus_studio_selected(StudioState& state) {
    const Project& project = state.session.project();
    const BodySpec& selected = project.bodies[state.selected];
    const SceneSnapshot snapshot = state.demos.snapshot(state.session.snapshot());
    const auto found = std::find_if(snapshot.bodies.begin(), snapshot.bodies.end(),
                                    [&](const BodyState& body) { return body.id == selected.id; });
    const Vec2 position = found == snapshot.bodies.end() ? selected.position : found->position;
    const double radius = found == snapshot.bodies.end() ? selected.radius : found->radius;
    const double visible_radius = project.solar_system ? selected.display_radius : radius;
    const RenderCamera from = world_camera(state);
    RenderCamera to = from;
    to.target = position;
    to.zoom = std::clamp(visible_radius * project.display_scale /
                             (project.view_extent * 0.55), 0.08, 1.0);
    const bool resume = state.session.mode() == SessionMode::Simulate && state.session.playing();
    if (resume) state.session.set_playing(false);
    start_camera_transition(state, from, to, StudioCameraDestination::Camera, {}, resume, position);
}

void reset_studio_view(StudioState& state) {
    const RenderCamera from = state.render.camera;
    RenderCamera to = overview_camera(state);
    if (state.demos.active() == DemoPreset::Collision) {
        to.target = {4.6, 0.0};
        to.zoom = 0.45;
    } else if (state.demos.active() == DemoPreset::EarthTerrain) {
        to.yaw = -0.5;
        to.pitch = 0.23;
        to.zoom = 0.62;
    } else if (state.demos.active() == DemoPreset::SaturnRing) {
        to.pitch = 0.36;
        to.zoom = 0.72;
    } else if (state.design_view && state.session.project().solar_system) {
        to.zoom = design_zoom;
    }
    const bool resume = state.session.mode() == SessionMode::Simulate && state.session.playing();
    if (resume) state.session.set_playing(false);
    start_camera_transition(state, from, to, StudioCameraDestination::Camera, {}, resume);
}

void activate_studio_preset(StudioState& state, DemoPreset preset) {
    if (preset == DemoPreset::None) { exit_studio_preset(state); return; }
    if (state.session.mode() != SessionMode::Design) {
        throw std::logic_error("Return to Design before starting a demo");
    }
    state.demos.activate(preset, state.session.project());
    state.camera_transition.reset();
    if (!state.saved_view) {
        state.saved_view = StudioState::SavedView{state.selected, state.render.camera,
            state.design_view, state.compare_baseline, state.compare_modes,
            state.inspect_pixel, state.selected_pixel_x, state.selected_pixel_y};
    }
    state.sculpt_stroke.end();
    state.compare_baseline = false;
    state.compare_modes = state.compare_modes && preset != DemoPreset::Collision;
    state.inspect_pixel = state.compare_modes;
    state.selected_pixel_x = state.selected_pixel_y = -1;
    if (state.compare_modes) {
        state.selected_pixel_x = std::max(1, state.render.width / 2) / 2;
        state.selected_pixel_y = state.render.height / 2;
        state.select_pixel_tab = true;
    }
    select_body(state, preset == DemoPreset::SaturnRing ? 6 : 3);
    state.render.camera = {};
    if (preset == DemoPreset::Collision) {
        state.design_view = false;
        state.render.camera.target = {4.6, 0.0};
        state.render.camera.zoom = 0.45;
    } else {
        state.design_view = true;
        state.render.camera.zoom = preset == DemoPreset::EarthTerrain ? 0.62 : 0.72;
        state.render.camera.yaw = preset == DemoPreset::EarthTerrain ? -0.5 : 0.0;
        state.render.camera.pitch = preset == DemoPreset::EarthTerrain ? 0.23 : 0.36;
    }
}

void exit_studio_preset(StudioState& state) {
    state.camera_transition.reset();
    state.demos.clear();
    if (!state.saved_view) return;
    const StudioState::SavedView saved = *state.saved_view;
    state.saved_view.reset();
    select_body(state, saved.selected);
    state.render.camera = saved.camera;
    state.design_view = saved.design_view;
    state.compare_baseline = saved.compare_baseline;
    state.compare_modes = saved.compare_modes;
    state.inspect_pixel = saved.inspect_pixel;
    state.selected_pixel_x = saved.pixel_x;
    state.selected_pixel_y = saved.pixel_y;
}

void advance_studio(StudioState& state, double real_seconds) {
    if (state.camera_transition) {
        StudioCameraTransition& flight = *state.camera_transition;
        if (std::isfinite(real_seconds) && real_seconds > 0.0) {
            flight.elapsed = std::min(flight.duration, flight.elapsed + std::min(real_seconds, 0.75));
        }
        const double fraction = flight.elapsed / flight.duration;
        state.render.camera = camera_between(flight, fraction);
        if (fraction >= 1.0) {
            if (flight.destination == StudioCameraDestination::Design) {
                state.design_view = true;
                state.render.camera = flight.detail_camera;
            } else if (flight.destination == StudioCameraDestination::DesignOverview) {
                state.session.return_to_design();
            } else if (flight.destination == StudioCameraDestination::Simulation &&
                       flight.resume_playback) {
                state.session.set_playing(true);
            } else if (flight.destination == StudioCameraDestination::Camera &&
                       flight.resume_playback && state.session.mode() == SessionMode::Simulate) {
                state.session.set_playing(true);
            }
            state.camera_transition.reset();
        }
        return;
    }
    if (state.compare_modes) return;
    if (state.demos.active() == DemoPreset::Collision) {
        state.demos.advance(real_seconds, state.session.speed());
    } else {
        state.session.advance(real_seconds);
        if (state.session.mode() == SessionMode::Simulate) {
            state.timeline_horizon = std::max(state.timeline_horizon,
                                               std::ceil(state.session.snapshot().time));
        }
    }
}

double studio_timeline_limit(const StudioState& state) {
    return std::min(100000.0, std::max({80.0, state.timeline_horizon,
                                        std::ceil(state.session.snapshot().time)}));
}

void seek_studio_time(StudioState& state, double time) {
    if (state.session.mode() != SessionMode::Simulate ||
        !std::isfinite(time) || time > 100000.0) return;
    const double target = std::max(0.0, time);
    state.session.seek(target);
    ++state.scene_generation;
    state.timeline_horizon = std::max(state.timeline_horizon, std::ceil(target));
}

StudioRenderInput make_studio_render_input(const StudioState& state) {
    StudioRenderInput result{state.demos.project(state.session.project()),
                             state.demos.snapshot(state.session.snapshot()), state.render};
    if (state.show_reference_orbit && state.session.mode() == SessionMode::Simulate &&
        state.session.project().solar_system) {
        const std::string& id = state.session.project().bodies[state.selected].id;
        const auto reference = state.session.reference_snapshot();
        if (reference) {
            for (const BodyTrail& trail : reference->trails) {
                if (trail.id == id) result.snapshot.reference_trails.push_back(trail);
            }
        }
        result.snapshot.trails.erase(
            std::remove_if(result.snapshot.trails.begin(), result.snapshot.trails.end(),
                           [&](const BodyTrail& trail) { return trail.id != id; }),
            result.snapshot.trails.end());
    }
    if (state.design_view && result.project.solar_system &&
        state.session.mode() == SessionMode::Design) {
        const BodySpec& selected = result.project.bodies[state.selected];
        if (state.compare_baseline) {
            result.project.bodies[state.selected].design = baseline_design(selected.kind);
        }
        result.snapshot.bodies = {{selected.id, {0, 0}, {}, selected.mass, selected.radius}};
        result.snapshot.trails.clear();
        result.snapshot.events.clear();
        const double framing = selected.kind == BodyKind::Saturn ? 2.0 :
                               selected.kind == BodyKind::Uranus ? 1.4 : 0.95;
        result.project.view_extent = selected.display_radius * result.project.display_scale * framing;
        result.config.camera.target = {};
    }
    if (state.compare_modes) {
        result.config.width = std::max(1, result.config.width / 2);
        result.config.camera.zoom *= 2.0;
    }
    if (state.inspect_pixel && state.selected_pixel_x >= 0 && state.selected_pixel_y >= 0) {
        result.config.trace_x = state.selected_pixel_x;
        result.config.trace_y = state.selected_pixel_y;
    }
    if (state.camera_transition && !state.compare_modes && !state.inspect_pixel &&
        !state.pixel_tab_active) result.config.sampling = SamplingMode::Single;
    return result;
}

RenderFrame render_studio_frame(const StudioState& state, const StudioRenderInput& input) {
    if (state.compare_modes) {
        return render_comparison_scene(input.project, input.snapshot, input.config,
                                       state.compare_left, state.compare_right);
    }
    return render_scene(input.project, input.snapshot, input.config);
}

void draw_studio(StudioState& state, ImTextureID texture, const StudioRenderInput& input,
                 const RenderFrame& frame,
                 bool flip_image_y) {
    const ImGuiIO& io = ImGui::GetIO();
    update_studio_document(state, 0.0);
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Planet Studio", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    ImGui::TextUnformatted("PLANET STUDIO");
    ImGui::SameLine();
    ImGui::SetCursorPosX(210.0f);
    ImGui::BeginDisabled(state.camera_transition.has_value());
    if (state.demos.active() == DemoPreset::Collision) {
        ImGui::TextDisabled("COLLISION DEMO");
        ImGui::SameLine();
        if (ImGui::Button("Exit demo")) exit_studio_preset(state);
        ImGui::SameLine();
        if (playback_button(state.demos.playing(), state.compare_modes)) {
            state.demos.set_playing(!state.demos.playing());
        }
        ImGui::SameLine();
        if (ImGui::Button("Step")) {
            state.demos.step(0.025);
            ++state.scene_generation;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            state.demos.reset();
            ++state.scene_generation;
        }
        ImGui::SameLine();
        float speed = static_cast<float>(state.session.speed());
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::SliderFloat("Speed", &speed, 0.1f, 20.0f, "%.1fx")) state.session.set_speed(speed);
        ImGui::SameLine();
        ImGui::Text("t=%.3f", input.snapshot.time);
    } else if (state.session.mode() == SessionMode::Design) {
        ImGui::TextDisabled("DESIGN");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.12f, 0.38f, 0.39f, 1.0f});
        if (ImGui::Button("Run simulation")) begin_studio_simulation(state);
        ImGui::SetItemTooltip("Fly back to the full system and simulate your current design.\nReturn to Design to keep editing.");
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("Design")) return_studio_design(state);
        ImGui::SameLine();
        if (playback_button(state.session.playing(), state.compare_modes)) {
            state.session.set_playing(!state.session.playing());
        }
        ImGui::SameLine();
        if (ImGui::Button("Step")) {
            state.session.step(0.025);
            ++state.scene_generation;
            state.timeline_horizon = std::max(state.timeline_horizon,
                                               std::ceil(state.session.snapshot().time));
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            state.session.reset_simulation();
            ++state.scene_generation;
            state.timeline_horizon = 80.0;
        }
        ImGui::SameLine();
        float speed = static_cast<float>(state.session.speed());
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::SliderFloat("Speed", &speed, 0.1f, 20.0f, "%.1fx")) state.session.set_speed(speed);
        ImGui::SameLine();
        ImGui::Text("t=%.3f", input.snapshot.time);
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    const float total = ImGui::GetContentRegionAvail().x;
    const float sidebar = std::clamp(total * 0.19f, 190.0f, 235.0f);
    const float inspector = std::clamp(total * 0.25f, 275.0f, 335.0f);
    draw_sidebar(state, sidebar);
    ImGui::SameLine();
    draw_scene(state, texture, input, frame, flip_image_y,
               std::max(300.0f, total - sidebar - inspector - 16.0f));
    ImGui::SameLine();
    draw_inspector(state, frame, inspector);
    if (!io.WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
        (io.KeyCtrl || io.KeySuper)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (io.KeyShift) redo_studio_edit(state); else undo_studio_edit(state);
        } else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo_studio_edit(state);
    }
    if (!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) state.history.finish();
    update_studio_document(state, 0.0);
    if (state.recovery_offer && !ImGui::IsPopupOpen("Recover unsaved design")) {
        ImGui::OpenPopup("Recover unsaved design");
    }
    if (ImGui::BeginPopupModal("Recover unsaved design", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("An unsaved design is available from your last session.");
        if (state.recovery_offer && state.recovery_offer->source_changed) {
            ImGui::TextUnformatted("The saved project has changed since this draft was created.");
            ImGui::TextUnformatted("Restoring loads the draft for review; Save updates the project.");
        }
        if (ImGui::Button("Restore draft") && restore_studio_draft(state)) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ImGui::Button("Discard draft")) { discard_studio_draft(state); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (state.pending_load && !ImGui::IsPopupOpen("Unsaved changes")) ImGui::OpenPopup("Unsaved changes");
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Loading will replace the edits in this view.");
        ImGui::TextUnformatted("Cancel to save your design first.");
        if (ImGui::Button("Load and discard edits")) {
            const auto target = *state.pending_load;
            state.pending_load.reset();
            load_studio_document(state, target, true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { state.pending_load.reset(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::End();
}

} // namespace planet_sandbox
