#include "Application/ModalLoopTick.h"
#include "Application/GenericApplication.h"
#include "ApplicationCoreGlobals.h"
#include "Runtime/ApplicationCore/Private/Misc/GlfwVulkanInitialization.h"
#include "Window/GenericWindow.h"

#include <gtest/gtest.h>

TEST(FGlfwVulkanInitializationTests, RejectsMissingRequiredExtensionStorage)
{
	const Durin::FGlfwVulkanExtensionQueryResult Result =
		Durin::QueryRequiredGlfwVulkanInstanceExtensions(
			[](uint32_t* Count) -> const char** {
				*Count = 0;
				return nullptr;
			},
			[](const char** Description) {
				*Description = "Vulkan is unavailable";
				return 65542;
			});

	EXPECT_FALSE(Result.Succeeded());
	EXPECT_TRUE(Result.Extensions.empty());
	EXPECT_EQ(Result.Diagnostic,
		"GLFW Vulkan instance-extension discovery failed (65542): Vulkan is unavailable.");
}

TEST(FGlfwVulkanInitializationTests, RejectsInvalidRequiredExtensionName)
{
	const char* Extensions[] = {"VK_KHR_surface", nullptr};
	const Durin::FGlfwVulkanExtensionQueryResult Result =
		Durin::QueryRequiredGlfwVulkanInstanceExtensions(
			[&Extensions](uint32_t* Count) {
				*Count = 2;
				return Extensions;
			},
			[](const char**) { return 0; });

	EXPECT_FALSE(Result.Succeeded());
	EXPECT_TRUE(Result.Extensions.empty());
	EXPECT_EQ(Result.Diagnostic,
		"GLFW Vulkan instance-extension discovery returned an invalid name at index 1.");
}

TEST(FGlfwVulkanInitializationTests, CopiesValidatedExtensionsIntoOwnedStorage)
{
	char SurfaceExtension[] = "VK_KHR_surface";
	char PlatformExtension[] = "VK_KHR_win32_surface";
	const char* Extensions[] = {SurfaceExtension, PlatformExtension};
	const Durin::FGlfwVulkanExtensionQueryResult Result =
		Durin::QueryRequiredGlfwVulkanInstanceExtensions(
			[&Extensions](uint32_t* Count) {
				*Count = 2;
				return Extensions;
			},
			[](const char**) { return 0; });

	ASSERT_TRUE(Result.Succeeded());
	ASSERT_EQ(Result.Extensions.size(), 2u);
	SurfaceExtension[0] = 'X';
	PlatformExtension[0] = 'X';
	EXPECT_EQ(Result.Extensions[0], "VK_KHR_surface");
	EXPECT_EQ(Result.Extensions[1], "VK_KHR_win32_surface");
}

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

	auto MakeHiddenWindow(
		Durin::EWindowDecorationMode DecorationMode =
			Durin::EWindowDecorationMode::System)
		-> std::shared_ptr<Durin::FGenericWindow>
	{
		auto Window = Durin::MakePlatformWindow();
		auto Definition = std::make_shared<Durin::FGenericWindowDefinition>();
		Definition->XDesiredPositionOnScreen = 0.0f;
		Definition->YDesiredPositionOnScreen = 0.0f;
		Definition->WidthDesiredOnScreen = 320.0f;
		Definition->HeightDesiredOnScreen = 200.0f;
		Definition->Title = "Native modal-loop test";
		Definition->DecorationMode = DecorationMode;
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
	RECT ProposedBounds{0, 0, 320, 200};

	::SendMessageW(WindowHandle, WM_TIMER, ModalLoopTimerIdentity, 0);
	EXPECT_EQ(ModalTickCount, 0);
	::SendMessageW(WindowHandle, WM_ENTERSIZEMOVE, 0, 0);
	::SendMessageW(WindowHandle, WM_ENTERSIZEMOVE, 0, 0);
	::SendMessageW(WindowHandle, WM_SIZING, WMSZ_RIGHT,
		reinterpret_cast<LPARAM>(&ProposedBounds));
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

TEST_F(FNativeWindowModalLoopTests, PrioritizesNativeMovementButContinuesSizingFrames)
{
	auto Window = MakeHiddenWindow();
	const HWND WindowHandle = static_cast<HWND>(Window->GetOSNativeWindowHandle());
	RECT ProposedBounds{0, 0, 320, 200};

	::SendMessageW(WindowHandle, WM_ENTERSIZEMOVE, 0, 0);
	::SendMessageW(WindowHandle, WM_TIMER, ModalLoopTimerIdentity, 0);
	EXPECT_EQ(ModalTickCount, 0);
	::SendMessageW(WindowHandle, WM_MOVING, 0,
		reinterpret_cast<LPARAM>(&ProposedBounds));
	::SendMessageW(WindowHandle, WM_TIMER, ModalLoopTimerIdentity, 0);
	EXPECT_EQ(ModalTickCount, 0);
	::SendMessageW(WindowHandle, WM_EXITSIZEMOVE, 0, 0);
	EXPECT_EQ(ModalTickCount, 1);

	::SendMessageW(WindowHandle, WM_ENTERSIZEMOVE, 0, 0);
	::SendMessageW(WindowHandle, WM_SIZING, WMSZ_RIGHT,
		reinterpret_cast<LPARAM>(&ProposedBounds));
	::SendMessageW(WindowHandle, WM_TIMER, ModalLoopTimerIdentity, 0);
	EXPECT_EQ(ModalTickCount, 2);
	::SendMessageW(WindowHandle, WM_EXITSIZEMOVE, 0, 0);
	EXPECT_EQ(ModalTickCount, 3);
}

TEST_F(FNativeWindowModalLoopTests, CustomFrameMovementUsesNativeMinMaxFastPath)
{
	auto Window = MakeHiddenWindow(Durin::EWindowDecorationMode::CustomTitleBar);
	const HWND WindowHandle = static_cast<HWND>(Window->GetOSNativeWindowHandle());
	ASSERT_EQ(Window->GetEffectiveWindowDecorationMode(),
		Durin::EWindowDecorationMode::CustomTitleBar);

	MINMAXINFO CustomFrameInfo{};
	::SendMessageW(WindowHandle, WM_GETMINMAXINFO, 0,
		reinterpret_cast<LPARAM>(&CustomFrameInfo));
	EXPECT_GE(CustomFrameInfo.ptMinTrackSize.x,
		::MulDiv(640, static_cast<int>(::GetDpiForWindow(WindowHandle)),
			USER_DEFAULT_SCREEN_DPI));

	RECT ProposedBounds{0, 0, 320, 200};
	::SendMessageW(WindowHandle, WM_ENTERSIZEMOVE, 0, 0);
	::SendMessageW(WindowHandle, WM_MOVING, 0,
		reinterpret_cast<LPARAM>(&ProposedBounds));
	MINMAXINFO MovingInfo{};
	::SendMessageW(WindowHandle, WM_GETMINMAXINFO, 0,
		reinterpret_cast<LPARAM>(&MovingInfo));
	EXPECT_LT(MovingInfo.ptMinTrackSize.x, CustomFrameInfo.ptMinTrackSize.x);
	::SendMessageW(WindowHandle, WM_EXITSIZEMOVE, 0, 0);
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
	RECT ProposedBounds{0, 0, 320, 200};
	::SendMessageW(WindowHandle, WM_SIZING, WMSZ_RIGHT,
		reinterpret_cast<LPARAM>(&ProposedBounds));
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
