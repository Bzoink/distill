cmake_minimum_required(VERSION 3.20)
project(distill LANGUAGES C CXX)

# The production application and all bespoke code are C23. C++ remains enabled
# only for the litehtml-facing adapter, which is linked into the probe target.
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(EXTERNAL_PREFIX "${CMAKE_SOURCE_DIR}/external/install" CACHE PATH "Distill external dependency prefix")

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DISTILL_RENDER REQUIRED IMPORTED_TARGET freetype2 harfbuzz cairo libunibreak)
pkg_check_modules(DISTILL_GTK IMPORTED_TARGET gtk+-3.0)

enable_testing()
add_executable(distill-shaper-test tests/shaper_test.c src/shaper.c)
target_include_directories(distill-shaper-test PRIVATE src)
target_link_libraries(distill-shaper-test PRIVATE PkgConfig::DISTILL_RENDER)
add_test(NAME distill-shaper COMMAND distill-shaper-test)

add_executable(distill-app-state-test tests/app_state_test.c src/app_state.c)
target_include_directories(distill-app-state-test PRIVATE src)
add_test(NAME distill-app-state COMMAND distill-app-state-test)

add_executable(distill-litehtml-geometry-test tests/litehtml_geometry_test.c src/litehtml_probe.cpp)
target_include_directories(distill-litehtml-geometry-test PRIVATE
    src
    "${EXTERNAL_PREFIX}/include"
)
target_link_directories(distill-litehtml-geometry-test PRIVATE "${EXTERNAL_PREFIX}/lib")
target_link_libraries(distill-litehtml-geometry-test PRIVATE
    litehtml
    gumbo
    distill-render-core
)
add_test(NAME distill-litehtml-geometry COMMAND distill-litehtml-geometry-test)

add_library(distill-render-core STATIC src/render.c src/shaper.c)
target_link_libraries(distill-render-core PUBLIC PkgConfig::DISTILL_RENDER)

add_executable(distill src/main.c src/litehtml_probe.cpp)
target_compile_definitions(distill PRIVATE DISTILL_SOURCE_DIR="${CMAKE_SOURCE_DIR}" DISTILL_RENDER=1)
target_include_directories(distill PRIVATE
    "${EXTERNAL_PREFIX}/include"
    ${CURL_INCLUDE_DIRS}
)
target_link_directories(distill PRIVATE "${EXTERNAL_PREFIX}/lib")
target_link_libraries(distill PRIVATE
    qjs
    lexbor
    litehtml
    gumbo
    CURL::libcurl
    Threads::Threads
    distill-render-core
    ${CMAKE_DL_LIBS}
    m
)
set_target_properties(distill PROPERTIES
    BUILD_RPATH "${EXTERNAL_PREFIX}/lib"
    INSTALL_RPATH "${EXTERNAL_PREFIX}/lib"
)

install(TARGETS distill RUNTIME DESTINATION bin)

if(DISTILL_GTK_FOUND)
    add_executable(distill-gtk src/app.c src/app_state.c src/litehtml_probe.cpp)
    target_compile_definitions(distill-gtk PRIVATE
        DISTILL_ENGINE_PATH="${CMAKE_BINARY_DIR}/distill"
        DISTILL_FONT_DIR="${CMAKE_SOURCE_DIR}/external/fonts"
    )
    target_include_directories(distill-gtk PRIVATE
        src
        "${EXTERNAL_PREFIX}/include"
    )
    target_link_directories(distill-gtk PRIVATE "${EXTERNAL_PREFIX}/lib")
    target_link_libraries(distill-gtk PRIVATE
        litehtml
        gumbo
        distill-render-core
        PkgConfig::DISTILL_GTK
    )
    set_target_properties(distill-gtk PROPERTIES
        BUILD_RPATH "${EXTERNAL_PREFIX}/lib"
        INSTALL_RPATH "${EXTERNAL_PREFIX}/lib"
    )
    install(TARGETS distill-gtk RUNTIME DESTINATION bin)
else()
    message(STATUS "GTK3 not found; skipping optional distill-gtk desktop shell")
endif()

add_executable(distill-arm-b src/main.c)
target_compile_definitions(distill-arm-b PRIVATE DISTILL_NO_EXTRACT=1)
target_include_directories(distill-arm-b PRIVATE "${EXTERNAL_PREFIX}/include" ${CURL_INCLUDE_DIRS})
target_link_directories(distill-arm-b PRIVATE "${EXTERNAL_PREFIX}/lib")
target_link_libraries(distill-arm-b PRIVATE qjs CURL::libcurl Threads::Threads ${CMAKE_DL_LIBS} m)
set_target_properties(distill-arm-b PROPERTIES
    BUILD_RPATH "${EXTERNAL_PREFIX}/lib"
    INSTALL_RPATH "${EXTERNAL_PREFIX}/lib"
)
install(TARGETS distill-arm-b RUNTIME DESTINATION bin)

add_executable(distill-arm-b-pruned src/main.c src/litehtml_probe.cpp src/render.c src/shaper.c)
target_compile_definitions(distill-arm-b-pruned PRIVATE DISTILL_B_PRUNED=1 DISTILL_NO_EXTRACT=1)
target_include_directories(distill-arm-b-pruned PRIVATE "${EXTERNAL_PREFIX}/include" ${CURL_INCLUDE_DIRS})
target_link_directories(distill-arm-b-pruned PRIVATE "${EXTERNAL_PREFIX}/lib")
target_link_libraries(distill-arm-b-pruned PRIVATE qjs CURL::libcurl litehtml gumbo Threads::Threads PkgConfig::DISTILL_RENDER ${CMAKE_DL_LIBS} m)
set_target_properties(distill-arm-b-pruned PROPERTIES
    BUILD_RPATH "${EXTERNAL_PREFIX}/lib"
    INSTALL_RPATH "${EXTERNAL_PREFIX}/lib"
)
install(TARGETS distill-arm-b-pruned RUNTIME DESTINATION bin)
