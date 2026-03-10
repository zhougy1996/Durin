#pragma once

#include "Window/GenericWindow.h"

struct GLFWwindow;

namespace Doge
{
	class APPLICATIONCORE_API FGlfwWindow final : public FGenericWindow
	{
	public:
		~FGlfwWindow() override;

		static auto Make() -> TSharedPtr<FGlfwWindow>;

		auto Initialize(FGenericApplication* InApplication, const TSharedPtr<FGenericWindowDefinition>& InDefinition) -> void override;

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

		FGenericApplication* OwningApplication = nullptr;

		GLFWwindow* GlfwWindow = nullptr;
	};
}