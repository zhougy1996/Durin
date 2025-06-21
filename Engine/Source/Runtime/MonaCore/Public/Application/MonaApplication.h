#pragma once

#include "Application/GenericApplication.h"

class FGenericWindow;
class MWidget;
class MWindow;
class FMonaRenderer;

class MONA_CORE_API FMonaApplication : public FGenericApplication
{
public:
	virtual ~FMonaApplication();

	static auto Create() -> void;

	static auto Shutdown() -> void;

	static auto Get() -> FMonaApplication&;

	virtual auto Tick() -> void override;

	auto GetActiveTopLevelWindow() -> TSharedPtr<MWindow>;

	auto AddWindow(TSharedPtr<MWindow> InMonaWindow, const bool bShowImmediately) -> TSharedPtr<MWindow>;

	auto Initialize() -> void;

	auto InitializeRenderer() -> void;

	auto RequestDestroyWindow(TSharedPtr<MWindow> Window) -> void;

	auto CloseAllWindowsImmediately() -> void;

	auto DestroyWindowsImmediately() -> void;

	auto OnWindowClose(TSharedPtr<FGenericWindow> PlatformWindow) -> void;

	auto PollEvents();

	auto ProcessDeferredEvents() -> void override;

	auto FindWidgetWindow(TSharedPtr<MWidget> Widget) -> TSharedPtr<MWindow>;

	auto GetRenderer() const -> FMonaRenderer*;

protected:
	auto MakeWindow(TSharedPtr<MWindow> MonaWindow, const bool bShowImmediately) -> TSharedPtr<FGenericWindow>;

	auto TickPlatform() -> void;

	auto TickTime() -> void;

	auto TickAndDrawWidgets() -> void;

	static TSharedPtr<FMonaApplication> CurrentApplication_;

	TArray<TSharedPtr<MWindow>> Windows_;

	TArray<TSharedPtr<MWindow>> WindowDestroyQueue_;

	TSharedPtr<FMonaRenderer> Renderer_;
};
