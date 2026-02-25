#pragma once

#include "Window/GenericWindow.h"

struct GLFWwindow;

class APPLICATIONCORE_API FGlfwWindow final : public FGenericWindow
{
public:
	virtual ~FGlfwWindow();

	static auto Make() -> TSharedPtr<FGlfwWindow>;

	virtual auto Initialize(FGenericApplication* const InApplication, const TSharedPtr<FGenericWindowDefinition>& InDefinition) -> void override;

	virtual auto PollEvents() const -> void override;

	virtual auto ReshapeWindow(int32 X, int32 Y, int32 Width, int32 Height) -> void override;

	virtual auto Close() -> void override;

	virtual auto ShouldClose() const -> bool override;

	virtual auto GetViewportSize() const -> FIntPoint override;

	auto GetGlfwWindow() const -> GLFWwindow* { return GlfwWindow_; }

	virtual auto CreateVulkanSurface(void* InVulkanInstance) const -> void* override;

private:
	FGlfwWindow();

	EWindowMode WindowMode_ = EWindowMode::Windowed;

	FGenericApplication* OwningApplication_ = nullptr;

	GLFWwindow* GlfwWindow_ = nullptr;
};
