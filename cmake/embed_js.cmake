# Turns src/js/*.js into a C++ file with the sources as string literals.
# Usage: cmake -DOUT=<file.cpp> -DINPUTS=<a.js|b.js> -P embed_js.cmake
string(REPLACE "|" ";" INPUT_LIST "${INPUTS}")
set(code "// Generated from src/js/*.js — do not edit.\n#include \"embedded_js.hpp\"\n\nnamespace rtn {\n\nconst EmbeddedJs kEmbeddedJs[] = {\n")
foreach(file IN LISTS INPUT_LIST)
    file(READ "${file}" js)
    get_filename_component(name "${file}" NAME)
    string(APPEND code "    {\"${name}\", R\"RTNJS(${js})RTNJS\"},\n")
endforeach()
string(APPEND code "};\n\nconst std::size_t kEmbeddedJsCount = sizeof(kEmbeddedJs) / sizeof(kEmbeddedJs[0]);\n\n}  // namespace rtn\n")
file(WRITE "${OUT}" "${code}")
