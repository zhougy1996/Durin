#pragma once

#include "Window/GenericWindow.h"

// The forward declaration of GLFWwindow in the glfw library.
struct GLFWwindow;

namespace Durin
{
	class FGlfwWindow final : public FGenericWindow
	{
	public:
		APPLICATIONCORE_API ~FGlfwWindow() override;

		APPLICATIONCORE_API static auto Make() -> std::shared_ptr<FGlfwWindow>;

		APPLICATIONCORE_API auto Initialize(const std::shared_ptr<FGenericWindowDefinition>& InDefinition) -> void override;

		APPLICATIONCORE_API auto PollEvents() const -> void override;

		APPLICATIONCORE_API auto ReshapeWindow(int32 X, int32 Y, int32 Width, int32 Height) -> void override;

		APPLICATIONCORE_API auto MoveWindowTo(int32 X, int32 Y) -> void override;

		APPLICATIONCORE_API auto GetWindowPosition() const -> FIntPoint override;

		APPLICATIONCORE_API auto GetWindowSize() const -> FIntPoint override;

		APPLICATIONCORE_API auto Close() -> void override;

		APPLICATIONCORE_API auto Show() -> void override;

		APPLICATIONCORE_API auto Hide() -> void override;

		APPLICATIONCORE_API auto Focus() -> void override;

		APPLICATIONCORE_API auto ShouldClose() const -> bool override;

		APPLICATIONCORE_API auto GetViewportSize() const -> FIntPoint override;

		APPLICATIONCORE_API auto CreateVulkanSurface(void* InVulkanInstance) const -> void* override;

		APPLICATIONCORE_API auto IsMinimized() const -> bool override;

		APPLICATIONCORE_API auto IsFocused() const -> bool override;

		APPLICATIONCORE_API auto SetTitle(const std::string& InTitle) -> void override;

		APPLICATIONCORE_API auto SetOpacity(float InOpacity) -> void override;

		APPLICATIONCORE_API auto SetWindowDecorated(bool bDecorated) -> void override;

		APPLICATIONCORE_API auto SetMousePassthrough(bool bPassthrough) -> void override;

		APPLICATIONCORE_API auto SetFocusOnShow(bool bFocusOnShow) -> void override;

		APPLICATIONCORE_API auto GetDpiScale() const -> float override;

		APPLICATIONCORE_API auto SetCursor(EMouseCursor Cursor) -> void override;

	private:
		FGlfwWindow();

		GLFWwindow* GlfwWindow = nullptr;

		void* CachedCursors[10] = {};
	};
}
