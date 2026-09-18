/*
 * KiChad: compiled implementation of the nanosvg rasterizer so the header stays declaration-only
 * for every other translation unit, matching how nanosvg.cpp carries the parser.
 */
#include "nanosvg.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
