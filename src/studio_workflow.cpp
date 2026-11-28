#include "planet_sandbox/studio_ui.h"
#include "planet_sandbox/impact_visual.h"
#include "planet_sandbox/planet_visual.h"
#include "planet_sandbox/solar_system.h"

#include <algorithm>
#include <cmath>

namespace planet_sandbox {

void capture_studio_design_reference(StudioState& state) {
    if(state.session.mode()!=SessionMode::Design || !state.design_view || state.camera_transition || state.compare_baseline || state.compare_modes) return;
    const auto& body=state.session.project().bodies[state.selected];
    state.design_reference=body.design;state.design_reference_id=body.id;
    ++state.scene_generation;state.notice="Reference captured. Compare it after your edits.";
}

std::vector<std::pair<Vec2,Vec2>> studio_brush_outline(const StudioRenderInput& input,const RenderFrame& frame,
                                                     int selected,int x,int y,double radius,SolarShapeLayer layer) {
    std::vector<std::pair<Vec2,Vec2>> result;
    const int width=frame.image.width,height=frame.image.height;
    if(frame.panel_count!=1 || width!=input.config.width || height!=input.config.height ||
       x<0 || y<0 || x>=width || y>=height || selected<0 || selected>=int(input.project.bodies.size()) ||
       frame.body_index.size()!=std::size_t(width)*height || frame.surface_uv.size()!=frame.body_index.size() ||
       !std::isfinite(radius) || radius<=0 || radius>3.14159) return result;
    const auto pick=std::size_t(y)*width+x;
    if(frame.body_index[pick]!=selected || frame.surface_uv[pick].x<0) return result;
    const auto& spec=input.project.bodies[selected];
    const auto found=std::find_if(input.snapshot.bodies.begin(),input.snapshot.bodies.end(),[&](const auto& b){return b.id==spec.id;});
    if(found==input.snapshot.bodies.end()) return result;
    constexpr double pi=3.14159265358979323846,tangent=.41421356237309503;
    using V=VisualVector;
    const auto dot=[](V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;};
    const auto unit=[&](V a){const auto length=std::sqrt(dot(a,a));return V{a.x/length,a.y/length,a.z/length};};
    const auto uv=frame.surface_uv[pick];
    const double theta=pi*uv.y,phi=2*pi*uv.x;
    const V center{std::sin(theta)*std::cos(phi),std::cos(theta),std::sin(theta)*std::sin(phi)};
    const V along{-std::sin(phi),0,std::cos(phi)};
    const V across{center.y*along.z-center.z*along.y,center.z*along.x-center.x*along.z,center.x*along.y-center.y*along.x};
    const auto& camera=input.config.camera;
    const V direction{std::sin(camera.yaw)*std::cos(camera.pitch),std::sin(camera.pitch),std::cos(camera.yaw)*std::cos(camera.pitch)};
    const V right{std::cos(camera.yaw),0,-std::sin(camera.yaw)};
    const V up{-std::sin(camera.yaw)*std::sin(camera.pitch),std::cos(camera.pitch),-std::cos(camera.yaw)*std::sin(camera.pitch)};
    const double distance=input.project.view_extent*1.35/tangent*camera.zoom,image_scale=height/(2*tangent);
    const V eye{camera.target.x+direction.x*distance,camera.target.y+direction.y*distance,camera.target.z+direction.z*distance};
    const auto rotation=body_surface_rotation(spec.kind,spec.appearance.seed,spec.design.axis_tilt_offset,input.snapshot.time);
    const double body_radius=found->radius*input.project.display_scale*(input.project.solar_system?spec.display_radius/spec.radius:1);
    std::array<std::optional<Vec2>,97> points;
    for(int i=0;i<=96;++i) {
        const double angle=2*pi*i/96,ca=std::cos(angle)*std::sin(radius),sa=std::sin(angle)*std::sin(radius);
        const V local{center.x*std::cos(radius)+along.x*ca+across.x*sa,
                      center.y*std::cos(radius)+along.y*ca+across.y*sa,
                      center.z*std::cos(radius)+along.z*ca+across.z*sa};
        const double u=std::atan2(local.z,local.x)/(2*pi),v=std::acos(std::clamp(local.y,-1.0,1.0))/pi;
        const double relief=solar_shape_multiplier(spec.kind,u,v,spec.design,layer);
        const V normal=rotation.to_world(local);
        const V world{found->position.x+normal.x*body_radius*relief,found->position.y+normal.y*body_radius*relief,found->position.z+normal.z*body_radius*relief};
        const V toward{eye.x-world.x,eye.y-world.y,eye.z-world.z};
        const double depth=dot(toward,direction);
        if(depth<=.05 || dot(normal,toward)<=0) continue;
        const V relative{world.x-eye.x,world.y-eye.y,world.z-eye.z};
        const Vec2 screen{width*.5+dot(relative,right)*image_scale/depth,height*.5-dot(relative,up)*image_scale/depth};
        const int px=static_cast<int>(screen.x),py=static_cast<int>(screen.y);
        if(px<0 || py<0 || px>=width || py>=height) continue;
        const auto p=std::size_t(py)*width+px;
        if(frame.body_index[p]!=selected || frame.surface_uv[p].x<0) continue;
        const auto visible=frame.surface_uv[p];
        const V seen{std::sin(pi*visible.y)*std::cos(2*pi*visible.x),std::cos(pi*visible.y),std::sin(pi*visible.y)*std::sin(2*pi*visible.x)};
        if(dot(unit(local),seen)<std::cos(.14)) continue;
        points[i]=screen;
    }
    for(int i=1;i<=96;++i) if(points[i-1] && points[i]) result.push_back({*points[i-1],*points[i]});
    return result;
}

StudioWorkflow studio_workflow(const StudioState& state) {
    const auto& name = state.session.project().bodies[state.selected].name;
    StudioWorkflow result;
    if (state.camera_transition) {
        result.stage = StudioWorkflowStage::Transition;
        const auto destination = state.camera_transition->destination;
        result.title = destination == StudioCameraDestination::Design ? "Opening " + name :
                       destination == StudioCameraDestination::Simulation ? "Starting simulation" :
                       destination == StudioCameraDestination::DesignOverview ? "Returning to design" :
                       "Moving camera";
        result.hint = "Camera in motion. Editing resumes when the view arrives.";
        return result;
    }
    if (state.demos.active() == DemoPreset::Collision) {
        result.stage = StudioWorkflowStage::Collision;
        result.title = "Collision demo";
        result.hint = "Watch Earth and Mars collide. Exit demo to return to your solar system.";
    } else if (state.session.mode() == SessionMode::Simulate) {
        result.stage = StudioWorkflowStage::Simulation;
        result.title = "Simulating / " + name;
        result.hint = state.simulation_follow_id.empty()
            ? "Select a planet for a tracking close-up; select a collision to replay. Return to Design to edit."
            : "Following selected planet. Right drag: rotate. Scroll: zoom. Back to system: overview.";
    } else if (state.design_view) {
        result.stage = StudioWorkflowStage::Design;
        result.title = "Designing / " + name;
        result.hint = "Left drag: sculpt with the brush. Right drag: rotate. Scroll: zoom. Undo: Ctrl/Cmd+Z.";
    } else {
        result.title = (state.session.project().solar_system ? "Solar system / " : "Body / ") + name;
        result.hint = "Click a planet or select its name to open its design. Drag empty space to rotate; scroll to zoom.";
        if (!state.session.project().solar_system) {
            result.hint = "Select a body to edit its appearance or initial orbit. Drag to rotate; scroll to zoom.";
        }
    }
    if (state.compare_modes) {
        result.hint = "Click either image to inspect the same pixel in both. Sculpting is paused during comparison.";
    } else if (state.inspect_pixel) {
        result.hint = "Click to inspect a pixel. Arrow keys: move sample. Right drag: rotate. ";
        result.hint += result.stage == StudioWorkflowStage::Design
            ? "Return to the Design tab and turn off Inspect to sculpt."
            : "Return to Design to edit your planets.";
    } else if (state.pixel_tab_active) {
        result.hint = "The Pixel tool is open. Return to the Design tab to use the brush.";
    } else if (state.compare_baseline && result.stage == StudioWorkflowStage::Design) {
        result.hint = "Viewing the baseline. Uncheck Show baseline to continue editing your design.";
    } else if (!state.design_tab_active && result.stage == StudioWorkflowStage::Design) {
        result.hint = "Adjust the initial orbit and physics here. Return to the Design tab to use the brush.";
    }
    return result;
}

std::vector<StudioBodyMarker> studio_body_markers(const RenderFrame& frame, int body_count) {
    if (body_count <= 0 || frame.image.width <= 0 || frame.image.height <= 0) return {};
    const auto size = static_cast<std::size_t>(frame.image.width) * frame.image.height;
    if (frame.body_index.size() != size || frame.surface_uv.size() != size) return {};
    std::vector<StudioBodyMarker> bounds(static_cast<std::size_t>(body_count));
    for (auto& bound : bounds) {
        bound.min_x = frame.image.width;
        bound.min_y = frame.image.height;
    }
    for (std::size_t pixel = 0; pixel < size; ++pixel) {
        const int body = frame.body_index[pixel];
        // Trails have no surface UV and must not create or enlarge planet markers.
        if (body < 0 || body >= body_count || frame.surface_uv[pixel].x < 0.0 ||
            frame.surface_uv[pixel].y < 0.0) continue;
        auto& bound = bounds[body];
        const int x = static_cast<int>(pixel % frame.image.width);
        const int y = static_cast<int>(pixel / frame.image.width);
        bound.body_index = body;
        bound.min_x = std::min(bound.min_x, x);
        bound.min_y = std::min(bound.min_y, y);
        bound.max_x = std::max(bound.max_x, x);
        bound.max_y = std::max(bound.max_y, y);
    }
    bounds.erase(std::remove_if(bounds.begin(), bounds.end(),
                               [](const auto& bound) { return bound.body_index < 0; }), bounds.end());
    return bounds;
}

bool studio_scene_annotations_visible(const StudioState& state) {
    return !state.compare_modes && !state.inspect_pixel && !state.pixel_tab_active &&
           !state.camera_transition;
}

bool replay_studio_collision(StudioState& state, const CollisionEvent& event) {
    if (state.session.mode() != SessionMode::Simulate || state.camera_transition ||
        state.demos.active() == DemoPreset::Collision || state.compare_modes) return false;
    const CollisionEvent bookmark = event;
    const auto& recorded = state.session.observed_events();
    if (std::none_of(recorded.begin(), recorded.end(), [&](const auto& known) {
            return known.time == bookmark.time && known.survivor == bookmark.survivor &&
                   known.absorbed == bookmark.absorbed;
        })) return false;
    seek_studio_time(state, std::max(0.0, bookmark.time - .5));
    const auto snapshot = state.session.snapshot();
    auto survivor = bookmark.survivor;
    // A second impact in the same replay interval can merge the first survivor again.
    for (const auto& impact : snapshot.events) {
        if (impact.absorbed == survivor) survivor = impact.survivor;
    }
    const auto& bodies = state.session.project().bodies;
    const auto body = std::find_if(bodies.begin(), bodies.end(),
                                   [&](const auto& candidate) { return candidate.id == survivor; });
    if (body != bodies.end()) select_studio_scene_body(state, static_cast<int>(body - bodies.begin()));
    state.simulation_follow_id.clear();
    if(state.camera_transition && bookmark.contact) {
        const auto& c=*bookmark.contact;
        auto& to=state.camera_transition->to;
        const double age=snapshot.time-bookmark.time;
        to.target={c.position.x+c.velocity.x*age,c.position.y+c.velocity.y*age,c.position.z+c.velocity.z*age};
        const auto input=make_studio_render_input(state);
        const double merged=std::cbrt(c.survivor_radius*c.survivor_radius*c.survivor_radius+
                                    c.absorbed_radius*c.absorbed_radius*c.absorbed_radius);
        const double radius=merged*(input.project.solar_system && body!=bodies.end()
            ?body->display_radius/body->radius:1.0);
        const double extent=2.5*radius*input.project.display_scale+.5*c.relative_speed;
        to.zoom=std::max(.01,extent/(1.35*input.project.view_extent));
        state.camera_transition->screen_anchor.reset();
        state.camera_transition->resume_playback=false;
    }
    state.collision_replay=StudioState::CollisionReplay{bookmark,
        std::min(simulation_time_limit(state.session.project()),bookmark.time+impact_visual_lifetime),true};
    state.notice="Collision replay / 0.25x. Visual ejecta do not change the merge physics.";
    return true;
}

void cancel_studio_collision_replay(StudioState& state) {
    if(!state.collision_replay) return;
    state.collision_replay.reset();
    state.session.set_playing(false);
}

bool advance_studio_collision_replay(StudioState& state, double seconds) {
    if(!state.collision_replay) return false;
    if(state.session.mode()!=SessionMode::Simulate) { state.collision_replay.reset();return false; }
    auto& replay=*state.collision_replay;
    if(replay.playing && std::isfinite(seconds) && seconds>0) {
        const double current=state.session.snapshot().time;
        const double target=std::min(replay.end_time,current+std::min(seconds,.1)*.25);
        if(target>current) state.session.step(target-current);
        if(target>=replay.end_time) replay.playing=false;
    }
    if(replay.event.contact) {
        const auto& c=*replay.event.contact;
        const double age=state.session.snapshot().time-replay.event.time;
        state.render.camera.target={c.position.x+c.velocity.x*age,c.position.y+c.velocity.y*age,c.position.z+c.velocity.z*age};
    }
    return true;
}

} // namespace planet_sandbox
