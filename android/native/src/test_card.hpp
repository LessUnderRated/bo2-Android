// test_card.hpp — the game's frame stand-in on the diagnostics screen.
#pragma once

#include <SDL3/SDL.h>

#include "display_plan.hpp"

namespace bo2display {

// Draws into the current render target, which is plan.guest_w x plan.guest_h: a grid, the picture's
// edge and corners in orange, and in blue where a 16:9 picture would end. Shows on the phone whether
// the image fills the panel and what the camera cutout covers.
void draw_test_card(SDL_Renderer* r, const Plan& p);

}  // namespace bo2display
