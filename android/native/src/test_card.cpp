// test_card.cpp — see test_card.hpp.
#include "test_card.hpp"

namespace bo2display {

// The game's frame stand-in: a test card at the size the game will render, drawn where the game's
// image will go. Edges, corners and a 16:9 frame show whether the picture fills the panel, what the
// camera cutout covers, and what 16:9 would have left out.
void draw_test_card(SDL_Renderer* r, const bo2display::Plan& p) {
    const float w = static_cast<float>(p.guest_w), h = static_cast<float>(p.guest_h);
    SDL_SetRenderDrawColor(r, 24, 26, 34, 255);
    SDL_RenderClear(r);
    SDL_SetRenderDrawColor(r, 44, 48, 60, 255);
    for (float gx = 0; gx < w; gx += h / 9) SDL_RenderLine(r, gx, 0, gx, h);
    for (float gy = 0; gy < h; gy += h / 9) SDL_RenderLine(r, 0, gy, w, gy);
    // Where a 16:9 picture would end.
    const float wide = h * 16 / 9, x0 = (w - wide) / 2;
    SDL_SetRenderDrawColor(r, 90, 140, 255, 255);
    SDL_FRect frame{x0, 0, wide, h};
    SDL_RenderRect(r, &frame);
    // The picture's own edge, 4 px thick, and its corners.
    SDL_SetRenderDrawColor(r, 255, 106, 0, 255);
    for (int i = 0; i < 4; i++) {
        SDL_FRect edge{static_cast<float>(i), static_cast<float>(i), w - 2 * i, h - 2 * i};
        SDL_RenderRect(r, &edge);
    }
    const float c = h * 0.08f;
    SDL_FRect corners[4] = {{0, 0, c, c}, {w - c, 0, c, c}, {0, h - c, c, c}, {w - c, h - c, c, c}};
    SDL_RenderFillRects(r, corners, 4);
    SDL_SetRenderDrawColor(r, 244, 241, 234, 255);
    SDL_RenderLine(r, w / 2 - c, h / 2, w / 2 + c, h / 2);
    SDL_RenderLine(r, w / 2, h / 2 - c, w / 2, h / 2 + c);
}

}  // namespace bo2display
