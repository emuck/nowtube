cmake_minimum_required(VERSION 3.16)
project(nowtube_tests CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The test binary compiles only the pure validation module.
# The main/ tree is on the include path so device_config.h and mode_manager.h
# resolve normally — neither pulls in any ESP-IDF headers.
add_executable(test_config_validation
    test_config_validation.cpp
    ${CMAKE_SOURCE_DIR}/../main/config_validation.cpp
)

target_include_directories(test_config_validation PRIVATE
    ${CMAKE_SOURCE_DIR}/../main
)

# Treat warnings as errors so test build quality matches firmware build quality.
target_compile_options(test_config_validation PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

enable_testing()
add_test(NAME config_validation COMMAND test_config_validation)

# ---------------------------------------------------------------------------
# cJSON resolution order:
#   1. ESP-IDF bundle at $IDF_PATH (compile from source — matches firmware build)
#   2. System libcjson-dev  (Ubuntu: apt install libcjson-dev)
# ---------------------------------------------------------------------------
if(DEFINED ENV{IDF_PATH} AND EXISTS "$ENV{IDF_PATH}/components/json/cJSON/cJSON.h")
    set(_CJSON_SOURCES $ENV{IDF_PATH}/components/json/cJSON/cJSON.c)
    set(_CJSON_INCLUDES $ENV{IDF_PATH}/components/json/cJSON)
    set(_CJSON_LIBS "")
else()
    # Fall back to system package.  Install with: sudo apt install libcjson-dev
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(_CJSON REQUIRED libcjson)
    set(_CJSON_SOURCES "")
    set(_CJSON_INCLUDES ${_CJSON_INCLUDE_DIRS})
    set(_CJSON_LIBS ${_CJSON_LIBRARIES})
endif()

# ---------------------------------------------------------------------------
# test_game_logic — host-side tests for game_logic.cpp (pure C++, no ESP-IDF)
# ---------------------------------------------------------------------------
add_executable(test_game_logic
    test_game_logic.cpp
    ${CMAKE_SOURCE_DIR}/../main/displays/game/game_logic.cpp
)

target_include_directories(test_game_logic PRIVATE
    ${CMAKE_SOURCE_DIR}/../main
)

target_compile_options(test_game_logic PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

add_test(NAME game_logic COMMAND test_game_logic)

# ---------------------------------------------------------------------------
# test_mode_policy — host-side tests for mode_policy.cpp and ModeManager
# ---------------------------------------------------------------------------
add_executable(test_mode_policy
    test_mode_policy.cpp
    ${CMAKE_SOURCE_DIR}/../main/mode_policy.cpp
    ${CMAKE_SOURCE_DIR}/../main/mode_manager.cpp
)

target_include_directories(test_mode_policy PRIVATE
    ${CMAKE_SOURCE_DIR}/../main
)

target_compile_options(test_mode_policy PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

add_test(NAME mode_policy COMMAND test_mode_policy)

# ---------------------------------------------------------------------------
# test_conditions_parser — host-side tests for conditions_parser.cpp
# ---------------------------------------------------------------------------
add_executable(test_conditions_parser
    test_conditions_parser.cpp
    ${CMAKE_SOURCE_DIR}/../main/conditions_parser.cpp
    ${_CJSON_SOURCES}
)

target_include_directories(test_conditions_parser PRIVATE
    ${CMAKE_SOURCE_DIR}/../main
    ${_CJSON_INCLUDES}
)
target_link_libraries(test_conditions_parser ${_CJSON_LIBS})

target_compile_options(test_conditions_parser PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

add_test(NAME conditions_parser COMMAND test_conditions_parser)

# ---------------------------------------------------------------------------
# test_forecast_parser — host-side tests for forecast_parser.cpp
# ---------------------------------------------------------------------------
add_executable(test_forecast_parser
    test_forecast_parser.cpp
    ${CMAKE_SOURCE_DIR}/../main/forecast_parser.cpp
    ${_CJSON_SOURCES}
)

target_include_directories(test_forecast_parser PRIVATE
    ${CMAKE_SOURCE_DIR}/../main
    ${_CJSON_INCLUDES}
)
target_link_libraries(test_forecast_parser ${_CJSON_LIBS})

target_compile_options(test_forecast_parser PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

add_test(NAME forecast_parser COMMAND test_forecast_parser)
