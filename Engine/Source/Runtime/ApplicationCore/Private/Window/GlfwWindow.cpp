#include "Window/GlfwWindow.h"

#include "ThirdParty/Glfw/GlfwCommon.h"
#include "Application/GenericApplication.h"
#include "Application/GenericApplicationMessageHandler.h"
#include "Misc/ApplicationCoreGlobals.h"
#include "vulkan/vulkan.hpp"

namespace Doge
{
	namespace
	{
		FORCEINLINE auto FindPlatformWindow(GLFWwindow* InGlfwWindow) -> std::shared_ptr<FGenericWindow>
		{
			return GApp->FindWindowByNativeWindowHandle(glfwGetWindowUserPointer(InGlfwWindow));
		}

		auto KeyCallBack(GLFWwindow* InGlfwWindow, int Key, int Scancode, int Action, int Mods) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				GApp->GetMessageHandler()->OnKeyEvent(PlatformWindow.get(), Key, Scancode, Action, Mods);
			}
		}

		auto CharCallBack(GLFWwindow* InGlfwWindow, uint32 Codepoint) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				GApp->GetMessageHandler()->OnCharEvent(PlatformWindow.get(), Codepoint);
			}
		}

		auto MouseButtonCallBack(GLFWwindow* InGlfwWindow, int Button, int Action, int Mods) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				GApp->GetMessageHandler()->OnMouseButton(PlatformWindow.get(), Button, Action, Mods);
			}
		}

		auto CursorPosCallBack(GLFWwindow* InGlfwWindow, double XPos, double YPos) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				GApp->GetMessageHandler()->OnMouseMove(PlatformWindow.get(), static_cast<float>(XPos), static_cast<float>(YPos));
			}
		}

		auto ScrollCallBack(GLFWwindow* InGlfwWindow, double XOffset, double YOffset) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				GApp->GetMessageHandler()->OnMouseWheel(PlatformWindow.get(), static_cast<float>(XOffset), static_cast<float>(YOffset));
			}
		}

		auto WindowFocusCallBack(GLFWwindow* InGlfwWindow, int Focused) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				GApp->GetMessageHandler()->OnWindowFocus(PlatformWindow.get(), Focused != 0);
			}
		}

		auto WindowResizeCallBack(GLFWwindow* InGlfwWindow, int Width, int Height) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				GApp->GetMessageHandler()->OnWindowResize(PlatformWindow.get(), Width, Height, PlatformWindow->IsMinimized());
			}
		}

		auto MapToGlfwCursorShape(EMouseCursor Cursor) -> int
		{
			switch (Cursor)
			{
			case EMouseCursor::Arrow: return GLFW_ARROW_CURSOR;
			case EMouseCursor::TextInput: return GLFW_IBEAM_CURSOR;
			case EMouseCursor::ResizeAll: return GLFW_RESIZE_ALL_CURSOR;
			case EMouseCursor::ResizeNS: return GLFW_VRESIZE_CURSOR;
			case EMouseCursor::ResizeEW: return GLFW_HRESIZE_CURSOR;
			case EMouseCursor::ResizeNESW: return GLFW_RESIZE_NESW_CURSOR;
			case EMouseCursor::ResizeNWSE: return GLFW_RESIZE_NWSE_CURSOR;
			case EMouseCursor::Hand: return GLFW_POINTING_HAND_CURSOR;
			case EMouseCursor::NotAllowed: return GLFW_NOT_ALLOWED_CURSOR;
			case EMouseCursor::None: return -1;
			default: return GLFW_ARROW_CURSOR;
			}
		}
	}



	FGlfwWindow::FGlfwWindow() = default;

	FGlfwWindow::~FGlfwWindow()
	{
		glfwDestroyWindow(GlfwWindow);
		glfwTerminate();
	}

	auto FGlfwWindow::Make() -> std::shared_ptr<FGlfwWindow>
	{
		return std::shared_ptr<FGlfwWindow>(new FGlfwWindow());
	}

	auto FGlfwWindow::Initialize(const std::shared_ptr<FGenericWindowDefinition>& InDefinition) -> void
	{
		Definition = InDefinition;

		glfwInit();
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_RESIZABLE, WindowMode == EWindowMode::Windowed);
#if defined(__APPLE__)
		glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
		glfwWindowHint(GLFW_COCOA_GRAPHICS_SWITCHING, GLFW_TRUE);
#endif

		const int DesiredWidth = FMath::TruncToInt32(Definition->WidthDesiredOnScreen);
		const int DesiredHeight = FMath::TruncToInt32(Definition->HeightDesiredOnScreen);
		GlfwWindow = glfwCreateWindow(DesiredWidth, DesiredHeight, Definition->Title.c_str(), nullptr, nullptr);
#if defined(_WIN32)
		OSNativeWindowHandle = glfwGetWin32Window(GlfwWindow);
#elif defined(__APPLE__)
		OSNativeWindowHandle = glfwGetCocoaWindow(GlfwWindow);
#endif

		glfwSetWindowUserPointer(GlfwWindow, OSNativeWindowHandle);

		// Register all GLFW callbacks
		glfwSetFramebufferSizeCallback(GlfwWindow, WindowResizeCallBack);
		glfwSetKeyCallback(GlfwWindow, KeyCallBack);
		glfwSetCharCallback(GlfwWindow, CharCallBack);
		glfwSetMouseButtonCallback(GlfwWindow, MouseButtonCallBack);
		glfwSetCursorPosCallback(GlfwWindow, CursorPosCallBack);
		glfwSetScrollCallback(GlfwWindow, ScrollCallBack);
		glfwSetWindowFocusCallback(GlfwWindow, WindowFocusCallBack);

		glfwMakeContextCurrent(GlfwWindow);
	}

	void FGlfwWindow::PollEvents() const
	{
		glfwPollEvents();
	}

	auto FGlfwWindow::ReshapeWindow(int32 X, int32 Y, int32 Width, int32 Height) -> void
	{
		glfwSetWindowPos(GlfwWindow, X, Y);
		glfwSetWindowSize(GlfwWindow, Width, Height);
	}

	void FGlfwWindow::Close()
	{
		glfwSetWindowShouldClose(GlfwWindow, true);
	}

	bool FGlfwWindow::ShouldClose() const
	{
		return glfwWindowShouldClose(GlfwWindow);
	}

	FIntPoint FGlfwWindow::GetViewportSize() const
	{
		int Width, Height;
		glfwGetFramebufferSize(GlfwWindow, &Width, &Height);
		return {Width, Height};
	}

	void* FGlfwWindow::CreateVulkanSurface(void* InInstance) const
	{
		VkSurfaceKHR Surface;
		auto VulkanInstance = static_cast<VkInstance>(InInstance);
		if (glfwCreateWindowSurface(VulkanInstance, GlfwWindow, nullptr, &Surface) != VK_SUCCESS)
		{
			DOGE_ERROR("Failed to create window surface.");
			return nullptr;
		}

		return Surface;
	}

	bool FGlfwWindow::IsMinimized() const
	{
		return glfwGetWindowAttrib(GlfwWindow, GLFW_ICONIFIED);
	}

	auto FGlfwWindow::SetCursor(EMouseCursor Cursor) -> void
	{
		const int CursorIndex = static_cast<int>(Cursor);
		if (Cursor == EMouseCursor::None)
		{
			glfwSetInputMode(GlfwWindow, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
			return;
		}

		glfwSetInputMode(GlfwWindow, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

		if (!CachedCursors[CursorIndex])
		{
			const int GlfwShape = MapToGlfwCursorShape(Cursor);
			CachedCursors[CursorIndex] = glfwCreateStandardCursor(GlfwShape);
		}

		glfwSetCursor(GlfwWindow, static_cast<GLFWcursor*>(CachedCursors[CursorIndex]));
	}
} // namespace Doge
