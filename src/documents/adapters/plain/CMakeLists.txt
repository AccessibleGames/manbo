add_library(manbo_adapter_plain adapter.cc)
target_include_directories(manbo_adapter_plain PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(manbo_adapter_plain
  PUBLIC tl::expected
  PRIVATE manbo_adapter_html)
manbo_warnings(manbo_adapter_plain)
