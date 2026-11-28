#include "planet_sandbox/studio_ui.h"
#include "planet_sandbox/studio_render_pipeline.h"
#ifdef NDEBUG
#error Studio tests require enabled assertions
#endif
#include "planet_sandbox/solar_system.h"
#include <imgui_internal.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <fstream>

using namespace planet_sandbox;

static void test_design_reference_light_and_brush_feedback() {
    StudioState state(make_solar_system_project(),"design-tools.json");
    state.selected=4;state.design_view=true;state.render.width=180;state.render.height=160;state.render.camera.zoom=1.15;
    const auto original=project_document(state.session.project());
    capture_studio_design_reference(state);
    assert(state.design_reference && state.design_reference_id=="mars");
    auto edited=state.session.project();edited.bodies[4].design.relief=3;
    state.session.replace_project(edited);state.compare_design=true;
    state.render.preview_light=true;state.render.light_azimuth=1.2;
    auto input=make_studio_render_input(state);
    assert(input.reference_project && input.config.preview_light);
    assert(input.reference_project->bodies[4].design.relief==1);
    assert(input.project.bodies[4].design.relief==3);
    const auto compared=render_studio_frame(state,input);
    assert(compared.panel_count==2);
    const auto& cached=update_studio_frame(state,input,0);
    assert(cached.panel_count==2 && cached.image.rgb==compared.image.rgb);
    bool different=false;
    for(int y=0;y<compared.image.height;++y) for(int x=0;x<compared.image.width/2;++x)
        for(int c=0;c<3;++c) different|=compared.image.rgb[(y*compared.image.width+x)*3+c]!=
            compared.image.rgb[(y*compared.image.width+x+compared.image.width/2)*3+c];
    assert(different);
    state.compare_design=false;state.render.width=180;state.render.height=160;
    input=make_studio_render_input(state);
    const auto frame=render_studio_frame(state,input);
    auto default_light=input;default_light.config.preview_light=false;
    assert(render_studio_frame(state,default_light).image.rgb!=frame.image.rgb);
    int pixel=-1;
    for(std::size_t p=0;p<frame.body_index.size();++p)
        if(frame.body_index[p]==4 && frame.surface_uv[p].x>=0) {pixel=static_cast<int>(p);break;}
    assert(pixel>=0);
    // A center pick gives a complete visible contour; background picks give none.
    const auto outline=studio_brush_outline(input,frame,4,90,80,.2,SolarShapeLayer::Visible);
    assert(outline.size()>16);
    assert(studio_brush_outline(input,frame,4,0,0,.2,SolarShapeLayer::Visible).empty());
    state.design_view=false;state.session.begin_simulation();
    assert(!make_studio_render_input(state).config.preview_light);
    assert(!make_studio_render_input(state).reference_project);
    assert(original!=project_document(state.session.project()));
}

static void test_reference_comparison_clears_when_leaving_design_focus() {
    StudioState state(make_solar_system_project(),"compare-lifecycle.json");
    state.selected=3;state.design_view=true;
    capture_studio_design_reference(state);state.compare_design=true;
    return_studio_overview(state);
    assert(!state.compare_design);
    while(state.camera_transition) advance_studio(state,.1);
    select_studio_body(state,3);
    while(state.camera_transition) advance_studio(state,.1);
    state.compare_design=true;
    begin_studio_simulation(state);
    assert(!state.compare_design);
}

static void test_workflow_guidance_follows_the_active_tool() {
    StudioState state(make_solar_system_project(), "workflow.json");
    state.selected = 3;
    auto workflow = studio_workflow(state);
    assert(workflow.stage == StudioWorkflowStage::Overview);
    assert(workflow.title.find("Earth") != std::string::npos);
    assert(workflow.hint.find("select") != std::string::npos);
    begin_studio_design_focus(state, 3);
    assert(studio_workflow(state).stage == StudioWorkflowStage::Transition);
    while (state.camera_transition) advance_studio(state, 0.1);
    workflow = studio_workflow(state);
    assert(workflow.stage == StudioWorkflowStage::Design);
    assert(workflow.hint.find("brush") != std::string::npos);
    state.compare_baseline = true;
    assert(studio_workflow(state).hint.find("baseline") != std::string::npos);
    state.inspect_pixel = state.pixel_tab_active = true;
    assert(studio_workflow(state).hint.find("pixel") != std::string::npos);
    state.inspect_pixel = state.pixel_tab_active = false;
    begin_studio_simulation(state);
    while (state.camera_transition) advance_studio(state, 0.1);
    assert(studio_workflow(state).stage == StudioWorkflowStage::Simulation);
    assert(studio_workflow(state).hint.find("collision") != std::string::npos);
}

static void test_markers_use_visible_surface_pixels_not_orbit_trails() {
    RenderFrame frame;
    frame.image.width = 8; frame.image.height = 6;
    frame.body_index.assign(48, -1);
    frame.surface_uv.assign(48, {-1.0, -1.0});
    for (int y = 2; y <= 4; ++y) {
        for (int x = 3; x <= 5; ++x) {
            frame.body_index[y * 8 + x] = 3;
            frame.surface_uv[y * 8 + x] = {0.25, 0.5};
        }
    }
    frame.body_index[0] = 3; // Same body's trajectory must not expand its label bounds.
    frame.body_index[1] = 4; // A trail-only/merged body must not get a planet label.
    frame.body_index[47] = 200; // Ignore identities outside the current catalog.
    frame.surface_uv[47] = {0.5, 0.5};
    const auto markers = studio_body_markers(frame, 9);
    assert(markers.size() == 1);
    assert(markers[0].body_index == 3);
    assert(markers[0].min_x == 3 && markers[0].max_x == 5);
    assert(markers[0].min_y == 2 && markers[0].max_y == 4);
    frame.surface_uv.clear();
    assert(studio_body_markers(frame, 9).empty());
}

static void test_collision_replay_pauses_selects_survivor_and_keeps_design() {
    auto project = make_solar_system_project();
    project.bodies[4].position = project.bodies[3].position;
    StudioState state(project, "collision-workflow.json");
    const auto original = project_document(state.session.project());
    state.session.begin_simulation();
    assert(!state.session.observed_events().empty());
    const auto event = state.session.observed_events().front();
    state.selected = 4;
    assert(replay_studio_collision(state, event));
    assert(!state.session.playing());
    assert(state.session.project().bodies[state.selected].id == event.survivor);
    assert(state.session.snapshot().time >= event.time);
    assert(project_document(state.session.project()) == original);
    state.session.return_to_design();
    assert(!replay_studio_collision(state, event));
}

static void test_collision_replay_runs_continuously_and_preserves_normal_speed() {
    Project project;
    project.bodies={{"a","A",2,.1,{0,0},{0,0},{}},
                    {"b","B",1,.1,{1,0},{-1,0},{}}};
    StudioState state(project,"continuous-impact.json");
    state.session.begin_simulation();
    state.session.set_speed(7);
    seek_studio_time(state,1);
    assert(state.session.observed_events().size()==1);
    const auto event=state.session.observed_events().front();
    const auto saved=project_document(state.session.project());
    assert(replay_studio_collision(state,event));
    assert(state.collision_replay && state.session.snapshot().time<event.time);
    while(state.camera_transition) advance_studio(state,.1);
    const double before=state.session.snapshot().time;
    advance_studio(state,.1);
    assert(std::abs(state.session.snapshot().time-before-.025)<1e-10);
    state.collision_replay->playing=false;
    const double paused=state.session.snapshot().time;
    advance_studio(state,.1);
    assert(state.session.snapshot().time==paused);
    state.collision_replay->playing=true;
    state.compare_modes=true;
    advance_studio(state,.1);
    assert(state.session.snapshot().time==paused);
    state.compare_modes=false;
    bool crossed=false;
    for(int i=0;i<120 && state.collision_replay && state.collision_replay->playing;++i) {
        advance_studio(state,.1);
        crossed|=state.session.snapshot().time>event.time;
    }
    assert(crossed && state.collision_replay && !state.collision_replay->playing);
    assert(!state.session.playing() && state.session.speed()==7);
    assert(project_document(state.session.project())==saved);
    assert(replay_studio_collision(state,event));
    while(state.camera_transition) advance_studio(state,.1);
    seek_studio_time(state,0);
    assert(!state.collision_replay);

    auto solar=make_solar_system_project();
    solar.bodies[3].position=solar.bodies[4].position;
    StudioState solar_state(solar,"replay-overview.json");
    solar_state.session.begin_simulation();
    const auto solar_event=solar_state.session.observed_events().front();
    assert(replay_studio_collision(solar_state,solar_event));
    while(solar_state.camera_transition) advance_studio(solar_state,.1);
    select_studio_overview(solar_state,StudioOverview::FullSystem);
    assert(!solar_state.collision_replay && solar_state.camera_transition);
    while(solar_state.camera_transition) advance_studio(solar_state,.1);
    assert(solar_state.render.camera.zoom==.55);

    Project large;
    large.view_extent=2;large.display_scale=1;
    large.bodies={{"a","A",2,.1,{0,0},{0,0},{}},
                  {"b","B",1,1,{.9,0},{0,0},{}}};
    StudioState large_state(large,"large-impact-replay.json");
    large_state.session.begin_simulation();
    const auto large_event=large_state.session.observed_events().front();
    assert(replay_studio_collision(large_state,large_event));
    assert(large_state.camera_transition->to.zoom>.8);
}

// Exercise the same render/UI order as the desktop with actual mouse events.
struct StudioUiTest {
    StudioState state{make_solar_system_project(), "ui-regression.json"};
    double now = 0.0;
    bool hold_frame = false;
    bool capture_text = false;
    std::string drawn_text;

    StudioUiTest() {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1280, 800};
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        state.render.width = 128;
        state.render.height = 96;
        activate_studio_preset(state, DemoPreset::EarthTerrain);
        draw(); draw();
    }
    ~StudioUiTest() { ImGui::DestroyContext(); }

    void draw() {
        now += 1.0 / 60.0;
        advance_studio(state, 1.0 / 60.0);
        const auto input = make_studio_render_input(state);
        const auto& frame = hold_frame ? *state.frame_cache.frame
                                      : update_studio_frame(state, input, now);
        ImGui::NewFrame();
        if (capture_text) ImGui::LogToBuffer();
        draw_studio(state, 1, input, frame, false);
        if (capture_text) {
            drawn_text = GImGui->LogBuffer.c_str();
            ImGui::LogFinish();
        }
        ImGui::Render();
    }
    std::string text() {
        capture_text = true; draw(); capture_text = false;
        return drawn_text;
    }
    void click(float x, float y) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(x, y); draw();
        io.AddMouseButtonEvent(0, true); draw();
        io.AddMouseButtonEvent(0, false); draw();
    }
    ImGuiWindow* inspector() const {
        for (ImGuiWindow* window : GImGui->Windows) {
            if (std::strstr(window->Name, "/##inspector_")) return window;
        }
        assert(false && "Inspector missing");
        return nullptr;
    }
    void click_tab(int index) {
        auto* window = inspector();
        const auto* tabs = GImGui->TabBars.GetByKey(window->GetID("##inspector-tabs"));
        assert(tabs && tabs->Tabs.Size == 3);
        const auto& tab = tabs->Tabs[index];
        click(tabs->BarRect.Min.x + tab.Offset + tab.Width / 2, tabs->BarRect.GetCenter().y);
        draw();
    }
    std::string project_text() const {
        std::ostringstream text;
        write_project(text, state.session.project());
        return text.str();
    }
};

static void test_inspector_switches_from_pixel_to_simulation_and_back() {
    StudioUiTest ui;
    const auto design = ui.project_text();
    ui.click_tab(2);
    assert(ui.state.pixel_tab_active);
    begin_studio_simulation(ui.state);
    while (ui.state.camera_transition) ui.draw();
    ui.state.session.set_playing(false);
    ui.draw(); ui.draw();
    assert(!ui.state.pixel_tab_active);
    auto text = ui.text();
    assert(text.find("Live orbit") != std::string::npos);
    assert(text.find("Collisions") != std::string::npos);
    assert(text.find("Relative terrain relief") == std::string::npos);
    ui.click_tab(2);
    return_studio_design(ui.state);
    while (ui.state.camera_transition) ui.draw();
    ui.draw(); ui.draw();
    assert(!ui.state.pixel_tab_active);
    assert(ui.project_text() == design);
    assert(ui.text().find("Design Earth") != std::string::npos);
}

static void test_analysis_hides_scene_annotations_and_guides_the_tool() {
    StudioUiTest ui;
    assert(studio_scene_annotations_visible(ui.state));
    ui.click_tab(2);
    assert(!studio_scene_annotations_visible(ui.state));
    ui.state.inspect_pixel = false;
    assert(!studio_scene_annotations_visible(ui.state));
    ui.click_tab(0);
    assert(studio_scene_annotations_visible(ui.state));
    ui.state.compare_modes = true;
    assert(!studio_scene_annotations_visible(ui.state));
}

static void test_reselecting_closeup_keeps_pixel_analysis() {
    for (bool preset : {false, true}) {
        StudioUiTest ui;
        ImGui::GetIO().DisplaySize.y = 6000;
        auto& state = ui.state;
        if (!preset) {
            exit_studio_preset(state);
            // Selecting the current body from the overview must still enter it.
            state.selected = 3;
            select_studio_body(state, 3);
            assert(state.camera_transition);
            while (state.camera_transition) ui.draw();
        }
        assert(state.design_view && state.selected == 3);
        state.render.camera.yaw = .21;
        state.render.camera.pitch = -.17;
        state.render.camera.zoom = .67;
        state.compare_baseline = true;
        state.compare_modes = state.inspect_pixel = state.select_pixel_tab = true;
        state.compare_left = SamplingMode::SSAA4;
        state.compare_right = SamplingMode::MSAA4;
        state.selected_pixel_x = 96; state.selected_pixel_y = 48;
        state.pixel_region_size = 17;
        for (int i = 0; i < 100 && !state.measurement.ready; ++i) ui.draw();
        ui.draw(); ui.draw(); // Restore the untimed trace after measurement.
        assert(state.measurement.ready && state.frame_cache.frame->trace);
        assert(state.frame_cache.frame->trace->events.size() > 1);
        state.pixel_disclosure = {true, true, true, true, true, {true, true, true, true}};
        ui.draw(); // Opening history first initializes its displayed trace identity.
        state.trace_event_index = 1;
        ui.draw();
        assert(state.trace_event_index == 1);
        const auto camera = state.render.camera;
        const auto measurement = state.measurement;
        const auto render_count = state.frame_cache.render_count;
        const auto target = state.pixel_inspector_target;
        const auto project = ui.project_text();

        // Click the actual Earth row, not just the selection function. Assert
        // its navigation ID too, so a miss cannot silently pass as a no-op.
        ui.click(40, 121);
        ImGuiWindow* sidebar = nullptr;
        for (ImGuiWindow* window : GImGui->Windows)
            if (std::strstr(window->Name, "/##sidebar_")) sidebar = window;
        assert(sidebar && GImGui->NavId == sidebar->GetID("Earth##earth"));
        ui.draw();
        assert(!state.camera_transition && state.design_view && state.selected == 3);
        assert(state.demos.active() == (preset ? DemoPreset::EarthTerrain : DemoPreset::None));
        assert(state.inspect_pixel && state.compare_modes && state.compare_baseline && state.pixel_tab_active);
        assert(state.selected_pixel_x == 96 && state.selected_pixel_y == 48 && state.pixel_region_size == 17);
        assert(state.compare_left == SamplingMode::SSAA4 && state.compare_right == SamplingMode::MSAA4);
        assert(state.pixel_inspector_target == target && state.trace_event_index == 1);
        const auto& open = state.pixel_disclosure;
        assert(open.details && open.history && open.background && open.shading && open.resolve);
        assert(open.samples == std::vector<bool>({true, true, true, true}));
        assert(state.render.camera.yaw == camera.yaw && state.render.camera.pitch == camera.pitch &&
               state.render.camera.zoom == camera.zoom && state.render.camera.target.x == camera.target.x &&
               state.render.camera.target.y == camera.target.y);
        assert(state.measurement.ready && !state.measurement.running && !state.measurement.waiting);
        assert(state.measurement.input_key == measurement.input_key &&
               state.measurement.samples == measurement.samples && state.measurement.median_ms == measurement.median_ms);
        assert(state.frame_cache.render_count == render_count && ui.project_text() == project);

        // Explicit reset and selection of another planet keep their old meaning.
        reset_studio_view(state);
        assert(state.camera_transition);
        while (state.camera_transition) ui.draw();
        select_studio_body(state, 6);
        assert(state.selected == 6 && state.camera_transition);
        assert(!state.inspect_pixel && !state.compare_modes);
        assert(state.selected_pixel_x == -1 && state.selected_pixel_y == -1);
    }
}

static void test_orbit_tab_does_not_paint_the_closeup() {
    StudioUiTest ui;
    ui.click_tab(1);
    const auto before = ui.project_text();
    ui.click(600, 347);
    assert(ui.project_text() == before);
    assert(studio_workflow(ui.state).hint.find("initial") != std::string::npos);
    ui.click_tab(0);
    ui.click(600, 347);
    assert(ui.project_text() != before);
}

static void test_collision_bookmark_click_after_rewind() {
    StudioUiTest ui;
    exit_studio_preset(ui.state);
    auto project = ui.state.session.project();
    project.bodies[3].position = {4.0, 0.0};
    project.bodies[3].velocity = {0.0, 0.0};
    project.bodies[4].position = {4.5, 0.0};
    project.bodies[4].velocity = {-1.0, 0.0};
    ui.state.session.replace_project(project);
    ui.state.session.begin_simulation();
    seek_studio_time(ui.state, 0.8);
    assert(ui.state.session.observed_events().size() == 1);
    const auto event = ui.state.session.observed_events().front();
    seek_studio_time(ui.state, 0.0);
    ui.draw(); ui.draw();
    const auto text = ui.text();
    assert(text.find("Earth + Mars") != std::string::npos);
    assert(text.find("later in this run") != std::string::npos);
    ImGuiWindow* list = nullptr;
    for (auto* window : GImGui->Windows) {
        if (std::strstr(window->Name, "/##collision-list_")) list = window;
    }
    assert(list);
    ui.click(list->WorkRect.Min.x + 45, list->WorkRect.Min.y + 14);
    assert(!ui.state.session.playing());
    assert(ui.state.collision_replay && ui.state.session.snapshot().time < event.time);
    while(ui.state.camera_transition) ui.draw();
    seek_studio_time(ui.state,event.time+.05);
    assert(ui.state.session.project().bodies[ui.state.selected].id == "earth");
    assert(ui.state.session.observed_events().size() == 1);
    select_studio_scene_body(ui.state, 4);
    assert(ui.text().find("Merged into Earth") != std::string::npos);
}

static void test_replay_pause_and_reset_buttons_use_the_replay_clock() {
    StudioUiTest ui;
    exit_studio_preset(ui.state);
    Project project;
    project.bodies={{"a","A",2,.1,{0,0},{0,0},{}},
                    {"b","B",1,.1,{1,0},{-1,0},{}}};
    ui.state.session.replace_project(project);
    ui.state.session.begin_simulation();
    ui.state.session.set_speed(5);
    seek_studio_time(ui.state,1);
    const auto event=ui.state.session.observed_events().front();
    assert(replay_studio_collision(ui.state,event));
    while(ui.state.camera_transition) ui.draw();
    ui.draw();
    const float padding=ImGui::GetStyle().FramePadding.x*2;
    const float spacing=ImGui::GetStyle().ItemSpacing.x;
    const float play_x=210+ImGui::CalcTextSize("Design").x+padding+spacing;
    const float y=ImGui::GetStyle().WindowPadding.y+ImGui::GetFrameHeight()/2;
    ui.click(play_x+12,y);
    assert(ui.state.collision_replay && !ui.state.collision_replay->playing);
    const double paused=ui.state.session.snapshot().time;
    ui.draw();ui.draw();
    assert(ui.state.session.snapshot().time==paused);
    ui.click(play_x+12,y);
    assert(ui.state.collision_replay->playing && ui.state.session.speed()==5);
    const float reset_x=play_x+ImGui::CalcTextSize("Pause").x+padding+spacing+
        ImGui::CalcTextSize("Step").x+padding+spacing;
    ui.click(reset_x+12,y);
    assert(!ui.state.collision_replay && !ui.state.session.playing());
    assert(ui.state.session.snapshot().time==0 && ui.state.session.speed()==5);
}

static void test_collision_demo_inspector_uses_its_temporary_initial_conditions() {
    StudioUiTest ui;
    activate_studio_preset(ui.state, DemoPreset::Collision);
    ui.state.demos.set_playing(false);
    ui.draw(); ui.draw();
    assert(ui.text().find("Temporary Earth and Mars demo") != std::string::npos);
    ui.click_tab(1);
    assert(ui.text().find("Position: 4.000, 0.000") != std::string::npos);
    assert(ui.state.session.project().bodies[3].position.x != 4.0);
}

static void test_old_frame_markers_do_not_relabel_a_changed_catalog() {
    StudioUiTest ui;
    exit_studio_preset(ui.state);
    ui.state.design_view = false;
    ui.state.selected = 0;
    ui.state.render.camera = {};
    ui.state.render.camera.zoom = 0.55;
    ui.draw(); ui.draw();
    assert(ui.state.body_markers.size() > 2);
    const auto original = ui.state.session.project();
    // Keep displaying the previous project's image while loading/removing bodies.
    ui.state.frame_cache.displayed_input = make_studio_render_input(ui.state);
    ui.hold_frame = true;
    auto smaller = original;
    smaller.solar_system = false;
    smaller.bodies.resize(2);
    ui.state.session.replace_project(smaller);
    ui.draw();
    assert(ui.state.body_markers.empty());
    auto reordered = original;
    reordered.solar_system = false;
    for (auto& body : reordered.bodies) body.radius = 0.5;
    std::swap(reordered.bodies[3], reordered.bodies[4]);
    ui.state.session.replace_project(reordered);
    ui.draw();
    assert(ui.state.body_markers.empty());
    ui.hold_frame = false;
    ui.draw();
    assert(!ui.state.body_markers.empty());
}

static void test_stale_async_view_blocks_sculpt_and_pixel_selection() {
    StudioUiTest ui;
    auto& state = ui.state;
    StudioRenderPipeline pipeline;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        pipeline.update(state, make_studio_render_input(state), ui.now);
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (!studio_frame_can_interact(state));
    ui.hold_frame = true; // Keep displaying the last completed image during a slow render.
    const auto unsculpted = ui.project_text();
    ui.click(600, 347);
    assert(ui.project_text() != unsculpted);
    assert(studio_frame_can_interact(state)); // Sculpt revisions keep the displayed UVs usable.
    const auto original = ui.project_text();
    state.render.camera.yaw += 0.4;
    assert(!studio_frame_can_interact(state));
    ui.click(600, 347);
    assert(ui.project_text() == original);
    state.inspect_pixel = true;
    state.selected_pixel_x = state.selected_pixel_y = -1;
    ui.click(600, 347);
    assert(state.selected_pixel_x == -1 && state.selected_pixel_y == -1);
}

static void test_loading_same_catalog_rejects_old_async_view() {
    StudioUiTest ui;
    auto& state = ui.state;
    exit_studio_preset(state);
    state.design_view = false;
    state.selected = 0;
    state.render.camera = {};
    state.render.camera.zoom = 0.55;
    StudioRenderPipeline pipeline;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        pipeline.update(state, make_studio_render_input(state), ui.now);
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (!studio_frame_can_interact(state));
    ui.hold_frame = true;
    const auto path = std::filesystem::temp_directory_path() /
        ("planet-ui-load-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    {
        std::ofstream output(path);
        write_project(output, state.session.project());
    }
    const auto name = path.string();
    assert(name.size() < state.path.size());
    std::strcpy(state.path.data(), name.c_str());
    ui.click(180, 277); // Load the same catalog and camera while the old image is held.
    std::filesystem::remove(path);
    assert(state.notice == "Loaded");
    assert(!studio_frame_can_interact(state));
}

static void test_pixel_tab_never_sculpts() {
    StudioUiTest ui;
    auto& state = ui.state;
    const auto original = ui.project_text();
    assert(!state.inspect_pixel && !state.compare_modes && !state.pixel_tab_active);
    ui.click_tab(2);
    assert(state.pixel_tab_active && state.inspect_pixel && !state.compare_modes);
    ui.click(600, 347); // Earth surface in the single view.
    assert(state.frame_cache.frame->trace);
    assert(state.selected_pixel_x >= 0 && state.selected_pixel_y >= 0);
    assert(ui.project_text() == original);

    auto& io = ImGui::GetIO();
    io.AddMouseButtonEvent(0, true); ui.draw();
    io.AddMousePosEvent(610, 350); ui.draw();
    io.AddMousePosEvent(620, 353); ui.draw();
    io.AddMouseButtonEvent(0, false); ui.draw();
    assert(ui.project_text() == original);

    ui.click(540, 44); // Explicitly disable Inspect while Pixel remains visible.
    assert(!state.inspect_pixel && state.pixel_tab_active);
    ui.click(600, 347);
    assert(!state.inspect_pixel && !state.frame_cache.frame->trace);
    assert(ui.project_text() == original);

    state.compare_modes = state.inspect_pixel = true;
    ui.draw();
    ui.click(290, 44); // Reset view must keep its existing mode-reset behavior.
    while (state.camera_transition) ui.draw();
    ui.draw();
    assert(!state.compare_modes && !state.inspect_pixel && state.pixel_tab_active);
    ui.click(600, 347);
    assert(ui.project_text() == original);
    assert(!state.frame_cache.frame->trace);

    ui.click_tab(0);
    assert(!state.pixel_tab_active && !state.inspect_pixel);
    ui.click(600, 347);
    assert(ui.project_text() != original); // Normal Design sculpting still works.
    const auto sculpted = ui.project_text();
    ui.click_tab(2);
    ui.click(600, 347);
    assert(state.inspect_pixel && state.frame_cache.frame->trace);
    assert(ui.project_text() == sculpted);
}

static void test_design_slider_and_reset_keep_valid_body_identity() {
    StudioUiTest ui;
    auto& state = ui.state;
    ui.click(600, 347); // Give Reset a sculpt stroke and undo history to clear.
    assert(!state.session.project().bodies[3].design.sculpt_visible.empty());
    ui.click(1200, 176); // Relative terrain relief slider in the Design tab.
    assert(state.session.project().bodies[3].design.relief > 1.0);
    const auto* inspector = ui.inspector();
    ui.click(inspector->WorkRect.Min.x + 50,
             inspector->DC.CursorMaxPos.y - ImGui::GetFrameHeight() / 2);
    const auto& design = state.session.project().bodies[3].design;
    assert(design.relief == baseline_design(BodyKind::Earth).relief);
    assert(design.sculpt_visible.empty());
    assert(state.notice == "Body reset");
    auto restored = design;
    assert(!state.sculpt_history.undo(BodyKind::Earth, restored));
}

static void test_parameter_drag_is_one_undo_step_and_redo_restores_it() {
    StudioUiTest ui;
    auto& state = ui.state;
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(1060, 176); ui.draw();
    io.AddMouseButtonEvent(0, true); ui.draw();
    io.AddMousePosEvent(1120, 176); ui.draw();
    io.AddMousePosEvent(1200, 176); ui.draw();
    io.AddMouseButtonEvent(0, false); ui.draw();
    const auto relief = state.session.project().bodies[3].design.relief;
    assert(relief > 1.0 && state.history.dirty());
    assert(state.history.undo_count() == 1);
    assert(undo_studio_edit(state));
    assert(state.session.project().bodies[3].design.relief == 1.0 && !state.history.dirty());
    assert(redo_studio_edit(state));
    assert(state.session.project().bodies[3].design.relief == relief);
    ui.click(600, 347);
    assert(!state.session.project().bodies[3].design.sculpt_visible.empty());
    assert(undo_studio_edit(state));
    assert(state.session.project().bodies[3].design.sculpt_visible.empty());
    assert(state.session.project().bodies[3].design.relief == relief);
    io.AddMousePosEvent(1200, 176); ui.draw();
    io.AddMouseButtonEvent(0, true); ui.draw();
    io.AddMousePosEvent(1150, 176); ui.draw();
    io.AddMouseButtonEvent(0, false); ui.draw();
    assert(!state.history.can_redo());
}

static void test_history_shortcuts_respect_text_input() {
    StudioUiTest ui;
    auto& state = ui.state;
    auto& io = ImGui::GetIO();
    ui.click(1200, 176);
    const auto relief = state.session.project().bodies[3].design.relief;
    assert(relief > 1.0 && state.history.can_undo());
    const auto shortcut = [&](ImGuiKey key, bool shift = false) {
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiMod_Shift, shift);
        io.AddKeyEvent(key, true); ui.draw();
        io.AddKeyEvent(key, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        io.AddKeyEvent(ImGuiMod_Shift, false); ui.draw();
    };
    shortcut(ImGuiKey_Z);
    assert(state.session.project().bodies[3].design.relief == 1.0);
    shortcut(ImGuiKey_Z, true);
    assert(state.session.project().bodies[3].design.relief == relief);
    ui.click(70, 253);
    ui.draw();
    assert(io.WantTextInput);
    shortcut(ImGuiKey_Z);
    assert(state.session.project().bodies[3].design.relief == relief);
}

static void test_pixel_keyboard_then_mouse_details() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // Keep the expanded history on screen so the real mouse checks below do
    // not depend on scrolling or on scrollbar-induced changes to the zoom.
    io.DisplaySize = {1280, 6000};
    io.DeltaTime = 1.0f / 60.0f;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    StudioState state(make_solar_system_project(), "keyboard-mouse.json");
    state.render.width = 128;
    state.render.height = 96;
    activate_studio_preset(state, DemoPreset::EarthTerrain);
    state.compare_modes = state.inspect_pixel = state.select_pixel_tab = true;
    state.selected_pixel_x = 32;
    state.selected_pixel_y = 48;
    std::string visible_text;
    auto draw = [&](bool capture_text = false) {
        const auto input = make_studio_render_input(state);
        const auto frame = render_studio_frame(state, input);
        ImGui::NewFrame();
        if (capture_text) ImGui::LogToBuffer(0); // Capture only explicitly opened nodes.
        draw_studio(state, 1, input, frame, false);
        if (capture_text) {
            visible_text = GImGui->LogBuffer.c_str();
            ImGui::LogFinish();
        }
        ImGui::Render();
    };
    auto click = [&](float x, float y) {
        io.AddMousePosEvent(x, y); draw();
        io.AddMouseButtonEvent(0, true); draw();
        io.AddMouseButtonEvent(0, false); draw();
    };
    draw(); draw();
    click(426, 347);
    const int x = state.selected_pixel_x, y = state.selected_pixel_y;
    io.AddKeyEvent(ImGuiKey_RightArrow, true); draw();
    io.AddKeyEvent(ImGuiKey_RightArrow, false); draw();
    assert(state.selected_pixel_x == x + 1 && state.selected_pixel_y == y);
    ImGuiWindow* inspector = nullptr;
    for (ImGuiWindow* window : GImGui->Windows) {
        if (std::strstr(window->Name, "/##inspector_")) inspector = window;
    }
    assert(inspector);
    auto* tabs = GImGui->TabBars.GetByKey(inspector->GetID("##inspector-tabs"));
    assert(tabs && tabs->Tabs.Size == 3);
    const ImGuiID details = ImHashStr("Details", 0, tabs->Tabs[2].ID);
    assert(inspector->StateStorage.GetInt(details, 0) == 0);
    // Details is the last control while collapsed; derive its position from layout.
    const float control_x = inspector->WorkRect.Min.x + 50;
    const float details_y = inspector->DC.CursorMaxPos.y - ImGui::GetFrameHeight() / 2;
    click(control_x, details_y);
    assert(inspector->StateStorage.GetInt(details, 0) == 1);
    assert(state.selected_pixel_x == x + 1 && state.selected_pixel_y == y);
    const ImGuiID history = ImHashStr("Processing history", 0, tabs->Tabs[2].ID);
    const ImGuiID background = ImHashStr("Initial background", 0, history);
    const ImGuiID shading = ImHashStr("Shading and depth values", 0, history);
    const ImGuiID resolve = ImHashStr("Final resolve", 0, history);
    auto activate_tree = [&](ImGuiID id) {
        // Use the same keyboard activation path as a focused disclosure row.
        ImGui::FocusWindow(inspector);
        ImGui::SetNavID(id, ImGuiNavLayer_Main, 0, ImRect(0, 0, 100, 20));
        ImGui::SetNavCursorVisible(true);
        io.AddKeyEvent(ImGuiKey_Space, true); draw();
        io.AddKeyEvent(ImGuiKey_Space, false); draw();
    };
    auto sample_id = [&](int sample) {
        return ImHashStr("Sample", 0, ImHashData(&sample, sizeof(sample), shading));
    };
    auto descendants_are_closed = [&] {
        draw(true);
        assert(visible_text.find("Initial depth: infinity") == std::string::npos);
        assert(visible_text.find("Positions are inside this pixel") == std::string::npos);
        assert(visible_text.find("Depth test:") == std::string::npos);
        assert(visible_text.find("All final samples averaged") == std::string::npos);
    };
    auto open_descendants = [&] {
        activate_tree(background);
        activate_tree(shading);
        for (int s = 0; s < int(state.pixel_disclosure.samples.size()); ++s) {
            activate_tree(sample_id(s));
            assert(inspector->StateStorage.GetInt(sample_id(s), 0) == 1);
        }
        activate_tree(resolve);
        draw(true);
        assert(visible_text.find("Initial depth: infinity") != std::string::npos);
        assert(visible_text.find("Positions are inside this pixel") != std::string::npos);
        assert(visible_text.find("Depth test:") != std::string::npos);
        assert(visible_text.find("All final samples averaged") != std::string::npos);
    };
    const float history_y = details_y + ImGui::GetFrameHeight() / 2 +
        ImGui::GetStyle().ItemSpacing.y + 5 * ImGui::GetTextLineHeightWithSpacing() +
        ImGui::GetTextLineHeight() / 2;
    auto open_both = [&] {
        click(control_x, details_y);
        assert(inspector->StateStorage.GetInt(details, 0) == 1);
        assert(inspector->StateStorage.GetInt(history, 0) == 0);
        click(control_x, history_y);
        assert(inspector->StateStorage.GetInt(history, 0) == 1);
        descendants_are_closed();
    };
    click(control_x, history_y);
    assert(inspector->StateStorage.GetInt(history, 0) == 1);
    open_descendants();
    draw(); draw(); // Refreshing the same selection must not interrupt reading.
    assert(inspector->StateStorage.GetInt(details, 0) == 1);
    assert(inspector->StateStorage.GetInt(history, 0) == 1);

    activate_tree(shading);
    activate_tree(shading);
    draw(true);
    assert(visible_text.find("Depth test:") == std::string::npos);
    assert(visible_text.find("Positions are inside this pixel") != std::string::npos);
    // Closing this branch must not close its siblings.
    assert(visible_text.find("Initial depth: infinity") != std::string::npos);
    assert(visible_text.find("All final samples averaged") != std::string::npos);
    activate_tree(sample_id(0));
    click(control_x, history_y);
    click(control_x, history_y);
    descendants_are_closed();
    open_descendants();
    click(control_x, details_y);
    assert(inspector->StateStorage.GetInt(details, 0) == 0);
    open_both(); // Reopening Details must leave history closed until its click.

    open_descendants();
    click(445, 347); // A different pixel closes both disclosures.
    assert(state.selected_pixel_x != x + 1);
    assert(inspector->StateStorage.GetInt(details, 0) == 0);
    open_both();

    open_descendants();
    const int pixel_x = state.selected_pixel_x, pixel_y = state.selected_pixel_y;
    state.selected = 4; // A different planet, even at the same pixel coordinates.
    draw(); draw();
    assert(state.selected_pixel_x == pixel_x && state.selected_pixel_y == pixel_y);
    assert(inspector->StateStorage.GetInt(details, 0) == 0);
    open_both();

    open_descendants();
    state.compare_right = SamplingMode::SSAA4;
    draw(); draw();
    assert(inspector->StateStorage.GetInt(details, 0) == 0);
    open_both();
    for (auto mode : {SamplingMode::SSAA4, SamplingMode::SSAA16}) {
        state.compare_left = mode;
        draw(); draw();
        open_both();
        open_descendants();
        const auto samples = state.pixel_disclosure.samples.size();
        assert(samples == (mode == SamplingMode::SSAA4 ? 4 : 16));
        activate_tree(shading);
        activate_tree(shading);
        draw(true);
        assert(visible_text.find("Depth test:") == std::string::npos);
        for (int s = 0; s < int(samples); ++s)
            assert(inspector->StateStorage.GetInt(sample_id(s), 0) == 0);
    }
    ImGui::DestroyContext();
}

// Send real ImGui mouse events to the playback button while Compare freezes time.
static void test_compare_playback_button_stays_paused() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1280, 800};
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (bool collision : {false, true}) {
        StudioState state(make_solar_system_project(), "compare-playback.json");
        state.render.width = 128;
        state.render.height = 96;
        if (collision) {
            activate_studio_preset(state, DemoPreset::Collision);
            state.demos.set_playing(false);
        } else {
            state.session.begin_simulation();
            state.session.set_playing(false);
        }
        state.compare_modes = state.inspect_pixel = true;
        const auto input = make_studio_render_input(state);
        const auto frame = render_studio_frame(state, input);
        auto draw = [&] {
            ImGui::NewFrame();
            draw_studio(state, 1, input, frame, false);
            ImGui::Render();
        };
        draw(); draw();
        const auto& style = ImGui::GetStyle();
        const float preceding = collision
            ? ImGui::CalcTextSize("COLLISION DEMO").x + style.ItemSpacing.x +
                ImGui::CalcTextSize("Exit demo").x + style.FramePadding.x * 2
            : ImGui::CalcTextSize("Design").x + style.FramePadding.x * 2;
        const float x = 210 + preceding + style.ItemSpacing.x +
                        (ImGui::CalcTextSize("Play").x + style.FramePadding.x * 2) / 2;
        const float y = style.WindowPadding.y + ImGui::GetFrameHeight() / 2;
        auto click_play = [&] {
            io.AddMousePosEvent(x, y); draw();
            io.AddMouseButtonEvent(0, true); draw();
            io.AddMouseButtonEvent(0, false); draw();
        };
        click_play();
        assert(!(collision ? state.demos.playing() : state.session.playing()));
        advance_studio(state, 0.1);
        assert(make_studio_render_input(state).snapshot.time == input.snapshot.time);
        state.compare_modes = false;
        click_play();
        assert(collision ? state.demos.playing() : state.session.playing());
        advance_studio(state, 0.1);
        assert(make_studio_render_input(state).snapshot.time > input.snapshot.time);
    }
    ImGui::DestroyContext();
}

static void test_design_camera_flight() {
    StudioState state(make_solar_system_project(), "camera-flight.json");
    state.render.width = 128;
    state.render.height = 96;
    state.render.sampling = SamplingMode::SSAA4;
    const double initial_zoom = state.render.camera.zoom;
    begin_studio_design_focus(state, 3);
    assert(state.selected == 3 && !state.design_view);
    assert(state.camera_transition.has_value());
    assert(state.render.camera.zoom == initial_zoom);
    const StudioRenderInput departure = make_studio_render_input(state);
    assert(departure.snapshot.bodies.size() == 9);
    assert(departure.config.sampling == SamplingMode::Single);
    const RenderCamera world_close = state.camera_transition->to;
    assert(std::abs(world_close.target.x - state.session.project().bodies[3].position.x) < 1e-12);
    assert(world_close.zoom < initial_zoom);
    const auto earth_position = state.session.project().bodies[3].position;
    const auto screen_offset = [&](const RenderCamera& camera) {
        return std::hypot(earth_position.x - camera.target.x,
                          earth_position.y - camera.target.y) / camera.zoom;
    };
    const double initial_offset = screen_offset(state.render.camera);
    advance_studio(state, 0.1);
    assert(screen_offset(state.render.camera) < initial_offset);

    StudioRenderInput matched_world = departure;
    const BodySpec& earth = state.session.project().bodies[3];
    matched_world.snapshot.bodies = {{earth.id, earth.position, earth.velocity, earth.mass, earth.radius}};
    matched_world.snapshot.trails.clear();
    matched_world.config.camera = world_close;
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(!state.camera_transition && state.design_view);
    assert(state.render.sampling == SamplingMode::SSAA4);
    StudioRenderInput detail = make_studio_render_input(state);
    assert(detail.snapshot.bodies.size() == 1 && detail.snapshot.bodies[0].id == "earth");
    detail.config.sampling = SamplingMode::Single;
    const RenderFrame world_frame = render_scene(matched_world.project, matched_world.snapshot,
                                                  matched_world.config);
    const RenderFrame detail_frame = render_scene(detail.project, detail.snapshot, detail.config);
    assert(world_frame.body_index == detail_frame.body_index);

    const double earth_x = earth.position.x;
    const double earth_y = earth.position.y;
    state.render.camera.yaw = 0.31;
    state.render.camera.zoom = 0.64;
    begin_studio_design_focus(state, 6);
    assert(state.selected == 6 && !state.design_view);
    assert(std::abs(state.render.camera.target.x - earth_x) < 1e-12);
    assert(std::abs(state.render.camera.target.y - earth_y) < 1e-12);
    assert(std::abs(state.render.camera.yaw - 0.31) < 1e-12);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(!state.camera_transition && state.design_view && state.selected == 6);
    return_studio_overview(state);
    assert(!state.design_view && state.camera_transition);
    assert(std::abs(state.render.camera.target.x -
                    state.session.project().bodies[6].position.x) < 1e-12);
    const double return_start_zoom = state.render.camera.zoom;
    advance_studio(state, 0.1);
    assert(state.render.camera.zoom > return_start_zoom);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(!state.camera_transition);
    assert(std::abs(state.render.camera.target.x) < 1e-12);
    assert(std::abs(state.render.camera.target.y) < 1e-12);
    assert(std::abs(state.render.camera.zoom - 0.55) < 1e-12);
}

static void test_simulation_waits_for_return_flight() {
    StudioState state(make_solar_system_project(), "simulation-flight.json");
    state.render.sampling = SamplingMode::MSAA4;
    begin_studio_design_focus(state, 3);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(state.design_view);
    begin_studio_simulation(state);
    assert(state.session.mode() == SessionMode::Simulate);
    assert(!state.session.playing());
    assert(!state.design_view && state.camera_transition);
    assert(make_studio_render_input(state).config.sampling == SamplingMode::Single);
    assert(make_studio_render_input(state).snapshot.bodies.size() == 9);
    advance_studio(state, 0.25);
    assert(state.session.snapshot().time == 0.0);
    assert(!state.session.playing());
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(!state.camera_transition && state.session.playing());
    assert(state.render.sampling == SamplingMode::MSAA4);
    assert(make_studio_render_input(state).config.sampling == SamplingMode::MSAA4);
    advance_studio(state, 0.05);
    assert(state.session.snapshot().time > 0.0);

    const double frozen_time = state.session.snapshot().time;
    return_studio_design(state);
    assert(state.session.mode() == SessionMode::Simulate && state.camera_transition);
    assert(!state.session.playing());
    advance_studio(state, 0.2);
    assert(state.session.snapshot().time == frozen_time);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(!state.camera_transition && !state.design_view);
    assert(state.session.mode() == SessionMode::Design);
    assert(std::abs(state.render.camera.zoom - 0.55) < 1e-12);
}

static void test_focus_and_reset_animate_without_losing_playback() {
    StudioState state(make_solar_system_project(), "focus-flight.json");
    state.selected = 3;
    state.session.begin_simulation();
    const double start_zoom = state.render.camera.zoom;
    focus_studio_selected(state);
    assert(state.camera_transition && !state.session.playing());
    advance_studio(state, 0.1);
    assert(state.render.camera.zoom < start_zoom);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(state.session.playing());
    assert(std::abs(state.render.camera.target.x -
                    state.session.project().bodies[3].position.x) < 1e-12);

    state.render.camera.yaw = 6.2;
    reset_studio_view(state);
    assert(state.camera_transition && !state.session.playing());
    advance_studio(state, 0.45);
    assert(state.render.camera.yaw > 6.2);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    assert(state.session.playing());
    assert(std::abs(state.render.camera.yaw) < 1e-12);
    assert(std::abs(state.render.camera.zoom - 0.55) < 1e-12);
}

static void test_roughness_edits_survive_undo_and_redo() {
    StudioState state(make_solar_system_project(),"roughness-history.json");
    state.selected=1;
    auto project=state.session.project();project.bodies[1].design.surface_roughness=.12;
    assert(apply_studio_edit(state,project,"Surface roughness","mercury"));
    state.history.finish();
    assert(state.session.project().bodies[1].design.surface_roughness==.12);
    assert(undo_studio_edit(state));
    assert(state.session.project().bodies[1].design.surface_roughness==.7);
    assert(redo_studio_edit(state));
    assert(state.session.project().bodies[1].design.surface_roughness==.12);
}

static void test_solar_size_guard_preserves_physics_and_restores_exaggeration() {
    auto project = make_solar_system_project();
    project.display_scale = 8.0;
    StudioState state(project, "size-guard.json");
    const auto saved = project_document(state.session.project());
    const auto snapshot = state.session.snapshot();
    auto input = make_studio_render_input(state);
    assert(input.project.display_scale == 1.0);
    assert(project_document(state.session.project()) == saved);
    assert(input.snapshot.bodies.size() == snapshot.bodies.size());
    for (std::size_t i = 0; i < snapshot.bodies.size(); ++i) {
        assert(input.snapshot.bodies[i].radius == snapshot.bodies[i].radius);
        assert(input.snapshot.bodies[i].position.x == snapshot.bodies[i].position.x);
        assert(input.snapshot.bodies[i].position.y == snapshot.bodies[i].position.y);
    }
    state.protect_display_sizes = false;
    assert(make_studio_render_input(state).project.display_scale == 8.0);
    state.protect_display_sizes = true;
    begin_studio_design_focus(state, 3);
    const auto arrival = state.camera_transition->to;
    while (state.camera_transition) advance_studio(state, .1);
    input = make_studio_render_input(state);
    assert(input.project.display_scale == 1.0);
    assert(std::abs(arrival.zoom * project.view_extent -
                    input.config.camera.zoom * input.project.view_extent) < 1e-12);
    begin_studio_simulation(state);
    while (state.camera_transition) advance_studio(state, .1);
    state.session.set_playing(false);
    select_studio_body(state, 6);
    while (state.camera_transition) advance_studio(state, .1);
    input = make_studio_render_input(state);
    assert(input.project.display_scale == 1.0);
    assert(input.config.camera.zoom < .08);
    assert(project_document(state.session.project()) == saved);

    StudioState baseline(make_solar_system_project(), "guard-baseline.json");
    assert(project_document(make_studio_render_input(baseline).project) ==
           project_document(baseline.session.project()));
    project.display_scale = .5;
    StudioState small(project, "guard-small.json");
    assert(make_studio_render_input(small).project.display_scale == .5);
    project.solar_system = false;
    StudioState custom(project, "guard-custom.json");
    project.display_scale = 8;
    custom.session.replace_project(project);
    assert(make_studio_render_input(custom).project.display_scale == 8);
    activate_studio_preset(baseline, DemoPreset::SaturnRing);
    assert(make_studio_render_input(baseline).project.display_scale ==
           baseline.demos.project(baseline.session.project()).display_scale);
}

static void test_inner_overview_frames_live_inner_planets_without_changing_scene() {
    StudioState state(make_solar_system_project(), "inner-overview.json");
    state.render.width = 320; state.render.height = 240;
    const auto original = project_document(state.session.project());
    const auto full_input = make_studio_render_input(state);
    const auto full = render_studio_frame(state, full_input);
    const auto full_markers = studio_body_markers(full, 9);
    state.overview = StudioOverview::InnerSystem;
    reset_studio_view(state);
    assert(state.camera_transition);
    while (state.camera_transition) advance_studio(state, .1);
    auto input = make_studio_render_input(state);
    assert(input.config.camera.zoom < .3);
    assert(input.snapshot.bodies.size() == 9);
    assert(project_document(state.session.project()) == original);
    const auto frame = render_studio_frame(state, input);
    const auto markers = studio_body_markers(frame, 9);
    for (int selected : {1,2,3,4}) {
        const auto find = [selected](const auto& candidates) {
            return std::find_if(candidates.begin(), candidates.end(),
                [selected](const auto& marker) { return marker.body_index == selected; });
        };
        const auto inner_marker = find(markers), full_marker = find(full_markers);
        assert(inner_marker != markers.end() && full_marker != full_markers.end());
        assert(inner_marker->max_y - inner_marker->min_y >
               1.8 * (full_marker->max_y - full_marker->min_y));
    }
    begin_studio_design_focus(state,3);
    while (state.camera_transition) advance_studio(state,.1);
    return_studio_overview(state);
    while (state.camera_transition) advance_studio(state,.1);
    assert(std::abs(state.render.camera.zoom-input.config.camera.zoom)<1e-12);
    begin_studio_simulation(state);
    while(state.camera_transition) advance_studio(state,.1);
    assert(state.session.playing());
    advance_studio(state,.1);
    input = make_studio_render_input(state);
    const auto sun = std::find_if(input.snapshot.bodies.begin(),input.snapshot.bodies.end(),
        [](const auto& body){return body.id=="sun";});
    assert(input.config.camera.target.x == sun->position.x);
    assert(input.config.camera.target.y == sun->position.y);
    select_studio_body(state,3);
    while(state.camera_transition) advance_studio(state,.1);
    return_studio_overview(state);
    while(state.camera_transition) advance_studio(state,.1);
    assert(state.simulation_follow_id.empty() && state.session.playing());
    assert(state.render.camera.zoom < .3);
    state.overview=StudioOverview::FullSystem;
    reset_studio_view(state);
    const auto frozen_time=state.session.snapshot().time;
    advance_studio(state,.1);
    assert(!state.session.playing() && state.session.snapshot().time==frozen_time);
    while(state.camera_transition) advance_studio(state,.1);
    assert(state.session.playing() && state.render.camera.zoom==.55);
    assert(project_document(state.session.project())==original);
}

static void test_inner_overview_returns_to_initial_sun_without_camera_jump() {
    auto project=make_solar_system_project();
    for(auto& body:project.bodies) body.velocity.x+=1;
    StudioState state(project,"drifting-sun.json");
    state.overview=StudioOverview::InnerSystem;
    begin_studio_simulation(state);
    while(state.camera_transition) advance_studio(state,.1);
    seek_studio_time(state,10);
    assert(make_studio_render_input(state).config.camera.target.x>9);
    return_studio_design(state);
    assert(state.camera_transition->to.target.x==0);
    while(state.camera_transition) advance_studio(state,.1);
    const auto input=make_studio_render_input(state);
    assert(state.session.mode()==SessionMode::Design);
    assert(input.config.camera.target.x==state.render.camera.target.x);
    assert(input.config.camera.target.y==state.render.camera.target.y);
    begin_studio_design_focus(state,3);
    assert(state.camera_transition->from.target.x==input.config.camera.target.x);
}

static void test_size_guard_keeps_demo_departure_framing() {
    for(const auto preset:{DemoPreset::EarthTerrain,DemoPreset::SaturnRing}) {
        for(int destination=0;destination<3;++destination) {
            auto project=make_solar_system_project();
            project.display_scale=8;
            StudioState state(project,"guard-demo.json");
            state.render.width=128; state.render.height=96;
            activate_studio_preset(state,preset);
            const int selected=state.selected;
            const auto before=make_studio_render_input(state);
            const auto before_frame=render_studio_frame(state,before);
            if(destination==0) begin_studio_design_focus(state,selected);
            else if(destination==1) return_studio_overview(state);
            else begin_studio_simulation(state);
            const auto after=make_studio_render_input(state);
            const auto after_frame=render_studio_frame(state,after);
            assert(after.project.display_scale==1);
            const double relative_before=before.project.display_scale/
                (before.project.view_extent*before.config.camera.zoom);
            const double relative_after=after.project.display_scale/
                (after.project.view_extent*after.config.camera.zoom);
            assert(std::abs(relative_after/relative_before-1)<1e-10);
            const auto height=[selected](const RenderFrame& frame) {
                const auto markers=studio_body_markers(frame,9);
                const auto marker=std::find_if(markers.begin(),markers.end(),
                    [selected](const auto& item){return item.body_index==selected;});
                assert(marker!=markers.end());
                return marker->max_y-marker->min_y+1;
            };
            assert(std::abs(height(before_frame)-height(after_frame))<=2);
        }
    }
}

static void test_overview_controls_and_size_guard_invalidate_old_frames() {
    StudioUiTest ui;
    exit_studio_preset(ui.state);
    auto project=ui.state.session.project();
    project.display_scale=8;
    ui.state.session.replace_project(project);
    ui.draw(); ui.draw();
    ImGuiWindow* scene=nullptr;
    for(auto* window:GImGui->Windows)
        if(std::strstr(window->Name,"/##scene_")) scene=window;
    assert(scene);
    const float button_height=ImGui::GetFrameHeight();
    const float spacing=ImGui::GetStyle().ItemSpacing.x;
    const float full_width=ImGui::CalcTextSize("Full system").x+2*ImGui::GetStyle().FramePadding.x;
    const float inner_width=ImGui::CalcTextSize("Inner system").x+2*ImGui::GetStyle().FramePadding.x;
    const float y=scene->WorkRect.Min.y+2*ImGui::GetFrameHeightWithSpacing()+button_height/2;
    const float x=scene->WorkRect.Min.x;
    ui.click(x+full_width+spacing+inner_width/2,y);
    assert(ui.state.overview==StudioOverview::InnerSystem && ui.state.camera_transition);
    while(ui.state.camera_transition) ui.draw();
    assert(ui.state.render.camera.zoom<.3);
    ui.draw();
    StudioRenderPipeline pipeline;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    do {
        pipeline.update(ui.state,make_studio_render_input(ui.state),ui.now);
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while(!studio_frame_can_interact(ui.state));
    assert(studio_frame_can_interact(ui.state));
    set_studio_size_protection(ui.state,false);
    assert(!studio_frame_can_interact(ui.state));
    ui.draw();
    assert(make_studio_render_input(ui.state).project.display_scale==8);
    ui.click(x+full_width+spacing+inner_width+spacing+button_height/2,y);
    assert(ui.state.protect_display_sizes);
    assert(make_studio_render_input(ui.state).project.display_scale==1);
    ui.click(x+full_width/2,y);
    assert(ui.state.overview==StudioOverview::FullSystem && ui.state.camera_transition);
    while(ui.state.camera_transition) ui.draw();
    assert(ui.state.render.camera.zoom==.55);
    assert(ui.text().find("Illustrative sizes")!=std::string::npos);

    ui.state.session.begin_simulation();
    ui.state.session.set_playing(false);
    select_studio_body(ui.state,3);
    while(ui.state.camera_transition) advance_studio(ui.state,.1);
    const auto protected_input=make_studio_render_input(ui.state);
    set_studio_size_protection(ui.state,false);
    const auto original_input=make_studio_render_input(ui.state);
    assert(std::abs(original_input.config.camera.zoom/protected_input.config.camera.zoom-8)<1e-10);
    assert(!ui.state.session.playing() && ui.state.simulation_follow_id=="earth");
}

static void test_simulation_closeup_tracks_live_state_and_keeps_scene() {
    StudioState state(make_solar_system_project(), "simulation-follow.json");
    const auto original = project_document(state.session.project());
    state.session.begin_simulation();
    state.render.width = 96;
    state.render.height = 72;
    for (int selected : {1, 3, 6}) {
        select_studio_body(state, selected);
        assert(state.camera_transition && !state.session.playing());
        while (state.camera_transition) advance_studio(state, 0.1);
        assert(state.session.playing());
        assert(!state.design_view);
        const auto id = state.session.project().bodies[selected].id;
        assert(state.simulation_follow_id == id);
        auto input = make_studio_render_input(state);
        // The close-up keeps every simulated body and the original lighting/model.
        assert(input.snapshot.bodies.size() == 9);
        assert(project_document(input.project) == original);
        assert(input.config.camera.zoom < 0.08);
        const auto frame = render_studio_frame(state, input);
        const auto markers = studio_body_markers(frame, 9);
        const auto marker = std::find_if(markers.begin(), markers.end(),
            [&](const auto& candidate) { return candidate.body_index == selected; });
        assert(marker != markers.end());
        // Bounds include both endpoint pixels; filtered rings can cover one extra surface row.
        assert(marker->max_y - marker->min_y + 1 > input.config.height / 4);
        assert(frame.body_index[(input.config.height / 2) * input.config.width + input.config.width / 2] == selected);

        const auto start = input.config.camera.target;
        advance_studio(state, 0.1);
        input = make_studio_render_input(state);
        const auto body = std::find_if(input.snapshot.bodies.begin(), input.snapshot.bodies.end(),
            [&](const auto& candidate) { return candidate.id == id; });
        assert(body != input.snapshot.bodies.end());
        assert(input.config.camera.target.x == body->position.x && input.config.camera.target.y == body->position.y);
        assert(input.config.camera.target.x != start.x || input.config.camera.target.y != start.y);
        // Reselecting the followed body preserves inspection and the user's view.
        state.inspect_pixel = true;
        state.selected_pixel_x = 48;
        select_studio_body(state, selected);
        assert(!state.camera_transition && state.inspect_pixel && state.selected_pixel_x == 48);
    }
    state.session.set_playing(false);
    const auto paused = make_studio_render_input(state);
    advance_studio(state, 0.1);
    assert(make_studio_render_input(state).config.camera.target.x == paused.config.camera.target.x);
    seek_studio_time(state, 2.0);
    auto sought = make_studio_render_input(state);
    assert(sought.config.camera.target.x == sought.snapshot.bodies[6].position.x);
    assert(sought.config.camera.target.y == sought.snapshot.bodies[6].position.y);
    assert(sought.config.camera.target.x != paused.config.camera.target.x);
    state.compare_modes = true;
    const auto frozen = make_studio_render_input(state);
    advance_studio(state, 0.1);
    assert(make_studio_render_input(state).snapshot.time == frozen.snapshot.time);
    assert(make_studio_render_input(state).config.camera.target.x == frozen.config.camera.target.x);
    state.compare_modes = false;
    state.session.step(0.025);
    sought = make_studio_render_input(state);
    assert(sought.config.camera.target.x == sought.snapshot.bodies[6].position.x);
    state.session.reset_simulation();
    sought = make_studio_render_input(state);
    assert(sought.snapshot.time == 0.0);
    assert(sought.config.camera.target.x == sought.snapshot.bodies[6].position.x);
    return_studio_overview(state);
    assert(state.simulation_follow_id.empty() && state.camera_transition);
    while (state.camera_transition) advance_studio(state, 0.1);
    assert(state.render.camera.target.x == 0.0 && state.render.camera.target.y == 0.0);
    assert(!state.session.playing());
    assert(project_document(state.session.project()) == original);
    focus_studio_selected(state);
    while (state.camera_transition) advance_studio(state, 0.1);
    return_studio_design(state);
    while (state.camera_transition) advance_studio(state, 0.1);
    assert(state.simulation_follow_id.empty() && state.session.mode() == SessionMode::Design);
}

static void test_simulation_closeup_mouse_controls() {
    StudioUiTest ui;
    exit_studio_preset(ui.state);
    ui.state.session.begin_simulation();
    select_studio_body(ui.state, 3);
    while (ui.state.camera_transition) ui.draw();
    ui.state.session.set_playing(false);
    ui.draw();
    assert(ui.text().find("Back to system") != std::string::npos);
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(600, 320);
    ui.draw();
    const double zoom = ui.state.render.camera.zoom;
    io.AddMouseWheelEvent(0.0f, 1.0f);
    ui.draw();
    assert(ui.state.render.camera.zoom < zoom);
    assert(ui.state.render.camera.zoom < 0.08);
    const double yaw = ui.state.render.camera.yaw;
    io.AddMouseButtonEvent(1, true);
    ui.draw();
    io.AddMousePosEvent(640, 340);
    ui.draw();
    io.AddMouseButtonEvent(1, false);
    ui.draw();
    assert(ui.state.render.camera.yaw > yaw);
    assert(ui.state.simulation_follow_id == "earth");
}

static void test_simulation_follow_scroll_preserves_direction_at_view_scales() {
    struct ViewCase { double extent, scale; };
    for (const ViewCase view : {ViewCase{19.0, 1.0}, ViewCase{1.0, 2.0}, ViewCase{1.0, 8.0}}) {
        StudioUiTest ui;
        exit_studio_preset(ui.state);
        auto project = ui.state.session.project();
        project.view_extent = view.extent;
        project.display_scale = view.scale;
        ui.state.session.replace_project(project);
        ui.state.protect_display_sizes = false; // Exercise the original unrestricted size range.
        ui.state.session.begin_simulation();
        ui.state.session.set_playing(false);
        select_studio_body(ui.state, 6);
        while (ui.state.camera_transition) advance_studio(ui.state, 0.1);
        ui.draw(); ui.draw();
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(600, 320);
        ui.draw();
        const double initial_zoom = ui.state.render.camera.zoom;

        // Zooming out must move smoothly away, even when the close-up starts above 3.
        io.AddMouseWheelEvent(0.0f, -1.0f);
        ui.draw();
        assert(ui.state.render.camera.zoom > initial_zoom);
        assert(std::abs(ui.state.render.camera.zoom / initial_zoom - 1.0 / 0.88) < 1e-10);
        io.AddMouseWheelEvent(0.0f, 1.0f);
        ui.draw();
        assert(std::abs(ui.state.render.camera.zoom / initial_zoom - 1.0) < 1e-10);

        // Both ends must saturate without reversing direction or losing the target.
        io.AddMouseWheelEvent(0.0f, 100.0f);
        ui.draw();
        const double closest = ui.state.render.camera.zoom;
        assert(std::isfinite(closest) && closest > 0.0 && closest < initial_zoom);
        io.AddMouseWheelEvent(0.0f, 1.0f);
        ui.draw();
        assert(ui.state.render.camera.zoom == closest);
        io.AddMouseWheelEvent(0.0f, -100.0f);
        ui.draw();
        const double farthest = ui.state.render.camera.zoom;
        assert(std::isfinite(farthest) && farthest > initial_zoom);
        io.AddMouseWheelEvent(0.0f, -1.0f);
        ui.draw();
        assert(ui.state.render.camera.zoom == farthest);
        assert(ui.state.simulation_follow_id == "saturn");
        assert(!ui.state.session.playing());
    }
}

static void test_simulation_follow_exits_when_target_merges() {
    Project project;
    project.bodies = {
        {"sun", "Sun", 10.0, 0.1, {0, 0}, {0, 0}, {}},
        {"earth", "Earth", 1.0, 0.1, {1.0, 0}, {-1, 0}, {}}
    };
    StudioState state(project, "follow-collision.json");
    state.session.begin_simulation();
    select_studio_body(state, 1);
    while (state.camera_transition) advance_studio(state, 0.1);
    assert(state.simulation_follow_id == "earth");
    seek_studio_time(state, 1.0);
    assert(state.session.snapshot().bodies.size() == 1);
    assert(state.simulation_follow_id.empty());
    assert(state.notice.find("no longer active") != std::string::npos);
    assert(state.render.camera.target.x == 0.0 && state.render.camera.target.y == 0.0);
    focus_studio_selected(state);
    assert(!state.camera_transition && state.simulation_follow_id.empty());
}

static void test_collision_demo_exits_to_saved_camera_before_simulation() {
    StudioState state(make_solar_system_project(), "demo-flight.json");
    state.render.camera.yaw = 0.42;
    state.render.camera.zoom = 0.69;
    const RenderCamera saved = state.render.camera;
    activate_studio_preset(state, DemoPreset::Collision);
    assert(state.render.camera.target.x == 4.6);
    begin_studio_simulation(state);
    assert(state.demos.active() == DemoPreset::None);
    assert(state.camera_transition);
    assert(state.camera_transition->from.yaw == saved.yaw);
    assert(state.camera_transition->from.zoom == saved.zoom);
    assert(state.camera_transition->from.target.x == saved.target.x);
}

static void test_demo_return_keeps_departing_planet_in_view() {
    StudioState state(make_solar_system_project(), "demo-back.json");
    assert(state.selected == 1);
    activate_studio_preset(state, DemoPreset::EarthTerrain);
    const auto earth = state.session.project().bodies[3].position;
    return_studio_overview(state);
    assert(state.demos.active() == DemoPreset::None);
    assert(state.selected == 1 && state.camera_transition);
    assert(state.camera_transition->screen_anchor);
    assert(std::abs(state.camera_transition->screen_anchor->x - earth.x) < 1e-12);
    assert(std::abs(state.camera_transition->screen_anchor->y - earth.y) < 1e-12);
    advance_studio(state, 0.45);
    assert(std::abs(state.render.camera.target.x) < 10.0);
    assert(std::abs(state.render.camera.target.y) < 10.0);
}

static void test_demo_reset_returns_to_preset_pose() {
    StudioState state(make_solar_system_project(), "demo-reset.json");
    activate_studio_preset(state, DemoPreset::EarthTerrain);
    state.render.camera.yaw = 0.3;
    state.render.camera.pitch = 0.0;
    state.render.camera.zoom = 1.8;
    reset_studio_view(state);
    assert(state.camera_transition);
    assert(std::abs(state.camera_transition->to.yaw + 0.5) < 1e-12);
    assert(std::abs(state.camera_transition->to.pitch - 0.23) < 1e-12);
    assert(std::abs(state.camera_transition->to.zoom - 0.62) < 1e-12);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    activate_studio_preset(state, DemoPreset::SaturnRing);
    reset_studio_view(state);
    assert(std::abs(state.camera_transition->to.pitch - 0.36) < 1e-12);
    assert(std::abs(state.camera_transition->to.zoom - 0.72) < 1e-12);
}

static void test_camera_flight_respects_sustained_frame_time() {
    StudioState state(make_solar_system_project(), "slow-frame.json");
    begin_studio_design_focus(state, 3);
    advance_studio(state, 0.5);
    assert(state.camera_transition);
    assert(std::abs(state.camera_transition->elapsed - 0.5) < 1e-12);
    advance_studio(state, 0.5);
    assert(!state.camera_transition && state.design_view);
}

static void test_switching_planet_closeups_keeps_bodies_visible() {
    StudioState state(make_solar_system_project(), "cross-planet-flight.json");
    state.render.width = 160;
    state.render.height = 120;
    begin_studio_design_focus(state, 3);
    for (int step = 0; step < 20 && state.camera_transition; ++step) advance_studio(state, 0.1);
    begin_studio_design_focus(state, 6);
    advance_studio(state, 0.63);
    const StudioRenderInput midpoint = make_studio_render_input(state);
    const RenderFrame frame = render_studio_frame(state, midpoint);
    assert(std::any_of(frame.body_index.begin(), frame.body_index.end(),
                       [](int body) { return body >= 0; }));
}

static void test_demo_sidebar_switch_uses_overview_waypoint() {
    StudioState state(make_solar_system_project(), "demo-switch.json");
    state.render.width = 160;
    state.render.height = 120;
    activate_studio_preset(state, DemoPreset::EarthTerrain);
    select_studio_body(state, 6);
    assert(state.demos.active() == DemoPreset::None);
    assert(state.selected == 6 && state.camera_transition);
    advance_studio(state, 0.63);
    const StudioRenderInput midpoint = make_studio_render_input(state);
    const RenderFrame frame = render_studio_frame(state, midpoint);
    assert(std::any_of(frame.body_index.begin(), frame.body_index.end(),
                       [](int body) { return body >= 0; }));
}

static void test_collision_scene_selection_keeps_demo_running() {
    StudioState state(make_solar_system_project(), "collision-scene-click.json");
    activate_studio_preset(state, DemoPreset::Collision);
    assert(state.selected == 3 && state.demos.playing());

    select_studio_scene_body(state, 3);
    assert(state.demos.active() == DemoPreset::Collision);
    assert(state.demos.playing() && state.selected == 3);
    const double before = make_studio_render_input(state).snapshot.time;
    advance_studio(state, 0.1);
    assert(make_studio_render_input(state).snapshot.time > before);

    select_studio_scene_body(state, 4);
    assert(state.demos.active() == DemoPreset::Collision);
    assert(state.demos.playing() && state.selected == 4);

    exit_studio_preset(state);
    select_studio_scene_body(state, 3);
    assert(state.camera_transition && state.selected == 3);
}

static void test_pixel_regions_stay_in_matching_panels() {
    RenderFrame frame;
    frame.image.width = 64;
    frame.image.height = 48;
    frame.panel_count = 2;
    for (int cells : {9, 17, 25}) {
        for (int y : {0, 24, 47}) {
            for (int x : {0, 16, 31}) {
                const auto left = studio_pixel_region(frame, x, y, cells);
                const auto right = studio_pixel_region(frame, x + 32, y, cells);
                assert(left.x == right.x && left.y == right.y && left.size == right.size);
                assert(left.size == cells);
                assert(left.x >= 0 && left.x + cells <= 32);
                assert(left.y >= 0 && left.y + cells <= 48);
                assert(x >= left.x && x < left.x + cells);
                assert(y >= left.y && y < left.y + cells);
            }
        }
    }
    assert(studio_pixel_region(frame, -1, 0, 9).size == 0);
    assert(studio_pixel_region(frame, 64, 0, 9).size == 0);
    assert(studio_pixel_region(frame, 0, 48, 9).size == 0);
    frame.image.width = 8;
    frame.image.height = 3;
    assert(studio_pixel_region(frame, 7, 2, 25).size == 3);
    frame.panel_count = 1;
    frame.image.width = 64;
    frame.image.height = 48;
    const auto single = studio_pixel_region(frame, 63, 47, 25);
    assert(single.x == 39 && single.y == 23 && single.size == 25);
}

static void test_comparison_stats_and_preset_switch() {
    StudioState state(make_solar_system_project(), "pixel-compare.json");
    state.render.width = 128;
    state.render.height = 96;
    activate_studio_preset(state, DemoPreset::EarthTerrain);
    state.compare_modes = state.inspect_pixel = true;
    state.compare_left = SamplingMode::SSAA4;
    state.compare_right = SamplingMode::MSAA4;
    state.selected_pixel_x = 96;
    state.selected_pixel_y = 48;
    const auto input = make_studio_render_input(state);
    const auto frame = render_studio_frame(state, input);
    assert(frame.panel_count == 2);
    assert(frame.image.width == 128 && frame.image.height == 96);
    assert(frame.panel_sampling[0] == SamplingMode::SSAA4);
    assert(frame.panel_sampling[1] == SamplingMode::MSAA4);
    assert(frame.trace && frame.trace->x == 96 && frame.trace->y == 48);
    for (double time : frame.panel_render_ms) assert(std::isfinite(time) && time > 0.0);
    assert(frame.render_ms == frame.panel_render_ms[0] + frame.panel_render_ms[1]);
    assert(frame.panel_shade_evaluations[0] > frame.panel_shade_evaluations[1]);
    RenderConfig single_config = input.config;
    single_config.sampling = SamplingMode::MSAA4;
    single_config.trace_x = 32;
    const auto single = render_scene(input.project, input.snapshot, single_config);
    assert(single.panel_count == 1 && single.panel_sampling[0] == SamplingMode::MSAA4);
    assert(single.panel_render_ms[0] == single.render_ms && single.render_ms > 0.0);
    for (int y = 0; y < 96; ++y) {
        for (int x = 0; x < 64; ++x) {
            for (int c = 0; c < 3; ++c) {
                assert(single.image.rgb[(y * 64 + x) * 3 + c] ==
                       frame.image.rgb[(y * 128 + x + 64) * 3 + c]);
            }
        }
    }
    activate_studio_preset(state, DemoPreset::SaturnRing);
    assert(state.compare_modes && state.inspect_pixel && state.select_pixel_tab);
    assert(state.compare_left == SamplingMode::SSAA4 && state.compare_right == SamplingMode::MSAA4);
    assert(state.selected == 6 && state.selected_pixel_x == 32 && state.selected_pixel_y == 48);
    activate_studio_preset(state, DemoPreset::Collision);
    assert(!state.compare_modes && !state.inspect_pixel && state.demos.playing());
}

static void test_baseline_preview_preserves_edits() {
    Project project = make_solar_system_project();
    project.bodies[3].design.relief = 3.5;
    project.bodies[3].design.color_shift = 0.8;
    StudioState state(project, "baseline-preview.json");
    state.render.width = 128;
    state.render.height = 96;
    activate_studio_preset(state, DemoPreset::EarthTerrain);
    const auto custom_input = make_studio_render_input(state);
    const auto custom = render_studio_frame(state, custom_input);
    state.compare_baseline = true;
    const auto baseline_input = make_studio_render_input(state);
    const auto baseline = render_studio_frame(state, baseline_input);
    assert(custom.image.rgb != baseline.image.rgb);
    auto expected_input = custom_input;
    expected_input.project.bodies[3].design = baseline_design(BodyKind::Earth);
    assert(baseline.image.rgb == render_studio_frame(state, expected_input).image.rgb);
    assert(state.session.project().bodies[3].design.relief == 3.5);
    assert(state.session.project().bodies[3].design.color_shift == 0.8);
    state.compare_baseline = false;
    assert(custom.image.rgb == render_studio_frame(state, make_studio_render_input(state)).image.rgb);
}

static void test_auto_measurement_reuses_frames_and_invalidates() {
    StudioState state(make_solar_system_project(), "auto-measurement.json");
    state.render.width = 64;
    state.render.height = 48;
    activate_studio_preset(state, DemoPreset::EarthTerrain);
    state.compare_modes = state.inspect_pixel = true;
    state.compare_left = SamplingMode::SSAA4;
    state.compare_right = SamplingMode::MSAA4;
    state.selected_pixel_x = 16;
    state.selected_pixel_y = 24;
    double now = 0.0;
    auto tick = [&](double seconds = 0.1) -> const RenderFrame& {
        now += seconds;
        return update_studio_frame(state, make_studio_render_input(state), now);
    };
    auto finish = [&] {
        assert(state.measurement.waiting);
        tick(StudioRenderMeasurement::settle_seconds + 0.01);
        assert(state.measurement.running && state.measurement.completed_runs == 1);
        assert(!state.frame_cache.frame->trace || !state.frame_cache.frame->trace->process_recorded);
        for (int i = 1; i < 14; ++i) {
            tick();
            assert(state.measurement.ready == (i == 13));
            assert(!state.frame_cache.frame->trace || !state.frame_cache.frame->trace->process_recorded);
            assert(state.measurement.samples[0].size() ==
                   static_cast<std::size_t>(std::max(0, i - 2)));
        }
    };
    tick();
    assert(state.measurement.waiting && !state.measurement.ready);
    const auto first_count = state.frame_cache.render_count;
    tick(0.1);
    assert(state.measurement.waiting && state.frame_cache.render_count == first_count);
    finish();
    // Exactly one displayed render pair for each warm-up/sample; no duplicate pass.
    assert(state.frame_cache.render_count == first_count + 14);
    assert(!state.measurement.running);
    for (int side = 0; side < 2; ++side) {
        auto values = state.measurement.samples[side];
        assert(values.size() == 11);
        for (double value : values) assert(std::isfinite(value) && value > 0);
        std::sort(values.begin(), values.end());
        assert(state.measurement.median_ms[side] == values[5]);
    }
    const auto reference = render_studio_frame(state, make_studio_render_input(state));
    assert(state.frame_cache.frame->image.rgb == reference.image.rgb);
    assert(state.frame_cache.frame->body_index == reference.body_index);
    const auto medians = state.measurement.median_ms;
    tick(); // Restore process history once, outside the completed timing series.
    assert(state.frame_cache.frame->trace->process_recorded);
    assert(state.frame_cache.render_count == first_count + 15);
    const auto ready_count = state.frame_cache.render_count;
    for (int i = 0; i < 5; ++i) tick();
    state.pixel_region_size = 25;
    tick();
    assert(state.frame_cache.render_count == ready_count);
    state.selected_pixel_x = 50; // New diagnostic history keeps the completed median.
    const auto& moved = tick();
    assert(moved.trace && moved.trace->x == 50 && moved.trace->y == 24);
    assert(moved.image.rgb == reference.image.rgb);
    assert(state.frame_cache.render_count == ready_count + 1);
    assert(state.measurement.ready && state.measurement.median_ms == medians);

    auto expect_pending = [&] {
        tick();
        assert(state.measurement.waiting && !state.measurement.running && !state.measurement.ready);
        assert(state.measurement.samples[0].empty() && state.measurement.samples[1].empty());
    };
    state.render.camera.yaw += 0.1;
    expect_pending();
    // Continuous changes postpone measurement instead of mixing camera poses.
    for (int i = 0; i < 4; ++i) {
        state.render.camera.yaw += 0.02;
        expect_pending();
    }
    finish();
    state.compare_left = SamplingMode::Single;
    expect_pending();
    tick(0.3);
    assert(state.measurement.running);
    Project edited = state.session.project();
    edited.bodies[3].design.relief = 2;
    state.session.replace_project(edited);
    expect_pending();
    state.compare_baseline = true;
    expect_pending();
    activate_studio_preset(state, DemoPreset::SaturnRing);
    expect_pending();
    state.render.width = 96;
    expect_pending();
    finish();

    exit_studio_preset(state);
    state.compare_modes = false;
    state.inspect_pixel = true;
    state.session.begin_simulation();
    const double time = state.session.snapshot().time;
    advance_studio(state, 0.1);
    tick();
    assert(state.session.snapshot().time > time && state.session.playing());
    assert(!state.measurement.ready && !state.measurement.running && !state.measurement.waiting);
    state.session.set_playing(false);
    expect_pending();
    finish();
    assert(state.measurement.samples[1].empty());
    state.session.step(0.025);
    expect_pending();
    state.session.set_playing(true);
    advance_studio(state, 0.1);
    tick();
    assert(state.session.playing() && !state.measurement.waiting && !state.measurement.running);

    state.session.return_to_design();
    activate_studio_preset(state, DemoPreset::Collision);
    state.inspect_pixel = true;
    tick();
    assert(state.demos.playing() && !state.measurement.waiting);
    state.demos.set_playing(false);
    expect_pending();
    finish();
    state.demos.reset();
    // Identical snapshots can reuse the result; advancing must invalidate it.
    tick();
    assert(state.measurement.ready);
    state.demos.step(0.025);
    expect_pending();
}

static void test_observed_year_spatial_follow_and_analysis() {
    StudioState state(make_observed_year_project(), "observed-year-test.json");
    state.render.width = 128;
    state.render.height = 96;
    state.session.begin_simulation();
    select_studio_body(state, 1);
    while (state.camera_transition) advance_studio(state, .1);
    state.session.set_playing(false);
    assert(state.simulation_follow_id == "mercury");
    assert(studio_timeline_limit(state) == 365);
    seek_studio_time(state, 273);
    const auto input = make_studio_render_input(state);
    const auto mercury = input.snapshot.bodies[1].position;
    assert(input.config.camera.target.z == mercury.z && mercury.z != 0);
    const auto frame = render_studio_frame(state, input);
    assert(frame.body_index[(96/2)*128+128/2] == 1);
    const auto old_pixels = frame.image.rgb;
    // Translation along z must translate both camera and full scene identically.
    auto translated = input;
    translated.config.camera.target.z += 2;
    for (auto& body : translated.snapshot.bodies) body.position.z += 2;
    for (auto& trail : translated.snapshot.trails) for (auto& p : trail.points) p.z += 2;
    for (auto& trail : translated.snapshot.reference_trails) for (auto& p : trail.points) p.z += 2;
    assert(render_studio_frame(state, translated).image.rgb == old_pixels);
    state.compare_modes = true;
    auto compared = make_studio_render_input(state);
    assert(compared.config.camera.target.z == mercury.z);
    const auto comparison = render_studio_frame(state, compared);
    assert(comparison.panel_count == 2 && comparison.image.width == 128);
    const auto frozen = state.session.snapshot().time;
    advance_studio(state, .1);
    assert(state.session.snapshot().time == frozen);
    state.compare_modes = false;
    return_studio_overview(state);
    while (state.camera_transition) advance_studio(state, .1);
    assert(state.simulation_follow_id.empty());
    return_studio_design(state);
    while (state.camera_transition) advance_studio(state, .1);
    assert(state.session.mode() == SessionMode::Design);
}

static void test_running_loaded_dated_project_preserves_diy_physics() {
    auto project = make_observed_year_project();
    project.bodies[3].mass *= 2;
    project.bodies[3].position.x += .01;
    project.bodies[3].velocity.z += .0001;
    project.bodies[3].design.relief = 1.7;
    std::ostringstream saved;
    write_project(saved, project);
    std::istringstream input(saved.str());
    StudioState state(read_project(input), "dated-diy.json");
    const auto original = project_document(state.session.project());
    for (int run = 0; run < 2; ++run) {
        start_studio_simulation_mode(state, StudioSimulationMode::Real2026);
        while (state.camera_transition) advance_studio(state, .1);
        state.session.set_playing(false);
        assert(state.session.mode() == SessionMode::Simulate);
        assert(project_document(state.session.project()) == original);
        const auto earth = state.session.snapshot().bodies[3];
        assert(earth.mass == project.bodies[3].mass);
        assert(earth.position.x == project.bodies[3].position.x);
        assert(earth.velocity.z == project.bodies[3].velocity.z);
        seek_studio_time(state, 20);
        return_studio_design(state);
        while (state.camera_transition) advance_studio(state, .1);
        assert(project_document(state.session.project()) == original);
    }
}

static void test_simulation_modes_keep_theoretical_initial_conditions() {
    auto project = make_solar_system_project();
    project.bodies[3].position = {4.3, .2, .1};
    project.bodies[3].mass *= 1.5;
    project.bodies[3].design.relief = 1.7;
    StudioState state(project, "mode-selection.json");
    const auto original = project_document(project);
    start_studio_simulation_mode(state, StudioSimulationMode::Real2026);
    while (state.camera_transition) advance_studio(state, .1);
    assert(state.session.mode() == SessionMode::Simulate);
    assert(state.session.project().epoch_jd > 0 && state.session.project().duration_days == 365);
    assert(state.session.project().bodies[3].design.relief == 1.7);
    seek_studio_time(state, 20);
    start_studio_simulation_mode(state, StudioSimulationMode::Theoretical);
    while (state.camera_transition) advance_studio(state, .1);
    assert(state.session.project().epoch_jd == 0);
    assert(project_document(state.session.project()) == original);
    assert(state.session.snapshot().time == 0);
    assert(state.session.project().gravitational_constant == 1);
    advance_studio(state, .1);
    const double time = state.session.snapshot().time;
    start_studio_simulation_mode(state, StudioSimulationMode::Theoretical);
    assert(!state.camera_transition && state.session.snapshot().time == time);
    return_studio_design(state);
    while (state.camera_transition) advance_studio(state, .1);
    assert(project_document(state.session.project()) == original);
    // A dated file opened directly has no cached DIY project: use original teaching defaults.
    StudioState dated(make_observed_year_project(), "dated-mode.json");
    start_studio_simulation_mode(dated, StudioSimulationMode::Theoretical);
    while (dated.camera_transition) advance_studio(dated, .1);
    assert(dated.session.project().epoch_jd == 0);
    assert(dated.session.project().bodies[3].position.x == make_solar_system_project().bodies[3].position.x);
}

int main() {
    test_running_loaded_dated_project_preserves_diy_physics();
    test_simulation_modes_keep_theoretical_initial_conditions();
    test_observed_year_spatial_follow_and_analysis();
    test_reference_comparison_clears_when_leaving_design_focus();
    test_design_reference_light_and_brush_feedback();
    test_old_frame_markers_do_not_relabel_a_changed_catalog();
    test_collision_demo_inspector_uses_its_temporary_initial_conditions();
    test_orbit_tab_does_not_paint_the_closeup();
    test_collision_bookmark_click_after_rewind();
    test_replay_pause_and_reset_buttons_use_the_replay_clock();
    test_collision_replay_pauses_selects_survivor_and_keeps_design();
    test_collision_replay_runs_continuously_and_preserves_normal_speed();
    test_inspector_switches_from_pixel_to_simulation_and_back();
    test_analysis_hides_scene_annotations_and_guides_the_tool();
    test_reselecting_closeup_keeps_pixel_analysis();
    test_workflow_guidance_follows_the_active_tool();
    test_markers_use_visible_surface_pixels_not_orbit_trails();
    test_history_shortcuts_respect_text_input();
    test_parameter_drag_is_one_undo_step_and_redo_restores_it();
    test_loading_same_catalog_rejects_old_async_view();
    test_stale_async_view_blocks_sculpt_and_pixel_selection();
    test_pixel_tab_never_sculpts();
    test_design_slider_and_reset_keep_valid_body_identity();
    test_pixel_keyboard_then_mouse_details();
    test_compare_playback_button_stays_paused();
    test_pixel_regions_stay_in_matching_panels();
    test_comparison_stats_and_preset_switch();
    test_baseline_preview_preserves_edits();
    test_auto_measurement_reuses_frames_and_invalidates();
    test_design_camera_flight();
    test_simulation_waits_for_return_flight();
    test_focus_and_reset_animate_without_losing_playback();
    test_simulation_closeup_tracks_live_state_and_keeps_scene();
    test_solar_size_guard_preserves_physics_and_restores_exaggeration();
    test_roughness_edits_survive_undo_and_redo();
    test_inner_overview_frames_live_inner_planets_without_changing_scene();
    test_inner_overview_returns_to_initial_sun_without_camera_jump();
    test_size_guard_keeps_demo_departure_framing();
    test_overview_controls_and_size_guard_invalidate_old_frames();
    test_simulation_closeup_mouse_controls();
    test_simulation_follow_scroll_preserves_direction_at_view_scales();
    test_simulation_follow_exits_when_target_merges();
    test_collision_demo_exits_to_saved_camera_before_simulation();
    test_demo_return_keeps_departing_planet_in_view();
    test_demo_reset_returns_to_preset_pose();
    test_camera_flight_respects_sustained_frame_time();
    test_switching_planet_closeups_keeps_bodies_visible();
    test_demo_sidebar_switch_uses_overview_waypoint();
    test_collision_scene_selection_keeps_demo_running();
    Project project = make_solar_system_project();
    project.bodies[3].design.relief = 2.25;
    StudioState state(project, "test-solar-system.json");
    state.selected = 5;
    state.render.camera.yaw = 0.42;
    state.render.camera.zoom = 0.7;
    const RenderCamera original_camera = state.render.camera;

    activate_studio_preset(state, DemoPreset::EarthTerrain);
    assert(state.demos.active() == DemoPreset::EarthTerrain);
    assert(state.selected == 3 && state.design_view);
    assert(state.session.project().bodies[3].design.relief == 2.25);
    const StudioRenderInput earth = make_studio_render_input(state);
    assert(earth.snapshot.bodies.size() == 1 && earth.snapshot.bodies[0].id == "earth");
    assert(earth.project.bodies[3].design.relief == 2.25);

    activate_studio_preset(state, DemoPreset::SaturnRing);
    assert(state.selected == 6 && state.design_view);
    assert(std::abs(state.render.camera.pitch) > 0.3);
    const StudioRenderInput saturn = make_studio_render_input(state);
    assert(saturn.snapshot.bodies.size() == 1 && saturn.snapshot.bodies[0].id == "saturn");

    activate_studio_preset(state, DemoPreset::Collision);
    assert(state.selected == 3 && !state.design_view);
    assert(state.demos.playing());
    const StudioRenderInput before = make_studio_render_input(state);
    assert(before.snapshot.bodies.size() == 2);
    assert(before.project.bodies[3].position.x == 4.0);
    assert(state.session.project().bodies[3].position.x != 4.0);
    state.compare_modes = true;
    assert(state.demos.playing());
    const double paused = make_studio_render_input(state).snapshot.time;
    advance_studio(state, 0.1);
    assert(make_studio_render_input(state).snapshot.time == paused);
    state.compare_modes = false;
    for (int i = 0; i < 60 && make_studio_render_input(state).snapshot.events.empty(); ++i) {
        advance_studio(state, 0.05);
    }
    const StudioRenderInput impact = make_studio_render_input(state);
    assert(impact.snapshot.events.size() == 1);
    assert(impact.snapshot.bodies.size() == 1);

    exit_studio_preset(state);
    assert(state.demos.active() == DemoPreset::None);
    assert(state.selected == 5 && !state.design_view);
    assert(state.render.camera.yaw == original_camera.yaw);
    assert(state.render.camera.zoom == original_camera.zoom);
    assert(state.session.project().bodies[3].design.relief == 2.25);

    state.design_view = true;
    state.selected = 4;
    activate_studio_preset(state, DemoPreset::EarthTerrain);
    exit_studio_preset(state);
    assert(state.design_view && state.selected == 4);

    Project changed = make_solar_system_project();
    changed.bodies[3].velocity.x += 0.2;
    StudioState experiment(changed, "orbit-lab.json");
    experiment.selected = 3;
    experiment.session.begin_simulation();
    experiment.session.seek(0.5);
    experiment.show_reference_orbit = true;
    const StudioRenderInput earth_orbit = make_studio_render_input(experiment);
    assert(earth_orbit.snapshot.reference_trails.size() == 1);
    assert(earth_orbit.snapshot.reference_trails[0].id == "earth");
    assert(earth_orbit.snapshot.trails.size() == 1);
    assert(earth_orbit.snapshot.trails[0].id == "earth");
    experiment.selected = 4;
    const StudioRenderInput mars_orbit = make_studio_render_input(experiment);
    assert(mars_orbit.snapshot.reference_trails[0].id == "mars");
    experiment.show_reference_orbit = false;
    assert(make_studio_render_input(experiment).snapshot.reference_trails.empty());

    StudioState timeline(make_solar_system_project(), "timeline.json");
    timeline.session.begin_simulation();
    seek_studio_time(timeline, 90.0);
    assert(std::abs(timeline.session.snapshot().time - 90.0) < 1e-12);
    assert(studio_timeline_limit(timeline) >= 90.0);
    seek_studio_time(timeline, 45.0);
    assert(std::abs(timeline.session.snapshot().time - 45.0) < 1e-12);
    assert(studio_timeline_limit(timeline) >= 90.0);
    seek_studio_time(timeline, -1.0);
    assert(timeline.session.snapshot().time == 0.0);
    seek_studio_time(timeline, 100001.0);
    assert(timeline.session.snapshot().time == 0.0);
    seek_studio_time(timeline, 90.0);
    assert(std::abs(timeline.session.snapshot().time - 90.0) < 1e-12);
    seek_studio_time(timeline, std::numeric_limits<double>::quiet_NaN());
    assert(std::abs(timeline.session.snapshot().time - 90.0) < 1e-12);
    assert(std::abs(timeline.session.reference_snapshot()->time - 90.0) < 1e-12);
}
