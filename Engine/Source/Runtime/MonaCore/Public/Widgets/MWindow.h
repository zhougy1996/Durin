#pragma once

#include "RHIFwd.h"
#include "Window/GenericWindow.h"
#include "Widgets/MCompoundWidget.h"

namespace Doge::Mona
{
	class MONACORE_API MWindow : public MCompoundWidget
	{
	public:
		~MWindow() override;

		auto PollEvents() const -> void;

		auto SetNativeWindow(TSharedPtr<FGenericWindow> InNativeWindow) -> void;

		auto GetNativeWindow() const -> TSharedPtr<FGenericWindow>;

		auto GetChildWindows() const -> const std::vector<TSharedPtr<MWindow>>&;

		auto RequestDestroyWindow() -> void;

		auto GetTitle() const -> FString { return Title; }

		auto SetTitle(const FString& InTitle) -> void { Title = InTitle; }

		auto GetDesiredScreenPosition() const -> FVector2f { return InitialDesiredScreenPosition; }

		auto GetDesiredSize() const -> FVector2f { return InitialDesiredSize; }

		auto MoveWindowTo(const FVector2f& NewScreenPosition) -> void;

		auto ReshapeWindow(const FVector2f& NewScreenPosition, const FVector2f& NewSize) -> void;

		auto ResizeWindow(const FVector2f& NewSize) -> void;

		auto GetScreenPosition() const -> FVector2f { return ScreenPosition; }

		auto GetWindowSize() const -> FVector2f { return Size; }

		auto GetViewportSize() const -> FVector2f;

		auto SetCachedScreenPosition(const FVector2f& NewScreenPosition) -> void;

		auto SetCachedSize(const FVector2f& NewSize) -> void;

		auto SetRHIViewport(TSharedPtr<FRHIViewport> InRHIViewport) -> void;

		auto GetRHIViewport() const -> const TSharedPtr<FRHIViewport>&;

		auto GetWindowMode() const -> EWindowMode;

		auto IsWindow() -> bool override { return true; }

	protected:
		FString Title;

		FVector2f InitialDesiredScreenPosition = {};

		FVector2f InitialDesiredSize = {};

		FVector2f ScreenPosition = {};

		FVector2f Size = {};

		FVector2f ViewportSize = {};

		TSharedPtr<FGenericWindow> NativeWindow;

		std::vector<TSharedPtr<MWindow>> ChildWindows;

		TSharedPtr<FRHIViewport> RHIViewport;
	};
}