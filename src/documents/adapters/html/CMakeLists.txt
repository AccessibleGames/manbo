add_library(manbo_adapter_html adapter.cc)
target_include_directories(manbo_adapter_html PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(manbo_adapter_html
  PUBLIC tl::expected
         LibXml2::LibXml2)
manbo_warnings(manbo_adapter_html)
