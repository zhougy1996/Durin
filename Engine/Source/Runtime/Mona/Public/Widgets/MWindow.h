#pragma once

#include "MonaAPI.h"
#include "Misc/ViewportPresentModePolicy.h"
#include "Window/GenericWindow.h"
#include "Widgets/MWidget.h"

namespace Durin
{
	// Coordinates a Mona widget tree with its native platform window.
	class MWindow : public MWidget
	{
	public:
		MONA_API ~MWindow() override;

		MONA_API auto Draw() -> void override;

		MONA_API auto IsWindow() -> bool override;

		MONA_API auto SetContent(const std::shared_ptr<MWidget>& InContent) -> MWindow&;

		MONA_API auto PollEvents() const ->  void;

		MONA_API auto SetNativeWindow(std::shared_ptr<FGenericWindow> InNativeWindow) -> void;

		MONA_API auto GetNativeWindow() const -> std::shared_ptr<FGenericWindow>;

		MONA_API auto SetParentWindow(const std::shared_ptr<MWindow>& InParentWindow) -> void;

		MONA_API auto GetParentWindow() const -> std::shared_ptr<MWindow>;

		MONA_API auto AddChildWindow(const std::shared_ptr<MWindow>& InChildWindow) -> void;

		MONA_API auto RemoveChildWindow(const std::shared_ptr<MWindow>& InChildWindow) -> void;

		MONA_API auto GetChildWindows() const -> const std::vector<std::shared_ptr<MWindow>>&;

		MONA_API auto RequestDestroyWindow() -> void;

		MONA_API auto GetTitle() const -> std::string;

		MONA_API auto SetTitle(const std::string& InTitle) -> void;

		MONA_API auto SetWindowDecorationMode(EWindowDecorationMode Mode) -> void;

		MONA_API auto GetWindowDecorationMode() const -> EWindowDecorationMode;

		MONA_API auto GetEffectiveWindowDecorationMode() const -> EWindowDecorationMode;

		MONA_API auto PublishTitleBarLayout(const FWindowTitleBarLayout& Layout) -> void;

		MONA_API auto GetTitleBarInteractionState() const -> FWindowTitleBarInteractionState;

		MONA_API auto GetTitleBarPlatformMetrics() const -> FWindowTitleBarPlatformMetrics;

		MONA_API auto SetTitleBarDarkMode(bool bDarkMode) -> void;

		MONA_API auto GetDesiredScreenPosition() const -> FVector2f;

		MONA_API auto GetDesiredSize() const -> FVector2f;

		MONA_API auto MoveWindowTo(const FVector2f& NewScreenPosition) -> void;

		MONA_API auto ReshapeWindow(const FVector2f& NewScreenPosition, const FVector2f& NewSize) -> void;

		MONA_API auto ResizeWindow(const FVector2f& NewSize) -> void;

		MONA_API auto GetScreenPosition() const -> FVector2f;

		MONA_API auto GetWindowSize() const -> FVector2f;

		MONA_API auto GetViewportSize() const -> FVector2f;

		MONA_API auto SetCachedScreenPosition(const FVector2f& NewScreenPosition) -> void;

		MONA_API auto SetCachedSize(const FVector2f& NewSize) -> void;

		MONA_API auto SetCachedViewportSize(const FVector2f& NewViewportSize) -> void;

		MONA_API auto GetWindowMode() const -> EWindowMode;

		MONA_API auto SetViewportPresentModePolicy(EViewportPresentModePolicy InPolicy) -> void;

		MONA_API auto GetViewportPresentModePolicy() const -> EViewportPresentModePolicy;

		MONA_API auto ShowWindow() -> void;

		MONA_API auto HideWindow() -> void;

		MONA_API auto IsMinimized() const -> bool;

		MONA_API auto IsMaximized() const -> bool;

		MONA_API auto MaximizeWindow() -> void;

		MONA_API auto RestoreWindow() -> void;

		MONA_API auto MinimizeWindow() -> void;

	protected:
		std::string Title;

		EWindowDecorationMode WindowDecorationMode = EWindowDecorationMode::System;

		bool bTitleBarDarkMode = true;

		FVector2f InitialDesiredScreenPosition = {};

		FVector2f InitialDesiredSize = {};

		// Cached screen and viewport geometry is updated from platform callbacks.
		FVector2f ScreenPosition = {};

		FVector2f Size = {};

		FVector2f ViewportSize = {};

		EViewportPresentModePolicy ViewportPresentModePolicy = EViewportPresentModePolicy::MainWindow;

		std::shared_ptr<MWidget> ContentWidget;

		// The Mona window owns its platform window for the same visible lifetime.
		std::shared_ptr<FGenericWindow> NativeWindow;

		// Parent is weak while children are strong to keep the window tree acyclic.
		std::weak_ptr<MWindow> ParentWindow;

		std::vector<std::shared_ptr<MWindow>> ChildWindows;
	};
}
