find_package(GTest REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB existing_tests CONFIGURE_DEPENDS ../src/uran_core_framework/test/test_*.cpp)
add_executable(uran_tests ${existing_tests})
target_link_libraries(uran_tests PRIVATE uran::uran_core_framework GTest::gtest_main)
if(URAN_BUILD_NATIVE)
  target_sources(uran_tests PRIVATE
    ../src/uran_core_example_plugins/test/test_loopback_multi_instance.cpp
    test_runtime.cpp test_platform.cpp test_native_loader.cpp test_storage.cpp)
  target_link_libraries(uran_tests PRIVATE uran::uran_platform_native uran::uran_core_example_plugins)
endif()
add_test(NAME uran_components COMMAND uran_tests)
set_tests_properties(uran_components PROPERTIES TIMEOUT 60)
add_test(NAME uran_platform_boundary COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/test_platform_boundary.py ${PROJECT_SOURCE_DIR})
if(NOT URAN_BUILD_NATIVE)
  return()
endif()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/plugin.json" CONTENT
  "{\"schema_version\":1,\"package\":\"uran_loopback\",\"version\":\"2.0.0\",\"api_version\":\"2.0\",\"plugins\":[{\"class_type\":\"uran_core_example_plugins/LoopbackProtocolPlugin\",\"library\":\"$<TARGET_FILE:uran_core_example_plugins>\"}]}")
target_compile_definitions(uran_tests PRIVATE URAN_TEST_MANIFEST="${CMAKE_CURRENT_BINARY_DIR}/plugin.json")
add_test(NAME uran_native_host COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_native_host.py
  $<TARGET_FILE:uran_runtime> ${CMAKE_CURRENT_BINARY_DIR}/plugin.json ${PROJECT_SOURCE_DIR}/config/loopback.yaml)
set_tests_properties(uran_native_host PROPERTIES TIMEOUT 30)
add_test(NAME uran_plugin_install COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_plugin_install.py
  ${PROJECT_SOURCE_DIR}/scripts/uran_plugin.py $<TARGET_FILE:uran_runtime> $<TARGET_FILE:uran_core_example_plugins>
  ${PROJECT_BINARY_DIR} ${PROJECT_SOURCE_DIR}/examples/extension)
set_tests_properties(uran_plugin_install PROPERTIES TIMEOUT 60)
