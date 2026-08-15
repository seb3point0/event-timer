// Presentation timer for the ZECTRIX NOTE4.
//
// Everything runs on the device: the three buttons configure a session and
// drive it, the e-paper panel is the stage display, and the onboard speaker
// carries the cues. No network, no phone, no server.

#include "benchmark.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sound.h"
#include "ui.h"
#include "zectrix_board.h"
#include "zectrix_epd.h"

namespace {

constexpr char kTag[] = "counter";

}  // namespace

extern "C" void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Static: the board owns a button task and I2C handles that must outlive
    // this function, and app_main's stack is not the place for them.
    static ZectrixBoard board;
    ESP_ERROR_CHECK(board.Init());

    zectrix_epd_config_t epd_config;
    zectrix_epd_get_default_config(&epd_config);

    zectrix_epd_handle_t epd = nullptr;
    ESP_ERROR_CHECK(zectrix_epd_new(&epd_config, &epd));
    ESP_ERROR_CHECK(zectrix_epd_power_on(epd));

    SoundInit(&board);

#if CONFIG_COUNTER_PANEL_BENCHMARK
    PanelBenchmark(epd);
#endif

    ESP_LOGI(kTag, "ready");
    UiRun(&board, epd);
}
