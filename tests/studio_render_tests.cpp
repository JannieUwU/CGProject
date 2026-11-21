#include "planet_sandbox/studio_render_pipeline.h"
#include "planet_sandbox/solar_system.h"

#ifdef NDEBUG
#error Render worker tests require enabled assertions
#endif

#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace planet_sandbox;
using namespace std::chrono_literals;

struct RenderGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;

    void block() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return released; });
    }
    void wait_until_entered() {
        std::unique_lock<std::mutex> lock(mutex);
        assert(condition.wait_for(lock, 5s, [&] { return entered; }));
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        condition.notify_all();
    }
};

static StudioRenderResult wait_for_result(StudioRenderWorker& worker, std::uint64_t serial) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = worker.poll()) {
            if (result->request.serial == serial) return std::move(*result);
        }
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error("Render worker did not publish its result");
}

static void test_worker_keeps_ui_free_and_replaces_pending_work() {
    RenderGate gate;
    std::vector<int> rendered;
    StudioRenderWorker worker([&](const StudioRenderRequest& request) {
        if (request.input.config.width == 1) gate.block();
        rendered.push_back(request.input.config.width);
        RenderFrame frame;
        frame.image.width = request.input.config.width;
        frame.image.height = static_cast<int>(request.input.project.bodies.front().mass);
        return frame;
    });
    StudioRenderRequest request;
    request.input.project = make_solar_system_project();
    request.input.project.bodies.front().mass = 7;
    request.input.config.width = 1;
    worker.submit(request);
    gate.wait_until_entered();
    auto submit = std::async(std::launch::async, [&] {
        request.input.config.width = 2;
        worker.submit(request);
        request.input.config.width = 3;
        return worker.submit(request);
    });
    assert(submit.wait_for(500ms) == std::future_status::ready);
    const auto last = submit.get();
    request.input.project.bodies.front().mass = 99;
    gate.release();
    const auto result = wait_for_result(worker, last);
    assert(!result.error && result.frame);
    assert(result.frame->image.width == 3 && result.frame->image.height == 7);
    assert((rendered == std::vector<int>{1, 3}));
}

static void test_worker_reports_exceptions_and_continues() {
    StudioRenderWorker worker([](const StudioRenderRequest& request) {
        if (request.input.config.width == 1) throw std::runtime_error("test render failure");
        RenderFrame frame;
        frame.image.width = request.input.config.width;
        return frame;
    });
    StudioRenderRequest request;
    request.input.config.width = 1;
    const auto failed = wait_for_result(worker, worker.submit(request));
    assert(failed.error && !failed.frame);
    try {
        std::rethrow_exception(failed.error);
        assert(false);
    } catch (const std::runtime_error& error) {
        assert(std::string(error.what()) == "test render failure");
    }
    request.input.config.width = 2;
    const auto recovered = wait_for_result(worker, worker.submit(request));
    assert(recovered.frame && recovered.frame->image.width == 2 && !recovered.error);
}

static void test_worker_shutdown_finishes_active_render() {
    RenderGate gate;
    auto worker = std::make_unique<StudioRenderWorker>([&](const StudioRenderRequest&) {
        gate.block();
        return RenderFrame{};
    });
    worker->submit({});
    gate.wait_until_entered();
    auto shutdown = std::async(std::launch::async, [&] { worker.reset(); });
    assert(shutdown.wait_for(20ms) == std::future_status::timeout);
    gate.release();
    assert(shutdown.wait_for(5s) == std::future_status::ready);
    shutdown.get();
}

template <class Predicate>
static void pump_until(StudioRenderPipeline& pipeline, StudioState& state,
                       double now, Predicate complete) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    do {
        pipeline.update(state, make_studio_render_input(state), now);
        if (complete()) return;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Preview pipeline did not reach the expected state");
}

static StudioState detail_state() {
    StudioState state(make_solar_system_project(), "responsive-preview.json");
    state.render.width = 64;
    state.render.height = 48;
    activate_studio_preset(state, DemoPreset::EarthTerrain);
    return state;
}

static void test_preview_refines_and_picking_matches_displayed_view() {
    auto state = detail_state();
    state.render.sampling = SamplingMode::SSAA4;
    StudioRenderPipeline pipeline;
    pipeline.update(state, make_studio_render_input(state), 0.0);
    assert(!studio_frame_can_interact(state));
    pump_until(pipeline, state, 0.0, [&] { return studio_frame_can_interact(state); });
    assert(!state.frame_cache.preview && state.frame_cache.frame->image.width == 64);
    state.preview_interacting = true;
    state.render.camera.yaw += 0.2;
    assert(!studio_frame_can_interact(state));
    pump_until(pipeline, state, 0.01, [&] {
        return state.frame_cache.preview && studio_frame_can_interact(state);
    });
    assert(state.frame_cache.frame->image.width == 32);
    assert(state.frame_cache.frame->panel_sampling[0] == SamplingMode::Single);
    assert(state.render.width == 64 && state.render.sampling == SamplingMode::SSAA4);
    state.preview_interacting = false;
    pump_until(pipeline, state, 0.3, [&] {
        return !state.frame_cache.preview && studio_frame_can_interact(state);
    });
    assert(state.frame_cache.frame->image.width == 64);
    assert(state.frame_cache.frame->panel_sampling[0] == SamplingMode::SSAA4);

    state.preview_interacting = true;
    pump_until(pipeline, state, 1.0, [&] { return state.frame_cache.preview; });
    pipeline.update(state, make_studio_render_input(state), 2.0);
    state.preview_interacting = false;
    pump_until(pipeline, state, 2.01, [&] { return state.frame_cache.preview; });
    assert(state.frame_cache.preview);
    // A held pointer ends later than the last camera change.
    for (int i = 0; i < 10; ++i) {
        pipeline.update(state, make_studio_render_input(state), 2.1);
        std::this_thread::sleep_for(1ms);
    }
    assert(state.frame_cache.preview);
    pump_until(pipeline, state, 2.3, [&] { return !state.frame_cache.preview; });

    // A sidebar change happens after the rendered frame is already on screen.
    state.selected = 6;
    assert(!studio_frame_can_interact(state));
    pump_until(pipeline, state, 0.31, [&] { return studio_frame_can_interact(state); });
    assert(state.frame_cache.displayed_input->snapshot.bodies.front().id == "saturn");
    state.compare_baseline = true;
    assert(!studio_frame_can_interact(state));
}

static void test_camera_motion_publishes_completed_frames_without_starving() {
    auto state = detail_state();
    RenderGate first, second;
    StudioRenderPipeline pipeline([&](const StudioRenderRequest& request) {
        if (request.input.config.camera.yaw == 0.1) first.block();
        if (request.input.config.camera.yaw == 0.2) second.block();
        return render_scene(request.input.project, request.input.snapshot, request.input.config);
    });
    state.preview_interacting = true;
    state.render.camera.yaw = 0.1;
    pipeline.update(state, make_studio_render_input(state), 0.0);
    first.wait_until_entered();
    state.render.camera.yaw = 0.2;
    pipeline.update(state, make_studio_render_input(state), 0.01);
    first.release();
    second.wait_until_entered();
    pipeline.update(state, make_studio_render_input(state), 0.02);
    assert(state.frame_cache.displayed_input);
    assert(state.frame_cache.displayed_input->config.camera.yaw == 0.1);
    assert(!studio_frame_can_interact(state));
    second.release();
    pump_until(pipeline, state, 0.03, [&] { return studio_frame_can_interact(state); });
}

static void test_async_measurement_has_fixed_quality_and_exact_sample_count() {
    auto state = detail_state();
    state.compare_modes = state.inspect_pixel = true;
    state.selected_pixel_x = 16;
    state.selected_pixel_y = 24;
    std::atomic<int> renders{0};
    StudioRenderPipeline pipeline([&](const StudioRenderRequest& request) {
        assert(!request.preview && request.input.config.width == 32);
        if (request.measuring) assert(!request.input.config.trace_process);
        ++renders;
        return render_comparison_scene(request.input.project, request.input.snapshot,
            request.input.config, request.left, request.right, request.right_first);
    });
    pump_until(pipeline, state, 0.0, [&] { return studio_frame_can_interact(state); });
    assert(state.measurement.waiting && renders == 1);
    pump_until(pipeline, state, 0.3, [&] { return state.measurement.ready; });
    assert(renders >= 15 && renders <= 16); // Untimed history restore may already be in flight.
    assert(state.measurement.completed_runs == 14);
    assert(state.measurement.samples[0].size() == 11 && state.measurement.samples[1].size() == 11);
    assert(state.frame_cache.frame->panel_count == 2);
    const auto median = state.measurement.median_ms;
    pump_until(pipeline, state, 0.4, [&] {
        return state.frame_cache.frame->trace && state.frame_cache.frame->trace->process_recorded;
    });
    assert(renders == 16);
    for (int i = 0; i < 20; ++i) pipeline.update(state, make_studio_render_input(state), 0.4);
    assert(renders == 16);
    state.preview_interacting = true;
    state.selected_pixel_x = 40;
    pump_until(pipeline, state, 0.41, [&] {
        return state.frame_cache.frame->trace && state.frame_cache.frame->trace->x == 40;
    });
    assert(state.measurement.ready && state.measurement.median_ms == median && renders == 17);
    state.preview_interacting = false;
    state.render.camera.yaw += 0.1;
    pipeline.update(state, make_studio_render_input(state), 0.42);
    assert(!state.measurement.ready && state.measurement.waiting);
}

static void test_analysis_keeps_configured_sampling_during_camera_flight() {
    auto state = detail_state();
    state.render.sampling = SamplingMode::MSAA4;
    return_studio_overview(state);
    state.pixel_tab_active = true;
    assert(state.camera_transition);
    assert(make_studio_render_input(state).config.sampling == SamplingMode::MSAA4);
}

static void test_scene_seek_blocks_old_image_and_old_inflight_result() {
    StudioState state(make_solar_system_project(), "scene-freshness.json");
    state.render.width = 64;
    state.render.height = 48;
    state.session.begin_simulation();
    state.session.set_playing(false);
    RenderGate first, second;
    StudioRenderPipeline pipeline([&](const StudioRenderRequest& request) {
        if (request.input.snapshot.time == 0.25) first.block();
        if (request.input.snapshot.time == 0.5) second.block();
        return render_scene(request.input.project, request.input.snapshot, request.input.config);
    });
    pump_until(pipeline, state, 0.0, [&] { return studio_frame_can_interact(state); });
    seek_studio_time(state, 0.25);
    assert(!studio_frame_can_interact(state));
    pipeline.update(state, make_studio_render_input(state), 0.01);
    first.wait_until_entered();
    seek_studio_time(state, 0.5);
    pipeline.update(state, make_studio_render_input(state), 0.02);
    first.release();
    second.wait_until_entered();
    pipeline.update(state, make_studio_render_input(state), 0.03);
    assert(state.frame_cache.displayed_input->snapshot.time == 0.0);
    assert(!studio_frame_can_interact(state));
    second.release();
    pump_until(pipeline, state, 0.04, [&] { return studio_frame_can_interact(state); });
    state.session.reset_simulation();
    assert(!studio_frame_can_interact(state));
}

static void test_async_failure_keeps_ui_alive_and_retries_changed_input() {
    auto state = detail_state();
    std::atomic<int> renders{0};
    StudioRenderPipeline pipeline([&](const StudioRenderRequest& request) {
        if (++renders == 1) throw std::runtime_error("test preview failure");
        return render_scene(request.input.project, request.input.snapshot, request.input.config);
    });
    pump_until(pipeline, state, 0.0, [&] { return !state.notice.empty(); });
    assert(state.notice.find("test preview failure") != std::string::npos);
    assert(!studio_frame_can_interact(state));
    for (int i = 0; i < 20; ++i) pipeline.update(state, make_studio_render_input(state), 0.5);
    assert(renders == 1);
    state.render.camera.yaw += 0.1;
    pump_until(pipeline, state, 0.6, [&] { return studio_frame_can_interact(state); });
    assert(state.notice.empty() && renders == 2);
    state.render.camera.yaw -= 0.1;
    pump_until(pipeline, state, 0.7, [&] { return studio_frame_can_interact(state); });
    pump_until(pipeline, state, 1.0, [&] {
        return studio_frame_can_interact(state) && !state.frame_cache.preview;
    });
    assert(state.notice.empty()); // Returning to a previously failed input must be retryable.
}

static void test_sculpt_stroke_reprojects_when_preview_dimensions_change() {
    auto frame = [](int width, int height) {
        RenderFrame result;
        result.image.width = width;
        result.image.height = height;
        result.body_index.assign(width * height, 3);
        result.surface_uv.resize(width * height);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                result.surface_uv[y * width + x] = {double(x) / width, double(y) / height};
            }
        }
        return result;
    };
    const auto full = frame(64, 48), preview = frame(32, 24);
    SculptStroke stroke;
    assert(stroke.begin(full, 3, 16, 24));
    assert(stroke.move(preview, 3, 8, 12).empty());
    const auto dabs = stroke.move(preview, 3, 14, 12);
    assert(dabs.size() == 1 && dabs.front().x > 0.4 && dabs.front().y == 0.5);
}

int main() {
    test_scene_seek_blocks_old_image_and_old_inflight_result();
    test_analysis_keeps_configured_sampling_during_camera_flight();
    test_worker_keeps_ui_free_and_replaces_pending_work();
    test_worker_reports_exceptions_and_continues();
    test_worker_shutdown_finishes_active_render();
    test_preview_refines_and_picking_matches_displayed_view();
    test_camera_motion_publishes_completed_frames_without_starving();
    test_async_measurement_has_fixed_quality_and_exact_sample_count();
    test_async_failure_keeps_ui_alive_and_retries_changed_input();
    test_sculpt_stroke_reprojects_when_preview_dimensions_change();
}
