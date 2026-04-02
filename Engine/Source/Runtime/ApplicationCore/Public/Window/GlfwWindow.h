#pragma once

#include "Window/GenericWindow.h"

struct GLFWwindow;

namespace Doge
{
	class FGlfwWindow final : public FGenericWindow
	{
	public:
		APPLICATIONCORE_API ~FGlfwWindow() override;

		APPLICATIONCORE_API static auto Make() -> std::shared_ptr<FGlfwWindow>;

		APPLICATIONCORE_API auto Initialize(const std::shared_ptr<FGenericWindowDefinition>& InDefinition) -> void override;

		APPLICATIONCORE_API auto PollEvents() const -> void override;

		APPLICATIONCORE_API auto ReshapeWindow(int32 X, int32 Y, int32 Width, int32 Height) -> void override;

		APPLICATIONCORE_API auto Close() -> void override;

		APPLICATIONCORE_API auto ShouldClose() const -> bool override;

		APPLICATIONCORE_API auto GetViewportSize() const -> FIntPoint override;

		APPLICATIONCORE_API auto CreateVulkanSurface(void* InVulkanInstance) const -> void* override;

		auto GetGlfwWindow() const -> GLFWwindow* { return GlfwWindow; }

		APPLICATIONCORE_API auto IsMinimized() const -> bool override;

	private:
		FGlfwWindow();

		EWindowMode WindowMode = EWindowMode::Windowed;

		GLFWwindow* GlfwWindow = nullptr;
	};
}