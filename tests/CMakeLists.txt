# Minimal consumer checks also run when the application and terminal UI are off.
add_executable(manbo_library_smoke integration/library_smoke.cc)
target_link_libraries(manbo_library_smoke PRIVATE manbo::api)
set_target_properties(manbo_library_smoke PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
manbo_warnings(manbo_library_smoke)
add_test(NAME library_smoke COMMAND manbo_library_smoke)
add_test(NAME architecture_contract
  COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_SOURCE_DIR}/contracts/architecture.cmake")

if(MANBO_BUILD_TERMINAL_UI)
  find_package(GTest CONFIG REQUIRED)
  add_executable(manbo_tests
    support/fixtures.cc
    unit/core/browser_test.cc
    unit/core/command_test.cc
    unit/documents/content_test.cc
    unit/repositories/sample_test.cc
    unit/repositories/offline_test.cc
    unit/library/library_test.cc
    unit/runtime/channel_test.cc
    unit/runtime/jobs_test.cc
    unit/runtime/session_test.cc
    unit/ui/layout_test.cc
    unit/ui/terminal_test.cc
    integration/corpus_test.cc
    integration/manual_test.cc
    integration/idle_test.cc)
  if(MANBO_BUILD_APP)
    target_sources(manbo_tests PRIVATE
      integration/repository_setup_test.cc
      "${PROJECT_SOURCE_DIR}/app/manbo/repository_setup.cc")
    target_include_directories(manbo_tests PRIVATE "${PROJECT_SOURCE_DIR}/app/manbo")
  endif()
  target_include_directories(manbo_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(manbo_tests PRIVATE
    manbo_terminal_ui manbo_samples manbo_offline manbo_manual manbo_library
    GTest::gtest_main Threads::Threads)
  target_compile_definitions(manbo_tests PRIVATE
    MANBO_TEST_CORPUS="${PROJECT_SOURCE_DIR}/data/official-samples"
    MANBO_TEST_SOURCE="${PROJECT_SOURCE_DIR}")
  set_target_properties(manbo_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
  manbo_warnings(manbo_tests)
  if(MSVC)
    target_compile_definitions(manbo_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  include(GoogleTest)
  gtest_discover_tests(manbo_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 15)
endif()

if(MANBO_BUILD_APP)
  add_test(NAME cli_contract
    COMMAND ${CMAKE_COMMAND} -DMANBO_EXE=$<TARGET_FILE:manbo>
      -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/cli.cmake")
  set_tests_properties(cli_contract PROPERTIES TIMEOUT 30)
endif()

if(CLANG_FORMAT_EXECUTABLE)
  add_test(NAME format_style_contract
    COMMAND ${CMAKE_COMMAND}
      -DCLANG_FORMAT_EXECUTABLE=${CLANG_FORMAT_EXECUTABLE}
      -DMANBO_TEST_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/contracts
      -P "${CMAKE_CURRENT_SOURCE_DIR}/contracts/formatting.cmake")
endif()
