#pragma once

// The game this host is built for, as its files and windows name it: set by
// the game layer's CMakeLists (GHR_NAME="GH2", GHR_SLUG="gh2"); GH1 Recomp's
// names otherwise.
#ifndef GHR_NAME
#define GHR_NAME "GH1"
#endif
#ifndef GHR_SLUG
#define GHR_SLUG "gh1"
#endif

#define GHR_INI GHR_NAME "Recomp.ini"     // settings beside the exe
#define GHR_TITLE GHR_NAME " Recomp"      // message boxes and the keyboard window
#define GHR_PROGRAMS GHR_SLUG "_programs.bin" // the shader list that comes with the game
