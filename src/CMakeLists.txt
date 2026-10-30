# Reusable libraries only. Do not add executable entrypoints or frontend code.
add_library(manbo_core core/browser.cc core/command.cc)
target_include_directories(manbo_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(manbo_core PUBLIC tl::expected)

add_subdirectory(documents/adapters)

add_library(manbo_content documents/content.cc)
target_include_directories(manbo_content PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(manbo_content
  PUBLIC tl::expected
  PRIVATE
    manbo_adapter_plain
    manbo_adapter_cppreference
    manbo_adapter_posix
    manbo_adapter_gnu
    manbo_adapter_linux_man)

add_library(manbo_samples repositories/sample_repository.cc)
target_link_libraries(manbo_samples PUBLIC manbo_core manbo_content)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/docs/manbo.1.txt")
file(READ "${PROJECT_SOURCE_DIR}/docs/manbo.1.txt" MANBO_MANUAL_TEXT)
configure_file(repositories/manual_data.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/manual_data.h" @ONLY)
add_library(manbo_manual repositories/manual_repository.cc)
target_include_directories(manbo_manual PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(manbo_manual PUBLIC manbo_core manbo_content)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
add_library(manbo_offline
  repositories/offline_repository.cc
  repositories/catalog_record.cc
  repositories/link_target.cc
  repositories/catalog_index.cc)
target_link_libraries(manbo_offline PUBLIC manbo_core manbo_content
  PRIVATE nlohmann_json::nlohmann_json SQLite::SQLite3 LibXml2::LibXml2)
if(UNIX)
  target_compile_options(manbo_offline PRIVATE "SHELL:-idirafter /usr/include")
endif()

find_package(OpenSSL REQUIRED COMPONENTS Crypto)
add_library(manbo_library library/library.cc)
target_link_libraries(manbo_library PUBLIC manbo_offline
  PRIVATE nlohmann_json::nlohmann_json LibXml2::LibXml2 OpenSSL::Crypto)

if(WIN32 AND NOT MANBO_HEADLESS_ONLY)
  add_library(manbo_platform platform/windows.cc)
  target_compile_definitions(manbo_platform PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
elseif(UNIX AND NOT MANBO_HEADLESS_ONLY)
  add_library(manbo_platform platform/linux.cc)
else()
  add_library(manbo_platform platform/portable.cc)
endif()
target_include_directories(manbo_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(manbo_platform PUBLIC tl::expected)

add_library(manbo_runtime runtime/channel.cc runtime/jobs.cc runtime/session.cc)
target_link_libraries(manbo_runtime
  PUBLIC manbo_core manbo_platform Boost::headers
  PRIVATE Threads::Threads)

# Application-facing convenience target; no executable or frontend dependency.
add_library(manbo_api INTERFACE)
add_library(manbo::api ALIAS manbo_api)
target_link_libraries(manbo_api INTERFACE
  manbo_runtime manbo_content manbo_samples manbo_manual manbo_offline manbo_library)

foreach(target manbo_core manbo_content manbo_samples manbo_manual manbo_offline manbo_library manbo_platform manbo_runtime)
  target_compile_features(${target} PUBLIC cxx_std_20)
  manbo_warnings(${target})
endforeach()
