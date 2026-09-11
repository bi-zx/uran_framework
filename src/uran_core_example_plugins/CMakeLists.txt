cmake_minimum_required(VERSION 3.8)
project(uran_core_example_plugins)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

find_package(ament_cmake REQUIRED)
find_package(pluginlib REQUIRED)
find_package(rclcpp REQUIRED)
find_package(uran_plugin_api REQUIRED)
find_package(yaml_cpp_vendor REQUIRED)
find_package(yaml-cpp REQUIRED)

add_library(${PROJECT_NAME} SHARED
  src/loopback_protocol_plugin.cpp
)

target_include_directories(${PROJECT_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

ament_target_dependencies(${PROJECT_NAME}
  pluginlib
  rclcpp
  uran_plugin_api
)
target_link_libraries(${PROJECT_NAME} yaml-cpp)

pluginlib_export_plugin_description_file(
  uran_plugin_api
  plugin_description.xml
)

install(
  DIRECTORY include/
  DESTINATION include
)

install(
  TARGETS ${PROJECT_NAME}
  EXPORT export_${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

ament_export_targets(export_${PROJECT_NAME} HAS_LIBRARY_TARGET)
ament_export_dependencies(pluginlib rclcpp uran_plugin_api yaml_cpp_vendor)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(uran_core_framework REQUIRED)

  ament_add_gtest(
    test_loopback_multi_instance
    test/test_loopback_multi_instance.cpp
  )
  target_link_libraries(test_loopback_multi_instance ${PROJECT_NAME})
  ament_target_dependencies(
    test_loopback_multi_instance
    rclcpp
    uran_core_framework
    uran_plugin_api
  )
endif()

ament_package()
