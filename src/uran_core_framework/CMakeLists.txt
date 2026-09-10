cmake_minimum_required(VERSION 3.8)
project(uran_core_framework)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

find_package(ament_cmake REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(pluginlib REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(std_srvs REQUIRED)
find_package(uran_plugin_api REQUIRED)
find_package(yaml_cpp_vendor REQUIRED)
find_package(yaml-cpp REQUIRED)

add_library(${PROJECT_NAME} SHARED
  src/config_manager.cpp
  src/message_router.cpp
  src/plugin_manager.cpp
  src/protocol_gateway.cpp
  src/route_catalog.cpp
  src/scoped_state_access.cpp
  src/state_store.cpp
  src/runtime_node.cpp
)

target_compile_definitions(${PROJECT_NAME}
  PRIVATE URAN_CORE_FRAMEWORK_BUILDING_DLL
)
set_target_properties(${PROJECT_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
)

target_include_directories(${PROJECT_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

ament_target_dependencies(${PROJECT_NAME}
  pluginlib
  rclcpp
  std_msgs
  std_srvs
  uran_plugin_api
)
target_link_libraries(${PROJECT_NAME} yaml-cpp nlohmann_json::nlohmann_json)

add_executable(uran_runtime_node src/main.cpp)
target_link_libraries(uran_runtime_node ${PROJECT_NAME})
ament_target_dependencies(uran_runtime_node rclcpp)

install(
  DIRECTORY include/
  DESTINATION include
)

install(
  TARGETS ${PROJECT_NAME} uran_runtime_node
  EXPORT export_${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION lib/${PROJECT_NAME}
)

install(
  DIRECTORY config launch
  DESTINATION share/${PROJECT_NAME}
)

ament_export_targets(export_${PROJECT_NAME} HAS_LIBRARY_TARGET)
ament_export_dependencies(
  nlohmann_json
  pluginlib
  rclcpp
  std_msgs
  std_srvs
  uran_plugin_api
  yaml_cpp_vendor
)
ament_export_include_directories(include)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)

  ament_add_gtest(test_config_manager test/test_config_manager.cpp)
  target_link_libraries(test_config_manager ${PROJECT_NAME})

  ament_add_gtest(test_state_store test/test_state_store.cpp)
  target_link_libraries(test_state_store ${PROJECT_NAME})

  ament_add_gtest(test_route_catalog test/test_route_catalog.cpp)
  target_link_libraries(test_route_catalog ${PROJECT_NAME})

  ament_add_gtest(test_message_router test/test_message_router.cpp)
  target_link_libraries(test_message_router ${PROJECT_NAME})

  ament_add_gtest(test_protocol_gateway test/test_protocol_gateway.cpp)
  target_link_libraries(test_protocol_gateway ${PROJECT_NAME})
endif()

ament_package()
