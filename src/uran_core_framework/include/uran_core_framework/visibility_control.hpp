#pragma once

// 统一控制共享库符号导出
#if defined _WIN32 || defined __CYGWIN__
  #ifdef __GNUC__
    #define URAN_CORE_FRAMEWORK_EXPORT __attribute__((dllexport))
    #define URAN_CORE_FRAMEWORK_IMPORT __attribute__((dllimport))
  #else
    #define URAN_CORE_FRAMEWORK_EXPORT __declspec(dllexport)
    #define URAN_CORE_FRAMEWORK_IMPORT __declspec(dllimport)
  #endif
  #ifdef URAN_CORE_FRAMEWORK_BUILDING_DLL
    #define URAN_CORE_FRAMEWORK_PUBLIC URAN_CORE_FRAMEWORK_EXPORT
  #else
    #define URAN_CORE_FRAMEWORK_PUBLIC URAN_CORE_FRAMEWORK_IMPORT
  #endif
  #define URAN_CORE_FRAMEWORK_LOCAL
#else
  #define URAN_CORE_FRAMEWORK_EXPORT __attribute__((visibility("default")))
  #define URAN_CORE_FRAMEWORK_IMPORT
  #if __GNUC__ >= 4
    #define URAN_CORE_FRAMEWORK_PUBLIC __attribute__((visibility("default")))
    #define URAN_CORE_FRAMEWORK_LOCAL __attribute__((visibility("hidden")))
  #else
    #define URAN_CORE_FRAMEWORK_PUBLIC
    #define URAN_CORE_FRAMEWORK_LOCAL
  #endif
#endif
