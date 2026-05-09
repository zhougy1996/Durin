#pragma once

#include "MonaCoreAPI.h"
#include "Application/GenericApplication.h"
#include "Application/GenericApplicationMessageHandler.h"
#include "Application/MonaEventHandler.h"

namespace Doge::Mona
{
	class MWidget;
	class MWindow;
	class FMonaRenderer;
	class FMonaEventHandler;

	class FMonaApplication : public FGenericApplication, public FGenericApplicationMessageHandler
	{
	public:
		MONACORE_API ~FMonaApplication() override;

		MONACORE_API static auto Create() -> void;

		MONACORE_API static auto Shutdown() -> void;

		MONACORE_API static auto Get() -> FMonaApplication&;

		MONACORE_API static auto IsInitialized() -> bool;

		MONACORE_API auto Tick() -> void override;

		MONACORE_API auto GetActiveTopLevelWindow() -> std::shared_ptr<MWindow>;

		MONACORE_API auto AddWindow(std::shared_ptr<MWindow> InMonaWindow, bool bShowImmediately) -> std::shared_ptr<MWindow>;

		MONACORE_API auto Initialize() -> void;

		MONACORE_API auto InitializeRenderer() -> void;

		MONACORE_API auto RequestDestroyWindow(std::shared_ptr<MWindow> InWindow) -> void;

		MONACORE_API auto CloseAllWindowsImmediately() -> void;

		MONACORE_API auto DestroyWindowsImmediately() -> void;

		MONACORE_API auto OnWindowClose(const std::shared_ptr<FGenericWindow>& PlatformWindow) -> void;

		MONACORE_API auto PollEvents();

		MONACORE_API auto FindWidgetWindow(const std::shared_ptr<MWidget>& InWidget) -> std::shared_ptr<MWindow>;

		MONACORE_API auto GetRenderer() const -> FMonaRenderer*;

		MONACORE_API auto DrawWindows() -> void;

		MONACORE_API auto ProcessDeferredEvents() -> void override;

		MONACORE_API auto FindWindowByNativeWindowHandle(void* InNativeWindowHandle) -> std::shared_ptr<FGenericWindow> override;

		MONACORE_API auto SetMonaEventHandler(std::unique_ptr<FMonaEventHandler> InHandler) -> void;

		// Message handler functions
		auto OnWindowResize(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InWidth, int32 InHeight, bool bInWasMinimized) -> void override;

		auto OnKeyDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods, bool IsRepeat) -> bool override;

		auto OnKeyUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods) -> bool override;

		auto OnKeyChar(const std::shared_ptr<FGenericWindow>& InPlatformWindow, uint32 Codepoint) -> bool override;

		auto OnMouseMove(const std::shared_ptr<FGenericWindow>& InPlatformWindow, FVector2d CursorPos) -> bool override;

		auto OnMouseDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool override;

		auto OnMouseUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool override;

	protected:
		FMonaApplication();

		auto MakeWindow(const std::shared_ptr<MWindow>& InMonaWindow, bool bInShowImmediately) -> std::shared_ptr<FGenericWindow>;

		auto TickPlatform() -> void;

		auto TickTime() -> void;

		auto TickAndDrawWidgets() -> void;

		static std::shared_ptr<FMonaApplication> CurrentApplication;

		std::vector<std::shared_ptr<MWindow>> Windows;

		std::vector<std::shared_ptr<MWindow>> WindowDestroyQueue;

		std::shared_ptr<FMonaRenderer> Renderer{};

		// UI event handler
		std::unique_ptr<FMonaEventHandler> MonaEventHandler{};
	};
} // namespace Doge::Mona
