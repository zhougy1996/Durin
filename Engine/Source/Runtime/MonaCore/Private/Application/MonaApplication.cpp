#include "Application/MonaApplication.h"

#include "CoreGlobals.h"
#include "RHIResources.h"

#include "Application/MonaWindowHelper.h"
#include "Misc/ApplicationCoreGlobals.h"
#include "Rendering/MonaRHIRenderer.h"
#include "Widgets/MWindow.h"
#include "Window/GlfwWindow.h"

namespace Doge::Mona
{
	TSharedPtr<FMonaApplication> FMonaApplication::CurrentApplication_ = nullptr;

	FMonaApplication::~FMonaApplication()
	{
	}

	auto FMonaApplication::Create() -> void
	{
		CurrentApplication_ = std::shared_ptr<FMonaApplication>(new FMonaApplication());
		GApp = CurrentApplication_;
	}

	auto FMonaApplication::Shutdown() -> void
	{
		CurrentApplication_->CloseAllWindowsImmediately();
	}

	auto FMonaApplication::Get() -> FMonaApplication&
	{
		return *CurrentApplication_;
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
		if (Windows_.empty())
		{
			return nullptr;
		}
		return Windows_[0];
	}

	auto FMonaApplication::AddWindow(TSharedPtr<MWindow> InMonaWindow, const bool bShowImmediately) -> TSharedPtr<MWindow>
	{
		FMonaWindowHelper::ArrangeWindowToFront(Windows_, InMonaWindow);
		TSharedPtr<FGenericWindow> NewWindow = MakeWindow(InMonaWindow, bShowImmediately);

		return InMonaWindow;
	}

	auto FMonaApplication::Initialize() -> void
	{
		InitializeRenderer();
	}

	auto FMonaApplication::InitializeRenderer() -> void
	{
		Renderer_ = std::make_shared<FMonaRHIRenderer>();
	}

	auto FMonaApplication::RequestDestroyWindow(TSharedPtr<MWindow> Window) -> void
	{
		WindowDestroyQueue_.push_back(Window);
		DestroyWindowsImmediately();
	}

	auto FMonaApplication::CloseAllWindowsImmediately() -> void
	{
		std::for_each(Windows_.rbegin(), Windows_.rend(), [](auto& window) { window->RequestDestroyWindow(); });
	}

	auto FMonaApplication::DestroyWindowsImmediately() -> void
	{
		while (!WindowDestroyQueue_.empty())
		{
			TSharedPtr<MWindow> Window = WindowDestroyQueue_.front();
			TSharedPtr<FRHIViewport> Viewport = Window->GetRHIViewport();
			Viewport->WaitForLastFrameCompletion();
			WindowDestroyQueue_.erase(WindowDestroyQueue_.begin());
			Windows_.erase(std::remove(Windows_.begin(), Windows_.end(), Window), Windows_.end());
		}
		WindowDestroyQueue_.clear();

		if (Windows_.empty())
		{
			RequestEngineExit();
		}
	}

	auto FMonaApplication::OnWindowClose(TSharedPtr<FGenericWindow> PlatformWindow) -> void
	{
		TSharedPtr<MWindow> Window = FMonaWindowHelper::FindWindowByPlatformWindow(Windows_, PlatformWindow);
		if (Window)
		{
			Window->RequestDestroyWindow();
		}
	}

	auto FMonaApplication::PollEvents()
	{
		for (auto& EventWindow : Windows_)
		{
			EventWindow->PollEvents();
		}
	}

	auto FMonaApplication::ProcessDeferredEvents() -> void
	{
		if (Windows_.size())
		{
			TSharedPtr<FGenericWindow> PlatformWindowToClose = nullptr;
			for (auto& EventWindow : Windows_)
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

	auto FMonaApplication::FindWidgetWindow(TSharedPtr<MWidget> Widget) -> TSharedPtr<MWindow>
	{
		TSharedPtr<MWidget> Curr = Widget;
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
		return Renderer_.get();
	}

	auto FMonaApplication::FindWindowByNativeWindowHandle(void* InNativeWindowHandle) -> TSharedPtr<FGenericWindow>
	{
		for (auto& Window : Windows_)
		{
			TSharedPtr<FGenericWindow> PlatformWindow = Window->GetNativeWindow();
			if (PlatformWindow && PlatformWindow->GetOSNativeWindowHandle() == InNativeWindowHandle)
			{
				return PlatformWindow;
			}
		}
		return nullptr;
	}

	auto FMonaApplication::MakeWindow(TSharedPtr<MWindow> MonaWindow, bool bShowImmediately) -> TSharedPtr<FGenericWindow>
	{
		TSharedPtr<FGenericWindow> NewWindow = FGlfwWindow::Make();

		TSharedPtr<FGenericWindowDefinition> Definition = std::make_shared<FGenericWindowDefinition>();

		Definition->Title = MonaWindow->GetTitle();

		const FVector2f DesiredScreenPosition = MonaWindow->GetDesiredScreenPosition();
		Definition->XDesiredPositionOnScreen = DesiredScreenPosition.x;
		Definition->YDesiredPositionOnScreen = DesiredScreenPosition.y;
		const FVector2f DesiredSize = MonaWindow->GetDesiredSize();
		Definition->WidthDesiredOnScreen = DesiredSize.x;
		Definition->HeightDesiredOnScreen = DesiredSize.y;

		NewWindow->Initialize(this, Definition);
		MonaWindow->SetNativeWindow(NewWindow);

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