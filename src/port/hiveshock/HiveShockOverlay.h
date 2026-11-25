#pragma once

#include <libultraship/libultraship.h>

namespace HiveShock {

// On-stream feed: who sent what, and the timed effects that are running. Takes no input.
class OverlayWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;
    void Draw() override;

  protected:
    void InitElement() override {
    }
    void DrawElement() override {
    }
    void UpdateElement() override {
    }
};

} // namespace HiveShock
