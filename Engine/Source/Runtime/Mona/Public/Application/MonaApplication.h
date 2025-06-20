#pragma once

#include "Application/GenericApplication.h"

class FGenericWindow;
class KWidget;
class KWindow;
class FKleeRenderer;

class KLEE_API FKleeApplication : public FGenericApplication
{
public:
	virtual ~FKleeApplication();

	static auto Create() -> void;

	static auto Shutdown() -> void;

	static auto Get() -> FKleeApplication&;

	virtual auto Tick() -> void override;

	auto GetActiveTopLevelWindow() -> TSharedPtr<KWindow>;

	auto AddWindow(TSharedPtr<KWindow> InKleeWindow, const bool bShowImmediately) -> TSharedPtr<KWindow>;

	auto Initialize() -> void;

	auto InitializeRenderer() -> void;

	auto RequestDestroyWindow(TSharedPtr<KWindow> Window) -> void;

	auto CloseAllWindowsImmediately() -> void;

	auto DestroyWindowsImmediately() -> void;

	auto OnWindowClose(TSharedPtr<FGenericWindow> PlatformWindow) -> void;

	auto PollEvents();

	auto ProcessDeferredEvents() -> void override;

	auto FindWidgetWindow(TSharedPtr<KWidget> Widget) -> TSharedPtr<KWindow>;

	auto GetRenderer() const -> FKleeRenderer*;

protected:
	auto MakeWindow(TSharedPtr<KWindow> KleeWindow, const bool bShowImmediately) -> TSharedPtr<FGenericWindow>;

	auto TickPlatform() -> void;

	auto TickTime() -> void;

	auto TickAndDrawWidgets() -> void;

	static TSharedPtr<FKleeApplication> CurrentApplication_;

	TArray<TSharedPtr<KWindow>> Windows_;

	TArray<TSharedPtr<KWindow>> WindowDestroyQueue_;

	TSharedPtr<FKleeRenderer> Renderer_;
};
