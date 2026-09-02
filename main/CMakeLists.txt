find_package(Git QUIET)

set(NOWTUBE_BUILD_GIT_SHA "unknown")
set(NOWTUBE_BUILD_GIT_DIRTY 0)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
        OUTPUT_VARIABLE NOWTUBE_BUILD_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" diff --quiet --ignore-submodules HEAD --
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
        RESULT_VARIABLE NOWTUBE_GIT_DIFF_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT NOWTUBE_GIT_DIFF_RESULT EQUAL 0)
        set(NOWTUBE_BUILD_GIT_DIRTY 1)
    endif()
endif()

string(TIMESTAMP NOWTUBE_BUILD_TIME_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/build_info.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/build_info.h"
    @ONLY
)

idf_component_register(
        EMBED_TXTFILES
        "spiffs/index.html"
        "spiffs/app.js"
        "spiffs/app.css"
        "spiffs/panels.html"
        SRCS
        "app_boot.cpp"
        "config_validation.cpp"
        "displays/clock/clock.cpp"
        "displays/clock/flapper.cpp"
        "displays/forecast/forecast_display.cpp"
        "displays/game/game_display.cpp"
        "displays/game/game_logic.cpp"
        "displays/today/today_display.cpp"
        "controllers/display_controller.cpp"
        "controllers/input_controller.cpp"
        "drivers/lcds.cpp"
        "drivers/leds.cpp"
        "drivers/touchpads.cpp"
        "drivers/wifi.cpp"
        "fonts/nixie_120.c"
        "fonts/nixie_60.c"
        "fonts/nixie_temp.c"
        "fonts/space_mono_44.c"
        "fonts/space_mono_54.c"
        "fonts/space_mono_120.c"
        "fonts/space_mono_60.c"
        "fonts/space_mono_48.c"
        "fonts/atkinson_hyperlegible_120.c"
        "fonts/atkinson_hyperlegible_60.c"
        "fonts/atkinson_hyperlegible_48.c"
        "fonts/aldrich_120.c"
        "fonts/aldrich_60.c"
        "fonts/aldrich_48.c"
        "font_catalog.cpp"
        "font_theme.cpp"
        "gui.cpp"
        "led_manager.cpp"
        "main.cpp"
        "mode_manager.cpp"
        "mode_policy.cpp"
        "weather_parser.cpp"
        "weather_service.cpp"
        "rtc.cpp"
        "services/backlight_service.cpp"
        "services/sound_manager.cpp"
        "services/config_service.cpp"
        "services/diagnostics_service.cpp"
        "services/status_service.cpp"
        "spiram_allocate.cpp"
        "webserver.cpp"
        INCLUDE_DIRS
        "."
        "displays/clock"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

spiffs_create_partition_image(spiffs spiffs FLASH_IN_PROJECT)
idf_build_set_property(COMPILE_OPTIONS "-DLV_LVGL_H_INCLUDE_SIMPLE" APPEND)
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-enum-enum-conversion>
    -Wno-missing-field-initializers)
