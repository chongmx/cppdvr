#pragma once

// DLL import/export macro
#ifdef _WIN32
#  ifdef CPPDVR_BUILDING_DLL
#    define CPPDVR_API __declspec(dllexport)
#  else
#    define CPPDVR_API __declspec(dllimport)
#  endif
#else
#  define CPPDVR_API __attribute__((visibility("default")))
#endif
