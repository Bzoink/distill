cmake_minimum_required(VERSION 3.20)
# The release version. This is the only place it is set: every binary reads it
# from the generated distill_version.h and reports it with --version.
project(distill VERSION 1.0 LANGUAGES C CXX)

include(GNUInstallDirs)

option(DISTILL_STATIC_LINK "Build production executables as static binaries" OFF)
if(DISTILL_STATIC_LINK)
    # M5's static gate is intentionally explicit. The caller supplies a
    # static-capable compiler (normally musl-gcc) and every dependency must
    # provide an archive; silently falling back to a shared object would make
    # the portability claim meaningless.
    set(BUILD_SHARED_LIBS OFF)
    set(CURL_USE_STATIC_LIBS TRUE CACHE BOOL "Prefer static libcurl" FORCE)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    add_link_options(-static -Wl,--no-undefined)
    message(STATUS "Distill static link: ON")
endif()

# The production application and all bespoke code are C23. C++ remains enabled
# for external HarfBuzz builds; Distill has no C++ source boundary of its own.
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# Strict ISO C, as the portability rules require (muroxi_compat.md): no GNU
# extensions, and -- because no core file defines a feature-test macro --
# no POSIX declaration is visible outside src/platform/<os>/. The GTK host is
# the one exception; GLib's own idioms are not ISO C.
add_compile_options($<$<COMPILE_LANGUAGE:C>:-pedantic-errors>)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(EXTERNAL_PREFIX "${CMAKE_SOURCE_DIR}/external/install" CACHE PATH "Distill external dependency prefix")

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
# The line breaker needs libunibreak and nothing else, so it is its own
# module: every target breaks lines, and only the desktop one shapes glyphs.
pkg_check_modules(DISTILL_UNIBREAK REQUIRED IMPORTED_TARGET libunibreak)
# Still-image decoding is linked directly rather than taken from a toolkit, so
# the framebuffer and TUI targets can use the same decoder as the desktop one.
pkg_check_modules(DISTILL_IMAGE REQUIRED IMPORTED_TARGET libpng libjpeg libwebp)

# The raster renderer -- reader_layout, FreeType and HarfBuzz shaping, and our
# software painter -- is what the desktop shell and --render draw with. It is
# found rather than required: a machine without the font stack still builds
# the engine, the reader page, the Markdown export and the terminal front end.
# Cairo is not part of it: only the reference painter used by check.py links
# Cairo.
option(DISTILL_RENDERER "Build the pixel renderer when its dependencies are present" ON)
set(DISTILL_HAVE_RENDERER OFF)
if(DISTILL_RENDERER)
    pkg_check_modules(DISTILL_RENDER IMPORTED_TARGET freetype2 harfbuzz)
    if(DISTILL_RENDER_FOUND)
        set(DISTILL_HAVE_RENDERER ON)
    else()
        message(STATUS "FreeType or HarfBuzz not found; building without the renderer")
    endif()
endif()
message(STATUS "Distill renderer: ${DISTILL_HAVE_RENDERER}")
if(DISTILL_HAVE_RENDERER)
    pkg_check_modules(DISTILL_GTK IMPORTED_TARGET gtk+-3.0)
    pkg_check_modules(DISTILL_CAIRO IMPORTED_TARGET cairo)
endif()

enable_testing()

configure_file(src/distill_version.h.in "${CMAKE_BINARY_DIR}/generated/distill_version.h" @ONLY)
add_library(distill-version INTERFACE)
target_include_directories(distill-version INTERFACE "${CMAKE_BINARY_DIR}/generated")

# The unit tests assert with assert(), and a Release build defines NDEBUG,
# which compiles every one of them away: the binaries contained no
# __assert_fail at all and reported success whatever they checked. A test that
# cannot fail is worse than no test, so the test targets keep their assertions
# regardless of build type.
function(distill_enable_assertions target)
    target_compile_options(${target} PRIVATE -UNDEBUG)
endfunction()

# The platform layer (src/documents/muroxi_compat.md). The core reaches the
# operating system only through the headers in src/platform/, and each
# platform supplies one source file per interface under src/platform/<os>/.
# A port is a new directory here, chosen at configure time; nothing outside
# src/platform/<os>/ includes an OS header (tools/platform_gate.py).
set(DISTILL_PLATFORM "linux" CACHE STRING "Which src/platform/<os> backends to build")
set(DISTILL_PLATFORM_DIR "${CMAKE_SOURCE_DIR}/src/platform/${DISTILL_PLATFORM}")

# Keep production source ownership in a format the Muroxi build can consume
# too. CMake reads the same four-column file rather than growing a second
# source list every time a platform seam is added.
set(DISTILL_SOURCE_MANIFEST "${CMAKE_SOURCE_DIR}/portability/sources.manifest")
if(NOT EXISTS "${DISTILL_SOURCE_MANIFEST}")
    message(FATAL_ERROR "missing portable source manifest: ${DISTILL_SOURCE_MANIFEST}")
endif()
file(STRINGS "${DISTILL_SOURCE_MANIFEST}" DISTILL_MANIFEST_LINES)
foreach(line IN LISTS DISTILL_MANIFEST_LINES)
    string(STRIP "${line}" stripped)
    if(stripped STREQUAL "" OR stripped MATCHES "^[#]")
        continue()
    endif()
    string(REPLACE "|" ";" fields "${stripped}")
    list(LENGTH fields field_count)
    if(NOT field_count EQUAL 4)
        message(FATAL_ERROR "${DISTILL_SOURCE_MANIFEST}: expected four fields: ${line}")
    endif()
    list(GET fields 0 manifest_target)
    list(GET fields 1 manifest_sources)
    list(GET fields 2 manifest_defines)
    list(GET fields 3 manifest_includes)
    string(REPLACE "," ";" manifest_sources "${manifest_sources}")
    string(REPLACE "," ";" manifest_defines "${manifest_defines}")
    string(REPLACE "," ";" manifest_includes "${manifest_includes}")
    string(REPLACE "{platform}" "${DISTILL_PLATFORM}" manifest_sources "${manifest_sources}")
    string(REPLACE "{platform}" "${DISTILL_PLATFORM}" manifest_includes "${manifest_includes}")
    set("DISTILL_MANIFEST_${manifest_target}_SOURCES" "${manifest_sources}")
    set("DISTILL_MANIFEST_${manifest_target}_DEFINES" "${manifest_defines}")
    set("DISTILL_MANIFEST_${manifest_target}_INCLUDES" "${manifest_includes}")
endforeach()

function(distill_manifest_sources target output)
    set(key "DISTILL_MANIFEST_${target}_SOURCES")
    if(NOT DEFINED ${key})
        message(FATAL_ERROR "target ${target} is missing from ${DISTILL_SOURCE_MANIFEST}")
    endif()
    set(${output} "${${key}}" PARENT_SCOPE)
endfunction()

function(distill_manifest_defines target output)
    set(key "DISTILL_MANIFEST_${target}_DEFINES")
    if(NOT DEFINED ${key})
        message(FATAL_ERROR "target ${target} is missing from ${DISTILL_SOURCE_MANIFEST}")
    endif()
    set(${output} "${${key}}" PARENT_SCOPE)
endfunction()

function(distill_manifest_includes target output)
    set(key "DISTILL_MANIFEST_${target}_INCLUDES")
    if(NOT DEFINED ${key})
        message(FATAL_ERROR "target ${target} is missing from ${DISTILL_SOURCE_MANIFEST}")
    endif()
    set(result)
    foreach(include IN LISTS ${key})
        list(APPEND result "${CMAKE_SOURCE_DIR}/${include}")
    endforeach()
    set(${output} "${result}" PARENT_SCOPE)
endfunction()

distill_manifest_sources(distill-platform DISTILL_PLATFORM_SOURCES)
distill_manifest_defines(distill-platform DISTILL_PLATFORM_DEFINES)
list(REMOVE_ITEM DISTILL_PLATFORM_DEFINES DISTILL_INSTALL_DATADIR)
list(APPEND DISTILL_PLATFORM_DEFINES
     "DISTILL_INSTALL_DATADIR=\"${CMAKE_INSTALL_FULL_DATADIR}/distill\"")
distill_manifest_includes(distill-platform DISTILL_PLATFORM_INCLUDES)
add_library(distill-platform STATIC ${DISTILL_PLATFORM_SOURCES})
target_include_directories(distill-platform PUBLIC ${DISTILL_PLATFORM_INCLUDES})
# Where data is found (src/datadir.h). No binary refers to the source tree:
# a build tree finds its data in build/share/distill, staged below, and an
# installed one beside itself or in the configured data directory.
target_compile_definitions(distill-platform PRIVATE
    ${DISTILL_PLATFORM_DEFINES})
set(DISTILL_STAGED_DATA "${CMAKE_BINARY_DIR}/share/distill")
file(MAKE_DIRECTORY "${DISTILL_STAGED_DATA}")
foreach(entry "fonts|external/fonts" "profiles|profiles" "welcome.html|share/welcome.html")
    string(REPLACE "|" ";" pair "${entry}")
    list(GET pair 0 staged_name)
    list(GET pair 1 staged_source)
    if(NOT EXISTS "${DISTILL_STAGED_DATA}/${staged_name}")
        file(CREATE_LINK "${CMAKE_SOURCE_DIR}/${staged_source}"
             "${DISTILL_STAGED_DATA}/${staged_name}" SYMBOLIC)
    endif()
endforeach()
find_program(DISTILL_PYTHON3 python3)
if(DISTILL_PYTHON3)
    add_test(NAME distill-portable-manifest
             COMMAND ${DISTILL_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/portable_manifest.py --check)
    add_test(NAME distill-portable-manifest-self-test
             COMMAND ${DISTILL_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/portable_manifest.py --self-test)
    add_test(NAME distill-portable-data
             COMMAND ${DISTILL_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/data_gate.py
                     --data "${DISTILL_STAGED_DATA}")
    add_test(NAME distill-portable-data-self-test
             COMMAND ${DISTILL_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/data_gate.py --self-test)
    add_test(NAME distill-platform-gate
             COMMAND ${DISTILL_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/platform_gate.py)
    add_test(NAME distill-platform-gate-self-test
             COMMAND ${DISTILL_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/platform_gate.py --self-test)
endif()

distill_manifest_sources(distill-shaper DISTILL_SHAPER_SOURCES)
distill_manifest_includes(distill-shaper DISTILL_SHAPER_INCLUDES)
add_library(distill-shaper STATIC ${DISTILL_SHAPER_SOURCES})
target_include_directories(distill-shaper PUBLIC ${DISTILL_SHAPER_INCLUDES})
target_link_libraries(distill-shaper PUBLIC PkgConfig::DISTILL_UNIBREAK)

distill_manifest_sources(distill-image DISTILL_IMAGE_SOURCES)
distill_manifest_includes(distill-image DISTILL_IMAGE_INCLUDES)
add_library(distill-image STATIC ${DISTILL_IMAGE_SOURCES})
target_include_directories(distill-image PUBLIC ${DISTILL_IMAGE_INCLUDES})
target_link_libraries(distill-image PUBLIC PkgConfig::DISTILL_IMAGE)

add_executable(distill-shaper-test tests/shaper_test.c)
target_link_libraries(distill-shaper-test PRIVATE distill-shaper)
distill_enable_assertions(distill-shaper-test)
add_test(NAME distill-shaper COMMAND distill-shaper-test)

add_executable(distill-image-test tests/image_test.c)
target_link_libraries(distill-image-test PRIVATE distill-image)
distill_enable_assertions(distill-image-test)
add_test(NAME distill-image COMMAND distill-image-test)

# Resource measurement for the Muroxi budgets, and the QuickJS allocator that
# makes the JS memory limit count on a C library without malloc_usable_size.
distill_manifest_sources(distill-measure DISTILL_MEASURE_SOURCES)
distill_manifest_includes(distill-measure DISTILL_MEASURE_INCLUDES)
add_library(distill-measure STATIC ${DISTILL_MEASURE_SOURCES})
target_include_directories(distill-measure PUBLIC ${DISTILL_MEASURE_INCLUDES})
target_link_libraries(distill-measure PUBLIC distill-platform)
add_executable(distill-measure-test tests/measure_test.c)
target_include_directories(distill-measure-test PRIVATE src)
target_link_libraries(distill-measure-test PRIVATE distill-measure)
distill_enable_assertions(distill-measure-test)
add_test(NAME distill-measure COMMAND distill-measure-test)

# What every front end shares that is not the page: the rules a form is
# submitted by, what a typed address means, the settings file, and fetching
# media through the engine. Shared so that the terminal and the desktop send
# the same request, go to the same place and read the same preferences.
distill_manifest_sources(distill-shell DISTILL_SHELL_SOURCES)
distill_manifest_includes(distill-shell DISTILL_SHELL_INCLUDES)
add_library(distill-shell STATIC ${DISTILL_SHELL_SOURCES})
target_include_directories(distill-shell PUBLIC ${DISTILL_SHELL_INCLUDES})
target_link_libraries(distill-shell PUBLIC distill-platform)

add_executable(distill-form-submit-test tests/form_submit_test.c)
target_link_libraries(distill-form-submit-test PRIVATE distill-shell)
distill_enable_assertions(distill-form-submit-test)
add_test(NAME distill-form-submit COMMAND distill-form-submit-test)

add_executable(distill-shell-input-test tests/shell_input_test.c)
target_link_libraries(distill-shell-input-test PRIVATE distill-shell)
distill_enable_assertions(distill-shell-input-test)
add_test(NAME distill-shell-input COMMAND distill-shell-input-test)

add_executable(distill-settings-test tests/settings_test.c)
target_link_libraries(distill-settings-test PRIVATE distill-shell)
# The reader's agreement with GKeyFile is checked against GKeyFile itself
# wherever GLib is installed; the test runs without it otherwise.
pkg_check_modules(DISTILL_TEST_GLIB IMPORTED_TARGET glib-2.0)
if(DISTILL_TEST_GLIB_FOUND)
    target_compile_definitions(distill-settings-test PRIVATE DISTILL_TEST_GLIB=1)
    target_link_libraries(distill-settings-test PRIVATE PkgConfig::DISTILL_TEST_GLIB)
endif()
distill_enable_assertions(distill-settings-test)
add_test(NAME distill-settings COMMAND distill-settings-test)

add_executable(distill-app-state-test tests/app_state_test.c src/app_state.c)
target_include_directories(distill-app-state-test PRIVATE src)
distill_enable_assertions(distill-app-state-test)
add_test(NAME distill-app-state COMMAND distill-app-state-test)

# The audio core. miniaudio is a single-source third-party header, vendored
# under external/ like every other dependency. The core decodes with it; the
# platform's audio backend plays what it decodes and is the one translation
# unit that compiles miniaudio's implementation. Its warnings are not ours to
# fix, so they are quieted for that file alone, and the header is a system
# include -- the C23 code around it keeps the project's warning level.
distill_manifest_sources(distill-media-audio DISTILL_MEDIA_AUDIO_SOURCES)
distill_manifest_includes(distill-media-audio DISTILL_MEDIA_AUDIO_INCLUDES)
add_library(distill-media-audio STATIC ${DISTILL_MEDIA_AUDIO_SOURCES})
target_include_directories(distill-media-audio PUBLIC ${DISTILL_MEDIA_AUDIO_INCLUDES})
target_include_directories(distill-media-audio SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/external/src/miniaudio")
set_source_files_properties(${DISTILL_PLATFORM_DIR}/audio.c PROPERTIES COMPILE_OPTIONS -w)
target_link_libraries(distill-media-audio PUBLIC distill-platform Threads::Threads ${CMAKE_DL_LIBS} m)

add_executable(distill-media-transfer-test tests/media_transfer_test.c)
target_link_libraries(distill-media-transfer-test PRIVATE distill-shell)
distill_enable_assertions(distill-media-transfer-test)
add_test(NAME distill-media-transfer COMMAND distill-media-transfer-test)

add_executable(distill-media-audio-test tests/media_audio_test.c)
target_link_libraries(distill-media-audio-test PRIVATE distill-media-audio m)
distill_enable_assertions(distill-media-audio-test)
add_test(NAME distill-media-audio COMMAND distill-media-audio-test)

if(DISTILL_HAVE_RENDERER)
    distill_manifest_sources(distill-render-core DISTILL_RENDER_CORE_SOURCES)
    distill_manifest_includes(distill-render-core DISTILL_RENDER_CORE_INCLUDES)
    distill_manifest_sources(distill-paint DISTILL_PAINT_SOURCES)
    distill_manifest_includes(distill-paint DISTILL_PAINT_INCLUDES)
    # render.c paints through paint.h; the painter is linked beside it, so a
    # target chooses by what it links: distill-paint everywhere that ships.
    add_library(distill-render-core STATIC ${DISTILL_RENDER_CORE_SOURCES})
    target_include_directories(distill-render-core PUBLIC ${DISTILL_RENDER_CORE_INCLUDES})
    target_link_libraries(distill-render-core PUBLIC PkgConfig::DISTILL_RENDER distill-shaper distill-image distill-platform)
    add_library(distill-paint STATIC ${DISTILL_PAINT_SOURCES})
    target_link_libraries(distill-paint PUBLIC PkgConfig::DISTILL_IMAGE m)
    target_include_directories(distill-paint PUBLIC ${DISTILL_PAINT_INCLUDES})

    # check.py --paint: the same reader page painted by the software painter,
    # by the Cairo reference painter, and by a software painter with a
    # deliberate defect, which the comparison must catch.
    function(distill_paint_tool name painter)
        add_executable(${name} tests/paint_tool.c)
        target_link_libraries(${name} PRIVATE distill-layout distill-render-core ${painter})
    endfunction()

    # The reader page's own layout (src/reader_layout.c), landed in M4.
    distill_manifest_sources(distill-layout DISTILL_LAYOUT_SOURCES)
    distill_manifest_includes(distill-layout DISTILL_LAYOUT_INCLUDES)
    add_library(distill-layout STATIC ${DISTILL_LAYOUT_SOURCES})
    target_include_directories(distill-layout PUBLIC ${DISTILL_LAYOUT_INCLUDES})
    target_link_directories(distill-layout PUBLIC "${EXTERNAL_PREFIX}/lib")
    target_link_libraries(distill-layout PUBLIC lexbor distill-render-core m)
    add_executable(distill-layout-reader tests/layout_dump.c)
    target_link_libraries(distill-layout-reader PRIVATE distill-layout distill-render-core distill-paint)
    add_executable(distill-layout-paint tests/paint_tool.c)
    target_link_libraries(distill-layout-paint PRIVATE distill-layout distill-render-core distill-paint)
    add_executable(distill-layout-geometry-test tests/layout_geometry_test.c)
    target_compile_definitions(distill-layout-geometry-test PRIVATE
        DISTILL_TEST_FONT_DIR="${CMAKE_SOURCE_DIR}/external/fonts"
    )
    target_link_libraries(distill-layout-geometry-test PRIVATE distill-layout distill-render-core distill-paint)
    distill_enable_assertions(distill-layout-geometry-test)
    add_test(NAME distill-layout-geometry COMMAND distill-layout-geometry-test)

    add_library(distill-layout-defect STATIC ${DISTILL_LAYOUT_SOURCES})
    target_compile_definitions(distill-layout-defect PRIVATE DISTILL_LAYOUT_DEFECT=1)
    target_include_directories(distill-layout-defect PUBLIC ${DISTILL_LAYOUT_INCLUDES})
    target_link_directories(distill-layout-defect PUBLIC "${EXTERNAL_PREFIX}/lib")
    target_link_libraries(distill-layout-defect PUBLIC lexbor distill-render-core m)
    add_executable(distill-layout-defect-paint tests/paint_tool.c)
    target_link_libraries(distill-layout-defect-paint PRIVATE distill-layout-defect distill-render-core distill-paint)

    add_executable(distill-paint-soft tests/paint_tool.c)
    target_link_libraries(distill-paint-soft PRIVATE distill-layout distill-render-core distill-paint)
    add_library(distill-paint-defect STATIC src/paint_soft.c)
    target_compile_definitions(distill-paint-defect PRIVATE DISTILL_PAINT_DEFECT=1)
    target_link_libraries(distill-paint-defect PUBLIC PkgConfig::DISTILL_IMAGE m)
    target_include_directories(distill-paint-defect PUBLIC src)
    distill_paint_tool(distill-paint-defect-tool distill-paint-defect)
    set_target_properties(distill-paint-defect-tool PROPERTIES OUTPUT_NAME distill-paint-defect)
    if(DISTILL_CAIRO_FOUND)
        add_library(distill-paint-cairo STATIC tests/paint_cairo.c)
        target_include_directories(distill-paint-cairo PUBLIC src)
        target_link_libraries(distill-paint-cairo PUBLIC PkgConfig::DISTILL_CAIRO m)
        distill_paint_tool(distill-paint-cairo-tool distill-paint-cairo)
        set_target_properties(distill-paint-cairo-tool PROPERTIES OUTPUT_NAME distill-paint-cairo)
    endif()

endif()

# The reader-page walker and the text front ends: the Markdown export today,
# the terminal renderer beside it. It reads the page
# Distill wrote, so it needs Lexbor and the line breaker and nothing that
# paints.
distill_manifest_sources(distill-reader DISTILL_READER_SOURCES)
distill_manifest_includes(distill-reader DISTILL_READER_INCLUDES)
add_library(distill-reader STATIC ${DISTILL_READER_SOURCES})
target_include_directories(distill-reader PUBLIC ${DISTILL_READER_INCLUDES})
target_link_directories(distill-reader PUBLIC "${EXTERNAL_PREFIX}/lib")
target_link_libraries(distill-reader PUBLIC lexbor distill-shaper)

add_executable(distill-reader-cells-test tests/reader_cells_test.c)
target_link_libraries(distill-reader-cells-test PRIVATE distill-reader)
set_target_properties(distill-reader-cells-test PROPERTIES BUILD_RPATH "${EXTERNAL_PREFIX}/lib")
distill_enable_assertions(distill-reader-cells-test)
add_test(NAME distill-reader-cells COMMAND distill-reader-cells-test)

# DISTILL_READER: this build produces the reader page -- the template, the
# Markdown export, media fetches. DISTILL_RENDER: it can also lay that page
# out and paint it. The first does not need the second, and a terminal
# front end is exactly the build that has one without the other.
#
# The engine is a library (src/engine.h): the distill command is a thin
# wrapper around it, and an in-process host runs the same library on a
# thread of its own.
#
# The engine's own JavaScript -- linkedom, Readability and its parser -- is
# compiled to QuickJS bytecode at build time by distill-jsc, a build-machine
# tool linked against the same libqjs, and embedded.
distill_manifest_sources(distill-jsc DISTILL_JSC_SOURCES)
distill_manifest_includes(distill-jsc DISTILL_JSC_INCLUDES)
add_executable(distill-jsc ${DISTILL_JSC_SOURCES})
target_include_directories(distill-jsc PRIVATE ${DISTILL_JSC_INCLUDES})
target_link_directories(distill-jsc PRIVATE "${EXTERNAL_PREFIX}/lib")
target_link_libraries(distill-jsc PRIVATE qjs m)
set_target_properties(distill-jsc PROPERTIES BUILD_RPATH "${EXTERNAL_PREFIX}/lib")
set(DISTILL_JS_LINKEDOM "${CMAKE_SOURCE_DIR}/external/js/linkedom/node_modules/linkedom/worker.js")
set(DISTILL_JS_READABILITY_DIR "${CMAKE_SOURCE_DIR}/external/src/readability")
set(DISTILL_EMBEDDED_JS "${CMAKE_BINARY_DIR}/generated/embedded_js.c")
add_custom_command(
    OUTPUT "${DISTILL_EMBEDDED_JS}"
    COMMAND distill-jsc "${DISTILL_EMBEDDED_JS}"
        distill_js_linkedom module "${DISTILL_JS_LINKEDOM}" linkedom/worker.js
        distill_js_jsdomparser script "${DISTILL_JS_READABILITY_DIR}/JSDOMParser.js" readability/JSDOMParser.js
        distill_js_readability script "${DISTILL_JS_READABILITY_DIR}/Readability.js" readability/Readability.js
    DEPENDS distill-jsc "${DISTILL_JS_LINKEDOM}"
        "${DISTILL_JS_READABILITY_DIR}/JSDOMParser.js" "${DISTILL_JS_READABILITY_DIR}/Readability.js"
    COMMENT "Compiling linkedom and Readability to QuickJS bytecode"
    VERBATIM)
set_source_files_properties("${DISTILL_EMBEDDED_JS}" PROPERTIES GENERATED TRUE)

distill_manifest_sources(distill-engine DISTILL_ENGINE_SOURCES)
distill_manifest_defines(distill-engine DISTILL_ENGINE_DEFINES)
distill_manifest_includes(distill-engine DISTILL_ENGINE_INCLUDES)
add_library(distill-engine STATIC ${DISTILL_ENGINE_SOURCES} "${DISTILL_EMBEDDED_JS}")
target_compile_definitions(distill-engine PRIVATE ${DISTILL_ENGINE_DEFINES})
target_include_directories(distill-engine PUBLIC ${DISTILL_ENGINE_INCLUDES} PRIVATE
    ${CURL_INCLUDE_DIRS}
)
target_link_directories(distill-engine PUBLIC "${EXTERNAL_PREFIX}/lib")
target_link_libraries(distill-engine PUBLIC
    qjs
    lexbor
    CURL::libcurl
    Threads::Threads
    distill-image
    distill-reader
    distill-measure
    distill-version
    ${CMAKE_DL_LIBS}
    m
)
if(DISTILL_HAVE_RENDERER)
    target_compile_definitions(distill-engine PRIVATE DISTILL_RENDER=1)
    target_link_libraries(distill-engine PUBLIC distill-layout distill-render-core distill-paint)
endif()

distill_manifest_sources(distill-command DISTILL_COMMAND_SOURCES)
distill_manifest_includes(distill-command DISTILL_COMMAND_INCLUDES)
add_executable(distill ${DISTILL_COMMAND_SOURCES})
target_include_directories(distill PRIVATE ${DISTILL_COMMAND_INCLUDES})
target_link_libraries(distill PRIVATE distill-engine)

# The engine in-process, many navigations in one process, each on a thread
# with its own stack: what `check.py --in-process` drives.
distill_manifest_sources(distill-host DISTILL_HOST_SOURCES)
distill_manifest_includes(distill-host DISTILL_HOST_INCLUDES)
add_executable(distill-host ${DISTILL_HOST_SOURCES})
target_include_directories(distill-host PRIVATE ${DISTILL_HOST_INCLUDES})
target_link_libraries(distill-host PRIVATE distill-engine)
set_target_properties(distill-host PROPERTIES BUILD_RPATH "${EXTERNAL_PREFIX}/lib")
set_target_properties(distill PROPERTIES
    BUILD_RPATH "${EXTERNAL_PREFIX}/lib"
    INSTALL_RPATH "${EXTERNAL_PREFIX}/lib"
)

install(TARGETS distill RUNTIME DESTINATION bin)

# The terminal front end. termbox2 is a vendored single header, like
# miniaudio, pinned to a commit rather than a release: v2.5.0 measures cell
# widths with libc's locale-dependent wcwidth(), and the built-in tables on
# master are what a reader whose output is gated needs.
#   termbox2 master cdf62e9990d8b200768780080fb10a4e2f680051 (2026-09-03)
#   sha256 edfa22b227c1a82a4a33ffcd1699b2903a7bff46075a210be2e8849127c63f62
set(DISTILL_TERMBOX_DIR "${CMAKE_SOURCE_DIR}/external/src/termbox2")
if(EXISTS "${DISTILL_TERMBOX_DIR}/termbox2.h")
    distill_manifest_sources(distill-termbox DISTILL_TERMBOX_SOURCES)
    distill_manifest_includes(distill-termbox DISTILL_TERMBOX_INCLUDES)
    add_library(distill-termbox STATIC ${DISTILL_TERMBOX_SOURCES})
    target_include_directories(distill-termbox PUBLIC ${DISTILL_TERMBOX_INCLUDES})
    target_compile_options(distill-termbox PRIVATE -w)

    distill_manifest_sources(distill-tui-core DISTILL_TUI_CORE_SOURCES)
    distill_manifest_includes(distill-tui-core DISTILL_TUI_CORE_INCLUDES)
    add_library(distill-tui-core STATIC ${DISTILL_TUI_CORE_SOURCES})
    target_include_directories(distill-tui-core PUBLIC ${DISTILL_TUI_CORE_INCLUDES})
    target_link_libraries(distill-tui-core PUBLIC distill-reader distill-image)

    distill_manifest_sources(distill-tui DISTILL_TUI_SOURCES)
    distill_manifest_includes(distill-tui DISTILL_TUI_INCLUDES)
    add_executable(distill-tui ${DISTILL_TUI_SOURCES})
    target_include_directories(distill-tui PRIVATE ${DISTILL_TUI_INCLUDES})
    target_link_libraries(distill-tui PRIVATE
        distill-tui-core
        distill-shell
        distill-media-audio
        distill-termbox
        distill-measure
        distill-version
    )
    set_target_properties(distill-tui PROPERTIES
        BUILD_RPATH "${EXTERNAL_PREFIX}/lib"
        INSTALL_RPATH "${EXTERNAL_PREFIX}/lib"
    )
    install(TARGETS distill-tui RUNTIME DESTINATION bin)
else()
    message(STATUS "termbox2 not found under external/src/termbox2; skipping distill-tui")
endif()
# The framebuffer shell (FRAMEBUFFER_PLAN.md). distill-fb is the target: the
# console sink, the memory sink the gates drive, and nothing that links GLib,
# Cairo or X11. distill-fb-x11 is the development build, the same sources
# with a window sink. distill-fb-defect is the stride gate's negative control:
# its blit ignores both strides, and check.py --fb must see it.
if(DISTILL_HAVE_RENDERER)
    distill_manifest_sources(distill-fb DISTILL_FB_SOURCES)
    distill_manifest_includes(distill-fb DISTILL_FB_INCLUDES)
    set(DISTILL_FB_LIBRARIES
        distill-shell
        distill-layout
        distill-render-core
        distill-paint
        distill-image
        distill-media-audio
        distill-measure
        distill-version
        m
    )
    add_executable(distill-fb ${DISTILL_FB_SOURCES})
    target_include_directories(distill-fb PRIVATE ${DISTILL_FB_INCLUDES})
    target_link_libraries(distill-fb PRIVATE ${DISTILL_FB_LIBRARIES})
    set_target_properties(distill-fb PROPERTIES
        BUILD_RPATH "${EXTERNAL_PREFIX}/lib"
        INSTALL_RPATH "${EXTERNAL_PREFIX}/lib"
    )
    install(TARGETS distill-fb RUNTIME DESTINATION bin)

    add_executable(distill-fb-defect ${DISTILL_FB_SOURCES})
    target_include_directories(distill-fb-defect PRIVATE ${DISTILL_FB_INCLUDES})
    target_compile_definitions(distill-fb-defect PRIVATE DISTILL_DISPLAY_DEFECT=1)
    target_link_libraries(distill-fb-defect PRIVATE ${DISTILL_FB_LIBRARIES})
    set_target_properties(distill-fb-defect PROPERTIES BUILD_RPATH "${EXTERNAL_PREFIX}/lib")

    pkg_check_modules(DISTILL_X11 IMPORTED_TARGET x11 xext)
    if(DISTILL_X11_FOUND)
        add_executable(distill-fb-x11 ${DISTILL_FB_SOURCES}
            src/platform/${DISTILL_PLATFORM}/display_x11.c)
        target_include_directories(distill-fb-x11 PRIVATE ${DISTILL_FB_INCLUDES})
        target_compile_definitions(distill-fb-x11 PRIVATE DISTILL_DISPLAY_X11=1)
        target_compile_options(distill-fb-x11 PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wno-pedantic>)
        target_link_libraries(distill-fb-x11 PRIVATE ${DISTILL_FB_LIBRARIES} PkgConfig::DISTILL_X11)
        set_target_properties(distill-fb-x11 PROPERTIES
            BUILD_RPATH "${EXTERNAL_PREFIX}/lib"
            INSTALL_RPATH "${EXTERNAL_PREFIX}/lib"
        )
        install(TARGETS distill-fb-x11 RUNTIME DESTINATION bin)
    else()
        message(STATUS "libX11 or libXext not found; skipping the distill-fb-x11 development build")
    endif()
endif()

install(DIRECTORY profiles/ DESTINATION ${CMAKE_INSTALL_DATADIR}/distill/profiles
        FILES_MATCHING PATTERN "*.json")
install(FILES share/welcome.html DESTINATION ${CMAKE_INSTALL_DATADIR}/distill)
# The bundled faces (SIL OFL, with their licence), so an installed reader
# draws in the same type as a build tree.
install(DIRECTORY external/fonts/ DESTINATION ${CMAKE_INSTALL_DATADIR}/distill/fonts)

# The GTK desktop shell. GLib is its platform -- it spawns, watches and
# files through GLib -- so it lives with the Linux backends rather than in the
# portable core. Whether it stays is an open decision (muroxi_compat.md).
if(DISTILL_HAVE_RENDERER AND DISTILL_GTK_FOUND)
    distill_manifest_sources(distill-gtk DISTILL_GTK_SOURCES)
    distill_manifest_includes(distill-gtk DISTILL_GTK_INCLUDES)
    add_executable(distill-gtk ${DISTILL_GTK_SOURCES})
    target_compile_options(distill-gtk PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wno-pedantic>)
    target_include_directories(distill-gtk PRIVATE
        ${DISTILL_GTK_INCLUDES}
    )
    target_link_directories(distill-gtk PRIVATE "${EXTERNAL_PREFIX}/lib")
    target_link_libraries(distill-gtk PRIVATE
        distill-media-audio
        distill-shell
        distill-layout
        distill-render-core
        distill-paint
        distill-version
        PkgConfig::DISTILL_GTK
    )
    set_target_properties(distill-gtk PROPERTIES
        BUILD_RPATH "${EXTERNAL_PREFIX}/lib"
        INSTALL_RPATH "${EXTERNAL_PREFIX}/lib"
    )
    install(TARGETS distill-gtk RUNTIME DESTINATION bin)
else()
    message(STATUS "GTK3 or the renderer not found; skipping optional distill-gtk desktop shell")
endif()

distill_manifest_sources(distill-arm-b DISTILL_ARM_B_SOURCES)
distill_manifest_includes(distill-arm-b DISTILL_ARM_B_INCLUDES)
add_executable(distill-arm-b ${DISTILL_ARM_B_SOURCES} "${DISTILL_EMBEDDED_JS}")
target_compile_definitions(distill-arm-b PRIVATE DISTILL_NO_EXTRACT=1)
target_include_directories(distill-arm-b PRIVATE ${DISTILL_ARM_B_INCLUDES} ${CURL_INCLUDE_DIRS})
target_link_directories(distill-arm-b PRIVATE "${EXTERNAL_PREFIX}/lib")
target_link_libraries(distill-arm-b PRIVATE qjs CURL::libcurl distill-measure distill-version Threads::Threads ${CMAKE_DL_LIBS} m)
set_target_properties(distill-arm-b PROPERTIES
    BUILD_RPATH "${EXTERNAL_PREFIX}/lib"
    INSTALL_RPATH "${EXTERNAL_PREFIX}/lib"
)
install(TARGETS distill-arm-b RUNTIME DESTINATION bin)
