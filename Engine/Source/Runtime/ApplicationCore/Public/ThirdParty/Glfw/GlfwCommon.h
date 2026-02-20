#pragma once

#define GLFW_INCLUDE_VULKAN

#ifdef _Win32
	#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined __APPLE__
	#define GLFW_EXPOSE_NATIVE_COCOA
#endif
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

inline auto GetNativeWindowHandle(GLFWwindow* InGlfwWindow) -> void*
{
#if defined(_Win32)
	return glfwGetWin32Window(GlfwWindow_);
#elif defined(__APPLE__)
	return glfwGetCocoaWindow(InGlfwWindow);
#endif
}