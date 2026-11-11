// The runner calls ps2xInstallGame once the ELF is loaded and before it runs.
// A game layer (PS2X_GAME_DIR) defines its own; this one, alone in its object
// file, is linked only when none does.

#include "ps2_runtime.h"

void ps2xInstallGame(PS2Runtime &runtime)
{
    (void)runtime;
}
