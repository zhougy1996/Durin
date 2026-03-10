#pragma once

#include "Application/GenericApplication.h"

namespace Doge::Mona
{
	class MWidget;
	class MWindow;
	class FMonaRenderer;

	class MONACORE_API FMonaApplication : public FGenericApplication
	{
	public:
		~FMonaApplication() override;

		static auto Create() -> void;

		static auto Shutdown() -> void;

		static auto Get() -> FMonaApplication&;

		auto Tick() -> void override;

		auto GetActiveTopLevelWindow() -> TSharedPtr<MWindow>;

		auto AddWindow(TSharedPtr<MWindow> InMonaWindow, const bool bShowImmediately) -> TSharedPtr<MWindow>;

		auto Initialize() -> void;

		auto InitializeRenderer() -> void;

		auto RequestDestroyWindow(TSharedPtr<MWindow> InWindow) -> void;

		auto CloseAllWindowsImmediately() -> void;

		auto DestroyWindowsImmediately() -> void;

		auto OnWindowClose(TSharedPtr<FGenericWindow> PlatformWindow) -> void;

		auto PollEvents();

		auto ProcessDeferredEvents() -> void override;

		auto FindWidgetWindow(TSharedPtr<MWidget> InWidget) -> TSharedPtr<MWindow>;

		auto GetRenderer() const -> FMonaRenderer*;

		auto FindWindowByNativeWindowHandle(void* InNativeWindowHandle) -> TSharedPtr<FGenericWindow> override;

	protected:
		auto MakeWindow(TSharedPtr<MWindow> InMonaWindow, bool bInShowImmediately) -> TSharedPtr<FGenericWindow>;

		auto TickPlatform() -> void;

		auto TickTime() -> void;

		auto TickAndDrawWidgets() -> void;

		static TSharedPtr<FMonaApplication> CurrentApplication;

		std::vector<TSharedPtr<MWindow>> Windows;

		std::vector<TSharedPtr<MWindow>> WindowDestroyQueue;

		TSharedPtr<FMonaRenderer> Renderer;
	};
}
