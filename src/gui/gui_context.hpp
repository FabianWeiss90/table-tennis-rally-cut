// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#pragma once

#include <filesystem>
#include <string>

struct SDL_Renderer;
struct SDL_Window;

namespace ttrally::gui {

/// SDL window, renderer and Dear ImGui context; released in reverse order. One per process.
class GuiContext {
  public:
    explicit GuiContext(const std::string& title, int width = 1600, int height = 950);
    ~GuiContext();
    GuiContext(const GuiContext&) = delete;
    GuiContext& operator=(const GuiContext&) = delete;

    [[nodiscard]] SDL_Renderer* renderer() const noexcept { return renderer_; }
    [[nodiscard]] SDL_Window* window() const noexcept { return window_; }

    /// Processes pending events; returns true if the user asked to close the window.
    [[nodiscard]] bool poll_events() const;
    void begin_frame() const;
    /// Renders the frame; with a path, also saves it as a BMP screenshot (for tests).
    void end_frame(const std::filesystem::path& screenshot = {}) const;

  private:
    void save_screenshot(const std::filesystem::path& path) const;
    void release();

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
};

} // namespace ttrally::gui
