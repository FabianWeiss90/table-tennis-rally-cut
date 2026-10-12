// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/gui_context.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <stdexcept>
#include <string>

namespace ttrally::gui {

GuiContext::GuiContext(const std::string& title, int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(std::string("cannot initialise SDL: ") + SDL_GetError());
    }
    window_ = SDL_CreateWindow(title.c_str(), width, height,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    renderer_ = window_ != nullptr ? SDL_CreateRenderer(window_, nullptr) : nullptr;
    if (renderer_ == nullptr) {
        const std::string error = SDL_GetError();
        release();
        throw std::runtime_error("cannot open the window: " + error);
    }
    SDL_SetRenderVSync(renderer_, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // the layout is fixed, nothing to remember
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);
}

GuiContext::~GuiContext() {
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    release();
}

bool GuiContext::poll_events() const {
    SDL_Event event;
    bool close_requested = false;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            close_requested = true;
        }
    }
    return close_requested;
}

void GuiContext::begin_frame() const {
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void GuiContext::end_frame(const std::filesystem::path& screenshot) const {
    ImGui::Render();
    SDL_SetRenderDrawColor(renderer_, 20, 21, 24, 255);
    SDL_RenderClear(renderer_);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    if (!screenshot.empty()) {
        save_screenshot(screenshot);
    }
    SDL_RenderPresent(renderer_);
}

void GuiContext::save_screenshot(const std::filesystem::path& path) const {
    SDL_Surface* surface = SDL_RenderReadPixels(renderer_, nullptr);
    const bool saved = surface != nullptr && SDL_SaveBMP(surface, path.string().c_str());
    SDL_DestroySurface(surface);
    if (!saved) {
        throw std::runtime_error(std::string("cannot save the screenshot: ") + SDL_GetError());
    }
}

void GuiContext::release() {
    if (renderer_ != nullptr) {
        SDL_DestroyRenderer(renderer_);
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
    }
    SDL_Quit();
}

} // namespace ttrally::gui
