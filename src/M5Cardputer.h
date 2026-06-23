#pragma once
// Header dispatcher: src/ is always on the include path (-Isrc), so this file
// intercepts every #include <M5Cardputer.h> in the project.
// For T-Embed builds we pull in the LovyanGFX shim; for Cardputer we forward
// to the real M5Cardputer library using GCC's #include_next extension.
#ifdef BOARD_TEMBED
#  include "boards/tembed/M5Cardputer.h"
#else
#  include_next <M5Cardputer.h>
#endif
