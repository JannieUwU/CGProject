#include "planet_sandbox/studio_ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace planet_sandbox {
namespace {

const char* surface_name(TraceSurface surface) {
    switch (surface) {
    case TraceSurface::Body: return "Surface";
    case TraceSurface::Ring: return "Ring";
    case TraceSurface::Cloud: return "Cloud";
    case TraceSurface::Trail: return "Trail";
    case TraceSurface::ReferenceTrail: return "Reference trail";
    case TraceSurface::Atmosphere: return "Atmosphere";
    default: return "Background";
    }
}

const char* action_name(TraceAction action) {
    switch (action) {
    case TraceAction::DepthRejected: return "Blocked by depth";
    case TraceAction::BackfaceRejected: return "Back-facing cloud";
    case TraceAction::Transparent: return "No opacity; no write";
    case TraceAction::Written: return "Color written";
    case TraceAction::Occluded: return "No visible ray interval";
    case TraceAction::InvalidAttributes: return "No valid interpolated attributes";
    default: return "Not covered";
    }
}

const char* point_issue_name(TracePointIssue issue) {
    switch(issue) {
    case TracePointIssue::OutsideCoverage:return "outside this triangle's coverage";
    case TracePointIssue::InvalidInterpolation:return "invalid interpolated attributes";
    case TracePointIssue::BackFacing:return "back-facing cloud position";
    case TracePointIssue::NoValidCoveredPoint:return "no valid geometry-based shading position";
    default:return "none";
    }
}

std::string depth_text(double value) {
    if (std::isinf(value)) return "infinity";
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

void color_value(const char* label, const std::array<double, 3>& color) {
    ImGui::TextWrapped("%s:", label);
    // Keep the values separate from the label so wrapping cannot split decimals
    // in the narrow inspector, including nested sample-detail sections.
    ImGui::Text("%.4f, %.4f, %.4f", color[0], color[1], color[2]);
}

std::string event_label(const TraceEvent& event, const Project& project, int index) {
    const std::string body = event.body_index >= 0 && event.body_index < int(project.bodies.size())
        ? project.bodies[event.body_index].name : "Scene";
    const char* primitive = event.primitive == TracePrimitive::Triangle ? "triangle" :
        event.primitive == TracePrimitive::TrailSegment ? "segment" : "pass";
    return std::to_string(index + 1) + ". " + body + " / " + surface_name(event.surface) +
        " / " + primitive + " " + std::to_string(event.primitive_id) +
        (event.clipped_part ? " (part " + std::to_string(event.clipped_part + 1) + ")" : "");
}

void sample_diagram(const TraceEvent& event) {
    const float size = std::min(160.0f, ImGui::GetContentRegionAvail().x);
    if (size < 48.0f) return;
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy({size, size});
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + size, origin.y + size}, IM_COL32(27, 32, 40, 255));
    const auto position = [&](Vec2 p) {
        return ImVec2(origin.x + float(p.x) * size, origin.y + float(p.y) * size);
    };
    const float radius = event.samples.size() > 4 ? 7.0f : 10.0f;
    for (std::size_t i = 0; i < event.samples.size(); ++i) {
        const auto& step = event.samples[i];
        const auto p = position(step.before.position);
        const auto color = !step.covered ? IM_COL32(130, 140, 155, 255) :
            step.action == TraceAction::Written ? IM_COL32(121, 220, 166, 255) :
                                                 IM_COL32(244, 178, 99, 255);
        draw->AddCircle(p, radius, color, 0, 2.0f);
        const auto label = std::to_string(i + 1);
        const auto extent = ImGui::CalcTextSize(label.c_str());
        draw->AddText({p.x - extent.x * .5f, p.y - extent.y * .5f}, color, label.c_str());
    }
    for (const auto& shading : event.shading) {
        const auto p = position(shading.position);
        const float r = radius + 5.0f;
        const ImVec2 points[] = {{p.x, p.y-r}, {p.x+r, p.y}, {p.x, p.y+r}, {p.x-r, p.y}};
        draw->AddPolyline(points, 4, IM_COL32(159, 193, 255, 255), ImDrawFlags_Closed, 2.0f);
    }
    ImGui::TextWrapped("Circles: samples. Diamonds: actual shading positions.");
    ImGui::TextWrapped("Green: written | Orange: skipped | Gray: outside");
}

} // namespace

void draw_pixel_process(StudioState& state, const PixelTrace& trace) {
    auto& disclosure = state.pixel_disclosure;
    ImGui::SetNextItemOpen(disclosure.history, ImGuiCond_Always);
    disclosure.history = ImGui::TreeNodeEx("Processing history", ImGuiTreeNodeFlags_SpanAvailWidth);
    if (!disclosure.history) {
        disclosure.close_history();
        return;
    }
    if (!trace.process_recorded) {
        ImGui::TextWrapped("Detailed recording is paused for timing. It resumes automatically afterwards.");
        ImGui::TreePop();
        return;
    }
    if (trace.process_truncated) {
        ImGui::TextWrapped("Showing the first %zu records only. The history limit was reached; final samples are complete.",
                           PixelTrace::max_events);
    }
    std::size_t calls = 0;
    for (const auto& event : trace.events) calls += event.shading.size();
    ImGui::TextWrapped("%zu processing records; %zu actual shading calls%s.", trace.events.size(), calls,
                       trace.process_truncated ? " in these records" : " for this pixel");
    ImGui::SetNextItemOpen(disclosure.background, ImGuiCond_Always);
    disclosure.background = ImGui::TreeNode("Initial background");
    if (disclosure.background) {
        for (std::size_t s = 0; s < trace.initial_samples.size(); ++s) {
            const auto label = "Sample " + std::to_string(s + 1) + " linear RGB";
            color_value(label.c_str(), trace.initial_samples[s].linear_rgb);
        }
        ImGui::TextWrapped("Initial depth: infinity. No geometry has written a sample yet.");
        ImGui::TreePop();
    }
    if (trace.events.empty()) {
        ImGui::TextWrapped("No geometry or effect covered a sample here. The background remains.");
        ImGui::TreePop();
        return;
    }
    if (state.trace_event_x != trace.x || state.trace_event_y != trace.y ||
        state.trace_event_sampling != trace.sampling) {
        state.trace_event_index = 0;
        state.trace_event_x = trace.x;
        state.trace_event_y = trace.y;
        state.trace_event_sampling = trace.sampling;
    }
    state.trace_event_index = std::clamp(state.trace_event_index, 0, int(trace.events.size()) - 1);
    const auto& project = state.session.project();
    const auto label = event_label(trace.events[state.trace_event_index], project, state.trace_event_index);
    ImGui::TextWrapped("%s", label.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    const std::string selection = "Record " + std::to_string(state.trace_event_index + 1) +
                                   " of " + std::to_string(trace.events.size());
    if (ImGui::BeginCombo("##process-record", selection.c_str())) {
        for (int i = 0; i < int(trace.events.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(event_label(trace.events[i], project, i).c_str(),
                                  state.trace_event_index == i)) state.trace_event_index = i;
            if (state.trace_event_index == i) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    const auto& event = trace.events[state.trace_event_index];
    int covered = 0, passed = 0, written = 0;
    for (const auto& step : event.samples) {
        covered += step.covered;
        passed += step.depth_test == TraceDepthTest::Passed;
        written += step.action == TraceAction::Written;
    }
    if (event.primitive == TracePrimitive::Atmosphere) {
        ImGui::TextWrapped("%d sample rays intersected the atmosphere; %zu shading calls; %d color writes. Depth stays unchanged.",
                           covered, event.shading.size(), written);
    } else {
        ImGui::TextWrapped("This %s covered %d of %zu samples. %d passed depth; %zu shading calls; %d color writes.",
            event.primitive == TracePrimitive::Triangle ? "triangle" : "segment",
            covered, event.samples.size(), passed, event.shading.size(), written);
    }
    if (event.primitive == TracePrimitive::TrailSegment)
        ImGui::TextWrapped("The trail uses a fixed input color; it makes no shader calls.");
    if(event.shading_failure!=TracePointIssue::None)
        ImGui::TextWrapped("No shader call: %s.",point_issue_name(event.shading_failure));
    sample_diagram(event);
    if (ImGui::BeginTable("##process-samples", 3, ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Sample", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("Covered", ImGuiTableColumnFlags_WidthFixed, 58.0f);
        ImGui::TableSetupColumn("Outcome");
        ImGui::TableHeadersRow();
        for (int s = 0; s < int(event.samples.size()); ++s) {
            const auto& step = event.samples[s];
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%d", s + 1);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(step.covered ? "Yes" : "No");
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", action_name(step.action));
        }
        ImGui::EndTable();
    }
    ImGui::SetNextItemOpen(disclosure.shading, ImGuiCond_Always);
    disclosure.shading = ImGui::TreeNode("Shading and depth values");
    if (disclosure.shading) {
        disclosure.samples.resize(event.samples.size(), false);
        ImGui::TextWrapped("Positions are inside this pixel (0 to 1). RGB values below are linear.");
        for (int i = 0; i < int(event.shading.size()); ++i) {
            const auto& shading = event.shading[i];
            ImGui::TextWrapped("Shading %d at (%.3f, %.3f), alpha %.4f", i + 1,
                               shading.position.x, shading.position.y, shading.alpha);
            const char* location=shading.location==TraceShadingLocation::PixelCenter?"Pixel center":
                shading.location==TraceShadingLocation::CoverageCentroid?"Coverage centroid":
                shading.location==TraceShadingLocation::CoveredSample?"Covered sample fallback":
                event.primitive==TracePrimitive::Atmosphere && trace.sampling==SamplingMode::MSAA4
                    ?"First eligible ray sample":"Sample position";
            ImGui::TextWrapped("Location rule: %s.",location);
            if(shading.location!=TraceShadingLocation::Sample)
                ImGui::TextWrapped("Chosen from geometry, independently of depth. The diamond is not an extra coverage sample.");
            if(shading.center_issue!=TracePointIssue::None)
                ImGui::TextWrapped("Center not used: %s.",point_issue_name(shading.center_issue));
            if(shading.centroid_issue!=TracePointIssue::None)
                ImGui::TextWrapped("Centroid not used: %s.",point_issue_name(shading.centroid_issue));
            color_value("Calculated RGB", shading.linear_rgb);
            std::string recipients;
            for (int s = 0; s < int(event.samples.size()); ++s)
                if (event.samples[s].shading_index == i && event.samples[s].action == TraceAction::Written)
                    recipients += (recipients.empty() ? "" : ", ") + std::to_string(s + 1);
            ImGui::TextWrapped("Written to samples: %s", recipients.empty() ? "none" : recipients.c_str());
        }
        for (int s = 0; s < int(event.samples.size()); ++s) {
            const auto& step = event.samples[s];
            ImGui::PushID(s);
            ImGui::SetNextItemOpen(disclosure.samples[s], ImGuiCond_Always);
            disclosure.samples[s] = ImGui::TreeNode("Sample", "Sample %d details", s + 1);
            if (disclosure.samples[s]) {
                const char* result = step.depth_test == TraceDepthTest::Passed ? "passed" :
                    step.depth_test == TraceDepthTest::Rejected ? "rejected" : "not performed";
                ImGui::TextWrapped("Depth test: %s (smaller is nearer).", result);
                ImGui::TextWrapped("Before: %s; candidate: %s; after: %s",
                    depth_text(step.before.depth).c_str(), step.candidate_depth ? depth_text(*step.candidate_depth).c_str() : "none",
                    depth_text(step.after.depth).c_str());
                if (step.ray_entry && step.ray_exit)
                    ImGui::TextWrapped("Ray interval: %.6g to %.6g", *step.ray_entry, *step.ray_exit);
                color_value("Before RGB", step.before.linear_rgb);
                if (step.action == TraceAction::Written) {
                    color_value("Source RGB", step.source_rgb);
                    ImGui::TextWrapped("%s; alpha %.4f", step.blend == TraceBlend::Replace ? "Replace" :
                        step.blend == TraceBlend::CloudBase ? "Blend over saved cloud-layer base" : "Alpha blend", step.alpha);
                    if (step.blend != TraceBlend::Replace) color_value("Actual blend base", step.blend_base_rgb);
                }
                color_value("After RGB", step.after.linear_rgb);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    } else {
        disclosure.close_shading();
    }
    ImGui::SetNextItemOpen(disclosure.resolve, ImGuiCond_Always);
    disclosure.resolve = ImGui::TreeNode("Final resolve");
    if (disclosure.resolve) {
        color_value("All final samples averaged", trace.resolved_linear_rgb);
        ImGui::TextWrapped("Display RGB: %u, %u, %u (after display conversion)",
                           trace.display_rgb[0], trace.display_rgb[1], trace.display_rgb[2]);
        ImGui::TreePop();
    }
    ImGui::TreePop();
}

} // namespace planet_sandbox
