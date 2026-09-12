cmake_minimum_required(VERSION 3.16)
project(uran_core_framework VERSION 2.0.0 LANGUAGES CXX)
include(../../cmake/Package.cmake)
if(NOT TARGET uran::uran_plugin_api)
  find_package(uran_plugin_api 2 REQUIRED CONFIG)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
  find_package(nlohmann_json 3.10 REQUIRED)
endif()
find_package(Threads REQUIRED)
add_library(${PROJECT_NAME} SHARED
  src/capability_registry.cpp src/config_manager.cpp src/message_router.cpp
  src/plugin_manager.cpp src/protocol_gateway.cpp src/route_catalog.cpp
  src/scoped_state_access.cpp src/scoped_platform_services.cpp src/state_store.cpp
  src/runtime.cpp)
uran_library(${PROJECT_NAME} "find_dependency(uran_plugin_api 2)\nfind_dependency(nlohmann_json 3.10)\nfind_dependency(Threads)")
target_compile_definitions(${PROJECT_NAME} PRIVATE URAN_CORE_FRAMEWORK_BUILDING_DLL)
target_link_libraries(${PROJECT_NAME} PUBLIC uran::uran_plugin_api nlohmann_json::nlohmann_json Threads::Threads)
