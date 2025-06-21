#include "Window/GlfwWindow.h"

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

auto FGlfwWindow::Initialize(FGenericApplication* const Application, const TSharedPtr<FGenericWindowDefinition>& Definition) -> void
{
	OwningApplication_ = Application;
	Definition_ = Definition;

	glfwInit();
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, WindowMode_ == EWindowMode::Windowed);
	int DesiredWidth = FMath::TruncToInt32(Definition_->WidthDesiredOnScreen);
	int DesiredHeight = FMath::TruncToInt32(Definition_->HeightDesiredOnScreen);
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
	return glfwGetWin32Window(GlfwWindow_);
}
