#pragma once

#include "Window/GenericWindow.h"

struct GLFWwindow;

namespace Doge
{
	class APPLICATIONCORE_API FGlfwWindow final : public FGenericWindow
	{
	public:
		~FGlfwWindow() override;

		static auto Make() -> std::shared_ptr<FGlfwWindow>;

		auto Initialize(const std::shared_ptr<FGenericWindowDefinition>& InDefinition) -> void override;

		auto PollEvents() const -> void override;

		auto ReshapeWindow(int32 X, int32 Y, int32 Width, int32 Height) -> void override;

		auto Close() -> void override;

		auto ShouldClose() const -> bool override;

		auto GetViewportSize() const -> FIntPoint override;

		auto GetGlfwWindow() const -> GLFWwindow* { return GlfwWindow; }

		auto CreateVulkanSurface(void* InVulkanInstance) const -> void* override;

	private:
		FGlfwWindow();

		EWindowMode WindowMode = EWindowMode::Windowed;

		GLFWwindow* GlfwWindow = nullptr;
	};
}