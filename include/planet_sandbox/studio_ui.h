#pragma once

#include "planet_sandbox/renderer.h"
#include "planet_sandbox/demo_presets.h"
#include "planet_sandbox/sculpt.h"
#include "planet_sandbox/session.h"
#include "planet_sandbox/project_history.h"
#include "planet_sandbox/project_recovery.h"

#include <imgui.h>

#include <array>
#include <optional>
#include <string>
#include <vector>
#include <utility>

namespace planet_sandbox {

enum class StudioCameraDestination { Design, Overview, Simulation, DesignOverview, Camera };
enum class StudioSimulationMode { Theoretical, Real2026 };

enum class StudioWorkflowStage { Overview, Design, Simulation, Collision, Transition };
enum class StudioOverview { FullSystem, InnerSystem };

struct StudioWorkflow {
    StudioWorkflowStage stage = StudioWorkflowStage::Overview;
    std::string title;
    std::string hint;
};

struct StudioBodyMarker {
    int body_index = -1;
    int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
};

struct StudioCameraTransition {
    RenderCamera from;
    RenderCamera to;
    RenderCamera detail_camera;
    std::optional<WorldVector> screen_anchor;
    std::optional<RenderCamera> via;
    std::optional<WorldVector> departure_anchor;
    double elapsed = 0.0;
    double duration = 0.9;
    StudioCameraDestination destination = StudioCameraDestination::Camera;
    bool resume_playback = false;
};

struct StudioRenderMeasurement {
    static constexpr int warmup_runs = 3;
    static constexpr int sample_runs = 11;
    static constexpr double settle_seconds = 0.25;
    bool waiting = false;
    bool running = false;
    bool ready = false;
    int completed_runs = 0;
    double stable_since = 0.0;
    std::string input_key;
    std::array<std::vector<double>, 2> samples;
    std::array<double, 2> median_ms{};
};

struct StudioRenderInput {
    Project project;
    SceneSnapshot snapshot;
    RenderConfig config;
    std::optional<Project> reference_project;
};

struct StudioFrameCache {
    std::optional<RenderFrame> frame;
    std::string input_key;
    int trace_x = -1;
    int trace_y = -1;
    // Also used by frontends to upload only newly rendered images.
    std::uint64_t render_count = 0;
    std::optional<StudioRenderInput> displayed_input;
    std::string context_key;
    bool asynchronous = false;
    bool preview = false;
};

// Disclosure state follows the visible hierarchy: closing a parent also
// clears every descendant, including sample rows that are not currently drawn.
struct StudioPixelDisclosure {
    bool details = false;
    bool history = false;
    bool background = false;
    bool shading = false;
    bool resolve = false;
    std::vector<bool> samples;

    void close_shading() { shading = false; samples.clear(); }
    void close_history() {
        history = background = resolve = false;
        close_shading();
    }
    void close_details() { details = false; close_history(); }
};

struct StudioState {
    StudioState(Project project, const std::string& project_path);

    SandboxSession session;
    ProjectHistory history;
    std::unique_ptr<RecoveryWriter> recovery_writer;
    std::filesystem::path document_path;
    std::optional<RecoveryDraft> recovery_offer;
    std::optional<std::filesystem::path> pending_load;
    std::string recovery_error;
    bool recovery_blocked = false;
    bool source_unavailable = false;
    std::string checkpoint_document;
    std::string protected_document;
    std::uint64_t document_revision = 0;
    double document_clock = 0.0, last_document_edit = 0.0, last_checkpoint = 0.0;
    DemoPresets demos;
    RenderConfig render;
    std::array<char, 512> path{};
    std::array<char, 96> name{};
    std::string notice;
    int selected = 0;
    bool design_view = false;
    // Retain this session's DIY initial conditions while viewing dated trajectories.
    std::optional<Project> theoretical_project;
    // Presentation preferences never change saved scale or physical collision radii.
    bool protect_display_sizes = true;
    StudioOverview overview = StudioOverview::FullSystem;
    // Simulation camera follows a stable body ID without altering the scene.
    std::string simulation_follow_id;
    struct CollisionReplay {
        CollisionEvent event;
        double end_time=0;
        bool playing=true;
    };
    std::optional<CollisionReplay> collision_replay;
    bool compare_baseline = false;
    bool compare_design = false;
    std::optional<DesignOverrides> design_reference;
    std::string design_reference_id;
    bool show_reference_orbit = true;
    double timeline_horizon = 80.0;
    bool compare_modes = false;
    bool inspect_pixel = false;
    SamplingMode compare_left = SamplingMode::Single;
    SamplingMode compare_right = SamplingMode::MSAA4;
    int selected_pixel_x = -1;
    int selected_pixel_y = -1;
    int pixel_region_size = 9;
    StudioPixelDisclosure pixel_disclosure;
    // Body, requested pixel, Inspect/Compare, and sampling modes. Render refreshes
    // do not change this target, so timing and animation cannot close the panel.
    std::array<int, 8> pixel_inspector_target{};
    int trace_event_index = 0;
    int trace_event_x = -1, trace_event_y = -1;
    SamplingMode trace_event_sampling = SamplingMode::Single;
    bool select_pixel_tab = false;
    bool pixel_tab_active = false;
    bool design_tab_active = true;
    StudioRenderMeasurement measurement;
    StudioFrameCache frame_cache;
    std::vector<StudioBodyMarker> body_markers;
    std::uint64_t marker_render_count = static_cast<std::uint64_t>(-1);
    const int* marker_pixels = nullptr;
    bool inspector_was_simulating = false;
    // Explicit scene changes invalidate old jobs without interrupting sculpt revisions.
    std::uint64_t scene_generation = 0;
    bool preview_interacting = false;
    bool request_close = false;
    std::optional<StudioCameraTransition> camera_transition;
    SculptMode sculpt_mode = SculptMode::Raise;
    SolarShapeLayer sculpt_layer = SolarShapeLayer::Visible;
    float sculpt_radius = 0.3f;
    float sculpt_strength = 0.6f;
    SculptStroke sculpt_stroke;
    SculptHistory sculpt_history;
    struct SavedView {
        int selected = 0;
        RenderCamera camera;
        bool design_view = false;
        bool compare_baseline = false;
        bool compare_modes = false;
        bool inspect_pixel = false;
        int pixel_x = -1;
        int pixel_y = -1;
    };
    std::optional<SavedView> saved_view;
};

// A crop in panel-local coordinates; it never crosses a comparison divider.
struct StudioPixelRegion {
    int x = 0;
    int y = 0;
    int size = 0;
};
StudioPixelRegion studio_pixel_region(const RenderFrame& frame, int pixel_x, int pixel_y,
                                      int cells);
void draw_pixel_process(StudioState& state, const PixelTrace& trace);

StudioWorkflow studio_workflow(const StudioState& state);
std::vector<StudioBodyMarker> studio_body_markers(const RenderFrame& frame, int body_count);
bool studio_scene_annotations_visible(const StudioState& state);

StudioRenderInput make_studio_render_input(const StudioState& state);
void capture_studio_design_reference(StudioState& state);
std::vector<std::pair<Vec2,Vec2>> studio_brush_outline(const StudioRenderInput& input,const RenderFrame& frame,
                                                     int selected,int x,int y,double radius,SolarShapeLayer layer);
RenderFrame render_studio_frame(const StudioState& state, const StudioRenderInput& input);
// Uses displayed renders for timing (process recording off), then restores the
// selected pixel's detailed history with one untimed render and caches it.
const RenderFrame& update_studio_frame(StudioState& state, const StudioRenderInput& input,
                                       double now_seconds);
void activate_studio_preset(StudioState& state, DemoPreset preset);
void exit_studio_preset(StudioState& state);
void advance_studio(StudioState& state, double real_seconds);
void select_studio_body(StudioState& state, int body_index);
void select_studio_scene_body(StudioState& state, int body_index);
void begin_studio_design_focus(StudioState& state, int body_index);
void return_studio_overview(StudioState& state);
void select_studio_overview(StudioState& state, StudioOverview overview);
void set_studio_size_protection(StudioState& state, bool enabled);
void begin_studio_simulation(StudioState& state);
void start_studio_simulation_mode(StudioState& state, StudioSimulationMode mode);
void return_studio_design(StudioState& state);
void focus_studio_selected(StudioState& state);
void reset_studio_view(StudioState& state);
double studio_timeline_limit(const StudioState& state);
void seek_studio_time(StudioState& state, double time);
bool replay_studio_collision(StudioState& state, const CollisionEvent& event);
bool advance_studio_collision_replay(StudioState& state, double seconds);
void cancel_studio_collision_replay(StudioState& state);
bool apply_studio_edit(StudioState& state, Project project, const std::string& label,
                       const std::string& body_id = {}, const std::string& group = {});
Project open_studio_project(const std::filesystem::path& path, bool create_if_missing);
bool undo_studio_edit(StudioState& state);
bool redo_studio_edit(StudioState& state);
bool save_studio_document(StudioState& state);
bool load_studio_document(StudioState& state, const std::filesystem::path& path,
                          bool discard_edits = false);
void initialize_studio_document(StudioState& state);
void update_studio_document(StudioState& state, double elapsed_seconds);
void shutdown_studio_document(StudioState& state);
bool restore_studio_draft(StudioState& state);
void discard_studio_draft(StudioState& state);

void draw_studio(StudioState& state, ImTextureID texture, const StudioRenderInput& input,
                 const RenderFrame& frame,
                 bool flip_image_y);

} // namespace planet_sandbox
