#pragma once

// The runner's command line, before it reads it: a game layer may fill in
// what a player launching the exe by itself leaves out (where the game's disc
// image is, which ELF to boot, where memory cards go). args[0] is the
// program. The default leaves the arguments as they are.

#include <string>
#include <vector>

void ps2xPrepareArguments(std::vector<std::string> &args);
