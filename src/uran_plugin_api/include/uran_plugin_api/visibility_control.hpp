#pragma once

// 统一控制共享库符号导出
#if defined _WIN32 || defined __CYGWIN__
  #ifdef __GNUC__
    #define URAN_PLUGIN_API_EXPORT __attribute__((dllexport))
    #define URAN_PLUGIN_API_IMPORT __attribute__((dllimport))
  #else
    #define URAN_PLUGIN_API_EXPORT __declspec(dllexport)
    #define URAN_PLUGIN_API_IMPORT __declspec(dllimport)
  #endif
  #ifdef URAN_PLUGIN_API_BUILDING_DLL
    #define URAN_PLUGIN_API_PUBLIC URAN_PLUGIN_API_EXPORT
  #else
    #define URAN_PLUGIN_API_PUBLIC URAN_PLUGIN_API_IMPORT
  #endif
  #define URAN_PLUGIN_API_LOCAL
#else
  #define URAN_PLUGIN_API_EXPORT __attribute__((visibility("default")))
  #define URAN_PLUGIN_API_IMPORT
  #if __GNUC__ >= 4
    #define URAN_PLUGIN_API_PUBLIC __attribute__((visibility("default")))
    #define URAN_PLUGIN_API_LOCAL __attribute__((visibility("hidden")))
  #else
    #define URAN_PLUGIN_API_PUBLIC
    #define URAN_PLUGIN_API_LOCAL
  #endif
#endif
