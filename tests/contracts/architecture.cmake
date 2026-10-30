get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Shared libraries must not acquire frontend dependencies or executable startup.
file(GLOB_RECURSE library_sources "${root}/src/*.cc" "${root}/src/*.h")
list(FILTER library_sources EXCLUDE REGEX "/src/ui/")
foreach(source IN LISTS library_sources)
  file(READ "${source}" content)
  if(content MATCHES "#[ \t]*include[ \t]*[<\"]((\\.\\./)+)?ui/")
    message(FATAL_ERROR "Reusable library depends on terminal UI: ${source}")
  endif()
  if(content MATCHES "(int|auto)[ \t\r\n]+main[ \t\r\n]*\\(")
    message(FATAL_ERROR "Executable entrypoint in reusable library: ${source}")
  endif()
endforeach()
file(READ "${root}/src/CMakeLists.txt" library_build)
if(library_build MATCHES "add_executable[ \t]*\\(" OR library_build MATCHES "manbo_terminal_ui")
  message(FATAL_ERROR "Library targets must not define applications or link terminal views")
endif()

# Native APIs stay in the two platform implementations, including in tests.
file(GLOB_RECURSE all_sources "${root}/src/*.cc" "${root}/src/*.h"
  "${root}/app/*.cc" "${root}/app/*.h" "${root}/tests/*.cc" "${root}/tests/*.h")
foreach(source IN LISTS all_sources)
  file(READ "${source}" content)
  if(content MATCHES "#[ \t]*include[ \t]*[<\"](windows[.]h|unistd[.]h|termios[.]h|poll[.]h|fcntl[.]h|signal[.]h|sys/(eventfd|ioctl)[.]h)[>\"]"
      AND NOT source MATCHES "/src/platform/(windows|linux)[.]cc$")
    message(FATAL_ERROR "Native OS header escaped its platform adapter: ${source}")
  endif()
  if(NOT source MATCHES "/tests/" AND NOT source STREQUAL "${root}/app/manbo/main.cc"
      AND content MATCHES "(int|auto)[ \t\r\n]+main[ \t\r\n]*\\(")
    message(FATAL_ERROR "Executable entrypoint outside application startup: ${source}")
  endif()
endforeach()
message(STATUS "Library, application, UI, and native API boundaries passed")
