cmake_minimum_required(VERSION 3.20)
project(distill LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(EXTERNAL_PREFIX "${CMAKE_SOURCE_DIR}/external/install" CACHE PATH "Distill external dependency prefix")

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

add_executable(distill src/main.cpp)
target_include_directories(distill PRIVATE
    "${EXTERNAL_PREFIX}/include"
    ${CURL_INCLUDE_DIRS}
)
target_link_directories(distill PRIVATE "${EXTERNAL_PREFIX}/lib")
target_link_libraries(distill PRIVATE
    qjs
    lexbor
    CURL::libcurl
    Threads::Threads
    ${CMAKE_DL_LIBS}
    m
)
set_target_properties(distill PROPERTIES
    BUILD_RPATH "${EXTERNAL_PREFIX}/lib"
    INSTALL_RPATH "${EXTERNAL_PREFIX}/lib"
)

install(TARGETS distill RUNTIME DESTINATION bin)

add_executable(distill-arm-b src/arm_b.cpp)
target_include_directories(distill-arm-b PRIVATE "${EXTERNAL_PREFIX}/include" ${CURL_INCLUDE_DIRS})
target_link_directories(distill-arm-b PRIVATE "${EXTERNAL_PREFIX}/lib")
target_link_libraries(distill-arm-b PRIVATE qjs CURL::libcurl Threads::Threads ${CMAKE_DL_LIBS} m)
set_target_properties(distill-arm-b PROPERTIES
    BUILD_RPATH "${EXTERNAL_PREFIX}/lib"
    INSTALL_RPATH "${EXTERNAL_PREFIX}/lib"
)
install(TARGETS distill-arm-b RUNTIME DESTINATION bin)

add_executable(distill-arm-b-pruned src/arm_b.cpp)
target_compile_definitions(distill-arm-b-pruned PRIVATE DISTILL_B_PRUNED=1)
target_include_directories(distill-arm-b-pruned PRIVATE "${EXTERNAL_PREFIX}/include" ${CURL_INCLUDE_DIRS})
target_link_directories(distill-arm-b-pruned PRIVATE "${EXTERNAL_PREFIX}/lib")
target_link_libraries(distill-arm-b-pruned PRIVATE qjs CURL::libcurl litehtml gumbo Threads::Threads ${CMAKE_DL_LIBS} m)
set_target_properties(distill-arm-b-pruned PROPERTIES
    BUILD_RPATH "${EXTERNAL_PREFIX}/lib"
    INSTALL_RPATH "${EXTERNAL_PREFIX}/lib"
)
install(TARGETS distill-arm-b-pruned RUNTIME DESTINATION bin)
