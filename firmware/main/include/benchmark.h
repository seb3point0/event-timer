#ifndef ZT_BENCHMARK_H_
#define ZT_BENCHMARK_H_

#include "zectrix_epd.h"

// Times full and partial refreshes at a range of heights and logs the results.
// Development aid, compiled in only under CONFIG_COUNTER_PANEL_BENCHMARK.
void PanelBenchmark(zectrix_epd_handle_t epd);

#endif  // ZT_BENCHMARK_H_
