#include "Application/MonaApplication.h"

#include "CoreGlobals.h"

#include "Application/MonaWindowHelper.h"
#include "Misc/ApplicationCoreGlobals.h"
#include "Rendering/MonaRHIRenderer.h"
#include "Widgets/MWindow.h"
#include "Window/GlfwWindow.h"

#include "RenderingThread.h"

namespace Doge::Mona
{
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

	auto FMonaApplication::GetActiveTopLevelWindow() -> std::shared_ptr<MWindow>
	{
		// TODO: tmp, implement it later
		if (Windows.empty())
		{
			return nullptr;
		}
		return Windows[0];
	}

	auto FMonaApplication::AddWindow(std::shared_ptr<MWindow> InMonaWindow, const bool bShowImmediately) -> std::shared_ptr<MWindow>
	{
		FMonaWindowHelper::ArrangeWindowToFront(Windows, InMonaWindow);
		std::shared_ptr<FGenericWindow> NewWindow = MakeWindow(InMonaWindow, bShowImmediately);

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
		WindowDestroyQueue.push_back(InWindow);
		Renderer->OnWindowDestroyed(InWindow);
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
			std::shared_ptr<MWindow> Window = WindowDestroyQueue.front();
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

	auto FMonaApplication::OnWindowClose(const std::shared_ptr<FGenericWindow>& PlatformWindow) -> void
	{
		std::shared_ptr<MWindow> Window = FMonaWindowHelper::FindWindowByPlatformWindow(Windows, PlatformWindow);
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
		if (!Windows.empty())
		{
			std::shared_ptr<FGenericWindow> PlatformWindowToClose = nullptr;
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
		Renderer->DrawWindows();
	}

	auto FMonaApplication::FindWindowByNativeWindowHandle(void* InNativeWindowHandle) -> std::shared_ptr<FGenericWindow>
	{
		for (auto& Window : Windows)
		{
			std::shared_ptr<FGenericWindow> PlatformWindow = Window->GetNativeWindow();
			if (PlatformWindow && PlatformWindow->GetOSNativeWindowHandle() == InNativeWindowHandle)
			{
				return PlatformWindow;
			}
		}
		return nullptr;
	}

	FMonaApplication::FMonaApplication()
	{
		MessageHandler = this;
	}

	auto FMonaApplication::MakeWindow(const std::shared_ptr<MWindow>& InMonaWindow, bool bInShowImmediately) -> std::shared_ptr<FGenericWindow>
	{
		std::shared_ptr<FGenericWindow> NewWindow = FGlfwWindow::Make();

		const auto Definition = std::make_shared<FGenericWindowDefinition>();

		Definition->Title = InMonaWindow->GetTitle();

		const FVector2f DesiredScreenPosition = InMonaWindow->GetDesiredScreenPosition();
		Definition->XDesiredPositionOnScreen = DesiredScreenPosition.x;
		Definition->YDesiredPositionOnScreen = DesiredScreenPosition.y;
		const FVector2f DesiredSize = InMonaWindow->GetDesiredSize();
		Definition->WidthDesiredOnScreen = DesiredSize.x;
		Definition->HeightDesiredOnScreen = DesiredSize.y;

		NewWindow->Initialize(Definition);
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

	auto FMonaApplication::OnWindowResize(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InWidth, int32 InHeight) -> void
	{
		if (const std::shared_ptr<MWindow> Window = FMonaWindowHelper::FindWindowByPlatformWindow(Windows, InPlatformWindow))
		{
			FVector2f NewSize = FVector2f(static_cast<float>(InWidth), static_cast<float>(InHeight));
			Window->SetCachedSize(NewSize);
			Renderer->RequestResize(Window, InWidth, InHeight);
		}
	}
} // namespace Doge::Mona