#include "planet_sandbox/studio_ui.h"
#include "planet_sandbox/studio_render_pipeline.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace planet_sandbox {
namespace {

// Exact in-process identity, with no struct padding or lossy numeric formatting.
// Project edits use the session revision; snapshots/cameras are checked directly.
std::string measurement_key(const StudioState& state, const StudioRenderInput& input) {
    std::string key;
    auto add = [&](auto value) {
        static_assert(std::is_arithmetic_v<decltype(value)> || std::is_enum_v<decltype(value)>);
        key.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    auto text = [&](const std::string& value) { add(value.size()); key.append(value); };
    auto point = [&](auto p) { add(p.x); add(p.y); add(p.z); };
    add(state.session.project_revision()); add(state.scene_generation); add(state.session.mode());
    add(state.demos.active()); add(state.selected); add(state.design_view);
    add(state.compare_baseline); add(state.compare_modes);
    add(input.project.view_extent); add(input.project.display_scale);
    const auto& config = input.config;
    add(config.width); add(config.height);
    add(config.impact_effects);
    add(config.texture_filtering);
    add(config.adaptive_geometry);
    add(config.preview_light);add(config.light_azimuth);add(config.light_elevation);add(input.reference_project.has_value());
    add(config.camera.yaw); add(config.camera.pitch); add(config.camera.zoom);
    point(config.camera.target);
    add(state.compare_modes ? state.compare_left : config.sampling);
    add(state.compare_modes ? state.compare_right : config.sampling);
    const auto& snapshot = input.snapshot;
    add(snapshot.time); add(snapshot.bodies.size());
    add(snapshot.events.size());
    for(const auto& event:snapshot.events) {
        add(event.time);text(event.survivor);text(event.absorbed);add(event.contact.has_value());
        if(event.contact) {
            const auto& c=*event.contact;
            point(c.position);point(c.normal);point(c.velocity);point(c.survivor_position);
            add(c.relative_speed);add(c.survivor_radius);add(c.absorbed_radius);
        }
    }
    for (const auto& body : snapshot.bodies) {
        text(body.id); point(body.position); point(body.velocity);
        add(body.mass); add(body.radius);
    }
    auto trails = [&](const std::vector<BodyTrail>& values) {
        add(values.size());
        for (const auto& trail : values) {
            text(trail.id); add(trail.points.size());
            for (const auto& p : trail.points) point(p);
        }
    };
    trails(snapshot.trails); trails(snapshot.reference_trails);
    // Selection/Region/recording are diagnostic state. Timed frames disable
    // in-render process recording, so these never invalidate a valid median.
    return key;
}

void prepare_measurement(StudioState& state, const std::string& key, double now_seconds) {
    auto& result = state.measurement;
    const bool playing = state.demos.active() == DemoPreset::Collision
        ? state.demos.playing() : state.collision_replay ? state.collision_replay->playing : state.session.playing();
    const bool eligible = !playing && !state.camera_transition &&
        (state.compare_modes || state.inspect_pixel || state.pixel_tab_active);
    if (!eligible) {
        result = {};
    } else if (result.input_key != key) {
        result = {};
        result.input_key = key;
        result.waiting = true;
        result.stable_since = now_seconds;
    }
    // A pixel click does not change the image or invalidate a completed median.
    // Pause unfinished measurements while the pointer is held, retaining valid samples.
    if (state.preview_interacting && !result.ready && eligible) {
        result.running = false;
        result.waiting = true;
        result.stable_since = now_seconds;
    }
    if (result.waiting && !state.preview_interacting && now_seconds - result.stable_since >=
                           StudioRenderMeasurement::settle_seconds) {
        result.waiting = false;
        result.running = true;
    }
}

void record_measurement(StudioRenderMeasurement& result, const RenderFrame& frame) {
    if (result.completed_runs >= StudioRenderMeasurement::warmup_runs) {
        for (int panel = 0; panel < frame.panel_count; ++panel) {
            result.samples[panel].push_back(frame.panel_render_ms[panel]);
        }
    }
    ++result.completed_runs;
    if (result.completed_runs == StudioRenderMeasurement::warmup_runs +
                                 StudioRenderMeasurement::sample_runs) {
        for (int panel = 0; panel < frame.panel_count; ++panel) {
            auto sorted = result.samples[panel];
            std::sort(sorted.begin(), sorted.end());
            result.median_ms[panel] = sorted[sorted.size() / 2];
        }
        result.running = false;
        result.ready = true;
    }
}

std::string context_key(const StudioState& state, const StudioRenderInput& input) {
    std::string key;
    auto add = [&](auto value) {
        key.append(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    add(state.scene_generation); add(state.selected); add(state.design_view); add(state.session.mode());
    add(state.demos.active()); add(state.compare_baseline); add(state.compare_modes);
    add(state.compare_modes || state.inspect_pixel || state.pixel_tab_active);
    add(state.render.sampling); add(state.compare_left); add(state.compare_right);
    add(input.config.width); add(input.config.height);
    add(input.config.impact_effects);
    add(input.config.texture_filtering);
    add(input.config.adaptive_geometry);
    add(input.config.preview_light);add(input.config.light_azimuth);add(input.config.light_elevation);
    add(input.reference_project.has_value());
    add(input.project.view_extent); add(input.project.display_scale); add(input.project.solar_system);
    add(state.show_reference_orbit);
    for (const auto& body : input.project.bodies) {
        add(body.id.size()); key.append(body.id); add(body.kind); add(body.display_radius);
    }
    return key;
}

} // namespace

const RenderFrame& update_studio_frame(StudioState& state, const StudioRenderInput& input,
                                       double now_seconds) {
    auto& result = state.measurement;
    auto& cache = state.frame_cache;
    cache.asynchronous = false;
    const auto key = measurement_key(state, input);
    prepare_measurement(state, key, now_seconds);
    RenderConfig config = input.config;
    if (result.running) config.trace_process = false;
    const bool needs_process = config.trace_x >= 0 && config.trace_process;
    const bool has_process = cache.frame && cache.frame->trace && cache.frame->trace->process_recorded;

    const bool needs_render = !cache.frame || cache.input_key != key || result.running ||
        cache.trace_x != input.config.trace_x || cache.trace_y != input.config.trace_y ||
        needs_process != has_process;
    if (needs_render) {
        // The timed image is the displayed image: no extra benchmark render.
        // Alternating sides avoids always giving one method the same execution order.
        cache.frame = input.reference_project
            ? render_comparison_scene(*input.reference_project,input.snapshot,config,
                config.sampling,config.sampling,result.completed_runs%2!=0,&input.project)
            : state.compare_modes
            ? render_comparison_scene(input.project, input.snapshot, config,
                state.compare_left, state.compare_right, result.completed_runs % 2 != 0)
            : render_scene(input.project, input.snapshot, config);
        cache.displayed_input = input;
        cache.input_key = key;
        cache.trace_x = input.config.trace_x;
        cache.trace_y = input.config.trace_y;
        ++cache.render_count;
        if (result.running) {
            record_measurement(result, *cache.frame);
        }
    }
    return *cache.frame;
}

struct StudioRenderPipeline::Impl {
    StudioRenderWorker worker;
    std::string motion_key;
    std::string last_key;
    std::string failed_key;
    std::string error_notice;
    double changed_at = -1.0;
    std::uint64_t submitted = 0;
    std::uint64_t published = 0;
    bool in_flight = false;

    explicit Impl(StudioRenderWorker::RenderFunction render) : worker(std::move(render)) {}
};

StudioRenderPipeline::StudioRenderPipeline(StudioRenderWorker::RenderFunction render)
    : impl_(std::make_unique<Impl>(std::move(render))) {}

StudioRenderPipeline::~StudioRenderPipeline() = default;

const RenderFrame& StudioRenderPipeline::update(StudioState& state,
                                                const StudioRenderInput& input,
                                                double now_seconds) {
    auto& pipeline = *impl_;
    auto& cache = state.frame_cache;
    cache.asynchronous = true;
    const auto full_key = measurement_key(state, input);
    const auto context = context_key(state, input);
    if (!pipeline.motion_key.empty() && pipeline.motion_key != full_key) {
        pipeline.changed_at = now_seconds;
    }
    pipeline.motion_key = full_key;
    const bool playing = state.demos.active() == DemoPreset::Collision
        ? state.demos.playing() : state.collision_replay ? state.collision_replay->playing : state.session.playing();
    const bool analysis = state.compare_modes || state.inspect_pixel || state.pixel_tab_active;
    if (playing || state.camera_transition || state.preview_interacting) {
        pipeline.changed_at = now_seconds;
    }
    const bool preview = !analysis && (playing || state.camera_transition ||
        state.preview_interacting || (pipeline.changed_at >= 0.0 &&
            now_seconds - pipeline.changed_at < refine_delay_seconds));
    prepare_measurement(state, full_key, now_seconds);

    if (auto completed = pipeline.worker.poll()) {
        if (completed->request.serial == pipeline.submitted) pipeline.in_flight = false;
        if (completed->request.context_key == context &&
            completed->request.serial > pipeline.published) {
            if (completed->error) {
                pipeline.failed_key = completed->request.key;
                try {
                    std::rethrow_exception(completed->error);
                } catch (const std::exception& error) {
                    pipeline.error_notice = std::string("Preview failed: ") + error.what();
                } catch (...) {
                    pipeline.error_notice = "Preview failed: unknown rendering error";
                }
                state.notice = pipeline.error_notice;
            } else {
                if (completed->request.measuring && state.measurement.running &&
                    completed->request.measurement_key == full_key) {
                    record_measurement(state.measurement, *completed->frame);
                }
                cache.frame = std::move(completed->frame);
                cache.displayed_input = std::move(completed->request.input);
                cache.context_key = context;
                cache.input_key = completed->request.key;
                cache.preview = completed->request.preview;
                ++cache.render_count;
                pipeline.published = completed->request.serial;
                if (!pipeline.error_notice.empty() && state.notice == pipeline.error_notice) {
                    state.notice.clear();
                }
                pipeline.error_notice.clear();
            }
        }
    }

    StudioRenderRequest request;
    request.input = input;
    request.context_key = context;
    request.measurement_key = full_key;
    request.preview = preview;
    request.compare_modes = state.compare_modes;
    request.left = state.compare_left;
    request.right = state.compare_right;
    request.right_first = state.measurement.completed_runs % 2 != 0;
    request.measuring = state.measurement.running;
    if (request.measuring) request.input.config.trace_process = false;
    if (preview) {
        request.input.config.width = std::max(1, input.config.width / 2);
        request.input.config.height = std::max(1, input.config.height / 2);
        request.input.config.sampling = SamplingMode::Single;
    }
    request.key = measurement_key(state, request.input) + context;
    request.key.append(reinterpret_cast<const char*>(&input.config.trace_x), sizeof(int));
    request.key.append(reinterpret_cast<const char*>(&input.config.trace_y), sizeof(int));
    const bool process = request.input.config.trace_x >= 0 && request.input.config.trace_process;
    request.key.append(reinterpret_cast<const char*>(&process), sizeof(process));
    if (request.key != pipeline.failed_key) pipeline.failed_key.clear();
    if (request.key != pipeline.failed_key &&
        (request.key != pipeline.last_key || (state.measurement.running && !pipeline.in_flight))) {
        pipeline.last_key = request.key;
        pipeline.submitted = pipeline.worker.submit(std::move(request));
        pipeline.in_flight = true;
    } else if (request.key == pipeline.failed_key) {
        state.measurement.waiting = state.measurement.running = false;
    }

    if (!cache.frame) {
        // A neutral first frame keeps the editor usable while assets load in the worker.
        RenderFrame placeholder;
        placeholder.image.width = std::max(1, input.config.width * (state.compare_modes ? 2 : 1));
        placeholder.image.height = std::max(1, input.config.height);
        const std::size_t pixels = static_cast<std::size_t>(placeholder.image.width) *
                                   placeholder.image.height;
        placeholder.image.rgb.assign(pixels * 3, 15);
        placeholder.body_index.assign(pixels, -1);
        placeholder.surface_uv.assign(pixels, {-1.0, -1.0});
        cache.frame = std::move(placeholder);
        ++cache.render_count;
    }
    return *cache.frame;
}

bool studio_frame_can_interact(const StudioState& state) {
    const auto& cache = state.frame_cache;
    if (!cache.asynchronous) return true;
    if (!cache.displayed_input) return false;
    const auto current = make_studio_render_input(state);
    if (cache.context_key != context_key(state, current)) return false;
    const auto& shown_snapshot = cache.displayed_input->snapshot;
    const auto& desired_snapshot = current.snapshot;
    if (shown_snapshot.time != desired_snapshot.time ||
        shown_snapshot.bodies.size() != desired_snapshot.bodies.size()) return false;
    for (std::size_t i = 0; i < shown_snapshot.bodies.size(); ++i) {
        const auto& shown_body = shown_snapshot.bodies[i];
        const auto& desired_body = desired_snapshot.bodies[i];
        if (shown_body.id != desired_body.id || shown_body.radius != desired_body.radius ||
            shown_body.position.x != desired_body.position.x ||
            shown_body.position.y != desired_body.position.y ||
            shown_body.position.z != desired_body.position.z) return false;
    }
    const auto& shown = cache.displayed_input->config.camera;
    const auto& desired = current.config.camera;
    return shown.yaw == desired.yaw && shown.pitch == desired.pitch &&
           shown.zoom == desired.zoom && shown.target.x == desired.target.x &&
           shown.target.y == desired.target.y && shown.target.z == desired.target.z;
}

} // namespace planet_sandbox
