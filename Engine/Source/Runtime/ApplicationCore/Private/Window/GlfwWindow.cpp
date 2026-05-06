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
		void KeyCallBack(GLFWwindow* InGlfwWindow, int Key, int Scancode, int Action, int Mods)
		{
			GApp->GetMessageHandler()->OnKeyEvent(Key, Scancode, Action, Mods);
		}
	}

	static void CharCallBack(GLFWwindow* InGlfwWindow, unsigned int Codepoint)
	{
		GApp->GetMessageHandler()->OnCharEvent(Codepoint);
	}

	static void MouseButtonCallBack(GLFWwindow* InGlfwWindow, int Button, int Action, int Mods)
	{
		GApp->GetMessageHandler()->OnMouseButton(Button, Action, Mods);
	}

	static void CursorPosCallBack(GLFWwindow* InGlfwWindow, double XPos, double YPos)
	{
		GApp->GetMessageHandler()->OnMouseMove(static_cast<float>(XPos), static_cast<float>(YPos));
	}

	static void ScrollCallBack(GLFWwindow* InGlfwWindow, double XOffset, double YOffset)
	{
		GApp->GetMessageHandler()->OnMouseWheel(static_cast<float>(XOffset), static_cast<float>(YOffset));
	}

	static void WindowFocusCallBack(GLFWwindow* InGlfwWindow, int Focused)
	{
		GApp->GetMessageHandler()->OnWindowFocus(Focused != 0);
	}

	static auto MapToGlfwCursorShape(EMouseCursor Cursor) -> int
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

	static void WindowResizeCallBack(GLFWwindow* InGlfwWindow, int Width, int Height)
	{
		auto* Handler = GApp->GetMessageHandler();
		std::shared_ptr<FGenericWindow> PlatformWindow = GApp->FindWindowByNativeWindowHandle(glfwGetWindowUserPointer(InGlfwWindow));
		if (PlatformWindow)
		{
			Handler->OnWindowResize(PlatformWindow, Width, Height, PlatformWindow->IsMinimized());
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
