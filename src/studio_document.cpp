#include "planet_sandbox/studio_ui.h"
#include "planet_sandbox/solar_system.h"
#include <algorithm>
#include <cstdio>
#include <fstream>

namespace planet_sandbox {
namespace {

bool can_edit(const StudioState& state) {
    return state.session.mode() == SessionMode::Design && !state.camera_transition &&
        state.demos.active() != DemoPreset::Collision && !state.compare_baseline;
}

void edited(StudioState& state) {
    state.document_revision = state.session.project_revision();
    state.last_document_edit = state.document_clock;
}

void sync_name(StudioState& state, const std::string& body_id = {}) {
    const auto& bodies = state.session.project().bodies;
    auto found = std::find_if(bodies.begin(), bodies.end(),
        [&](const BodySpec& body) { return body.id == body_id; });
    if (found != bodies.end()) state.selected = static_cast<int>(found - bodies.begin());
    state.selected = std::clamp(state.selected, 0, static_cast<int>(bodies.size()) - 1);
    std::snprintf(state.name.data(), state.name.size(), "%s", bodies[state.selected].name.c_str());
}

void check_draft(StudioState& state) {
    state.recovery_offer.reset();
    state.recovery_error.clear();
    state.recovery_blocked = false;
    state.source_unavailable = false;
    try {
        state.recovery_offer = read_recovery(state.document_path);
        if (state.recovery_offer) {
            try {
                std::ifstream source(state.document_path);
                if (!source) throw std::runtime_error("Saved source is missing");
                read_project(source);
            } catch (const std::exception&) { state.source_unavailable = true; }
        }
        if (state.recovery_offer && !state.source_unavailable &&
            state.recovery_offer->document == state.history.current_document()) {
            state.recovery_writer->clear(state.document_path);
            state.recovery_offer.reset();
        }
    } catch (const std::exception& error) {
        state.recovery_blocked = true;
        state.recovery_error = std::string("Could not read recovery draft: ") + error.what();
    }
}

void checkpoint(StudioState& state) {
    state.recovery_writer->submit(state.document_path, state.history.saved_document(),
                                 state.history.current_document());
    state.checkpoint_document = state.history.current_document();
    state.last_checkpoint = state.document_clock;
}

bool travel_history(StudioState& state, bool redo) {
    if (!can_edit(state)) return false;
    try {
        auto next = state.history;
        auto edit = redo ? next.redo() : next.undo();
        if (!edit) return false;
        state.session.replace_project(std::move(edit->project));
        state.history = std::move(next);
        state.sculpt_stroke.end();
        state.sculpt_history.clear();
        ++state.scene_generation;
        sync_name(state, edit->body_id);
        edited(state);
        state.notice = std::string(redo ? "Redo: " : "Undo: ") + edit->label;
        return true;
    } catch (const std::exception& error) { state.notice = error.what(); return false; }
}

} // namespace

Project open_studio_project(const std::filesystem::path& path, bool create_if_missing) {
    try {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("Cannot open project path");
        return read_project(input);
    } catch (...) {
        const auto source_error = std::current_exception();
        try {
            if (const auto draft = read_recovery(path)) return project_from_document(draft->base_document);
        } catch (const std::exception&) {
            // Preserve both files and report the original source failure if neither is usable.
        }
        if (create_if_missing && !std::filesystem::exists(path)) return make_solar_system_project();
        std::rethrow_exception(source_error);
    }
}

bool apply_studio_edit(StudioState& state, Project project, const std::string& label,
                       const std::string& body_id, const std::string& group) {
    if (!can_edit(state)) return false;
    try {
        // Callers can name the edited body through the project being replaced.
        const std::string owned_label = label, owned_body = body_id, owned_group = group;
        validate_project(project);
        state.session.replace_project(std::move(project));
        const bool changed = state.history.record(state.session.project(), owned_label, owned_body, owned_group);
        edited(state);
        sync_name(state);
        state.notice.clear();
        return changed;
    } catch (const std::exception& error) { state.notice = error.what(); return false; }
}

bool undo_studio_edit(StudioState& state) { return travel_history(state, false); }
bool redo_studio_edit(StudioState& state) { return travel_history(state, true); }

bool save_studio_document(StudioState& state) {
    try {
        const auto target = document_identity(std::filesystem::u8path(state.path.data()));
        const bool preserve_unreadable = state.recovery_blocked;
        const auto unreadable_error = state.recovery_error;
        if (state.recovery_writer) state.recovery_writer->flush();
        const auto document = project_document(state.session.project());
        atomic_write_document(target, document);
        state.history.record(state.session.project(), "Edit project");
        state.history.mark_saved();
        state.document_revision = state.session.project_revision();
        const auto previous = state.document_path;
        state.document_path = target;
        state.recovery_offer.reset();
        state.checkpoint_document.clear();
        state.protected_document.clear();
        state.recovery_blocked = preserve_unreadable && previous == target;
        state.recovery_error = state.recovery_blocked ? unreadable_error : std::string{};
        state.source_unavailable = false;
        state.notice = "Saved";
        if (state.recovery_writer) {
            try {
                if (!state.recovery_blocked) state.recovery_writer->clear(target);
                if (previous != target && !preserve_unreadable) state.recovery_writer->clear(previous);
            } catch (const std::exception& error) {
                state.recovery_error = std::string("Saved; draft cleanup failed: ") + error.what();
            }
        }
        return true;
    } catch (const std::exception& error) { state.notice = error.what(); return false; }
}

bool load_studio_document(StudioState& state, const std::filesystem::path& path, bool discard_edits) {
    if (state.session.mode() != SessionMode::Design || state.camera_transition) return false;
    if (state.history.dirty() && !discard_edits) { state.pending_load = path; return false; }
    try {
        const auto target = document_identity(path);
        std::ifstream input(target);
        if (!input) throw std::runtime_error("Cannot open project path");
        auto project = read_project(input);
        if (state.recovery_writer) {
            if (state.history.dirty() && state.document_path != target &&
                !state.recovery_blocked && !state.recovery_offer) checkpoint(state);
            state.recovery_writer->flush();
            if (discard_edits && state.document_path == target) state.recovery_writer->clear(target);
        }
        if (state.demos.active() != DemoPreset::None) exit_studio_preset(state);
        state.session.replace_project(std::move(project));
        state.history.reset_saved(state.session.project());
        state.theoretical_project.reset();
        state.compare_design=false;state.design_reference.reset();state.design_reference_id.clear();
        state.document_revision = state.session.project_revision();
        state.document_path = target;
        state.checkpoint_document.clear();
        state.protected_document.clear();
        state.pending_load.reset();
        ++state.scene_generation;
        state.camera_transition.reset();
        state.design_view = state.compare_baseline = false;
        state.overview = StudioOverview::FullSystem;
        state.simulation_follow_id.clear();
        state.collision_replay.reset();
        state.render.camera = {};
        if (state.session.project().solar_system) state.render.camera.zoom = 0.55;
        state.sculpt_history.clear();
        state.sculpt_stroke.end();
        state.selected = 0;
        sync_name(state);
        if (state.recovery_writer) check_draft(state);
        state.notice = "Loaded";
        return true;
    } catch (const std::exception& error) { state.notice = error.what(); return false; }
}

void initialize_studio_document(StudioState& state) {
    if (state.recovery_writer) return;
    state.document_path = document_identity(std::filesystem::u8path(state.path.data()));
    state.recovery_writer = std::make_unique<RecoveryWriter>();
    check_draft(state);
}

void update_studio_document(StudioState& state, double elapsed_seconds) {
    state.document_clock += std::max(0.0, elapsed_seconds);
    if (state.document_revision != state.session.project_revision()) {
        state.history.record(state.session.project(), "Edit project");
        edited(state);
    }
    if (!state.recovery_writer || state.recovery_offer || state.recovery_blocked) return;
    try {
        const bool busy = state.recovery_writer->busy();
        const auto failure = state.recovery_writer->error();
        if (!busy && !failure.empty()) {
            state.recovery_error = std::string("Recovery draft failed: ") + failure;
        } else if (!busy && failure.empty()) {
            state.protected_document = state.checkpoint_document;
            if (state.recovery_error.rfind("Recovery draft failed: ", 0) == 0) state.recovery_error.clear();
        }
        if (!state.history.dirty() && (!state.checkpoint_document.empty() || !state.protected_document.empty())) {
            state.recovery_writer->clear(state.document_path);
            state.checkpoint_document.clear();
            state.protected_document.clear();
        } else if (state.history.dirty() && !busy &&
                   ((!failure.empty() && state.document_clock - state.last_checkpoint >= 5.0) ||
                    (failure.empty() && state.history.current_document() != state.protected_document &&
                     state.document_clock - state.last_checkpoint >= 1.0 &&
                     (state.document_clock - state.last_document_edit >= 1.0 ||
                      state.document_clock - state.last_checkpoint >= 5.0)))) {
            checkpoint(state);
        }
    } catch (const std::exception& error) { state.recovery_error = error.what(); }
}

void shutdown_studio_document(StudioState& state) {
    if (!state.recovery_writer) return;
    update_studio_document(state, 0.0);
    try {
        if (state.history.dirty() && !state.recovery_offer && !state.recovery_blocked) checkpoint(state);
        state.recovery_writer->flush();
        if (!state.recovery_writer->error().empty()) state.recovery_error = state.recovery_writer->error();
    } catch (const std::exception& error) { state.recovery_error = error.what(); }
}

bool restore_studio_draft(StudioState& state) {
    if (!state.recovery_offer || !can_edit(state)) return false;
    const auto document = state.recovery_offer->document;
    if (state.source_unavailable) state.history.invalidate_saved();
    auto project = project_from_document(state.recovery_offer->document);
    if (!apply_studio_edit(state, std::move(project), "Restore draft") &&
        state.history.current_document() != document) return false;
    state.checkpoint_document = state.protected_document = document;
    state.recovery_offer.reset();
    state.sculpt_history.clear();
    state.sculpt_stroke.end();
    state.history.finish();
    state.design_view = false;
    ++state.scene_generation;
    state.notice = "Draft restored - save to keep these changes";
    return true;
}

void discard_studio_draft(StudioState& state) {
    if (!state.recovery_writer) return;
    try {
        state.recovery_writer->clear(state.document_path);
        state.recovery_offer.reset();
        state.recovery_error.clear();
        state.recovery_blocked = false;
        state.checkpoint_document.clear();
        state.protected_document.clear();
        if (state.source_unavailable) state.history.invalidate_saved();
    } catch (const std::exception& error) { state.recovery_error = error.what(); }
}

} // namespace planet_sandbox
