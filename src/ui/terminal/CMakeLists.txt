add_library(manbo_terminal_ui
  browser_view.cc
  document_view.cc
  text_layout.cc
  table_layout.cc
  search_view.cc
  frame_output.cc
  terminal.cc)
target_link_libraries(manbo_terminal_ui PUBLIC manbo_runtime)
manbo_warnings(manbo_terminal_ui)
