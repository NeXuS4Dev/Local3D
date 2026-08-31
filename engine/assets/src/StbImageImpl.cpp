/// @file StbImageImpl.cpp
/// @brief The single translation unit that instantiates stb_image.
///
/// stb is vendored third party code written in a C style that our warning
/// policy (see cmake/L3DCompilerFlags.cmake) rejects, so this file is compiled
/// without it - the same treatment Dear ImGui gets.  Keeping the implementation
/// in its own translation unit means our own importer code is still compiled
/// with the full warning set.

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
