#ifndef ZT_UI_H_
#define ZT_UI_H_

#include "zectrix_epd.h"

class ZectrixBoard;

// Owns the screens and the button loop. Never returns.
void UiRun(ZectrixBoard* board, zectrix_epd_handle_t epd);

#endif  // ZT_UI_H_
