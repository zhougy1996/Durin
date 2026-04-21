#pragma once

#include "MonaCoreAPI.h"
#include "RHIFwd.h"
#include "Window/GenericWindow.h"
#include "Widgets/MCompoundWidget.h"

namespace Doge::Mona
{
	class IMonaViewport;

	class MWindow : public MCompoundWidget
	{
	public:
		MONACORE_API ~MWindow() override;

		MONACORE_API auto IsWindow() -> bool override;

		MONACORE_API auto PollEvents() const -> void;

		MONACORE_API auto SetNativeWindow(std::shared_ptr<FGenericWindow> InNativeWindow) -> void;

		MONACORE_API auto GetNativeWindow() const -> std::shared_ptr<FGenericWindow>;

		MONACORE_API auto GetChildWindows() const -> const std::vector<std::shared_ptr<MWindow>>&;

		MONACORE_API auto RequestDestroyWindow() -> void;

		MONACORE_API auto GetTitle() const -> std::string;

		MONACORE_API auto SetTitle(const std::string& InTitle) -> void;

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

		MONACORE_API auto SetViewport(const std::shared_ptr<IMonaViewport>& InViewport) -> void;

		MONACORE_API auto GetViewport() const -> std::shared_ptr<IMonaViewport>;

		MONACORE_API auto GetWindowMode() const -> EWindowMode;

		MONACORE_API auto ShowWindow() -> void;

		MONACORE_API auto HideWindow() -> void;

		MONACORE_API auto IsMinimized() const -> bool;

	protected:
		std::string Title;

		FVector2f InitialDesiredScreenPosition = {};

		FVector2f InitialDesiredSize = {};

		FVector2f ScreenPosition = {};

		FVector2f Size = {};

		FVector2f ViewportSize = {};

		std::shared_ptr<FGenericWindow> NativeWindow;

		std::vector<std::shared_ptr<MWindow>> ChildWindows;

		std::weak_ptr<IMonaViewport> Viewport;
	};
}