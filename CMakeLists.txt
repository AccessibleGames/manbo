cmake_minimum_required(VERSION 3.25)
project(manbo VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(MANBO_HEADLESS_ONLY "Use only the OS-header-free console backend" OFF)
option(MANBO_BUILD_APP "Build the manbo command-line application" ON)
option(MANBO_BUILD_TERMINAL_UI "Build the terminal frontend library" ON)
include(CTest)

find_package(tl-expected CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(Threads REQUIRED)
# Conda's headers-only Boost package has no BoostConfig.cmake.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()
set(Boost_NO_WARN_NEW_VERSIONS ON)
find_package(Boost 1.86 REQUIRED)
find_package(LibXml2 REQUIRED)

function(manbo_warnings target)
  target_link_libraries(${target} PRIVATE fmt::fmt)
  if(UNIX AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # The Conda Clang binary is named clang-22 (not clang++), so its linker
    # driver does not add the C++ runtime automatically. Prefer Conda's runtime
    # over the compiler-bundled GCC 11 copy, which can be older than Conda ICU.
    if(DEFINED ENV{CONDA_PREFIX})
      find_library(MANBO_CONDA_STDCXX
        NAMES stdc++ PATHS "$ENV{CONDA_PREFIX}/lib" NO_DEFAULT_PATH)
    endif()
    if(MANBO_CONDA_STDCXX)
      target_link_libraries(${target} PRIVATE "${MANBO_CONDA_STDCXX}")
    else()
      target_link_libraries(${target} PRIVATE stdc++)
    endif()
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# Core/runtime/platform libraries do not depend on app/ or src/ui/.
add_subdirectory(src)
if(MANBO_BUILD_TERMINAL_UI)
  add_subdirectory(src/ui/terminal)
endif()
if(MANBO_BUILD_APP)
  if(NOT MANBO_BUILD_TERMINAL_UI)
    message(FATAL_ERROR "The manbo executable needs MANBO_BUILD_TERMINAL_UI; disable MANBO_BUILD_APP for a library-only build")
  endif()
  add_subdirectory(app/manbo)
endif()

# CMake writes the preset-specific database during generation. Refresh the
# ignored root copy explicitly, so library-only/other builds do not silently
# switch the editor away from its selected application configuration.
if(CMAKE_EXPORT_COMPILE_COMMANDS AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
  add_custom_target(compile-commands
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_BINARY_DIR}/compile_commands.json"
      "${PROJECT_SOURCE_DIR}/compile_commands.json"
    COMMENT "Refreshing the project-root editor compilation database" VERBATIM)
endif()

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
  file(GLOB_RECURSE MANBO_STYLE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h")
  add_custom_target(format-check
    COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${MANBO_STYLE_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking project Google-based C++ formatting" VERBATIM)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
