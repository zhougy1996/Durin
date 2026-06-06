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
		TickPlatform();
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
		MakeWindow(InMonaWindow, bShowImmediately);

		return InMonaWindow;
	}

	auto FMonaApplication::Initialize() -> void
	{
		InitializeRenderer();
	}

	auto FMonaApplication::InitializeRenderer() -> void
	{
		Renderer = std::make_shared<FMonaRHIRenderer>();
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
			Renderer->OnWindowDestroyed(WindowToDestroy);
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
		if (MonaEventHandler && MonaEventHandler->OnWindowCloseRequested(PlatformWindow))
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

	auto FMonaApplication::PollEvents()
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

		Renderer->RenderViewports();
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
		Definition->bHasOSWindowBorder = InMonaWindow->IsWindowDecorated();

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

	auto FMonaApplication::TickPlatform() -> void
	{
		PollEvents();
		ProcessDeferredEvents();
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
	}

	auto FMonaApplication::OnWindowViewportResize(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InWidth, int32 InHeight, bool bInWasMinimized) -> void
	{
		if (const std::shared_ptr<MWindow> Window = FMonaWindowHelper::FindWindowByPlatformWindow(Windows, InPlatformWindow))
		{
			const FVector2f NewViewportSize = FVector2f(static_cast<float>(InWidth), static_cast<float>(InHeight));
			Window->SetCachedViewportSize(NewViewportSize);

			if (!bInWasMinimized)
			{
				Renderer->RequestResize(Window, InWidth, InHeight);
			}
		}
	}

	auto FMonaApplication::OnWindowRefresh(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> void
	{
		if (MonaEventHandler)
		{
			MonaEventHandler->OnWindowRefresh(InPlatformWindow);
		}

		if (InPlatformWindow == nullptr || InPlatformWindow->IsMinimized())
		{
			return;
		}

		if (GRefreshRenderFrameHandler != nullptr)
		{
			GRefreshRenderFrameHandler(InPlatformWindow->GetOSNativeWindowHandle());
		}
	}

	auto FMonaApplication::OnKeyDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods, bool IsRepeat) -> bool
	{
		if (MonaEventHandler && MonaEventHandler->OnKeyDown(InPlatformWindow, Key, Mods, IsRepeat))
		{
			return true;
		}
		return false;
	}

	auto FMonaApplication::OnKeyUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods) -> bool
	{
		return MonaEventHandler ? MonaEventHandler->OnKeyUp(InPlatformWindow, Key, Mods) : false;
	}

	auto FMonaApplication::OnKeyChar(const std::shared_ptr<FGenericWindow>& InPlatformWindow, uint32 Codepoint) -> bool
	{
		return MonaEventHandler ? MonaEventHandler->OnKeyChar(InPlatformWindow, Codepoint) : false;
	}

	auto FMonaApplication::OnMouseMove(const std::shared_ptr<FGenericWindow>& InPlatformWindow, const FVector2d CursorPos) -> bool
	{
		return MonaEventHandler ? MonaEventHandler->OnMouseMove(InPlatformWindow, CursorPos) : false;
	}

	auto FMonaApplication::OnMouseDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool
	{
		return MonaEventHandler ? MonaEventHandler->OnMouseDown(InPlatformWindow, Button, CursorPos) : false;
	}

	auto FMonaApplication::OnMouseUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool
	{
		return MonaEventHandler ? MonaEventHandler->OnMouseUp(InPlatformWindow, Button, CursorPos) : false;
	}

	bool FMonaApplication::OnMouseWheel(const std::shared_ptr<FGenericWindow>& InPlatformWindow, double DeltaX, double DeltaY)
	{
		return MonaEventHandler ? MonaEventHandler->OnMouseWheel(InPlatformWindow, DeltaX, DeltaY) : false;
	}
} // namespace Durin::Mona
