cmake_minimum_required(VERSION 3.16)
project(uran_worker_adapter VERSION 1.0.0 LANGUAGES CXX)
include(../../cmake/Package.cmake)
if(NOT TARGET uran::uran_plugin_api)
  find_package(uran_plugin_api 2.1 REQUIRED CONFIG)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
  find_package(nlohmann_json 3.10 REQUIRED)
endif()
find_package(Threads REQUIRED)
add_library(uran_worker_adapter_impl OBJECT src/worker_process_plugin.cpp)
set_target_properties(uran_worker_adapter_impl PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(uran_worker_adapter_impl PRIVATE include)
target_link_libraries(uran_worker_adapter_impl PRIVATE
  uran::uran_plugin_api nlohmann_json::nlohmann_json Threads::Threads)
add_library(${PROJECT_NAME} SHARED
  $<TARGET_OBJECTS:uran_worker_adapter_impl> src/worker_message_export.cpp)
uran_library(${PROJECT_NAME}
  "find_dependency(uran_plugin_api 2.1)\nfind_dependency(nlohmann_json 3.10)\nfind_dependency(Threads)")
target_link_libraries(${PROJECT_NAME} PUBLIC uran::uran_plugin_api nlohmann_json::nlohmann_json Threads::Threads)
add_library(uran_worker_protocol_adapter SHARED
  $<TARGET_OBJECTS:uran_worker_adapter_impl> src/worker_protocol_export.cpp)
uran_library(uran_worker_protocol_adapter
  "find_dependency(uran_plugin_api 2.1)\nfind_dependency(nlohmann_json 3.10)\nfind_dependency(Threads)")
target_link_libraries(uran_worker_protocol_adapter PUBLIC
  uran::uran_plugin_api nlohmann_json::nlohmann_json Threads::Threads)
configure_file(plugin.json.in plugin.json @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/plugin.json"
  DESTINATION share/uran_framework/plugins/worker)
