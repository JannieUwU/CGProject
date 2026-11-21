#pragma once
#include "planet_sandbox/renderer.h"
#include <algorithm>
#include <cassert>
#include <cmath>

// Reconstruct all samples from initial state and actual writes. Comparing the
// entire state at each event detects missing passes as well as incorrect values.
inline void check_pixel_process(const planet_sandbox::PixelTrace& trace) {
    using namespace planet_sandbox;
    assert(trace.process_recorded && !trace.process_truncated);
    assert(trace.initial_samples.size() == trace.samples.size());
    auto samples = trace.initial_samples;
    const auto same = [](const TraceSample& a, const TraceSample& b) {
        assert(a.position.x == b.position.x && a.position.y == b.position.y);
        assert(a.depth == b.depth && a.body_index == b.body_index && a.surface == b.surface);
        assert(a.linear_rgb == b.linear_rgb);
    };
    for (const auto& event : trace.events) {
        assert(event.samples.size() == samples.size());
        if (trace.sampling == SamplingMode::MSAA4) assert(event.shading.size() <= 1);
        if (event.primitive == TracePrimitive::TrailSegment) assert(event.shading.empty());
        int covered = 0;
        for (std::size_t s = 0; s < samples.size(); ++s) {
            const auto& step = event.samples[s];
            same(step.before, samples[s]);
            covered += step.covered;
            if (!step.covered) {
                assert(step.action == TraceAction::Uncovered && !step.candidate_depth);
                assert(step.depth_test == TraceDepthTest::NotTested);
            }
            if (step.candidate_depth) {
                assert(step.covered);
                assert((step.depth_test == TraceDepthTest::Passed) == (*step.candidate_depth < step.before.depth));
            }
            if (step.depth_test == TraceDepthTest::Rejected)
                assert(step.action == TraceAction::DepthRejected);
            if (step.shading_index >= 0) {
                assert(step.shading_index < int(event.shading.size()));
                const auto& shading = event.shading[step.shading_index];
                assert(step.source_rgb == shading.linear_rgb && step.alpha == shading.alpha);
                if (trace.sampling != SamplingMode::MSAA4) {
                    assert(shading.position.x == step.before.position.x);
                    assert(shading.position.y == step.before.position.y);
                }
            }
            if (step.action == TraceAction::Written) {
                assert(step.covered);
                if (event.primitive == TracePrimitive::Atmosphere) {
                    assert(step.depth_test == TraceDepthTest::NotTested);
                    assert(step.ray_entry && step.ray_exit && *step.ray_exit > *step.ray_entry);
                    assert(step.after.depth == step.before.depth && step.after.body_index == step.before.body_index);
                } else {
                    assert(step.depth_test == TraceDepthTest::Passed);
                    assert(step.after.depth == *step.candidate_depth && step.after.body_index == event.body_index);
                }
                for (int c = 0; c < 3; ++c) {
                    const double expected = step.blend == TraceBlend::Replace ? step.source_rgb[c] :
                        step.blend_base_rgb[c] + (step.source_rgb[c] - step.blend_base_rgb[c]) * step.alpha;
                    assert(std::abs(expected - step.after.linear_rgb[c]) < 1e-12);
                }
            } else same(step.before, step.after);
            samples[s] = step.after;
        }
        assert(covered > 0);
        // Triangle location is geometry-based. The independent atmosphere pass
        // intentionally retains its first-eligible-ray rule.
        if (trace.sampling == SamplingMode::MSAA4 && !event.shading.empty()) {
            const auto& shading=event.shading.front();
            if(event.primitive==TracePrimitive::Triangle) {
                assert(shading.location!=TraceShadingLocation::Sample);
                if(shading.location==TraceShadingLocation::PixelCenter) {
                    assert(shading.position.x==.5 && shading.position.y==.5);
                    assert(shading.center_issue==TracePointIssue::None);
                } else {
                    assert(shading.center_issue!=TracePointIssue::None);
                    if(shading.location==TraceShadingLocation::CoverageCentroid) {
                        double x=0,y=0;int count=0;
                        for(const auto& step:event.samples) if(step.covered) {
                            x+=step.before.position.x;y+=step.before.position.y;++count;
                        }
                        assert(shading.position.x==x/count && shading.position.y==y/count);
                        assert(shading.centroid_issue==TracePointIssue::None);
                    } else {
                        assert(shading.centroid_issue!=TracePointIssue::None);
                        assert(std::any_of(event.samples.begin(),event.samples.end(),[&](const auto& s){
                            return s.covered && s.before.position.x==shading.position.x &&
                                s.before.position.y==shading.position.y;}));
                    }
                }
            } else {
                const auto first = std::find_if(event.samples.begin(), event.samples.end(),
                [](const TraceSampleStep& s) { return s.shading_index == 0; });
                assert(first != event.samples.end());
                assert(shading.location==TraceShadingLocation::Sample);
                assert(shading.position.x == first->before.position.x);
                assert(shading.position.y == first->before.position.y);
            }
        }
    }
    std::array<double, 3> average{};
    for (std::size_t s = 0; s < samples.size(); ++s) {
        same(samples[s], trace.samples[s]);
        for (int c = 0; c < 3; ++c)
            average[c] += (samples[s].linear_rgb[c] - average[c]) / double(s + 1);
    }
    for (int c = 0; c < 3; ++c) {
        assert(std::abs(average[c] - trace.resolved_linear_rgb[c]) < 1e-12);
        assert(trace.display_rgb[c] == std::uint8_t(std::round(
            std::pow(std::clamp(average[c], 0.0, 1.0), 1.0 / 2.2) * 255.0)));
    }
}
