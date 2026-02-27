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

		auto RequestDestroyWindow(TSharedPtr<MWindow> Window) -> void;

		auto CloseAllWindowsImmediately() -> void;

		auto DestroyWindowsImmediately() -> void;

		auto OnWindowClose(TSharedPtr<FGenericWindow> PlatformWindow) -> void;

		auto PollEvents();

		auto ProcessDeferredEvents() -> void override;

		auto FindWidgetWindow(TSharedPtr<MWidget> Widget) -> TSharedPtr<MWindow>;

		auto GetRenderer() const -> FMonaRenderer*;

		auto FindWindowByNativeWindowHandle(void* InNativeWindowHandle) -> TSharedPtr<FGenericWindow> override;

	protected:
		auto MakeWindow(TSharedPtr<MWindow> MonaWindow, bool bShowImmediately) -> TSharedPtr<FGenericWindow>;

		auto TickPlatform() -> void;

		auto TickTime() -> void;

		auto TickAndDrawWidgets() -> void;

		static TSharedPtr<FMonaApplication> CurrentApplication_;

		std::vector<TSharedPtr<MWindow>> Windows_;

		std::vector<TSharedPtr<MWindow>> WindowDestroyQueue_;

		TSharedPtr<FMonaRenderer> Renderer_;
	};
}
