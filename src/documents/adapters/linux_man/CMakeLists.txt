add_library(manbo_adapter_linux_man adapter.cc)
target_include_directories(manbo_adapter_linux_man PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(manbo_adapter_linux_man
  PUBLIC tl::expected
  PRIVATE manbo_adapter_html manbo_adapter_plain)
manbo_warnings(manbo_adapter_linux_man)
