#pragma once

#include "MonaAPI.h"
#include "Application/GenericApplication.h"
#include "Application/GenericApplicationMessageHandler.h"
#include "Application/MonaEventHandler.h"

namespace Durin
{
	class MWidget;
	class MWindow;
}

namespace Durin::Mona
{
	class FMonaRenderer;
	class FMonaEventHandler;

	// Owns the Mona window set and coordinates platform events, UI, and rendering.
	class FMonaApplication : public FGenericApplication, public FGenericApplicationMessageHandler
	{
	public:
		MONA_API ~FMonaApplication() override;

		MONA_API static auto Create() -> void;

		MONA_API static auto Shutdown() -> void;

		MONA_API static auto Get() -> FMonaApplication&;

		MONA_API static auto IsInitialized() -> bool;

		MONA_API auto Tick() -> void override;

		// Samples platform input before gameplay consumes the current frame.
		MONA_API auto PumpPlatformEvents() -> void;

		// Advances application time and widgets after gameplay has updated.
		MONA_API auto TickUI() -> void;

		MONA_API auto GetActiveTopLevelWindow() -> std::shared_ptr<MWindow>;

		MONA_API auto AddWindow(std::shared_ptr<MWindow> InMonaWindow, bool bShowImmediately) -> std::shared_ptr<MWindow>;

		MONA_API auto Initialize(
			bool bAdoptInitializationPresentationCandidate) -> void;

		MONA_API auto InitializeRenderer(
			bool bAdoptInitializationPresentationCandidate) -> void;
		MONA_API auto ShutdownRenderer() -> void;

		MONA_API auto RequestDestroyWindow(std::shared_ptr<MWindow> InWindow) -> void;

		MONA_API auto CloseAllWindowsImmediately() -> void;

		MONA_API auto DestroyWindowsImmediately() -> void;

		MONA_API auto FlushPendingWindowDestroys() -> void;

		MONA_API auto OnWindowCloseRequested(const std::shared_ptr<FGenericWindow>& PlatformWindow) -> void override;

		MONA_API auto PollEvents() -> void;

		MONA_API auto WaitForEvents(double TimeoutSeconds) const -> void;

		MONA_API auto AreAllWindowsMinimized() const -> bool;

		MONA_API auto GetWindows() const -> const std::vector<std::shared_ptr<MWindow>>&;

		MONA_API auto FindWidgetWindow(const std::shared_ptr<MWidget>& InWidget) -> std::shared_ptr<MWindow>;

		MONA_API auto FindWindowByPlatformWindow(const std::shared_ptr<FGenericWindow>& InPlatformWindow) const -> std::shared_ptr<MWindow>;

		MONA_API auto FindMonaWindowByNativeWindowHandle(void* InNativeWindowHandle) const -> std::shared_ptr<MWindow>;

		MONA_API auto GetRenderer() const -> FMonaRenderer*;

		MONA_API auto DrawWindows() -> void;

		MONA_API auto ProcessDeferredEvents() -> void override;

		MONA_API auto FindWindowByNativeWindowHandle(void* InNativeWindowHandle) -> std::shared_ptr<FGenericWindow> override;

		MONA_API auto SetMonaEventHandler(std::unique_ptr<FMonaEventHandler> InHandler) -> void;
		MONA_API auto SetGameEventHandler(std::unique_ptr<FMonaEventHandler> InHandler) -> void;

		MONA_API auto GetActiveTopLevelWindow() const -> std::shared_ptr<MWindow>;

		// Message handler functions
		auto OnWindowFocus(const std::shared_ptr<FGenericWindow> &InPlatformWindow, bool bFocused) -> void override;

		auto OnWindowResize(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InWidth, int32 InHeight, bool bInWasMinimized) -> void override;

		auto OnWindowViewportResize(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InWidth, int32 InHeight, bool bInWasMinimized) -> void override;

		auto OnWindowMoved(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InX, int32 InY) -> void override;

		auto OnKeyDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods, bool IsRepeat) -> bool override;

		auto OnKeyUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods) -> bool override;

		auto OnKeyChar(const std::shared_ptr<FGenericWindow>& InPlatformWindow, uint32 Codepoint) -> bool override;

		auto OnMouseMove(const std::shared_ptr<FGenericWindow>& InPlatformWindow, FVector2d CursorPos) -> bool override;

		auto OnMouseEnter(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> void override;

		auto OnMouseLeave(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> void override;

		auto OnMouseDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool override;

		auto OnMouseUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool override;

		auto OnMouseWheel(const std::shared_ptr<FGenericWindow>& InPlatformWindow, double DeltaX, double DeltaY) -> bool override;

	protected:
		FMonaApplication();

		auto MakeWindow(const std::shared_ptr<MWindow>& InMonaWindow, bool bInShowImmediately) -> std::shared_ptr<FGenericWindow>;

		auto TickTime() -> void;

		auto TickAndDrawWidgets() -> void;

		static std::shared_ptr<FMonaApplication> CurrentApplication;

		// Strong ownership keeps all registered top-level and child windows alive.
		std::vector<std::shared_ptr<MWindow>> Windows;

		std::weak_ptr<MWindow> ActiveTopLevelWindow;

		// Destruction is deferred until event dispatch and drawing are complete.
		std::vector<std::shared_ptr<MWindow>> WindowDestroyQueue;

		std::shared_ptr<FMonaRenderer> Renderer{};

		// UI event handler
		std::unique_ptr<FMonaEventHandler> MonaEventHandler{};
		std::unique_ptr<FMonaEventHandler> GameEventHandler{};

	};
} // namespace Durin::Mona
