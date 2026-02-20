#include "Window/GlfwWindow.h"

#include "ThirdParty/Glfw/GlfwCommon.h"

FGlfwWindow::FGlfwWindow()
{
}

FGlfwWindow::~FGlfwWindow()
{
	glfwDestroyWindow(GlfwWindow_);
	glfwTerminate();
}

auto FGlfwWindow::Make() -> TSharedPtr<FGlfwWindow>
{
	return std::shared_ptr<FGlfwWindow>(new FGlfwWindow());
}

auto FGlfwWindow::Initialize(FGenericApplication* const InApplication, const TSharedPtr<FGenericWindowDefinition>& InDefinition) -> void
{
	OwningApplication_ = InApplication;
	Definition_ = InDefinition;

	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, WindowMode_ == EWindowMode::Windowed);
#if defined (__APPLE__)
	glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
	glfwWindowHint(GLFW_COCOA_GRAPHICS_SWITCHING, GLFW_TRUE);
#endif

	const int DesiredWidth = FMath::TruncToInt32(Definition_->WidthDesiredOnScreen);
	const int DesiredHeight = FMath::TruncToInt32(Definition_->HeightDesiredOnScreen);
	GlfwWindow_ = glfwCreateWindow(DesiredWidth, DesiredHeight, Definition_->Title.c_str(), nullptr, nullptr);
	glfwSetWindowUserPointer(GlfwWindow_, this);
	glfwSetFramebufferSizeCallback(GlfwWindow_, nullptr); // TODO: Implement the framebuffer size callback
	glfwMakeContextCurrent(GlfwWindow_);
}

void FGlfwWindow::PollEvents() const
{
	glfwPollEvents();
}

auto FGlfwWindow::ReshapeWindow(int32 X, int32 Y, int32 Width, int32 Height) -> void
{
	glfwSetWindowPos(GlfwWindow_, X, Y);
	glfwSetWindowSize(GlfwWindow_, Width, Height);
}

void FGlfwWindow::Close()
{
	glfwSetWindowShouldClose(GlfwWindow_, true);
}

bool FGlfwWindow::ShouldClose() const
{
	return glfwWindowShouldClose(GlfwWindow_);
}

auto FGlfwWindow::GetOSWindowHandle() const -> void*
{
	#if defined(_Win32)
		return glfwGetWin32Window(GlfwWindow_);
	#elif defined(__APPLE__)
		return glfwGetCocoaWindow(GlfwWindow_);
	#endif
}
