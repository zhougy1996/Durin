#include "Application/ModalLoopTick.h"
#include "Application/GenericApplication.h"
#include "ApplicationCoreGlobals.h"
#include "Window/GenericWindow.h"

#include <gtest/gtest.h>

#if defined(_WIN32)
#include <Windows.h>

namespace
{
	constexpr WPARAM ModalLoopTimerIdentity = 0x44555249;
	int ModalTickCount = 0;
	int LaterHookMessageCount = 0;
	WNDPROC LaterHookPreviousWindowProc = nullptr;

	auto CountModalTick() -> void
	{
		++ModalTickCount;
	}

	auto LaterWindowProc(HWND WindowHandle, UINT Message, WPARAM WParam, LPARAM LParam) -> LRESULT
	{
		if (Message == WM_APP + 1)
		{
			++LaterHookMessageCount;
		}
		return ::CallWindowProcW(LaterHookPreviousWindowProc, WindowHandle, Message, WParam, LParam);
	}

	auto MakeHiddenWindow() -> std::shared_ptr<Durin::FGenericWindow>
	{
		auto Window = Durin::MakePlatformWindow();
		auto Definition = std::make_shared<Durin::FGenericWindowDefinition>();
		Definition->XDesiredPositionOnScreen = 0.0f;
		Definition->YDesiredPositionOnScreen = 0.0f;
		Definition->WidthDesiredOnScreen = 320.0f;
		Definition->HeightDesiredOnScreen = 200.0f;
		Definition->Title = "Native modal-loop test";
		Window->Initialize(Definition);
		return Window;
	}

	class FNativeWindowModalLoopTests : public testing::Test
	{
	protected:
		static auto SetUpTestSuite() -> void
		{
			Durin::InitializeApplicationCore();
		}

		static auto TearDownTestSuite() -> void
		{
			Durin::SetModalLoopTickCallback(nullptr);
			Durin::ShutdownApplicationCore();
		}

		auto SetUp() -> void override
		{
			ModalTickCount = 0;
			LaterHookMessageCount = 0;
			LaterHookPreviousWindowProc = nullptr;
			Durin::SetModalLoopTickCallback(CountModalTick);
		}

		auto TearDown() -> void override
		{
			Durin::SetModalLoopTickCallback(nullptr);
		}
	};
}

TEST_F(FNativeWindowModalLoopTests, BoundsTimerRequestsToModalLifetimeAndIdentity)
{
	auto Window = MakeHiddenWindow();
	const HWND WindowHandle = static_cast<HWND>(Window->GetOSNativeWindowHandle());

	::SendMessageW(WindowHandle, WM_TIMER, ModalLoopTimerIdentity, 0);
	EXPECT_EQ(ModalTickCount, 0);
	::SendMessageW(WindowHandle, WM_ENTERSIZEMOVE, 0, 0);
	::SendMessageW(WindowHandle, WM_ENTERSIZEMOVE, 0, 0);
	::SendMessageW(WindowHandle, WM_TIMER, ModalLoopTimerIdentity + 1, 0);
	EXPECT_EQ(ModalTickCount, 0);
	::SendMessageW(WindowHandle, WM_TIMER, ModalLoopTimerIdentity, 0);
	EXPECT_EQ(ModalTickCount, 1);

	::SendMessageW(WindowHandle, WM_EXITSIZEMOVE, 0, 0);
	EXPECT_EQ(ModalTickCount, 2);
	::SendMessageW(WindowHandle, WM_EXITSIZEMOVE, 0, 0);
	::SendMessageW(WindowHandle, WM_TIMER, ModalLoopTimerIdentity, 0);
	EXPECT_EQ(ModalTickCount, 2);
}

TEST_F(FNativeWindowModalLoopTests, MissingCallbackAndWindowDestructionAreSafe)
{
	auto Window = MakeHiddenWindow();
	const HWND WindowHandle = static_cast<HWND>(Window->GetOSNativeWindowHandle());
	Durin::SetModalLoopTickCallback(nullptr);
	::SendMessageW(WindowHandle, WM_ENTERSIZEMOVE, 0, 0);
	::SendMessageW(WindowHandle, WM_TIMER, ModalLoopTimerIdentity, 0);
	Window.reset();

	EXPECT_EQ(ModalTickCount, 0);
	EXPECT_EQ(::IsWindow(WindowHandle), FALSE);
}

TEST_F(FNativeWindowModalLoopTests, PreservesAWindowProcedureInstalledAfterTheDurinHook)
{
	auto Window = MakeHiddenWindow();
	const HWND WindowHandle = static_cast<HWND>(Window->GetOSNativeWindowHandle());
	LaterHookPreviousWindowProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
		WindowHandle,
		GWLP_WNDPROC,
		reinterpret_cast<LONG_PTR>(LaterWindowProc)));
	ASSERT_NE(LaterHookPreviousWindowProc, nullptr);

	::SendMessageW(WindowHandle, WM_APP + 1, 0, 0);
	::SendMessageW(WindowHandle, WM_ENTERSIZEMOVE, 0, 0);
	::SendMessageW(WindowHandle, WM_TIMER, ModalLoopTimerIdentity, 0);
	::SendMessageW(WindowHandle, WM_EXITSIZEMOVE, 0, 0);
	EXPECT_EQ(LaterHookMessageCount, 1);
	EXPECT_EQ(ModalTickCount, 2);

	::SetWindowLongPtrW(
		WindowHandle,
		GWLP_WNDPROC,
		reinterpret_cast<LONG_PTR>(LaterHookPreviousWindowProc));
	LaterHookPreviousWindowProc = nullptr;
}
#else
TEST(FNativeWindowModalLoopTests, NonWindowsBuildHasPlatformNeutralCallbackBoundary)
{
	Durin::SetModalLoopTickCallback(nullptr);
	Durin::RequestModalLoopTick();
	SUCCEED();
}
#endif
