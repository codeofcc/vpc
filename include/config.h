#ifndef CONFIG_H
#define CONFIG_H
// #if defined(AC_EXPORTS)
// #	if defined(_MSC_VER)
// #		define AC_API __declspec(dllexport)
// #       define AC_API_C extern "C" __declspec(dllexport)
// #	else
// #		define AC_API 
// #       define AC_API_C
// #	endif
// #else
// #	if defined(_MSC_VER)
// #		define AC_API __declspec(dllimport)
// #       define AC_API_C extern "C" __declspec(dllimport)
// #	else
// #		define AC_API 
// #       define AC_API_C
// #	endif
// #endif
#define AC_API
#define AC_API_C
#define AC_MODULE_MEDIA 1
#define AC_MODULE_GUI 1
#define AC_MODULE_OCR 1
#endif