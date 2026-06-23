// T-Embed hardware globals — compiled only for BOARD_TEMBED.
// Excluded from Cardputer build via platformio.ini src_filter.
#include <M5Cardputer.h>   // resolves to src/boards/tembed/M5Cardputer.h

// Definition order matters: gTEmbedDisplay first so _TEmbed_t ctor can bind ref.
LGFX_TEmbed  gTEmbedDisplay;
_TEmbed_t    M5Cardputer;
