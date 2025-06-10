#pragma once

#define LINE_TERMINATOR TEXT("\r\n")
#define LINE_TERMINATOR_ANSI "\r\n"

#define DLLEXPORT __declspec(dllexport)
#define DLLIMPORT __declspec(dllimport)

#define FORCEINLINE __forceinline

#define PLATFORM_BREAK() (/*__nop(), */__debugbreak())