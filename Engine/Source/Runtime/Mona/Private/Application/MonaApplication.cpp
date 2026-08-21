#include "Application/MonaApplication.h"

#include "CoreGlobals.h"

#include "Application/MonaWindowHelper.h"
#include "ApplicationCore.h"
#include "Rendering/MonaRHIRenderer.h"
#include "Widgets/MWindow.h"

#include "RenderingThread.h"

namespace Durin::Mona
{
	namespace
	{
		auto GetRootWindow(const std::shared_ptr<MWindow>& InWindow) -> std::shared_ptr<MWindow>
		{
			std::shared_ptr<MWindow> RootWindow = InWindow;
			while (RootWindow != nullptr)
			{
				const std::shared_ptr<MWindow> ParentWindow = RootWindow->GetParentWindow();
				if (ParentWindow == nullptr)
				{
					break;
				}
				RootWindow = ParentWindow;
			}
			return RootWindow;
		}

		auto QueueWindowHierarchyForDestroy(const std::shared_ptr<MWindow>& InWindow, std::vector<std::shared_ptr<MWindow>>& WindowDestroyQueue, std::vector<std::shared_ptr<MWindow>>& NewlyQueuedWindows) -> void
		{
			if (InWindow == nullptr)
			{
				return;
			}

			if (std::ranges::find(WindowDestroyQueue, InWindow) == WindowDestroyQueue.end())
			{
				WindowDestroyQueue.push_back(InWindow);
				NewlyQueuedWindows.push_back(InWindow);
			}

			for (const std::shared_ptr<MWindow>& ChildWindow : InWindow->GetChildWindows())
			{
				QueueWindowHierarchyForDestroy(ChildWindow, WindowDestroyQueue, NewlyQueuedWindows);
			}
		}
	}

	std::shared_ptr<FMonaApplication> FMonaApplication::CurrentApplication = nullptr;

	FMonaApplication::~FMonaApplication() = default;

	auto FMonaApplication::Create() -> void
	{
		CurrentApplication = std::shared_ptr<FMonaApplication>(new FMonaApplication());
		GApp = CurrentApplication;
	}

	auto FMonaApplication::Shutdown() -> void
	{
		CurrentApplication->CloseAllWindowsImmediately();
		CurrentApplication.reset();
		GApp.reset();
	}

	auto FMonaApplication::Get() -> FMonaApplication&
	{
		return *CurrentApplication;
	}

	auto FMonaApplication::IsInitialized() -> bool
	{
		return CurrentApplication != nullptr;
	}

	auto FMonaApplication::Tick() -> void
	{
		PumpPlatformEvents();
		TickUI();
	}

	auto FMonaApplication::PumpPlatformEvents() -> void
	{
		PollEvents();
		ProcessDeferredEvents();
	}

	auto FMonaApplication::TickUI() -> void
	{
		TickTime();
		TickAndDrawWidgets();
	}

	auto FMonaApplication::GetActiveTopLevelWindow() -> std::shared_ptr<MWindow>
	{
		return ActiveTopLevelWindow.lock();
	}

	auto FMonaApplication::AddWindow(std::shared_ptr<MWindow> InMonaWindow, const bool bShowImmediately) -> std::shared_ptr<MWindow>
	{
		if (InMonaWindow == nullptr)
		{
			return nullptr;
		}

		const std::shared_ptr<MWindow> RootWindow = GetRootWindow(InMonaWindow);
		std::vector<std::shared_ptr<MWindow>> WindowHierarchy;
		FMonaWindowHelper::CollectWindowAndDescendants(RootWindow, WindowHierarchy);
		for (const std::shared_ptr<MWindow>& Window : WindowHierarchy)
		{
			if (std::ranges::find(Windows, Window) == Windows.end())
			{
				Windows.push_back(Window);
			}
		}

		FMonaWindowHelper::ArrangeWindowToFront(Windows, InMonaWindow);
		if (bShowImmediately)
		{
			ActiveTopLevelWindow = RootWindow;
		}
		if (!InMonaWindow->GetNativeWindow())
		{
			MakeWindow(InMonaWindow, bShowImmediately);
		}
		else if (bShowImmediately)
		{
			InMonaWindow->ShowWindow();
		}

		return InMonaWindow;
	}

	auto FMonaApplication::Initialize(
		bool bAdoptInitializationPresentationCandidate) -> void
	{
		InitializeRenderer(bAdoptInitializationPresentationCandidate);
	}

	auto FMonaApplication::InitializeRenderer(
		bool bAdoptInitializationPresentationCandidate) -> void
	{
		if (Renderer) return;
		Renderer = std::make_shared<FMonaRHIRenderer>(
			bAdoptInitializationPresentationCandidate);
	}

	auto FMonaApplication::ShutdownRenderer() -> void
	{
		Renderer.reset();
	}

	auto FMonaApplication::RequestDestroyWindow(std::shared_ptr<MWindow> InWindow) -> void
	{
		if (InWindow == nullptr)
		{
			return;
		}

		std::vector<std::shared_ptr<MWindow>> NewlyQueuedWindows;
		QueueWindowHierarchyForDestroy(InWindow, WindowDestroyQueue, NewlyQueuedWindows);

		for (const std::shared_ptr<MWindow>& WindowToDestroy : NewlyQueuedWindows)
		{
			if (ActiveTopLevelWindow.lock() == WindowToDestroy)
			{
				ActiveTopLevelWindow.reset();
			}
			if (Renderer) Renderer->OnWindowDestroyed(WindowToDestroy);
		}
	}

	auto FMonaApplication::CloseAllWindowsImmediately() -> void
	{
		const std::vector<std::shared_ptr<MWindow>> WindowsToDestroy(Windows.rbegin(), Windows.rend());
		for (const std::shared_ptr<MWindow>& Window : WindowsToDestroy)
		{
			RequestDestroyWindow(Window);
		}

		FlushPendingWindowDestroys();
	}

	auto FMonaApplication::DestroyWindowsImmediately() -> void
	{
		while (!WindowDestroyQueue.empty())
		{
			std::shared_ptr<MWindow> Window = WindowDestroyQueue.front();
			FlushRenderingCommands();
			WindowDestroyQueue.erase(WindowDestroyQueue.begin());
			Window->SetParentWindow(nullptr);
			const std::vector<std::shared_ptr<MWindow>> ChildWindows = Window->GetChildWindows();
			for (const std::shared_ptr<MWindow>& ChildWindow : ChildWindows)
			{
				ChildWindow->SetParentWindow(nullptr);
			}
			std::erase(Windows, Window);
		}
		WindowDestroyQueue.clear();

		if (Windows.empty())
		{
			RequestEngineExit();
		}
	}

	auto FMonaApplication::OnWindowCloseRequested(const std::shared_ptr<FGenericWindow>& PlatformWindow) -> void
	{
		const bool bGameHandled = GameEventHandler && GameEventHandler->OnWindowCloseRequested(PlatformWindow);
		if ((MonaEventHandler && MonaEventHandler->OnWindowCloseRequested(PlatformWindow)) || bGameHandled)
		{
			return;
		}

		std::shared_ptr<MWindow> Window = FMonaWindowHelper::FindWindowByPlatformWindow(Windows, PlatformWindow);
		if (Window)
		{
			Window->RequestDestroyWindow();
		}
	}

	auto FMonaApplication::FlushPendingWindowDestroys() -> void
	{
		DestroyWindowsImmediately();
	}

	auto FMonaApplication::PollEvents() -> void
	{
		if (Windows.empty())
		{
			return;
		}

		// GLFW event pumping is process-global, so one poll dispatches callbacks for all Mona windows.
		if (const std::shared_ptr<MWindow>& EventWindow = Windows.front())
		{
			EventWindow->PollEvents();
		}

		FlushPendingWindowDestroys();
	}

	auto FMonaApplication::WaitForEvents(double TimeoutSeconds) const -> void
	{
		if (!Windows.empty())
		{
			// GLFW event waiting is process-global, just like polling.
			Windows.front()->GetNativeWindow()->WaitEventsTimeout(TimeoutSeconds);
		}
	}

	auto FMonaApplication::AreAllWindowsMinimized() const -> bool
	{
		bool bHasMinimizedWindow = false;
		for (const std::shared_ptr<MWindow>& Window : Windows)
		{
			if (Window == nullptr || Window->GetParentWindow() != nullptr)
			{
				continue;
			}

			const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow();
			if (NativeWindow == nullptr)
			{
				continue;
			}

			if (Window->IsMinimized())
			{
				bHasMinimizedWindow = true;
				continue;
			}

			if (NativeWindow->IsVisible()) return false;
		}
		return bHasMinimizedWindow;
	}

	auto FMonaApplication::GetWindows() const -> const std::vector<std::shared_ptr<MWindow>>&
	{
		return Windows;
	}

	auto FMonaApplication::ProcessDeferredEvents() -> void
	{
	}

	auto FMonaApplication::FindWidgetWindow(const std::shared_ptr<MWidget>& InWidget) -> std::shared_ptr<MWindow>
	{
		std::shared_ptr<MWidget> Curr = InWidget;
		while (Curr)
		{
			if (Curr->IsWindow())
			{
				return std::dynamic_pointer_cast<MWindow>(Curr);
			}
			Curr = Curr->GetParent();
		}

		return nullptr;
	}

	auto FMonaApplication::GetRenderer() const -> FMonaRenderer*
	{
		return Renderer.get();
	}

	auto FMonaApplication::DrawWindows() -> void
	{
		for (const std::shared_ptr<MWindow>& Window : Windows)
		{
			if (Window != nullptr)
			{
				Window->Draw();
			}
		}
	}

	auto FMonaApplication::FindWindowByPlatformWindow(const std::shared_ptr<FGenericWindow>& InPlatformWindow) const -> std::shared_ptr<MWindow>
	{
		return FMonaWindowHelper::FindWindowByPlatformWindow(Windows, InPlatformWindow);
	}

	auto FMonaApplication::FindMonaWindowByNativeWindowHandle(void* InNativeWindowHandle) const -> std::shared_ptr<MWindow>
	{
		for (const auto& Window : Windows)
		{
			std::shared_ptr<FGenericWindow> PlatformWindow = Window->GetNativeWindow();
			if (PlatformWindow && PlatformWindow->GetOSNativeWindowHandle() == InNativeWindowHandle)
			{
				return Window;
			}
		}
		return nullptr;
	}

	auto FMonaApplication::FindWindowByNativeWindowHandle(void* InNativeWindowHandle) -> std::shared_ptr<FGenericWindow>
	{
		if (const std::shared_ptr<MWindow> Window = FindMonaWindowByNativeWindowHandle(InNativeWindowHandle))
		{
			return Window->GetNativeWindow();
		}
		return nullptr;
	}

	auto FMonaApplication::SetMonaEventHandler(std::unique_ptr<FMonaEventHandler> InHandler) -> void
	{
		MonaEventHandler = std::move(InHandler);
	}

	auto FMonaApplication::SetGameEventHandler(std::unique_ptr<FMonaEventHandler> InHandler) -> void
	{
		GameEventHandler = std::move(InHandler);
	}

	auto FMonaApplication::GetActiveTopLevelWindow() const -> std::shared_ptr<MWindow>
	{
		return ActiveTopLevelWindow.lock();
	}

	void FMonaApplication::OnWindowFocus(const std::shared_ptr<FGenericWindow>& InPlatformWindow, bool bFocused)
	{
		const std::shared_ptr<MWindow> Window = FMonaWindowHelper::FindWindowByPlatformWindow(Windows, InPlatformWindow);

		if (bFocused)
		{
			if (Window)
			{
				FMonaWindowHelper::ArrangeWindowToFront(Windows, Window);
				ActiveTopLevelWindow = GetRootWindow(Window);
				// DURIN_DEBUG(STR("Window gained focus, setting active top level window to: {}"), Window->GetTitle());
			}
		}
		else
		{
			std::shared_ptr<MWindow> PinnedActiveTopLevelWindow = ActiveTopLevelWindow.lock();
			if (PinnedActiveTopLevelWindow && PinnedActiveTopLevelWindow->GetNativeWindow() == InPlatformWindow)
			{
				ActiveTopLevelWindow.reset();
				// DURIN_DEBUG(STR("Window lost focus, resetting active top level window"));
			}
		}

		if (MonaEventHandler)
		{
			MonaEventHandler->OnWindowFocused(InPlatformWindow, bFocused);
		}
		if (GameEventHandler) GameEventHandler->OnWindowFocused(InPlatformWindow, bFocused);
	}

	FMonaApplication::FMonaApplication()
	{
		MessageHandler = this;
		MonaEventHandler = std::make_unique<FMonaEventHandler>(); // Default handler, does nothing
	}

	auto FMonaApplication::MakeWindow(const std::shared_ptr<MWindow>& InMonaWindow, bool bInShowImmediately) -> std::shared_ptr<FGenericWindow>
	{
		std::shared_ptr<FGenericWindow> NewWindow = MakePlatformWindow();

		const auto Definition = std::make_shared<FGenericWindowDefinition>();

		Definition->Title = InMonaWindow->GetTitle();
		Definition->DecorationMode = InMonaWindow->GetWindowDecorationMode();

		const FVector2f DesiredScreenPosition = InMonaWindow->GetDesiredScreenPosition();
		Definition->XDesiredPositionOnScreen = DesiredScreenPosition.x;
		Definition->YDesiredPositionOnScreen = DesiredScreenPosition.y;
		const FVector2f DesiredSize = InMonaWindow->GetDesiredSize();
		Definition->WidthDesiredOnScreen = DesiredSize.x;
		Definition->HeightDesiredOnScreen = DesiredSize.y;

		NewWindow->Initialize(Definition);
		InMonaWindow->SetNativeWindow(NewWindow);
		InMonaWindow->SetCachedScreenPosition(DesiredScreenPosition);
		InMonaWindow->SetCachedSize(DesiredSize);

		if (bInShowImmediately)
		{
			NewWindow->Show();
		}

		return NewWindow;
	}

	auto FMonaApplication::TickTime() -> void
	{
	}

	auto FMonaApplication::TickAndDrawWidgets() -> void
	{
	}

	auto FMonaApplication::OnWindowResize(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InWidth, int32 InHeight, bool bInWasMinimized) -> void
	{
		if (const std::shared_ptr<MWindow> Window = FMonaWindowHelper::FindWindowByPlatformWindow(Windows, InPlatformWindow))
		{
			const FVector2f NewSize = FVector2f(static_cast<float>(InWidth), static_cast<float>(InHeight));
			Window->SetCachedSize(NewSize);
		}

		if (MonaEventHandler)
		{
			MonaEventHandler->OnWindowResized(InPlatformWindow, InWidth, InHeight, bInWasMinimized);
		}
	}

	auto FMonaApplication::OnWindowViewportResize(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InWidth, int32 InHeight, bool bInWasMinimized) -> void
	{
		if (const std::shared_ptr<MWindow> Window = FMonaWindowHelper::FindWindowByPlatformWindow(Windows, InPlatformWindow))
		{
			const FVector2f NewViewportSize = FVector2f(static_cast<float>(InWidth), static_cast<float>(InHeight));
			Window->SetCachedViewportSize(NewViewportSize);

			if (MonaEventHandler)
			{
				MonaEventHandler->OnWindowViewportResized(InPlatformWindow, InWidth, InHeight, bInWasMinimized);
			}

			if (!bInWasMinimized)
			{
				Renderer->RequestResize(Window, InWidth, InHeight);
			}
		}
	}

	auto FMonaApplication::OnWindowMoved(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InX, int32 InY) -> void
	{
		if (const std::shared_ptr<MWindow> Window = FMonaWindowHelper::FindWindowByPlatformWindow(Windows, InPlatformWindow))
		{
			Window->SetCachedScreenPosition(FVector2f(static_cast<float>(InX), static_cast<float>(InY)));
		}

		if (MonaEventHandler)
		{
			MonaEventHandler->OnWindowMoved(InPlatformWindow, InX, InY);
		}
	}

	auto FMonaApplication::OnKeyDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods, bool IsRepeat) -> bool
	{
		const bool bUIHandled = MonaEventHandler && MonaEventHandler->OnKeyDown(InPlatformWindow, Key, Mods, IsRepeat);
		const bool bGameHandled = GameEventHandler && GameEventHandler->OnKeyDown(InPlatformWindow, Key, Mods, IsRepeat);
		return bUIHandled || bGameHandled;
	}

	auto FMonaApplication::OnKeyUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods) -> bool
	{
		const bool bUIHandled = MonaEventHandler && MonaEventHandler->OnKeyUp(InPlatformWindow, Key, Mods);
		const bool bGameHandled = GameEventHandler && GameEventHandler->OnKeyUp(InPlatformWindow, Key, Mods);
		return bUIHandled || bGameHandled;
	}

	auto FMonaApplication::OnKeyChar(const std::shared_ptr<FGenericWindow>& InPlatformWindow, uint32 Codepoint) -> bool
	{
		return MonaEventHandler ? MonaEventHandler->OnKeyChar(InPlatformWindow, Codepoint) : false;
	}

	auto FMonaApplication::OnMouseMove(const std::shared_ptr<FGenericWindow>& InPlatformWindow, const FVector2d CursorPos) -> bool
	{
		const bool bUIHandled = MonaEventHandler && MonaEventHandler->OnMouseMove(InPlatformWindow, CursorPos);
		const bool bGameHandled = GameEventHandler && GameEventHandler->OnMouseMove(InPlatformWindow, CursorPos);
		return bUIHandled || bGameHandled;
	}

	auto FMonaApplication::OnMouseEnter(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> void
	{
		if (MonaEventHandler)
		{
			MonaEventHandler->OnMouseEnter(InPlatformWindow);
		}
		if (GameEventHandler) GameEventHandler->OnMouseEnter(InPlatformWindow);
	}

	auto FMonaApplication::OnMouseLeave(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> void
	{
		if (MonaEventHandler)
		{
			MonaEventHandler->OnMouseLeave(InPlatformWindow);
		}
		if (GameEventHandler) GameEventHandler->OnMouseLeave(InPlatformWindow);
	}

	auto FMonaApplication::OnMouseDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool
	{
		const bool bUIHandled = MonaEventHandler && MonaEventHandler->OnMouseDown(InPlatformWindow, Button, CursorPos);
		const bool bGameHandled = GameEventHandler && GameEventHandler->OnMouseDown(InPlatformWindow, Button, CursorPos);
		return bUIHandled || bGameHandled;
	}

	auto FMonaApplication::OnMouseUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool
	{
		const bool bUIHandled = MonaEventHandler && MonaEventHandler->OnMouseUp(InPlatformWindow, Button, CursorPos);
		const bool bGameHandled = GameEventHandler && GameEventHandler->OnMouseUp(InPlatformWindow, Button, CursorPos);
		return bUIHandled || bGameHandled;
	}

	bool FMonaApplication::OnMouseWheel(const std::shared_ptr<FGenericWindow>& InPlatformWindow, double DeltaX, double DeltaY)
	{
		const bool bUIHandled = MonaEventHandler && MonaEventHandler->OnMouseWheel(InPlatformWindow, DeltaX, DeltaY);
		const bool bGameHandled = GameEventHandler && GameEventHandler->OnMouseWheel(InPlatformWindow, DeltaX, DeltaY);
		return bUIHandled || bGameHandled;
	}
} // namespace Durin::Mona
