cmake_minimum_required(VERSION 3.20)
project(rentfree VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo)

# ---- wayland protocol code generation ----------------------------------
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

set(PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/protocols")
file(MAKE_DIRECTORY "${PROTO_DIR}")

# Generates <name>-protocol.c and <name>-client-protocol.h from an XML file.
function(wayland_protocol xml name)
  set(src "${PROTO_DIR}/${name}-protocol.c")
  set(hdr "${PROTO_DIR}/${name}-client-protocol.h")
  # wayland-scanner emits `const char *namespace` for layer-shell's
  # get_layer_surface. Legal C, reserved word in C++ -- rename it on the way out.
  add_custom_command(OUTPUT "${hdr}"
    COMMAND ${WAYLAND_SCANNER} client-header "${xml}" "${hdr}"
    COMMAND ${CMAKE_COMMAND} -DFILE=${hdr} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeNamespace.cmake"
    DEPENDS "${xml}" VERBATIM)
  add_custom_command(OUTPUT "${src}"
    COMMAND ${WAYLAND_SCANNER} private-code "${xml}" "${src}"
    DEPENDS "${xml}" VERBATIM)
  set(PROTO_SOURCES ${PROTO_SOURCES} "${src}" "${hdr}" PARENT_SCOPE)
endfunction()

wayland_protocol("${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml"
                 wlr-layer-shell-unstable-v1)
set(ALL_PROTO ${PROTO_SOURCES})
wayland_protocol("${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml" xdg-shell)
list(APPEND ALL_PROTO ${PROTO_SOURCES})

# ---- binary ------------------------------------------------------------
add_executable(rentfree
  src/main.cpp
  src/config.cpp
  src/confedit.cpp
  src/store.cpp
  src/paths.cpp
  src/ipc.cpp
  src/render.cpp
  src/daemon.cpp
  ${ALL_PROTO}
)

target_include_directories(rentfree PRIVATE "${PROTO_DIR}" src)
target_link_libraries(rentfree PRIVATE PkgConfig::WAYLAND PkgConfig::PANGOCAIRO)
target_compile_options(rentfree PRIVATE -Wall -Wextra -Wno-unused-parameter)

include(GNUInstallDirs)
install(TARGETS rentfree RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Shell completions. Each script asks the binary what exists rather than
# hardcoding it, so they don't need regenerating when settings are added.
install(FILES contrib/completions/rentfree.bash
        DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions
        RENAME rentfree)
install(FILES contrib/completions/_rentfree
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zsh/site-functions)
install(FILES contrib/completions/rentfree.fish
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fish/vendor_completions.d)
