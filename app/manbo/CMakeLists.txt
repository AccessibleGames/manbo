find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(manbo main.cc repository_setup.cc library_commands.cc)
target_link_libraries(manbo PRIVATE manbo::api manbo_terminal_ui CLI11::CLI11 nlohmann_json::nlohmann_json)
if(UNIX)
  target_compile_options(manbo PRIVATE "SHELL:-idirafter /usr/include")
endif()
# Keep documented executable paths stable despite the application source move.
set_target_properties(manbo PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
manbo_warnings(manbo)
