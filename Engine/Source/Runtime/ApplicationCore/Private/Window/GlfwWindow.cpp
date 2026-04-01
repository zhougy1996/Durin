#include "Window/GlfwWindow.h"

#include "ThirdParty/Glfw/GlfwCommon.h"
#include "vulkan/vulkan.hpp"

namespace Doge
{
	static void WindowResizeCallBack(GLFWwindow* InGlfwWindow, int Width, int Height) {
		DOGE_DEBUG("Window resized: {}x{}", Width, Height);
		auto* Window = static_cast<FGlfwWindow*>(glfwGetWindowUserPointer(InGlfwWindow));
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

	auto FGlfwWindow::Initialize(FGenericApplication* const InApplication, const std::shared_ptr<FGenericWindowDefinition>& InDefinition) -> void
	{
		OwningApplication = InApplication;
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
		glfwSetWindowUserPointer(GlfwWindow, this);
		glfwSetFramebufferSizeCallback(GlfwWindow, WindowResizeCallBack);
		glfwMakeContextCurrent(GlfwWindow);

#if defined(_WIN32)
		OSNativeWindowHandle = glfwGetWin32Window(GlfwWindow);
#elif defined(__APPLE__)
		OSNativeWindowHandle = glfwGetCocoaWindow(GlfwWindow);
#endif
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

	auto FGlfwWindow::OnWindowResized(int Width, int Height) -> void
	{
		OnWindowResizedDelegate.Broadcast(Width, Height);
	}
} // namespace Doge
