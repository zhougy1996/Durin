#include "Application/GenericApplication.h"
#include "Application/GenericApplicationMessageHandler.h"
#include "ApplicationCoreGlobals.h"
#include "Window/GenericWindow.h"

#include <gtest/gtest.h>

namespace
{
	class FWindowTestMessageHandler final
		: public Durin::FGenericApplicationMessageHandler
	{
	public:
		auto OnWindowResize(
			const std::shared_ptr<Durin::FGenericWindow>&,
			Durin::int32,
			Durin::int32,
			bool) -> void override
		{
			++ResizeCount;
		}

		auto OnWindowViewportResize(
			const std::shared_ptr<Durin::FGenericWindow>&,
			Durin::int32,
			Durin::int32,
			bool) -> void override
		{
			++ViewportResizeCount;
		}

		Durin::uint32 ResizeCount = 0;
		Durin::uint32 ViewportResizeCount = 0;
	};

	class FWindowTestApplication final : public Durin::FGenericApplication
	{
	public:
		explicit FWindowTestApplication(FWindowTestMessageHandler& InHandler)
		{
			SetMessageHandler(&InHandler);
		}

		auto FindWindowByNativeWindowHandle(void* Handle)
			-> std::shared_ptr<Durin::FGenericWindow> override
		{
			const auto Pinned = Window.lock();
			return Pinned && Pinned->GetOSNativeWindowHandle() == Handle
				? Pinned : nullptr;
		}

		std::weak_ptr<Durin::FGenericWindow> Window;
	};

	auto MakeDefinition() -> std::shared_ptr<Durin::FGenericWindowDefinition>
	{
		auto Definition = std::make_shared<Durin::FGenericWindowDefinition>();
		Definition->XDesiredPositionOnScreen = 80.0f;
		Definition->YDesiredPositionOnScreen = 80.0f;
		Definition->WidthDesiredOnScreen = 320.0f;
		Definition->HeightDesiredOnScreen = 240.0f;
		Definition->Title = "Durin macOS window qualification";
		return Definition;
	}
}

TEST(FMacOSWindowLifecycleTests, RepeatedHiddenCocoaWindowsExposeRetinaAndEventState)
{
	ASSERT_TRUE(Durin::InitializeApplicationCore());
	ASSERT_TRUE(Durin::InitializeApplicationCore());
	Durin::ShutdownApplicationCore();
	ASSERT_TRUE(Durin::IsApplicationCoreInitialized());

	FWindowTestMessageHandler Handler;
	auto Application = std::make_shared<FWindowTestApplication>(Handler);
	Durin::GApp = Application;
	const std::vector<Durin::FMonitorInfo> Monitors = Durin::EnumerateMonitors();
	ASSERT_FALSE(Monitors.empty());
	for (const Durin::FMonitorInfo& Monitor : Monitors)
	{
		EXPECT_GT(Monitor.MainSize.x, 0);
		EXPECT_GT(Monitor.MainSize.y, 0);
		EXPECT_GT(Monitor.DpiScale, 0.0f);
	}

	for (Durin::uint32 Cycle = 0; Cycle < 3; ++Cycle)
	{
		auto Window = Durin::MakePlatformWindow();
		Application->Window = Window;
		Window->Initialize(MakeDefinition());
		ASSERT_NE(Window->GetOSNativeWindowHandle(), nullptr) << "cycle " << Cycle;
		const Durin::FIntPoint LogicalSize = Window->GetWindowSize();
		const Durin::FIntPoint ViewportSize = Window->GetViewportSize();
		EXPECT_EQ(LogicalSize, (Durin::FIntPoint{320, 240}));
		EXPECT_GE(ViewportSize.x, LogicalSize.x);
		EXPECT_GE(ViewportSize.y, LogicalSize.y);
		EXPECT_GT(Window->GetDpiScale(), 0.0f);
		EXPECT_FALSE(Window->IsVisible());

		Window->ResizeWindow(360, 260);
		Window->PollEvents();
		EXPECT_EQ(Window->GetWindowSize(), (Durin::FIntPoint{360, 260}));
		Window->SetTitle("Durin macOS window qualification updated");
		Window->SetCursorMode(Durin::ECursorMode::Hidden);
		Window->SetCursorMode(Durin::ECursorMode::Free);
		Window->SetShouldClose(true);
		EXPECT_TRUE(Window->ShouldClose());
		Window->SetShouldClose(false);
		EXPECT_FALSE(Window->ShouldClose());
		Application->Window.reset();
		Window.reset();
	}

	Durin::GApp.reset();
	Application.reset();
	Durin::ShutdownApplicationCore();
	EXPECT_FALSE(Durin::IsApplicationCoreInitialized());
}

TEST(FMacOSWindowLifecycleTests, CocoaWindowCreationRejectsWorkerThreads)
{
	ASSERT_TRUE(Durin::InitializeApplicationCore());
	auto Window = Durin::MakePlatformWindow();
	std::thread Worker([&] { Window->Initialize(MakeDefinition()); });
	Worker.join();
	EXPECT_EQ(Window->GetOSNativeWindowHandle(), nullptr);
	Window.reset();
	Durin::ShutdownApplicationCore();
}
