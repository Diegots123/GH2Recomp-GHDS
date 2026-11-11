// The runner calls ps2xConfigureHost before the runtime opens its window, so a
// game layer can install its own host frontend and options. This default,
// alone in its object file, is linked only when the game layer defines none.

#include "ps2_runtime.h"

void ps2xConfigureHost(PS2Runtime &runtime)
{
    (void)runtime;
}
