/// @file StbImageWriteImpl.cpp
/// @brief Test only: the stb_image_write implementation, used to generate PNG
///        and HDR inputs for the importer tests.
///
/// Upstream C style code, so it is compiled without the engine warning policy
/// (the same treatment the engine's own stb_image TU gets).

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
