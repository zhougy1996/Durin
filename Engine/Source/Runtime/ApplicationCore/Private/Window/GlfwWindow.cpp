#include "Window/GlfwWindow.h"

#include "ThirdParty/Glfw/GlfwCommon.h"
#include "Application/GenericApplication.h"
#include "Application/GenericApplicationMessageHandler.h"
#include "Application/ModalLoopTick.h"
#include "ApplicationCore.h"
#include "CoreGlobals.h"

#if defined(_WIN32)
#include <dwmapi.h>
#include <shellapi.h>
#endif

namespace Durin
{
#if defined(_WIN32)
	struct FWindowsModalLoopBridge
	{
		static auto WindowProc(HWND WindowHandle, UINT Message, WPARAM WParam, LPARAM LParam) -> LRESULT;
	};
#endif

	namespace
	{
		auto SanitizeDpiScale(float DpiScale) -> float
		{
			// GLFW reports zero when the platform DPI query temporarily fails, such as while a display is sleeping.
			return DpiScale > 0.0f && DpiScale < 99.0f ? DpiScale : 1.0f;
		}

#if defined(_WIN32)
		constexpr wchar_t GlfwWindowInstanceProperty[] = L"DURIN_APPLICATION_CORE_GLFW_WINDOW";
		constexpr int32 AutoHideTaskbarActivationEdge = 1;

		auto GetWindowsMaximizedClientRect(HWND WindowHandle, MONITORINFO& OutMonitorInfo) -> RECT
		{
			const HMONITOR Monitor = MonitorFromWindow(WindowHandle, MONITOR_DEFAULTTONEAREST);
			OutMonitorInfo = {.cbSize = sizeof(MONITORINFO)};
			if (!GetMonitorInfoW(Monitor, &OutMonitorInfo))
			{
				OutMonitorInfo.cbSize = 0;
				return {};
			}

			RECT MaximizedRect = OutMonitorInfo.rcWork;
			for (const UINT Edge : {ABE_LEFT, ABE_TOP, ABE_RIGHT, ABE_BOTTOM})
			{
				APPBARDATA AppBarData{.cbSize = sizeof(APPBARDATA)};
				AppBarData.uEdge = Edge;
				AppBarData.rc = OutMonitorInfo.rcMonitor;
				if (SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &AppBarData) == 0) continue;

				// rcWork can continue reserving the full taskbar band while the bar
				// auto-hides. Maximize into that band and retain only one physical
				// pixel so the shell's reveal edge remains reachable.
				switch (Edge)
				{
				case ABE_LEFT:
					MaximizedRect.left = OutMonitorInfo.rcMonitor.left + AutoHideTaskbarActivationEdge;
					break;
				case ABE_TOP:
					MaximizedRect.top = OutMonitorInfo.rcMonitor.top + AutoHideTaskbarActivationEdge;
					break;
				case ABE_RIGHT:
					MaximizedRect.right = OutMonitorInfo.rcMonitor.right - AutoHideTaskbarActivationEdge;
					break;
				case ABE_BOTTOM:
					MaximizedRect.bottom = OutMonitorInfo.rcMonitor.bottom - AutoHideTaskbarActivationEdge;
					break;
				default:
					break;
				}
			}
			return MaximizedRect;
		}

		auto WindowTitleBarHitTestToWindows(EWindowTitleBarHitTest HitTest) -> LRESULT
		{
			switch (HitTest)
			{
			case EWindowTitleBarHitTest::Caption: return HTCAPTION;
			case EWindowTitleBarHitTest::Minimize: return HTMINBUTTON;
			case EWindowTitleBarHitTest::Maximize: return HTMAXBUTTON;
			case EWindowTitleBarHitTest::Close: return HTCLOSE;
			case EWindowTitleBarHitTest::ResizeLeft: return HTLEFT;
			case EWindowTitleBarHitTest::ResizeRight: return HTRIGHT;
			case EWindowTitleBarHitTest::ResizeTop: return HTTOP;
			case EWindowTitleBarHitTest::ResizeBottom: return HTBOTTOM;
			case EWindowTitleBarHitTest::ResizeTopLeft: return HTTOPLEFT;
			case EWindowTitleBarHitTest::ResizeTopRight: return HTTOPRIGHT;
			case EWindowTitleBarHitTest::ResizeBottomLeft: return HTBOTTOMLEFT;
			case EWindowTitleBarHitTest::ResizeBottomRight: return HTBOTTOMRIGHT;
			default: return HTCLIENT;
			}
		}

		auto WindowsToWindowTitleBarHitTest(WPARAM HitTest) -> EWindowTitleBarHitTest
		{
			switch (HitTest)
			{
			case HTMINBUTTON: return EWindowTitleBarHitTest::Minimize;
			case HTMAXBUTTON: return EWindowTitleBarHitTest::Maximize;
			case HTCLOSE: return EWindowTitleBarHitTest::Close;
			case HTCAPTION: return EWindowTitleBarHitTest::Caption;
			default: return EWindowTitleBarHitTest::Client;
			}
		}

		auto IsWindowTitleBarButton(EWindowTitleBarHitTest HitTest) -> bool
		{
			return HitTest == EWindowTitleBarHitTest::Minimize
				|| HitTest == EWindowTitleBarHitTest::Maximize
				|| HitTest == EWindowTitleBarHitTest::Close;
		}

		auto WindowTitleBarButtonSystemCommand(EWindowTitleBarHitTest HitTest, bool bMaximized) -> WPARAM
		{
			switch (HitTest)
			{
			case EWindowTitleBarHitTest::Minimize: return SC_MINIMIZE;
			case EWindowTitleBarHitTest::Maximize: return bMaximized ? SC_RESTORE : SC_MAXIMIZE;
			case EWindowTitleBarHitTest::Close: return SC_CLOSE;
			default: return 0;
			}
		}

		auto CALLBACK GlfwWindowMessageBridge(HWND WindowHandle, UINT Message, WPARAM WParam, LPARAM LParam) -> LRESULT
		{
			auto* Window = static_cast<FGlfwWindow*>(GetPropW(WindowHandle, GlfwWindowInstanceProperty));
			if (Window == nullptr) return DefWindowProcW(WindowHandle, Message, WParam, LParam);
			bool bHandled = false;
			const int64 Result = Window->ProcessWindowsMessage(
				static_cast<uint32>(Message), static_cast<uint64>(WParam), static_cast<int64>(LParam), bHandled);
			return static_cast<LRESULT>(Result);
		}

		auto ApplyWindowsWindowIcon(void* NativeWindowHandle) -> void
		{
			const HWND WindowHandle = static_cast<HWND>(NativeWindowHandle);
			constexpr int32 ApplicationIconResourceId = 101;
			const HINSTANCE ExecutableInstance = GetModuleHandleW(nullptr);
			const HICON LargeIcon = static_cast<HICON>(LoadImageW(
				ExecutableInstance,
				MAKEINTRESOURCEW(ApplicationIconResourceId),
				IMAGE_ICON,
				GetSystemMetrics(SM_CXICON),
				GetSystemMetrics(SM_CYICON),
				LR_SHARED));
			const HICON SmallIcon = static_cast<HICON>(LoadImageW(
				ExecutableInstance,
				MAKEINTRESOURCEW(ApplicationIconResourceId),
				IMAGE_ICON,
				GetSystemMetrics(SM_CXSMICON),
				GetSystemMetrics(SM_CYSMICON),
				LR_SHARED));
			if (LargeIcon != nullptr)
			{
				SendMessageW(WindowHandle, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(LargeIcon));
			}
			if (SmallIcon != nullptr)
			{
				SendMessageW(WindowHandle, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(SmallIcon));
			}
		}

		constexpr UINT_PTR RequestedModalLoopTimerIdentity = 0x44555249;
		constexpr UINT ModalLoopTimerIntervalMilliseconds = 16;
		constexpr const char* ModalLoopWindowProp = "DURIN_MODAL_LOOP_WINDOW";
		bool bModalLoopPropertyFailureReported = false;
		bool bModalLoopHookFailureReported = false;
		bool bModalLoopTimerFailureReported = false;

#endif

		const std::unordered_map<int32, EKey> GlfwKeyMap = {
			{GLFW_KEY_ESCAPE, EKey::Escape},
			{GLFW_KEY_CAPS_LOCK, EKey::CapsLock},
			{GLFW_KEY_LEFT_SHIFT, EKey::LShift},
			{GLFW_KEY_RIGHT_SHIFT, EKey::RShift},
			{GLFW_KEY_LEFT_ALT, EKey::LAlt},
			{GLFW_KEY_RIGHT_ALT, EKey::RAlt},
			{GLFW_KEY_LEFT_CONTROL, EKey::LControl},
			{GLFW_KEY_RIGHT_CONTROL, EKey::RControl},
			{GLFW_KEY_TAB, EKey::Tab},
			{GLFW_KEY_SPACE, EKey::Space},
			{GLFW_KEY_ENTER, EKey::Enter},
			{GLFW_KEY_BACKSPACE, EKey::Backspace},
			{GLFW_KEY_LEFT, EKey::Left},
			{GLFW_KEY_RIGHT, EKey::Right},
			{GLFW_KEY_UP, EKey::Up},
			{GLFW_KEY_DOWN, EKey::Down},
			{GLFW_KEY_PAGE_UP, EKey::PageUp},
			{GLFW_KEY_PAGE_DOWN, EKey::PageDown},
			{GLFW_KEY_HOME, EKey::Home},
			{GLFW_KEY_END, EKey::End},
			{GLFW_KEY_INSERT, EKey::Insert},
			{GLFW_KEY_DELETE, EKey::Delete},
			{GLFW_KEY_F1, EKey::F1},
			{GLFW_KEY_F2, EKey::F2},
			{GLFW_KEY_F3, EKey::F3},
			{GLFW_KEY_F4, EKey::F4},
			{GLFW_KEY_F5, EKey::F5},
			{GLFW_KEY_F6, EKey::F6},
			{GLFW_KEY_F7, EKey::F7},
			{GLFW_KEY_F8, EKey::F8},
			{GLFW_KEY_F9, EKey::F9},
			{GLFW_KEY_F10, EKey::F10},
			{GLFW_KEY_F11, EKey::F11},
			{GLFW_KEY_F12, EKey::F12},
			{GLFW_KEY_COMMA, EKey::Comma},
			{GLFW_KEY_PERIOD, EKey::Period},
			{GLFW_KEY_APOSTROPHE, EKey::Apostrophe},
			{GLFW_KEY_SEMICOLON, EKey::Semicolon},
			{GLFW_KEY_SLASH, EKey::Slash},
			{GLFW_KEY_BACKSLASH, EKey::Backslash},
			{GLFW_KEY_LEFT_BRACKET, EKey::LeftBracket},
			{GLFW_KEY_RIGHT_BRACKET, EKey::RightBracket},
			{GLFW_KEY_GRAVE_ACCENT, EKey::GraveAccent},
			{GLFW_KEY_MINUS, EKey::Minus},
			{GLFW_KEY_EQUAL, EKey::Equal},
			{GLFW_KEY_KP_0, EKey::Keypad0},
			{GLFW_KEY_KP_1, EKey::Keypad1},
			{GLFW_KEY_KP_2, EKey::Keypad2},
			{GLFW_KEY_KP_3, EKey::Keypad3},
			{GLFW_KEY_KP_4, EKey::Keypad4},
			{GLFW_KEY_KP_5, EKey::Keypad5},
			{GLFW_KEY_KP_6, EKey::Keypad6},
			{GLFW_KEY_KP_7, EKey::Keypad7},
			{GLFW_KEY_KP_8, EKey::Keypad8},
			{GLFW_KEY_KP_9, EKey::Keypad9},
			{GLFW_KEY_KP_DECIMAL, EKey::KeypadDecimal},
			{GLFW_KEY_KP_DIVIDE, EKey::KeypadDivide},
			{GLFW_KEY_KP_MULTIPLY, EKey::KeypadMultiply},
			{GLFW_KEY_KP_ADD, EKey::KeypadPlus},
			{GLFW_KEY_KP_SUBTRACT, EKey::KeypadMinus},
			{GLFW_KEY_KP_EQUAL, EKey::KeypadEquals},
			{GLFW_KEY_KP_ENTER, EKey::KeypadEnter},
		};

		auto FromGlfw_Key(int32 GlfwKey) -> EKey
		{
			if (const auto It = GlfwKeyMap.find(GlfwKey); It != GlfwKeyMap.end())
			{
				return It->second;
			}

			if (GlfwKey >= GLFW_KEY_A && GlfwKey <= GLFW_KEY_Z)
			{
				return static_cast<EKey>(GlfwKey);
			}

			if (GlfwKey >= GLFW_KEY_0 && GlfwKey <= GLFW_KEY_9)
			{
				const uint16 NumKey = static_cast<uint16>(EKey::Num0) + static_cast<uint16>(GlfwKey - GLFW_KEY_0);
				return static_cast<EKey>(NumKey);
			}

			return EKey::None;
		}

		auto FromGlfw_KeyModFlags(int32 InGlfwMods) -> EKeyModFlags
		{
			auto Modifier = EKeyModFlags::None;

			if (InGlfwMods & GLFW_MOD_SHIFT)
			{
				Modifier = Modifier | EKeyModFlags::Shift;
			}
			if (InGlfwMods & GLFW_MOD_CONTROL)
			{
				Modifier = Modifier | EKeyModFlags::Control;
			}
			if (InGlfwMods & GLFW_MOD_ALT)
			{
				Modifier = Modifier | EKeyModFlags::Alt;
			}
			if (InGlfwMods & GLFW_MOD_SUPER)
			{
				Modifier = Modifier | EKeyModFlags::Super;
			}
			return Modifier;
		}

		auto FromGlfw_KeyAction(int32 GlfwAction) -> EKeyAction
		{
			switch (GlfwAction)
			{
			case GLFW_PRESS: return EKeyAction::Press;
			case GLFW_RELEASE: return EKeyAction::Release;
			case GLFW_REPEAT: return EKeyAction::Repeat;
			default: return EKeyAction::Press;
			}
		}

		auto FromGlfw_MouseButton(int32 GlfwButton) -> EMouseButton
		{
			switch (GlfwButton)
			{
			case GLFW_MOUSE_BUTTON_LEFT: return EMouseButton::Left;
			case GLFW_MOUSE_BUTTON_RIGHT: return EMouseButton::Right;
			case GLFW_MOUSE_BUTTON_MIDDLE: return EMouseButton::Middle;
			default: return EMouseButton::Left;
			}
		}

		FORCEINLINE auto FindPlatformWindow(GLFWwindow* InGlfwWindow) -> std::shared_ptr<FGenericWindow>
		{
			return GApp->FindWindowByNativeWindowHandle(glfwGetWindowUserPointer(InGlfwWindow));
		}

		auto KeyCallBack(GLFWwindow* InGlfwWindow, int InKey, int Scancode, int InAction, int InMods) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				auto* Handler = GApp->GetMessageHandler();
				const EKeyAction Action = FromGlfw_KeyAction(InAction);
				const EKey Key = FromGlfw_Key(InKey);
				const EKeyModFlags Mods = FromGlfw_KeyModFlags(InMods);

				if (Action == EKeyAction::Press || Action == EKeyAction::Repeat)
				{
					Handler->OnKeyDown(PlatformWindow, Key, Mods, Action == EKeyAction::Repeat);
				}
				else // if (Action == EKeyAction::Release)
				{
					check(Action == EKeyAction::Release);
					Handler->OnKeyUp(PlatformWindow, Key, Mods);
				}
			}
		}

		auto CharCallBack(GLFWwindow* InGlfwWindow, uint32 Codepoint) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				GApp->GetMessageHandler()->OnKeyChar(PlatformWindow, Codepoint);
			}
		}

		auto MouseButtonCallBack(GLFWwindow* InGlfwWindow, int Button, int Action, int Mods) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				auto* Handler = GApp->GetMessageHandler();
				EMouseButton MouseButton = FromGlfw_MouseButton(Button);
				FVector2d CursorPos;
				glfwGetCursorPos(InGlfwWindow, &CursorPos.x, &CursorPos.y);
				if (Action == GLFW_PRESS)
				{
					Handler->OnMouseDown(PlatformWindow, MouseButton, CursorPos);
				}
				else if (Action == GLFW_RELEASE)
				{
					Handler->OnMouseUp(PlatformWindow, MouseButton, CursorPos);
				}
			}
		}

		auto CursorPosCallBack(GLFWwindow* InGlfwWindow, double XPos, double YPos) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				GApp->GetMessageHandler()->OnMouseMove(PlatformWindow, {XPos, YPos});
			}
		}

		auto CursorEnterCallBack(GLFWwindow* InGlfwWindow, int Entered) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				if (Entered != 0)
				{
					GApp->GetMessageHandler()->OnMouseEnter(PlatformWindow);
				}
				else
				{
					GApp->GetMessageHandler()->OnMouseLeave(PlatformWindow);
				}
			}
		}

		auto ScrollCallBack(GLFWwindow* InGlfwWindow, double XOffset, double YOffset) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				GApp->GetMessageHandler()->OnMouseWheel(PlatformWindow, XOffset, YOffset);
			}
		}

		auto WindowFocusCallBack(GLFWwindow* InGlfwWindow, int Focused) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				GApp->GetMessageHandler()->OnWindowFocus(PlatformWindow, Focused != 0);
			}
		}

		auto WindowSizeCallBack(GLFWwindow* InGlfwWindow, int Width, int Height) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				GApp->GetMessageHandler()->OnWindowResize(PlatformWindow, Width, Height, PlatformWindow->IsMinimized());
			}
		}

		auto WindowPosCallBack(GLFWwindow* InGlfwWindow, int X, int Y) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				GApp->GetMessageHandler()->OnWindowMoved(PlatformWindow, X, Y);
			}
		}

		auto FramebufferSizeCallBack(GLFWwindow* InGlfwWindow, int Width, int Height) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				GApp->GetMessageHandler()->OnWindowViewportResize(PlatformWindow, Width, Height, PlatformWindow->IsMinimized());
			}
		}

		auto WindowCloseCallBack(GLFWwindow* InGlfwWindow) -> void
		{
			if (auto PlatformWindow = FindPlatformWindow(InGlfwWindow))
			{
				GApp->GetMessageHandler()->OnWindowCloseRequested(PlatformWindow);
			}
		}

		auto ToGlfw_MouseCursor(EMouseCursor Cursor) -> int
		{
			switch (Cursor)
			{
			case EMouseCursor::None: return 0;
			case EMouseCursor::Arrow: return GLFW_ARROW_CURSOR;
			case EMouseCursor::TextInput: return GLFW_IBEAM_CURSOR;
			case EMouseCursor::ResizeAll: return GLFW_RESIZE_ALL_CURSOR;
			case EMouseCursor::ResizeNS: return GLFW_VRESIZE_CURSOR;
			case EMouseCursor::ResizeEW: return GLFW_HRESIZE_CURSOR;
			case EMouseCursor::ResizeNESW: return GLFW_RESIZE_NESW_CURSOR;
			case EMouseCursor::ResizeNWSE: return GLFW_RESIZE_NWSE_CURSOR;
			case EMouseCursor::Hand: return GLFW_POINTING_HAND_CURSOR;
			case EMouseCursor::NotAllowed: return GLFW_NOT_ALLOWED_CURSOR;
			default: return GLFW_ARROW_CURSOR;
			}
		}
	} // namespace

#if defined(_WIN32)
	auto FWindowsModalLoopBridge::WindowProc(
		HWND WindowHandle,
		UINT Message,
		WPARAM WParam,
		LPARAM LParam) -> LRESULT
	{
		auto* Window = static_cast<FGlfwWindow*>(::GetPropA(WindowHandle, ModalLoopWindowProp));
		if (Window == nullptr)
		{
			return ::DefWindowProcW(WindowHandle, Message, WParam, LParam);
		}

		auto PreviousWindowProc = reinterpret_cast<WNDPROC>(Window->PreviousWindowProcedure);
		if (Message == WM_ENTERSIZEMOVE)
		{
			Window->EnterModalLoop();
		}
		else if (Message == WM_TIMER && Window->HandleModalLoopTimer(static_cast<uint64>(WParam)))
		{
			return 0;
		}

		const LRESULT Result = PreviousWindowProc != nullptr
			? ::CallWindowProcW(PreviousWindowProc, WindowHandle, Message, WParam, LParam)
			: ::DefWindowProcW(WindowHandle, Message, WParam, LParam);
		if (Message == WM_EXITSIZEMOVE)
		{
			Window->ExitModalLoop();
		}
		return Result;
	}
#endif

	GLFWcursor* GGlfwCursors[static_cast<int32>(EMouseCursor::Count)] = {nullptr};

	auto InitGlfwCursors() -> void
	{
		for (int32 CursorIndex = 0; CursorIndex < static_cast<int32>(EMouseCursor::Count); ++CursorIndex)
		{
			GGlfwCursors[CursorIndex] = glfwCreateStandardCursor(ToGlfw_MouseCursor(static_cast<EMouseCursor>(CursorIndex)));
		}
	}

	auto DestroyGlfwCursors() -> void
	{
		for (GLFWcursor*& Cursor : GGlfwCursors)
		{
			if (Cursor != nullptr)
			{
				glfwDestroyCursor(Cursor);
				Cursor = nullptr;
			}
		}
	}

	auto EnumerateMonitors() -> std::vector<FMonitorInfo>
	{
		std::vector<FMonitorInfo> Monitors;

		int32 MonitorCount = 0;
		GLFWmonitor** GlfwMonitors = glfwGetMonitors(&MonitorCount);
		if (GlfwMonitors == nullptr || MonitorCount <= 0)
		{
			return Monitors;
		}

		Monitors.reserve(static_cast<size_t>(MonitorCount));
		for (int32 MonitorIndex = 0; MonitorIndex < MonitorCount; ++MonitorIndex)
		{
			GLFWmonitor* GlfwMonitor = GlfwMonitors[MonitorIndex];
			const GLFWvidmode* VideoMode = glfwGetVideoMode(GlfwMonitor);
			if (VideoMode == nullptr)
			{
				continue;
			}

			FMonitorInfo& MonitorInfo = Monitors.emplace_back();
			MonitorInfo.NativeHandle = GlfwMonitor;

			int32 PositionX = 0;
			int32 PositionY = 0;
			glfwGetMonitorPos(GlfwMonitor, &PositionX, &PositionY);
			MonitorInfo.MainPosition = {PositionX, PositionY};
			MonitorInfo.MainSize = {VideoMode->width, VideoMode->height};

			int32 WorkX = PositionX;
			int32 WorkY = PositionY;
			int32 WorkWidth = VideoMode->width;
			int32 WorkHeight = VideoMode->height;
			glfwGetMonitorWorkarea(GlfwMonitor, &WorkX, &WorkY, &WorkWidth, &WorkHeight);
			MonitorInfo.WorkPosition = {WorkX, WorkY};
			MonitorInfo.WorkSize = {WorkWidth, WorkHeight};

			float ScaleX = 1.0f;
			float ScaleY = 1.0f;
			glfwGetMonitorContentScale(GlfwMonitor, &ScaleX, &ScaleY);
			MonitorInfo.DpiScale = SanitizeDpiScale(ScaleX);
		}

		return Monitors;
	}

	FGlfwWindow::FGlfwWindow() = default;

	FGlfwWindow::~FGlfwWindow()
	{
		if (GlfwWindow != nullptr && CursorMode == ECursorMode::Captured)
		{
			SetCursorMode(ECursorMode::Free);
		}
		RemoveWindowsMessageBridge();
		RemoveModalLoopHook();
		glfwDestroyWindow(GlfwWindow);
		GlfwWindow = nullptr;
	}

	auto FGlfwWindow::Make() -> std::shared_ptr<FGlfwWindow>
	{
		return std::shared_ptr<FGlfwWindow>(new FGlfwWindow());
	}

	auto FGlfwWindow::Initialize(const std::shared_ptr<FGenericWindowDefinition>& InDefinition) -> void
	{
		Definition = InDefinition;

		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_RESIZABLE, WindowMode == EWindowMode::Windowed);
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
		glfwWindowHint(GLFW_DECORATED, Definition->DecorationMode == EWindowDecorationMode::None ? GLFW_FALSE : GLFW_TRUE);
#if defined(__APPLE__)
		glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
		glfwWindowHint(GLFW_COCOA_GRAPHICS_SWITCHING, GLFW_TRUE);
#endif

		const int DesiredWidth = FMath::TruncToInt32(Definition->WidthDesiredOnScreen);
		const int DesiredHeight = FMath::TruncToInt32(Definition->HeightDesiredOnScreen);
		const int DesiredX = FMath::TruncToInt32(Definition->XDesiredPositionOnScreen);
		const int DesiredY = FMath::TruncToInt32(Definition->YDesiredPositionOnScreen);
		GlfwWindow = glfwCreateWindow(DesiredWidth, DesiredHeight, Definition->Title.c_str(), nullptr, nullptr);
#if defined(_WIN32)
		OSNativeWindowHandle = glfwGetWin32Window(GlfwWindow);
		InstallModalLoopHook();
		ApplyWindowsWindowIcon(OSNativeWindowHandle);
		EffectiveDecorationMode = Definition->DecorationMode;
		if (Definition->DecorationMode == EWindowDecorationMode::CustomTitleBar)
		{
			TitleBarLayout = {
				.Generation = 1,
				.bValid = true,
				.Height = 36,
				.MinimumWindowWidth = 640,
				.DragRegions = {{0, 0, std::max(0, DesiredWidth - 138), 36}},
				.MinimizeRegion = {std::max(0, DesiredWidth - 138), 0, std::max(0, DesiredWidth - 92), 36},
				.MaximizeRegion = {std::max(0, DesiredWidth - 92), 0, std::max(0, DesiredWidth - 46), 36},
				.CloseRegion = {std::max(0, DesiredWidth - 46), 0, DesiredWidth, 36}
			};
			if (!InstallWindowsMessageBridge() || !ApplyWindowsCustomFrame())
			{
				RemoveWindowsMessageBridge();
				EffectiveDecorationMode = EWindowDecorationMode::System;
				Definition->DecorationMode = EWindowDecorationMode::System;
				DURIN_ERROR("Failed to activate the Windows custom title bar; using the system frame.");
			}
			else
			{
				DURIN_DEBUG("Activated Windows custom title bar.");
			}
		}
#elif defined(__APPLE__)
		OSNativeWindowHandle = glfwGetCocoaWindow(GlfwWindow);
		EffectiveDecorationMode = Definition->DecorationMode == EWindowDecorationMode::CustomTitleBar
			? EWindowDecorationMode::System : Definition->DecorationMode;
#else
		EffectiveDecorationMode = Definition->DecorationMode == EWindowDecorationMode::CustomTitleBar
			? EWindowDecorationMode::System : Definition->DecorationMode;
#endif

		glfwSetWindowPos(GlfwWindow, DesiredX, DesiredY);

		glfwSetWindowUserPointer(GlfwWindow, OSNativeWindowHandle);

		// Register all GLFW callbacks
		glfwSetWindowPosCallback(GlfwWindow, WindowPosCallBack);
		glfwSetWindowSizeCallback(GlfwWindow, WindowSizeCallBack);
		glfwSetFramebufferSizeCallback(GlfwWindow, FramebufferSizeCallBack);
		glfwSetKeyCallback(GlfwWindow, KeyCallBack);
		glfwSetCharCallback(GlfwWindow, CharCallBack);
		glfwSetMouseButtonCallback(GlfwWindow, MouseButtonCallBack);
		glfwSetCursorPosCallback(GlfwWindow, CursorPosCallBack);
		glfwSetCursorEnterCallback(GlfwWindow, CursorEnterCallBack);
		glfwSetScrollCallback(GlfwWindow, ScrollCallBack);
		glfwSetWindowFocusCallback(GlfwWindow, WindowFocusCallBack);
		glfwSetWindowCloseCallback(GlfwWindow, WindowCloseCallBack);

		glfwMakeContextCurrent(GlfwWindow);
	}

	auto FGlfwWindow::InstallModalLoopHook() -> void
	{
#if defined(_WIN32)
		if (OSNativeWindowHandle == nullptr || PreviousWindowProcedure != nullptr) return;
		const HWND WindowHandle = static_cast<HWND>(OSNativeWindowHandle);
		if (::SetPropA(WindowHandle, ModalLoopWindowProp, this) == FALSE)
		{
			if (!bModalLoopPropertyFailureReported)
			{
				bModalLoopPropertyFailureReported = true;
				DURIN_ERROR("Failed to install the native modal-loop window property.");
			}
			return;
		}
		::SetLastError(ERROR_SUCCESS);
		const LONG_PTR PreviousProcedure = ::SetWindowLongPtrW(
			WindowHandle,
			GWLP_WNDPROC,
			reinterpret_cast<LONG_PTR>(FWindowsModalLoopBridge::WindowProc));
		if (PreviousProcedure == 0 && ::GetLastError() != ERROR_SUCCESS)
		{
			::RemovePropA(WindowHandle, ModalLoopWindowProp);
			if (!bModalLoopHookFailureReported)
			{
				bModalLoopHookFailureReported = true;
				DURIN_ERROR("Failed to install the native modal-loop window procedure.");
			}
			return;
		}
		PreviousWindowProcedure = reinterpret_cast<void*>(PreviousProcedure);
#endif
	}

	auto FGlfwWindow::RemoveModalLoopHook() -> void
	{
#if defined(_WIN32)
		if (OSNativeWindowHandle == nullptr) return;
		const HWND WindowHandle = static_cast<HWND>(OSNativeWindowHandle);
		if (ModalLoopTimerIdentity != 0)
		{
			::KillTimer(WindowHandle, static_cast<UINT_PTR>(ModalLoopTimerIdentity));
			ModalLoopTimerIdentity = 0;
		}
		bInModalLoop = false;
		if (PreviousWindowProcedure != nullptr)
		{
			const auto CurrentProcedure = reinterpret_cast<WNDPROC>(
				::GetWindowLongPtrW(WindowHandle, GWLP_WNDPROC));
			if (CurrentProcedure == FWindowsModalLoopBridge::WindowProc)
			{
				::SetWindowLongPtrW(
					WindowHandle,
					GWLP_WNDPROC,
					reinterpret_cast<LONG_PTR>(PreviousWindowProcedure));
			}
			PreviousWindowProcedure = nullptr;
		}
		::RemovePropA(WindowHandle, ModalLoopWindowProp);
#endif
	}

	auto FGlfwWindow::EnterModalLoop() -> void
	{
#if defined(_WIN32)
		if (bInModalLoop || OSNativeWindowHandle == nullptr) return;
		bInModalLoop = true;
		const UINT_PTR Timer = ::SetTimer(
			static_cast<HWND>(OSNativeWindowHandle),
			RequestedModalLoopTimerIdentity,
			ModalLoopTimerIntervalMilliseconds,
			nullptr);
		if (Timer == 0)
		{
			if (!bModalLoopTimerFailureReported)
			{
				bModalLoopTimerFailureReported = true;
				DURIN_ERROR("Failed to start the native modal-loop frame timer.");
			}
			return;
		}
		ModalLoopTimerIdentity = static_cast<uint64>(Timer);
#endif
	}

	auto FGlfwWindow::ExitModalLoop() -> void
	{
#if defined(_WIN32)
		if (!bInModalLoop) return;
		bInModalLoop = false;
		if (ModalLoopTimerIdentity != 0)
		{
			::KillTimer(
				static_cast<HWND>(OSNativeWindowHandle),
				static_cast<UINT_PTR>(ModalLoopTimerIdentity));
			ModalLoopTimerIdentity = 0;
		}
		RequestModalLoopTick();
#endif
	}

	auto FGlfwWindow::HandleModalLoopTimer(uint64 TimerIdentity) -> bool
	{
#if defined(_WIN32)
		if (!bInModalLoop || ModalLoopTimerIdentity == 0 || TimerIdentity != ModalLoopTimerIdentity)
		{
			return false;
		}
		RequestModalLoopTick();
		return true;
#else
		return false;
#endif
	}

	void FGlfwWindow::PollEvents() const
	{
		glfwPollEvents();
	}

	void FGlfwWindow::WaitEventsTimeout(double TimeoutSeconds) const
	{
		glfwWaitEventsTimeout(TimeoutSeconds);
	}

	auto FGlfwWindow::ReshapeWindow(int32 X, int32 Y, int32 Width, int32 Height) -> void
	{
		glfwSetWindowPos(GlfwWindow, X, Y);
		glfwSetWindowSize(GlfwWindow, Width, Height);
	}

	auto FGlfwWindow::ResizeWindow(int32 Width, int32 Height) -> void
	{
		glfwSetWindowSize(GlfwWindow, Width, Height);
	}

	auto FGlfwWindow::MoveWindowTo(int32 X, int32 Y) -> void
	{
		glfwSetWindowPos(GlfwWindow, X, Y);
	}

	auto FGlfwWindow::GetWindowPosition() const -> FIntPoint
	{
		int32 X = 0;
		int32 Y = 0;
		glfwGetWindowPos(GlfwWindow, &X, &Y);
		return {X, Y};
	}

	auto FGlfwWindow::GetWindowSize() const -> FIntPoint
	{
		int32 Width = 0;
		int32 Height = 0;
		glfwGetWindowSize(GlfwWindow, &Width, &Height);
		return {Width, Height};
	}

	auto FGlfwWindow::GetCursorPosition() const -> FVector2d
	{
		FVector2d CursorPos;
		glfwGetCursorPos(GlfwWindow, &CursorPos.x, &CursorPos.y);
		return CursorPos;
	}

	auto FGlfwWindow::SetCursorPosition(FVector2d Position) -> void
	{
		if (GlfwWindow != nullptr) glfwSetCursorPos(GlfwWindow, Position.x, Position.y);
	}

	void FGlfwWindow::Close()
	{
		SetShouldClose(true);
	}

	void FGlfwWindow::SetShouldClose(bool bShouldClose)
	{
		glfwSetWindowShouldClose(GlfwWindow, bShouldClose ? GLFW_TRUE : GLFW_FALSE);
	}

	auto FGlfwWindow::Show() -> void
	{
		if (GIsWindowDisplaySuppressed) return;
		glfwShowWindow(GlfwWindow);
	}

	auto FGlfwWindow::Hide() -> void
	{
		glfwHideWindow(GlfwWindow);
	}

	auto FGlfwWindow::IsVisible() const -> bool
	{
		return glfwGetWindowAttrib(GlfwWindow, GLFW_VISIBLE) != 0;
	}

	auto FGlfwWindow::Focus() -> void
	{
		glfwFocusWindow(GlfwWindow);
	}

	bool FGlfwWindow::ShouldClose() const
	{
		return glfwWindowShouldClose(GlfwWindow);
	}

	FIntPoint FGlfwWindow::GetViewportSize() const
	{
		int Width, Height;
		glfwGetFramebufferSize(GlfwWindow, &Width, &Height);
		return {Width, Height};
	}

	void* FGlfwWindow::CreateVulkanSurface(void* InInstance) const
	{
		VkSurfaceKHR Surface;
		auto VulkanInstance = static_cast<VkInstance>(InInstance);
		if (glfwCreateWindowSurface(VulkanInstance, GlfwWindow, nullptr, &Surface) != VK_SUCCESS)
		{
			DURIN_ERROR("Failed to create window surface.");
			return nullptr;
		}

		return Surface;
	}

	bool FGlfwWindow::IsMinimized() const
	{
		return glfwGetWindowAttrib(GlfwWindow, GLFW_ICONIFIED);
	}

	auto FGlfwWindow::IsMaximized() const -> bool
	{
		return glfwGetWindowAttrib(GlfwWindow, GLFW_MAXIMIZED);
	}

	auto FGlfwWindow::MaximizeWindow() -> void
	{
		glfwMaximizeWindow(GlfwWindow);
	}

	auto FGlfwWindow::RestoreWindow() -> void
	{
		glfwRestoreWindow(GlfwWindow);
	}

	auto FGlfwWindow::MinimizeWindow() -> void
	{
		glfwIconifyWindow(GlfwWindow);
	}

	auto FGlfwWindow::IsFocused() const -> bool
	{
		return glfwGetWindowAttrib(GlfwWindow, GLFW_FOCUSED) != 0;
	}

	auto FGlfwWindow::IsHovered() const -> bool
	{
		return glfwGetWindowAttrib(GlfwWindow, GLFW_HOVERED) != 0;
	}

	auto FGlfwWindow::SetTitle(const std::string& InTitle) -> void
	{
		glfwSetWindowTitle(GlfwWindow, InTitle.c_str());
	}

	auto FGlfwWindow::SetOpacity(float InOpacity) -> void
	{
		glfwSetWindowOpacity(GlfwWindow, InOpacity);
	}

	auto FGlfwWindow::SetWindowDecorationMode(EWindowDecorationMode Mode) -> void
	{
		if (Mode == EWindowDecorationMode::CustomTitleBar)
		{
			return;
		}
		glfwSetWindowAttrib(GlfwWindow, GLFW_DECORATED, Mode == EWindowDecorationMode::System ? GLFW_TRUE : GLFW_FALSE);
		if (Definition != nullptr)
		{
			Definition->DecorationMode = Mode;
		}
		EffectiveDecorationMode = Mode;
	}

	auto FGlfwWindow::PublishTitleBarLayout(const FWindowTitleBarLayout& Layout) -> void
	{
		if (EffectiveDecorationMode == EWindowDecorationMode::CustomTitleBar && !Layout.bValid && !bLoggedInvalidTitleBarLayout)
		{
			DURIN_WARN("Received an invalid custom title-bar layout; client hit testing remains active.");
			bLoggedInvalidTitleBarLayout = true;
		}
		if (EffectiveDecorationMode == EWindowDecorationMode::CustomTitleBar
			&& Layout.Generation < TitleBarLayout.Generation && !bLoggedStaleTitleBarLayout)
		{
			DURIN_WARN("Ignored a stale custom title-bar layout generation.");
			bLoggedStaleTitleBarLayout = true;
		}
		if (EffectiveDecorationMode == EWindowDecorationMode::CustomTitleBar && Layout.Generation >= TitleBarLayout.Generation)
		{
			TitleBarLayout = Layout;
		}
	}

	auto FGlfwWindow::GetTitleBarInteractionState() const -> FWindowTitleBarInteractionState
	{
		FWindowTitleBarInteractionState State = TitleBarInteractionState;
		State.bFocused = IsFocused();
		State.bMaximized = IsMaximized();
		return State;
	}

	auto FGlfwWindow::SetTitleBarDarkMode(bool bDarkMode) -> void
	{
#if defined(_WIN32)
		const BOOL bUseDarkMode = bDarkMode ? TRUE : FALSE;
		DwmSetWindowAttribute(
			static_cast<HWND>(OSNativeWindowHandle),
			DWMWA_USE_IMMERSIVE_DARK_MODE,
			&bUseDarkMode,
			sizeof(bUseDarkMode));
#endif
	}

	auto FGlfwWindow::SetMousePassthrough(bool bPassthrough) -> void
	{
		glfwSetWindowAttrib(GlfwWindow, GLFW_MOUSE_PASSTHROUGH, bPassthrough ? GLFW_TRUE : GLFW_FALSE);
	}

	auto FGlfwWindow::SetFocusOnShow(bool bFocusOnShow) -> void
	{
		glfwSetWindowAttrib(GlfwWindow, GLFW_FOCUS_ON_SHOW, bFocusOnShow ? GLFW_TRUE : GLFW_FALSE);
	}

	auto FGlfwWindow::GetDpiScale() const -> float
	{
		float ScaleX = 1.0f;
		float ScaleY = 1.0f;
		glfwGetWindowContentScale(GlfwWindow, &ScaleX, &ScaleY);
		return SanitizeDpiScale(ScaleX);
	}

	auto FGlfwWindow::InstallWindowsMessageBridge() -> bool
	{
#if defined(_WIN32)
		if (bWindowsMessageBridgeInstalled) return true;
		const HWND WindowHandle = static_cast<HWND>(OSNativeWindowHandle);
		if (WindowHandle == nullptr || !SetPropW(WindowHandle, GlfwWindowInstanceProperty, this)) return false;
		SetLastError(ERROR_SUCCESS);
		const LONG_PTR PreviousProcedure = SetWindowLongPtrW(
			WindowHandle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GlfwWindowMessageBridge));
		if (PreviousProcedure == 0 && GetLastError() != ERROR_SUCCESS)
		{
			RemovePropW(WindowHandle, GlfwWindowInstanceProperty);
			return false;
		}
		PreviousWindowsProcedure = reinterpret_cast<void*>(PreviousProcedure);
		bWindowsMessageBridgeInstalled = true;
		return true;
#else
		return false;
#endif
	}

	auto FGlfwWindow::RemoveWindowsMessageBridge() -> void
	{
#if defined(_WIN32)
		if (!bWindowsMessageBridgeInstalled) return;
		const HWND WindowHandle = static_cast<HWND>(OSNativeWindowHandle);
		if (WindowHandle != nullptr)
		{
			const auto CurrentProcedure = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(WindowHandle, GWLP_WNDPROC));
			if (CurrentProcedure == GlfwWindowMessageBridge && PreviousWindowsProcedure != nullptr)
			{
				SetWindowLongPtrW(WindowHandle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PreviousWindowsProcedure));
			}
			RemovePropW(WindowHandle, GlfwWindowInstanceProperty);
		}
		PreviousWindowsProcedure = nullptr;
		bWindowsMessageBridgeInstalled = false;
#endif
	}

	auto FGlfwWindow::ApplyWindowsCustomFrame() -> bool
	{
#if defined(_WIN32)
		const HWND WindowHandle = static_cast<HWND>(OSNativeWindowHandle);
		if (WindowHandle == nullptr || !bWindowsMessageBridgeInstalled) return false;
		SetLastError(ERROR_SUCCESS);
		const LONG_PTR CurrentStyle = GetWindowLongPtrW(WindowHandle, GWL_STYLE);
		if (CurrentStyle == 0 && GetLastError() != ERROR_SUCCESS) return false;
		const LONG_PTR CustomStyle = CurrentStyle
			| WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
		SetLastError(ERROR_SUCCESS);
		const LONG_PTR PreviousStyle = SetWindowLongPtrW(WindowHandle, GWL_STYLE, CustomStyle);
		if (PreviousStyle == 0 && GetLastError() != ERROR_SUCCESS) return false;
		if (!SetWindowPos(WindowHandle, nullptr, 0, 0, 0, 0,
			SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE))
		{
			SetWindowLongPtrW(WindowHandle, GWL_STYLE, CurrentStyle);
			SetWindowPos(WindowHandle, nullptr, 0, 0, 0, 0,
				SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			return false;
		}
		return true;
#else
		return false;
#endif
	}

#if defined(_WIN32)
	auto FGlfwWindow::ProcessWindowsMessage(uint32 Message, uint64 WParamValue, int64 LParamValue, bool& bHandled) -> int64
	{
		const HWND WindowHandle = static_cast<HWND>(OSNativeWindowHandle);
		const WPARAM WParam = static_cast<WPARAM>(WParamValue);
		const LPARAM LParam = static_cast<LPARAM>(LParamValue);
		const auto CallPrevious = [&]() -> LRESULT {
			return CallWindowProcW(reinterpret_cast<WNDPROC>(PreviousWindowsProcedure), WindowHandle, Message, WParam, LParam);
		};
		bHandled = false;
		if (EffectiveDecorationMode != EWindowDecorationMode::CustomTitleBar) return CallPrevious();

		switch (Message)
		{
		case WM_NCCALCSIZE:
			bHandled = true;
			if (WParam != 0 && IsZoomed(WindowHandle))
			{
				auto* Parameters = reinterpret_cast<NCCALCSIZE_PARAMS*>(LParam);
				MONITORINFO MonitorInfo{};
				const RECT MaximizedRect = GetWindowsMaximizedClientRect(WindowHandle, MonitorInfo);
				if (MonitorInfo.cbSize != 0) Parameters->rgrc[0] = MaximizedRect;
			}
			return 0;

		case WM_GETMINMAXINFO:
		{
			MONITORINFO MonitorInfo{};
			const RECT MaximizedRect = GetWindowsMaximizedClientRect(WindowHandle, MonitorInfo);
			if (MonitorInfo.cbSize != 0)
			{
				auto* MinMaxInfo = reinterpret_cast<MINMAXINFO*>(LParam);
				const UINT Dpi = GetDpiForWindow(WindowHandle);
				MinMaxInfo->ptMinTrackSize = {
					std::max(
						MulDiv(640, static_cast<int32>(Dpi), USER_DEFAULT_SCREEN_DPI),
						TitleBarLayout.MinimumWindowWidth),
					MulDiv(480, static_cast<int32>(Dpi), USER_DEFAULT_SCREEN_DPI)};
				MinMaxInfo->ptMaxPosition = {
					MaximizedRect.left - MonitorInfo.rcMonitor.left,
					MaximizedRect.top - MonitorInfo.rcMonitor.top};
				MinMaxInfo->ptMaxSize = {
					MaximizedRect.right - MaximizedRect.left,
					MaximizedRect.bottom - MaximizedRect.top};
				bHandled = true;
				return 0;
			}
			break;
		}

		case WM_NCHITTEST:
		{
			LRESULT DwmResult = 0;
			if (DwmDefWindowProc(WindowHandle, Message, WParam, LParam, &DwmResult))
			{
				bHandled = true;
				return DwmResult;
			}
			POINT ClientPoint{
				static_cast<int16>(LOWORD(static_cast<DWORD_PTR>(LParam))),
				static_cast<int16>(HIWORD(static_cast<DWORD_PTR>(LParam)))};
			ScreenToClient(WindowHandle, &ClientPoint);
			RECT ClientRect{};
			GetClientRect(WindowHandle, &ClientRect);
			const UINT Dpi = GetDpiForWindow(WindowHandle);
			const int32 ResizeBorderX = GetSystemMetricsForDpi(SM_CXSIZEFRAME, Dpi)
				+ GetSystemMetricsForDpi(SM_CXPADDEDBORDER, Dpi);
			const int32 ResizeBorderY = GetSystemMetricsForDpi(SM_CYSIZEFRAME, Dpi)
				+ GetSystemMetricsForDpi(SM_CXPADDEDBORDER, Dpi);
			const EWindowTitleBarHitTest HitTest = HitTestWindowTitleBar(
				TitleBarLayout,
				{ClientPoint.x, ClientPoint.y},
				{ClientRect.right, ClientRect.bottom},
				ResizeBorderX,
				ResizeBorderY,
				!IsZoomed(WindowHandle));
			bHandled = true;
			return WindowTitleBarHitTestToWindows(HitTest);
		}

		case WM_NCMOUSEMOVE:
			TitleBarInteractionState.HoveredPart = WindowsToWindowTitleBarHitTest(WParam);
			{
				TRACKMOUSEEVENT Tracking{.cbSize = sizeof(TRACKMOUSEEVENT), .dwFlags = TME_LEAVE | TME_NONCLIENT, .hwndTrack = WindowHandle};
				TrackMouseEvent(&Tracking);
			}
			break;

		case WM_NCMOUSELEAVE:
			TitleBarInteractionState.HoveredPart = EWindowTitleBarHitTest::Client;
			TitleBarInteractionState.PressedPart = EWindowTitleBarHitTest::Client;
			break;

		case WM_NCLBUTTONDOWN:
			TitleBarInteractionState.PressedPart = WindowsToWindowTitleBarHitTest(WParam);
			if (IsWindowTitleBarButton(TitleBarInteractionState.PressedPart))
			{
				// Standard caption processing is suppressed by the custom non-client
				// path, so keep native hit-test semantics for Snap Layout but own the
				// click-to-system-command transition here.
				bHandled = true;
				return 0;
			}
			break;

		case WM_NCLBUTTONUP:
		{
			const EWindowTitleBarHitTest ReleasedPart = WindowsToWindowTitleBarHitTest(WParam);
			const EWindowTitleBarHitTest PressedPart = TitleBarInteractionState.PressedPart;
			TitleBarInteractionState.PressedPart = EWindowTitleBarHitTest::Client;
			if (IsWindowTitleBarButton(PressedPart))
			{
				if (PressedPart == ReleasedPart)
				{
					const WPARAM SystemCommand = WindowTitleBarButtonSystemCommand(
						PressedPart, IsZoomed(WindowHandle) != FALSE);
					SendMessageW(WindowHandle, WM_SYSCOMMAND, SystemCommand, LParam);
				}
				bHandled = true;
				return 0;
			}
			break;
		}

		case WM_NCACTIVATE:
			TitleBarInteractionState.bFocused = WParam != 0;
			bHandled = true;
			return 1;

		case WM_NCPAINT:
			bHandled = true;
			return 0;

		default:
			break;
		}
		return CallPrevious();
	}
#endif

	auto FGlfwWindow::SetCursor(EMouseCursor Cursor) -> void
	{
		if (Cursor == EMouseCursor::None) return;
		CursorShape = Cursor;
		const int CursorIndex = static_cast<int>(Cursor);
		glfwSetCursor(GlfwWindow, GGlfwCursors[CursorIndex]);
	}

	auto FGlfwWindow::ApplyCursorMode(ECursorMode InCursorMode) -> void
	{
		if (GlfwWindow == nullptr) return;

		if (CursorMode == ECursorMode::Captured && glfwRawMouseMotionSupported() == GLFW_TRUE)
		{
			glfwSetInputMode(GlfwWindow, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
		}

		if (InCursorMode == ECursorMode::Captured)
		{
			glfwSetInputMode(GlfwWindow, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
			if (glfwRawMouseMotionSupported() == GLFW_TRUE)
			{
				glfwSetInputMode(GlfwWindow, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
			}
		}
		else if (InCursorMode == ECursorMode::Hidden)
		{
			glfwSetInputMode(GlfwWindow, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
		}
		else
		{
			glfwSetInputMode(GlfwWindow, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
			glfwSetCursor(GlfwWindow, GGlfwCursors[static_cast<int>(CursorShape)]);
		}
	}



} // namespace Durin
