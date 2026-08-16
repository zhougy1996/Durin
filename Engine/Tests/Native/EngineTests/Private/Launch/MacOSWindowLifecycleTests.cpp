#include "Application/GenericApplication.h"
#include "Application/GenericApplicationMessageHandler.h"
#include "ApplicationCoreGlobals.h"
#include "MacOS/MacOSCustomTitleBarBridge.h"
#include "Window/GenericWindow.h"

#include <gtest/gtest.h>
#include <objc/message.h>
#include <objc/runtime.h>

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

	auto MakeDefinition(
		Durin::EWindowDecorationMode DecorationMode = Durin::EWindowDecorationMode::System)
		-> std::shared_ptr<Durin::FGenericWindowDefinition>
	{
		auto Definition = std::make_shared<Durin::FGenericWindowDefinition>();
		Definition->XDesiredPositionOnScreen = 80.0f;
		Definition->YDesiredPositionOnScreen = 80.0f;
		Definition->WidthDesiredOnScreen = 320.0f;
		Definition->HeightDesiredOnScreen = 240.0f;
		Definition->DecorationMode = DecorationMode;
		Definition->Title = "Durin macOS window qualification";
		return Definition;
	}

	template <typename ReturnType, typename... ArgumentTypes>
	auto SendObjectiveCMessage(
		void* Receiver,
		const char* SelectorName,
		ArgumentTypes... Arguments)
		-> ReturnType
	{
		using FMessage = ReturnType (*)(void*, SEL, ArgumentTypes...);
		return reinterpret_cast<FMessage>(objc_msgSend)(
			Receiver, sel_registerName(SelectorName), Arguments...);
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
		void* ContentView = SendObjectiveCMessage<void*>(
			Window->GetOSNativeWindowHandle(), "contentView");
		ASSERT_NE(ContentView, nullptr) << "cycle " << Cycle;
		void* PresentationLayer = SendObjectiveCMessage<void*>(
			ContentView, "layer");
		ASSERT_NE(PresentationLayer, nullptr) << "cycle " << Cycle;
		EXPECT_STREQ(
			object_getClassName(reinterpret_cast<id>(PresentationLayer)),
			"CAMetalLayer")
			<< "cycle " << Cycle;
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

TEST(FMacOSWindowLifecycleTests, CustomTitleBarRoutesDoubleClickToNativeZoom)
{
	Durin::FWindowTitleBarLayout Layout;
	Layout.bValid = true;
	Layout.DragRegions.push_back({40, 0, 300, 36});

	EXPECT_EQ(
		Durin::ResolveMacOSCustomTitleBarMouseDown(Layout, {100, 10}, 1),
		Durin::EMacOSCustomTitleBarMouseDownAction::Drag);
	EXPECT_EQ(
		Durin::ResolveMacOSCustomTitleBarMouseDown(Layout, {100, 10}, 2),
		Durin::EMacOSCustomTitleBarMouseDownAction::Zoom);
	EXPECT_EQ(
		Durin::ResolveMacOSCustomTitleBarMouseDown(Layout, {20, 10}, 2),
		Durin::EMacOSCustomTitleBarMouseDownAction::PassThrough);

	Layout.bValid = false;
	EXPECT_EQ(
		Durin::ResolveMacOSCustomTitleBarMouseDown(Layout, {100, 10}, 2),
		Durin::EMacOSCustomTitleBarMouseDownAction::PassThrough);
}

TEST(FMacOSWindowLifecycleTests, CustomTitleBarRetainsNativeWindowControlsAndMetalLayer)
{
	ASSERT_TRUE(Durin::InitializeApplicationCore());
	FWindowTestMessageHandler Handler;
	auto Application = std::make_shared<FWindowTestApplication>(Handler);
	Durin::GApp = Application;

	for (Durin::uint32 Cycle = 0; Cycle < 3; ++Cycle)
	{
		auto Window = Durin::MakePlatformWindow();
		Application->Window = Window;
		Window->Initialize(MakeDefinition(Durin::EWindowDecorationMode::CustomTitleBar));
		ASSERT_NE(Window->GetOSNativeWindowHandle(), nullptr) << "cycle " << Cycle;
		EXPECT_EQ(
			Window->GetEffectiveWindowDecorationMode(),
			Durin::EWindowDecorationMode::CustomTitleBar);

		void* NativeWindow = Window->GetOSNativeWindowHandle();
		EXPECT_NE(SendObjectiveCMessage<void*>(NativeWindow, "delegate"), nullptr);
		constexpr Durin::uint64 FullSizeContentViewStyle = 1ull << 15;
		const Durin::uint64 StyleMask = SendObjectiveCMessage<Durin::uint64>(NativeWindow, "styleMask");
		EXPECT_NE(StyleMask & FullSizeContentViewStyle, 0u);
		EXPECT_EQ(SendObjectiveCMessage<Durin::int64>(NativeWindow, "titleVisibility"), 1);
		EXPECT_TRUE(SendObjectiveCMessage<bool>(NativeWindow, "titlebarAppearsTransparent"));

		for (const Durin::int64 ButtonKind : {0, 1, 2})
		{
			void* Button = SendObjectiveCMessage<void*>(
				NativeWindow, "standardWindowButton:", ButtonKind);
			ASSERT_NE(Button, nullptr) << "button " << ButtonKind << ", cycle " << Cycle;
			EXPECT_FALSE(SendObjectiveCMessage<bool>(Button, "isHidden"));
		}

		const Durin::FWindowTitleBarPlatformMetrics Metrics = Window->GetTitleBarPlatformMetrics();
		EXPECT_TRUE(Metrics.bNativeWindowControls);
		EXPECT_GT(Metrics.NativeControlExclusion.MaxX, Metrics.NativeControlExclusion.MinX);
		EXPECT_GT(Metrics.NativeControlExclusion.MaxY, Metrics.NativeControlExclusion.MinY);

		Durin::FWindowTitleBarLayout Layout;
		Layout.Generation = Cycle + 1;
		Layout.bValid = true;
		Layout.Height = 36;
		Layout.DragRegions.push_back({Metrics.NativeControlExclusion.MaxX, 0, 300, 36});
		Window->PublishTitleBarLayout(Layout);
		Window->SetTitleBarDarkMode(false);
		Window->SetTitleBarDarkMode(true);

		void* ContentView = SendObjectiveCMessage<void*>(NativeWindow, "contentView");
		void* PresentationLayer = SendObjectiveCMessage<void*>(ContentView, "layer");
		ASSERT_NE(PresentationLayer, nullptr);
		EXPECT_STREQ(
			object_getClassName(reinterpret_cast<id>(PresentationLayer)),
			"CAMetalLayer");

		Application->Window.reset();
		Window.reset();
	}

	Durin::GApp.reset();
	Application.reset();
	Durin::ShutdownApplicationCore();
}
