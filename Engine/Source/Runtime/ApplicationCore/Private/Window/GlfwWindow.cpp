#include "Window/GlfwWindow.h"

#include "ThirdParty/Glfw/GlfwCommon.h"
#include "Application/GenericApplication.h"
#include "Application/GenericApplicationMessageHandler.h"
#include "ApplicationCore.h"

namespace Durin
{
	namespace
	{
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
					check(Action == EKeyAction::Release)
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
			MonitorInfo.DpiScale = ScaleX;
		}

		return Monitors;
	}

	FGlfwWindow::FGlfwWindow() = default;

	FGlfwWindow::~FGlfwWindow()
	{
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
		glfwWindowHint(GLFW_DECORATED, Definition->bHasOSWindowBorder ? GLFW_TRUE : GLFW_FALSE);
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
#elif defined(__APPLE__)
		OSNativeWindowHandle = glfwGetCocoaWindow(GlfwWindow);
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

	void FGlfwWindow::PollEvents() const
	{
		glfwPollEvents();
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
		glfwShowWindow(GlfwWindow);
	}

	auto FGlfwWindow::Hide() -> void
	{
		glfwHideWindow(GlfwWindow);
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

	auto FGlfwWindow::SetWindowDecorated(bool bDecorated) -> void
	{
		glfwSetWindowAttrib(GlfwWindow, GLFW_DECORATED, bDecorated ? GLFW_TRUE : GLFW_FALSE);
		if (Definition != nullptr)
		{
			Definition->bHasOSWindowBorder = bDecorated;
		}
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
		return ScaleX;
	}

	auto FGlfwWindow::SetCursor(EMouseCursor Cursor) -> void
	{
		const int CursorIndex = static_cast<int>(Cursor);
		if (Cursor == EMouseCursor::None)
		{
			glfwSetInputMode(GlfwWindow, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
			return;
		}

		glfwSetInputMode(GlfwWindow, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
		glfwSetCursor(GlfwWindow, GGlfwCursors[CursorIndex]);
	}



} // namespace Durin
