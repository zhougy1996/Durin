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

		auto GetActiveTopLevelWindow() -> std::shared_ptr<MWindow>;

		auto AddWindow(std::shared_ptr<MWindow> InMonaWindow, const bool bShowImmediately) -> std::shared_ptr<MWindow>;

		auto Initialize() -> void;

		auto InitializeRenderer() -> void;

		auto RequestDestroyWindow(std::shared_ptr<MWindow> InWindow) -> void;

		auto CloseAllWindowsImmediately() -> void;

		auto DestroyWindowsImmediately() -> void;

		auto OnWindowClose(const std::shared_ptr<FGenericWindow>& PlatformWindow) -> void;

		auto PollEvents();

		auto ProcessDeferredEvents() -> void override;

		auto FindWidgetWindow(const std::shared_ptr<MWidget>& InWidget) -> std::shared_ptr<MWindow>;

		auto GetRenderer() const -> FMonaRenderer*;

		auto FindWindowByNativeWindowHandle(void* InNativeWindowHandle) -> std::shared_ptr<FGenericWindow> override;

	protected:
		auto MakeWindow(const std::shared_ptr<MWindow>& InMonaWindow, bool bInShowImmediately) -> std::shared_ptr<FGenericWindow>;

		auto TickPlatform() -> void;

		auto TickTime() -> void;

		auto TickAndDrawWidgets() -> void;

		static std::shared_ptr<FMonaApplication> CurrentApplication;

		std::vector<std::shared_ptr<MWindow>> Windows;

		std::vector<std::shared_ptr<MWindow>> WindowDestroyQueue;

		std::shared_ptr<FMonaRenderer> Renderer;
	};
}
