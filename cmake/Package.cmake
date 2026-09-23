include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
function(uran_library target)
  add_library(uran::${target} ALIAS ${target})
  target_compile_features(${target} PUBLIC cxx_std_17)
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  set_target_properties(${target} PROPERTIES VERSION 2.1.0 SOVERSION 3 INSTALL_RPATH "$ORIGIN")
  install(TARGETS ${target} EXPORT ${target}Targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(EXPORT ${target}Targets NAMESPACE uran:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${target})
  set(config "${CMAKE_CURRENT_BINARY_DIR}/${target}Config.cmake")
  file(WRITE "${config}" "include(CMakeFindDependencyMacro)\n${ARGN}\ninclude(\"\${CMAKE_CURRENT_LIST_DIR}/${target}Targets.cmake\")\n")
  write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/${target}ConfigVersion.cmake"
    VERSION 2.1.0 COMPATIBILITY SameMajorVersion)
  install(FILES "${config}" "${CMAKE_CURRENT_BINARY_DIR}/${target}ConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${target})
endfunction()
