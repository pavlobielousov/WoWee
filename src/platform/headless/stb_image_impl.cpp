// stb_image's implementation for wowee_core (VITA-47). On the desktop it lives in
// src/rendering/loading_screen.cpp, which the core does not have, and the asset manager (pipeline/)
// needs stbi_load for its PNG path. Only wowee_core builds this file; the desktop client is
// unchanged and keeps its own copy (the two are never linked together).
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
