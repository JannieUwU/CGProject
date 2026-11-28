#include "planet_sandbox/project.h"
#include "planet_sandbox/renderer.h"
#include "planet_sandbox/simulation.h"
#include "planet_sandbox/solar_system.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace planet_sandbox;

int main(int argc, char** argv) {
    try {
        std::string config;
        std::string output;
        std::string state_path;
        std::string focus_body;
        double time = 0.0;
        int width = 960;
        int height = 720;
        SamplingMode sampling = SamplingMode::Single;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") {
                std::cout << "Usage: planet_sandbox --output frame.ppm [--config project.json] "
                             "[--body earth|mars|...] [--time T] [--width W] [--height H] "
                             "[--sampling single|ssaa4|msaa4] [--samples 1|4] "
                             "[--state state.json]\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::invalid_argument("Missing value for " + option);
            const std::string value = argv[++i];
            if (option == "--config") config = value;
            else if (option == "--output") output = value;
            else if (option == "--state") state_path = value;
            else if (option == "--body") focus_body = value;
            else if (option == "--time") time = std::stod(value);
            else if (option == "--width") width = std::stoi(value);
            else if (option == "--height") height = std::stoi(value);
            else if (option == "--samples") {
                if (value == "1") sampling = SamplingMode::Single;
                else if (value == "4") sampling = SamplingMode::SSAA4;
                else throw std::invalid_argument("--samples must be 1 or 4");
            } else if (option == "--sampling") {
                if (value == "single") sampling = SamplingMode::Single;
                else if (value == "ssaa4") sampling = SamplingMode::SSAA4;
                else if (value == "msaa4") sampling = SamplingMode::MSAA4;
                else throw std::invalid_argument("--sampling must be single, ssaa4, or msaa4");
            }
            else throw std::invalid_argument("Unknown option: " + option);
        }
        if (output.empty()) throw std::invalid_argument("--output is required");
        Project project = make_solar_system_project();
        if (!config.empty()) {
            std::ifstream input(config);
            if (!input) throw std::runtime_error("Could not open project: " + config);
            project = read_project(input);
        }
        Simulation simulation(project);
        simulation.advance_to(time);
        SceneSnapshot snapshot = simulation.snapshot();
        SceneSnapshot render_snapshot = snapshot;
        RenderConfig render_config;
        render_config.width = width;
        render_config.height = height;
        render_config.sampling = sampling;
        if (project.solar_system && focus_body.empty()) render_config.camera.zoom = 0.55;
        if (!focus_body.empty()) {
            const auto spec = std::find_if(project.bodies.begin(), project.bodies.end(),
                                           [&](const BodySpec& body) { return body.id == focus_body; });
            const auto body = std::find_if(snapshot.bodies.begin(), snapshot.bodies.end(),
                                           [&](const BodyState& item) { return item.id == focus_body; });
            if (spec == project.bodies.end() || body == snapshot.bodies.end()) {
                throw std::invalid_argument("Body is not present in this scene: " + focus_body);
            }
            render_config.camera.target = body->position;
            const double framing = spec->kind == BodyKind::Saturn ? 2.0 :
                                   spec->kind == BodyKind::Uranus ? 1.4 : 0.95;
            project.view_extent = (project.solar_system ? spec->display_radius :
                                    body->radius) * project.display_scale * framing;
            render_snapshot.bodies = {*body};
            render_snapshot.trails.clear();
        }
        const Image image = render_scene(project, render_snapshot, render_config).image;
        std::ofstream image_output(output, std::ios::binary);
        if (!image_output) throw std::runtime_error("Could not open output: " + output);
        write_ppm(image_output, image);
        if (!state_path.empty()) {
            nlohmann::json state;
            state["time"] = snapshot.time;
            state["bodies"] = nlohmann::json::array();
            state["events"] = nlohmann::json::array();
            for (const auto& body : snapshot.bodies) {
                state["bodies"].push_back({{"id", body.id}, {"mass", body.mass}, {"radius", body.radius},
                                           {"position", {body.position.x, body.position.y, body.position.z}},
                                           {"velocity", {body.velocity.x, body.velocity.y, body.velocity.z}}});
            }
            for (const auto& event : snapshot.events) {
                state["events"].push_back({{"time", event.time}, {"survivor", event.survivor},
                                            {"absorbed", event.absorbed}});
            }
            std::ofstream state_output(state_path);
            if (!state_output) throw std::runtime_error("Could not open state output: " + state_path);
            state_output << state.dump(2) << '\n';
        }
        std::cout << "Rendered " << snapshot.bodies.size() << " bodies at t=" << snapshot.time
                  << "; collisions=" << snapshot.events.size() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
