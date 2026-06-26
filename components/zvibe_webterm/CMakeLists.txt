# zvibe_webterm — optional Z-machine web terminal for nowtube
#
# Disabled by default. Enable with CONFIG_NOWTUBE_ENABLE_ZVIBE_WEBTERM=y
# and initialize the public zvibe submodule at components/zvibe.

set(ZVIBE_DIR "${CMAKE_CURRENT_LIST_DIR}/../zvibe")
set(ZVIBE_SENTINEL "${ZVIBE_DIR}/include/zvibe_api.h")

if(CONFIG_NOWTUBE_ENABLE_ZVIBE_WEBTERM)
    if(NOT EXISTS "${ZVIBE_SENTINEL}")
        message(FATAL_ERROR
            "zvibe webterm enabled, but components/zvibe is not initialized. "
            "Run: git submodule update --init components/zvibe")
    endif()

    message(STATUS "zvibe: webterm enabled")

    set(ZVIBE_SRCS
        "zvibe_service.cpp"
        "${ZVIBE_DIR}/src/api/zvibe_api.c"
        "${ZVIBE_DIR}/src/core/zvibe_core.c"
        "${ZVIBE_DIR}/src/core/zvibe_memory.c"
        "${ZVIBE_DIR}/src/core/zvibe_object.c"
        "${ZVIBE_DIR}/src/core/zvibe_ops_base.c"
        "${ZVIBE_DIR}/src/core/zvibe_ops_math.c"
        "${ZVIBE_DIR}/src/core/zvibe_ops_mem.c"
        "${ZVIBE_DIR}/src/core/zvibe_ops_text.c"
        "${ZVIBE_DIR}/target/webterm/webterm_session.c"
    )

    idf_component_register(
        SRCS          ${ZVIBE_SRCS}
        INCLUDE_DIRS
            "."
            "${PROJECT_DIR}/main"
            "${ZVIBE_DIR}/include"
            "${ZVIBE_DIR}/target/webterm"
            "${IDF_PATH}/components/esp_http_server/include"
            "${IDF_PATH}/components/http_parser"
            "${IDF_PATH}/components/esp_event/include"
        REQUIRES esp_http_server esp_hw_support heap log spiffs
        EMBED_TXTFILES "zvibe.html"
    )

    idf_build_set_property(COMPILE_DEFINITIONS "ZVIBE_AVAILABLE=1" APPEND)
else()
    message(STATUS "zvibe: webterm disabled")
    idf_component_register(INCLUDE_DIRS ".")
endif()
