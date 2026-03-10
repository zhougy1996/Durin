#include "Application/MonaApplication.h"

#include "CoreGlobals.h"
#include "RHIResources.h"

#include "Application/MonaWindowHelper.h"
#include "Misc/ApplicationCoreGlobals.h"
#include "Rendering/MonaRHIRenderer.h"
#include "Widgets/MWindow.h"
#include "Window/GlfwWindow.h"

#include "RenderingThread.h"

namespace Doge::Mona
{
	TSharedPtr<FMonaApplication> FMonaApplication::CurrentApplication = nullptr;

	FMonaApplication::~FMonaApplication()
	{
	}

	auto FMonaApplication::Create() -> void
	{
		CurrentApplication = std::make_shared<FMonaApplication>();
		GApp = CurrentApplication;
	}

	auto FMonaApplication::Shutdown() -> void
	{
		CurrentApplication->CloseAllWindowsImmediately();
	}

	auto FMonaApplication::Get() -> FMonaApplication&
	{
		return *CurrentApplication;
	}

	auto FMonaApplication::Tick() -> void
	{
		TickPlatform();
		TickTime();
		TickAndDrawWidgets();
	}

	auto FMonaApplication::GetActiveTopLevelWindow() -> TSharedPtr<MWindow>
	{
		// TODO: tmp, implement it later
		if (Windows.empty())
		{
			return nullptr;
		}
		return Windows[0];
	}

	auto FMonaApplication::AddWindow(TSharedPtr<MWindow> InMonaWindow, const bool bShowImmediately) -> TSharedPtr<MWindow>
	{
		FMonaWindowHelper::ArrangeWindowToFront(Windows, InMonaWindow);
		TSharedPtr<FGenericWindow> NewWindow = MakeWindow(InMonaWindow, bShowImmediately);

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

	auto FMonaApplication::RequestDestroyWindow(TSharedPtr<MWindow> InWindow) -> void
	{
		WindowDestroyQueue.push_back(InWindow);
		DestroyWindowsImmediately();
	}

	auto FMonaApplication::CloseAllWindowsImmediately() -> void
	{
		std::for_each(Windows.rbegin(), Windows.rend(), [](auto& window) { window->RequestDestroyWindow(); });
	}

	auto FMonaApplication::DestroyWindowsImmediately() -> void
	{
		while (!WindowDestroyQueue.empty())
		{
			TSharedPtr<MWindow> Window = WindowDestroyQueue.front();
			FlushRenderingCommands();
			WindowDestroyQueue.erase(WindowDestroyQueue.begin());
			std::erase(Windows, Window);
		}
		WindowDestroyQueue.clear();

		if (Windows.empty())
		{
			RequestEngineExit();
		}
	}

	auto FMonaApplication::OnWindowClose(TSharedPtr<FGenericWindow> PlatformWindow) -> void
	{
		TSharedPtr<MWindow> Window = FMonaWindowHelper::FindWindowByPlatformWindow(Windows, PlatformWindow);
		if (Window)
		{
			Window->RequestDestroyWindow();
		}
	}

	auto FMonaApplication::PollEvents()
	{
		for (auto& EventWindow : Windows)
		{
			EventWindow->PollEvents();
		}
	}

	auto FMonaApplication::ProcessDeferredEvents() -> void
	{
		if (Windows.size())
		{
			TSharedPtr<FGenericWindow> PlatformWindowToClose = nullptr;
			for (auto& EventWindow : Windows)
			{
				auto PlatformWindow = EventWindow->GetNativeWindow();
				if (PlatformWindow && PlatformWindow->ShouldClose())
				{
					PlatformWindowToClose = PlatformWindow;
				}
			}
			if (PlatformWindowToClose)
			{
				OnWindowClose(PlatformWindowToClose);
			}
		}
	}

	auto FMonaApplication::FindWidgetWindow(TSharedPtr<MWidget> InWidget) -> TSharedPtr<MWindow>
	{
		TSharedPtr<MWidget> Curr = InWidget;
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

	auto FMonaApplication::FindWindowByNativeWindowHandle(void* InNativeWindowHandle) -> TSharedPtr<FGenericWindow>
	{
		for (auto& Window : Windows)
		{
			TSharedPtr<FGenericWindow> PlatformWindow = Window->GetNativeWindow();
			if (PlatformWindow && PlatformWindow->GetOSNativeWindowHandle() == InNativeWindowHandle)
			{
				return PlatformWindow;
			}
		}
		return nullptr;
	}

	auto FMonaApplication::MakeWindow(TSharedPtr<MWindow> InMonaWindow, bool bInShowImmediately) -> TSharedPtr<FGenericWindow>
	{
		TSharedPtr<FGenericWindow> NewWindow = FGlfwWindow::Make();

		TSharedPtr<FGenericWindowDefinition> Definition = std::make_shared<FGenericWindowDefinition>();

		Definition->Title = InMonaWindow->GetTitle();

		const FVector2f DesiredScreenPosition = InMonaWindow->GetDesiredScreenPosition();
		Definition->XDesiredPositionOnScreen = DesiredScreenPosition.x;
		Definition->YDesiredPositionOnScreen = DesiredScreenPosition.y;
		const FVector2f DesiredSize = InMonaWindow->GetDesiredSize();
		Definition->WidthDesiredOnScreen = DesiredSize.x;
		Definition->HeightDesiredOnScreen = DesiredSize.y;

		NewWindow->Initialize(this, Definition);
		InMonaWindow->SetNativeWindow(NewWindow);

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
}