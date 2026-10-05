cmake_minimum_required(VERSION 3.20)
project(RunTimeNow VERSION 1.6.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# GitHub repository that `rtn upgrade` and install.sh download releases from.
set(RTN_REPO "ByteForgeStudioLab/RunTime-Now" CACHE STRING "GitHub owner/repo for releases")
# Release builds (Alpine / musl) link everything statically so one binary runs on any Linux.
option(RTN_STATIC "Link rtn fully statically" OFF)

# --- JS engine: QuickJS-ng (third_party/quickjs) ---
set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/quickjs EXCLUDE_FROM_ALL)

# --- JS parts of the runtime, embedded into the binary ---
set(RTN_JS_FILES
    ${CMAKE_SOURCE_DIR}/src/js/events.js
    ${CMAKE_SOURCE_DIR}/src/js/web.js
    ${CMAKE_SOURCE_DIR}/src/js/fetch.js
    ${CMAKE_SOURCE_DIR}/src/js/crypto.js
    ${CMAKE_SOURCE_DIR}/src/js/modules.js
    ${CMAKE_SOURCE_DIR}/src/js/navigator.js
    ${CMAKE_SOURCE_DIR}/src/js/http.js
)
string(REPLACE ";" "|" RTN_JS_ARG "${RTN_JS_FILES}")
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/embedded_js.cpp
    COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_BINARY_DIR}/embedded_js.cpp "-DINPUTS=${RTN_JS_ARG}"
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_js.cmake
    DEPENDS ${RTN_JS_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_js.cmake
    COMMENT "Embedding src/js/*.js"
    VERBATIM
)

# --- rtn: the runtime executable ---
add_executable(rtn
    src/main.cpp
    src/runtime.cpp
    src/repl.cpp
    src/upgrade.cpp
    src/term.cpp
    src/modules.cpp
    src/util.cpp
    src/typescript/strip.cpp
    src/bindings/console.cpp
    src/bindings/timers.cpp
    src/bindings/process.cpp
    src/bindings/fs.cpp
    src/bindings/encoding.cpp
    src/bindings/http.cpp
    src/bindings/fetch.cpp
    src/bindings/crypto.cpp
    src/builtins.cpp
    ${CMAKE_BINARY_DIR}/embedded_js.cpp
)
target_include_directories(rtn PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(rtn PRIVATE qjs Threads::Threads)
target_compile_definitions(rtn PRIVATE RTN_VERSION="${PROJECT_VERSION}" RTN_REPO="${RTN_REPO}")
if(RTN_STATIC)
    target_link_options(rtn PRIVATE -static)
endif()
install(TARGETS rtn RUNTIME DESTINATION bin)
target_compile_options(rtn PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

# --- loadgen: small HTTP/1.1 benchmark tool (cmake --build build --target loadgen) ---
add_executable(loadgen EXCLUDE_FROM_ALL tools/loadgen.cpp)
target_link_libraries(loadgen PRIVATE pthread)
