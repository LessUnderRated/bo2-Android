// game_host.hpp — the recompiled game inside the shell (game_host.cpp).
#pragma once
#include <string>

// True when the game core is linked into libmain.so (it defines bo2_game_main).
bool game_linked();
// Runs the game until it returns: makes the window, sets the environment, fills the Vulkan backend's hooks.
int run_game(const std::string& game_dir, int argc, char** argv);
