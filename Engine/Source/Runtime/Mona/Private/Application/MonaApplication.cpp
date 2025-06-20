#include "Application/MonaApplication.h"

#include "Application/MonaWindowHelper.h"

TSharedPtr<FKleeApplication> FKleeApplication::CurrentApplication_ = nullptr;

FKleeApplication::~FKleeApplication()
{
}

auto FKleeApplication::Create() -> void
{
	CurrentApplication_ = std::shared_ptr<FKleeApplication>(new FKleeApplication());
}

auto FKleeApplication::Shutdown() -> void
{
	CurrentApplication_->CloseAllWindowsImmediately();
}

auto FKleeApplication::Get() -> FKleeApplication&
{
	return *CurrentApplication_;
}

auto FKleeApplication::Tick() -> void
{
	TickPlatform();
	TickTime();
	TickAndDrawWidgets();
}

auto FKleeApplication::GetActiveTopLevelWindow() -> TSharedPtr<KWindow>
{
	// TODO: tmp, implement it later
	if (Windows_.empty())
	{
		return nullptr;
	}
	return Windows_[0];
}

auto FKleeApplication::AddWindow(TSharedPtr<KWindow> InKleeWindow, const bool bShowImmediately) -> TSharedPtr<KWindow>
{
	FKleeWindowHelper::ArrangeWindowToFront(Windows_, InKleeWindow);
	TSharedPtr<FGenericWindow> NewWindow = MakeWindow(InKleeWindow, bShowImmediately);

	return InKleeWindow;
}

auto FKleeApplication::Initialize() -> void
{
	InitializeRenderer();
}

auto FKleeApplication::InitializeRenderer() -> void
{
	Renderer_ = std::make_shared<FKleeRHIRenderer>();
}

auto FKleeApplication::RequestDestroyWindow(TSharedPtr<KWindow> Window) -> void
{
	WindowDestroyQueue_.push_back(Window);
	DestroyWindowsImmediately();
}

auto FKleeApplication::CloseAllWindowsImmediately() -> void
{
	std::for_each(Windows_.rbegin(), Windows_.rend(), [](auto& window) { window->RequestDestroyWindow(); });
}

auto FKleeApplication::DestroyWindowsImmediately() -> void
{
	while (!WindowDestroyQueue_.empty())
	{
		TSharedPtr<KWindow> Window = WindowDestroyQueue_.front();
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

auto FKleeApplication::OnWindowClose(TSharedPtr<FGenericWindow> PlatformWindow) -> void
{
	TSharedPtr<KWindow> Window = FKleeWindowHelper::FindWindowByPlatformWindow(Windows_, PlatformWindow);
	if (Window)
	{
		Window->RequestDestroyWindow();
	}
}

auto FKleeApplication::PollEvents()
{
	for (auto& EventWindow : Windows_)
	{
		EventWindow->PollEvents();
	}
}

auto FKleeApplication::ProcessDeferredEvents() -> void
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

auto FKleeApplication::FindWidgetWindow(TSharedPtr<KWidget> Widget) -> TSharedPtr<KWindow>
{
	TSharedPtr<KWidget> Curr = Widget;
	while (Curr)
	{
		if (Curr->IsWindow())
		{
			return std::dynamic_pointer_cast<KWindow>(Curr);
		}
		Curr = Curr->GetParent();
	}

	return nullptr;
}

auto FKleeApplication::GetRenderer() const -> FKleeRenderer*
{
	return Renderer_.get();
}

auto FKleeApplication::MakeWindow(TSharedPtr<KWindow> KleeWindow, const bool bShowImmediately) -> TSharedPtr<FGenericWindow>
{
	TSharedPtr<FGenericWindow> NewWindow = FGlfwWindow::Make();
	KleeWindow->SetNativeWindow(NewWindow);

	TSharedPtr<FGenericWindowDefinition> Definition = std::make_shared<FGenericWindowDefinition>();

	Definition->Title = KleeWindow->GetTitle();

	const FVector2f DesiredScreenPosition = KleeWindow->GetDesiredScreenPosition();
	Definition->XDesiredPositionOnScreen = DesiredScreenPosition.x;
	Definition->YDesiredPositionOnScreen = DesiredScreenPosition.y;
	const FVector2f DesiredSize = KleeWindow->GetDesiredSize();
	Definition->WidthDesiredOnScreen = DesiredSize.x;
	Definition->HeightDesiredOnScreen = DesiredSize.y;

	NewWindow->Initialize(this, Definition);
	KleeWindow->SetNativeWindow(NewWindow);

	return NewWindow;
}

auto FKleeApplication::TickPlatform() -> void
{
	PollEvents();
	ProcessDeferredEvents();
}

auto FKleeApplication::TickTime() -> void
{
}

auto FKleeApplication::TickAndDrawWidgets() -> void
{
}
