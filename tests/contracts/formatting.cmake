get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT CLANG_FORMAT_EXECUTABLE)
  find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format REQUIRED)
endif()
execute_process(
  COMMAND "${CLANG_FORMAT_EXECUTABLE}" --style=file
    "--assume-filename=${root}/tests/fixtures/formatting/layout.cc"
  INPUT_FILE "${root}/tests/fixtures/formatting/layout.input"
  RESULT_VARIABLE code OUTPUT_VARIABLE actual ERROR_VARIABLE error
  WORKING_DIRECTORY "${root}")
if(NOT code EQUAL 0)
  message(FATAL_ERROR "Formatting example failed: ${error}")
endif()
if(NOT actual MATCHES "auto[ \t\r\n]+Function\\(T documented_value")
  message(FATAL_ERROR "Function names must start below the return/specifier prefix:\n${actual}")
endif()
if(NOT actual MATCHES "int[ \t\r\n]+FunctionDeclaration\\(")
  message(FATAL_ERROR "Function declarations must break after the return type:\n${actual}")
endif()
if(NOT actual MATCHES "Bar\\(1, 2\\);")
  message(FATAL_ERROR "Short calls should remain compact:\n${actual}")
endif()
if(NOT actual MATCHES "Bar\\(first_argument_with_context,[ \t\r\n]+second_argument_with_context,[ \t\r\n]+third_argument_with_context,[ \t\r\n]+fourth_argument_with_context\\);")
  message(FATAL_ERROR "Long argument lists should be easy to scan:\n${actual}")
endif()
if(NOT MANBO_TEST_OUTPUT_DIR)
  set(MANBO_TEST_OUTPUT_DIR "${root}/build/format-check")
endif()
file(MAKE_DIRECTORY "${MANBO_TEST_OUTPUT_DIR}")
set(probe "${MANBO_TEST_OUTPUT_DIR}/format-layout.cc")
file(WRITE "${probe}" "${actual}")
execute_process(
  COMMAND "${CLANG_FORMAT_EXECUTABLE}" --style=file
    "--assume-filename=${root}/tests/fixtures/formatting/layout.cc"
  INPUT_FILE "${probe}"
  RESULT_VARIABLE code OUTPUT_VARIABLE second ERROR_VARIABLE error
  WORKING_DIRECTORY "${root}")
if(NOT code EQUAL 0 OR NOT second STREQUAL actual)
  message(FATAL_ERROR "Project formatting must be idempotent: ${error}")
endif()
message(STATUS "Version-compatible formatting layout and idempotence passed")
