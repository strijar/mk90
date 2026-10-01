cmake_minimum_required(VERSION 3.16)

project(mk90 VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(MK90_GUI "Build the LVGL/X11 interface" ON)
option(MK90_SANITIZE "Enable address and undefined behavior sanitizers" OFF)

set(MK90_LVGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lvgl" CACHE PATH "LVGL 9.x sources")

if(MK90_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(mk90_core src/cpu.c src/devices.c src/storage.c)
target_include_directories(mk90_core PUBLIC include)
target_compile_options(mk90_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(mk90 src/main.c)

target_link_libraries(mk90 PRIVATE mk90_core)
target_compile_options(mk90 PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(mk90 PRIVATE
    MK90_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    MK90_STATE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/state")

if(MK90_GUI)
    find_package(X11 REQUIRED)
    find_package(Threads REQUIRED)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    if(NOT EXISTS "${MK90_LVGL_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Place LVGL 9.x in lvgl or set MK90_LVGL_DIR. Headless build: -DMK90_GUI=OFF")
    endif()

    set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
    set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
    set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CONFIG_LV_USE_THORVG OFF CACHE BOOL "" FORCE)
    set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
    set(LV_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)

    add_subdirectory("${MK90_LVGL_DIR}" lvgl)
    target_compile_definitions(lvgl PRIVATE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
    target_link_libraries(lvgl PUBLIC X11::X11 Threads::Threads m)

    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/artwork.c"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/artwork.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${CMAKE_CURRENT_BINARY_DIR}/artwork.c"
        DEPENDS tools/artwork.py assets/face.bmp assets/keys.bmp assets/overlay.bmp
        VERBATIM)

    target_sources(mk90 PRIVATE src/ui.c src/x11_host.c "${CMAKE_CURRENT_BINARY_DIR}/artwork.c")
    target_link_libraries(mk90 PRIVATE lvgl::lvgl)
    target_compile_definitions(mk90 PRIVATE MK90_GUI=1)
endif()

include(CTest)

if(BUILD_TESTING)
    add_executable(test_core tests/test_core.c)
    target_link_libraries(test_core PRIVATE mk90_core)
    target_compile_options(test_core PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    target_compile_definitions(test_core PRIVATE MK90_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
    add_test(NAME core COMMAND test_core)
    add_test(NAME boot COMMAND mk90 --headless --seconds 3 --no-save --lcd "${CMAKE_CURRENT_BINARY_DIR}/boot.pbm")
    if(MK90_GUI)
        add_test(NAME ui_render COMMAND mk90 --ui-smoke --seconds 2 --no-save --screenshot "${CMAKE_CURRENT_BINARY_DIR}/ui.ppm")
        add_executable(test_x11 tests/test_x11.c)
        target_link_libraries(test_x11 PRIVATE mk90_core X11::X11)
        target_compile_options(test_x11 PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        target_compile_definitions(test_x11 PRIVATE MK90_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")
    endif()
endif()
