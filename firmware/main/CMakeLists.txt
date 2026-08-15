idf_component_register(
    SRCS
        "app_main.cc"
        "benchmark.cc"
        "canvas.cc"
        "display.cc"
        "session.cc"
        "sound.cc"
        "ui.cc"
    INCLUDE_DIRS "." "include"
    REQUIRES
        esp_timer
        nvs_flash
        zectrix_board
        zectrix_epd
    EMBED_FILES
        "assets/chime_16k.pcm"
        "assets/alert_16k.pcm"
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
