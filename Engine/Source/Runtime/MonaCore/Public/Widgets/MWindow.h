#pragma once

#include "MonaCoreAPI.h"
#include "Misc/ViewportPresentModePolicy.h"
#include "Window/GenericWindow.h"
#include "Widgets/MWidget.h"

namespace Durin
{
	class MWindow : public MWidget
	{
	public:
		MONACORE_API ~MWindow() override;

		MONACORE_API auto Draw() -> void override;

		MONACORE_API auto IsWindow() -> bool override;

		MONACORE_API auto SetContent(const std::shared_ptr<MWidget>& InContent) -> MWindow&;

		MONACORE_API auto PollEvents() const ->  void;

		MONACORE_API auto SetNativeWindow(std::shared_ptr<FGenericWindow> InNativeWindow) -> void;

		MONACORE_API auto GetNativeWindow() const -> std::shared_ptr<FGenericWindow>;

		MONACORE_API auto SetParentWindow(const std::shared_ptr<MWindow>& InParentWindow) -> void;

		MONACORE_API auto GetParentWindow() const -> std::shared_ptr<MWindow>;

		MONACORE_API auto AddChildWindow(const std::shared_ptr<MWindow>& InChildWindow) -> void;

		MONACORE_API auto RemoveChildWindow(const std::shared_ptr<MWindow>& InChildWindow) -> void;

		MONACORE_API auto GetChildWindows() const -> const std::vector<std::shared_ptr<MWindow>>&;

		MONACORE_API auto RequestDestroyWindow() -> void;

		MONACORE_API auto GetTitle() const -> std::string;

		MONACORE_API auto SetTitle(const std::string& InTitle) -> void;

		MONACORE_API auto SetWindowDecorated(bool bDecorated) -> void;

		MONACORE_API auto IsWindowDecorated() const -> bool;

		MONACORE_API auto GetDesiredScreenPosition() const -> FVector2f;

		MONACORE_API auto GetDesiredSize() const -> FVector2f;

		MONACORE_API auto MoveWindowTo(const FVector2f& NewScreenPosition) -> void;

		MONACORE_API auto ReshapeWindow(const FVector2f& NewScreenPosition, const FVector2f& NewSize) -> void;

		MONACORE_API auto ResizeWindow(const FVector2f& NewSize) -> void;

		MONACORE_API auto GetScreenPosition() const -> FVector2f;

		MONACORE_API auto GetWindowSize() const -> FVector2f;

		MONACORE_API auto GetViewportSize() const -> FVector2f;

		MONACORE_API auto SetCachedScreenPosition(const FVector2f& NewScreenPosition) -> void;

		MONACORE_API auto SetCachedSize(const FVector2f& NewSize) -> void;

		MONACORE_API auto SetCachedViewportSize(const FVector2f& NewViewportSize) -> void;

		MONACORE_API auto GetWindowMode() const -> EWindowMode;

		MONACORE_API auto SetViewportPresentModePolicy(EViewportPresentModePolicy InPolicy) -> void;

		MONACORE_API auto GetViewportPresentModePolicy() const -> EViewportPresentModePolicy;

		MONACORE_API auto ShowWindow() -> void;

		MONACORE_API auto HideWindow() -> void;

		MONACORE_API auto IsMinimized() const -> bool;

		MONACORE_API auto IsMaximized() const -> bool;

		MONACORE_API auto MaximizeWindow() -> void;

		MONACORE_API auto RestoreWindow() -> void;

	protected:
		std::string Title;

		bool bWindowDecorated = true;

		FVector2f InitialDesiredScreenPosition = {};

		FVector2f InitialDesiredSize = {};

		FVector2f ScreenPosition = {};

		FVector2f Size = {};

		FVector2f ViewportSize = {};

		EViewportPresentModePolicy ViewportPresentModePolicy = EViewportPresentModePolicy::MainWindow;

		std::shared_ptr<MWidget> ContentWidget;

		std::shared_ptr<FGenericWindow> NativeWindow;

		std::weak_ptr<MWindow> ParentWindow;

		std::vector<std::shared_ptr<MWindow>> ChildWindows;
	};
}
