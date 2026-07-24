#include "ImGuiMonaImpl.h"
#include "MonaImGui.h"

#include "Application/MonaApplication.h"
#include "Application/MonaEventHandler.h"
#include "Misc/Paths.h"
#include "Widgets/MWindow.h"

namespace Durin::MonaImGui
{
	ImGuiContext* GMonaImGuiContext = nullptr;

	// Translates Mona application events into the active ImGui context.
	class FMonaImGuiEventHandler : public Mona::FMonaEventHandler
	{
	public:
		FMonaImGuiEventHandler() = default;
		~FMonaImGuiEventHandler() override = default;

		auto OnWindowCloseRequested(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> bool override;
		auto OnWindowFocused(const std::shared_ptr<FGenericWindow>& InPlatformWindow, bool bFocused) -> void override;
		auto OnWindowResized(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InWidth, int32 InHeight, bool bInWasMinimized) -> bool override;
		auto OnWindowViewportResized(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InWidth, int32 InHeight, bool bInWasMinimized) -> bool override;
		auto OnWindowMoved(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InX, int32 InY) -> bool override;
		auto OnKeyDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods, bool IsRepeat) -> bool override;
		auto OnKeyUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods) -> bool override;
		auto OnKeyChar(const std::shared_ptr<FGenericWindow>& InPlatformWindow, uint32 Codepoint) -> bool override;
		auto OnMouseMove(const std::shared_ptr<FGenericWindow>& InPlatformWindow, FVector2d CursorPos) -> bool override;
		auto OnMouseEnter(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> void override;
		auto OnMouseLeave(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> void override;
		auto OnMouseDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool override;
		auto OnMouseUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool override;
		auto OnMouseWheel(const std::shared_ptr<FGenericWindow>& InPlatformWindow, double DeltaX, double DeltaY) -> bool override;
	};

	using FMonaBackendEventHandler = FMonaImGuiEventHandler;

	// ---- Platform Viewport Data -----------------------------------------

	// Owns the Mona window and native callback state attached to an ImGui viewport.
	struct ImGuiMonaImpl_ViewportData
	{
		std::shared_ptr<MWindow> Window;
		int32 IgnoreWindowPosEventFrame = -1;
		int32 IgnoreWindowSizeEventFrame = -1;
#if defined(_WIN32)
		WNDPROC PrevWndProc = nullptr;
#endif
	};

	// Retains cross-frame mouse state needed by ImGui platform viewport events.
	struct FMonaImGuiMouseState
	{
		std::weak_ptr<FGenericWindow> MouseWindow;
		ImVec2 LastValidMousePos = ImVec2(-FLT_MAX, -FLT_MAX);
	};

	static FMonaImGuiMouseState GMonaImGuiMouseState;

#if defined(_WIN32)
	static constexpr const char* MonaImGuiViewportProp = "DURIN_MONA_IMGUI_VIEWPORT";
	static constexpr const char* MonaImGuiPrevWndProcProp = "DURIN_MONA_IMGUI_PREV_WNDPROC";
#endif

	static auto GetPlatformViewportData(ImGuiViewport* Viewport) -> ImGuiMonaImpl_ViewportData*
	{
		return Viewport != nullptr ? static_cast<ImGuiMonaImpl_ViewportData*>(Viewport->PlatformUserData) : nullptr;
	}

	static auto CreatePlatformViewportData(ImGuiViewport* Viewport, const std::shared_ptr<MWindow>& Window) -> ImGuiMonaImpl_ViewportData*
	{
		auto* ViewportData = GetPlatformViewportData(Viewport);
		if (ViewportData == nullptr)
		{
			ViewportData = new ImGuiMonaImpl_ViewportData();
			Viewport->PlatformUserData = ViewportData;
		}

		ViewportData->Window = Window;
		return ViewportData;
	}

	static auto DestroyPlatformViewportData(ImGuiViewport* Viewport) -> void
	{
		check(Viewport);
#if defined(_WIN32)
		if (auto* ViewportData = GetPlatformViewportData(Viewport))
		{
			if (ViewportData->PrevWndProc != nullptr && Viewport->PlatformHandleRaw != nullptr)
			{
				HWND WindowHandle = static_cast<HWND>(Viewport->PlatformHandleRaw);
				::SetWindowLongPtrW(WindowHandle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ViewportData->PrevWndProc));
				::RemovePropA(WindowHandle, MonaImGuiViewportProp);
				::RemovePropA(WindowHandle, MonaImGuiPrevWndProcProp);
				ViewportData->PrevWndProc = nullptr;
			}
		}
#endif
		delete GetPlatformViewportData(Viewport);
		Viewport->PlatformUserData = nullptr;
		Viewport->PlatformHandle = nullptr;
		Viewport->PlatformHandleRaw = nullptr;
	}

	static auto GetViewportWindow(ImGuiViewport* Viewport) -> std::shared_ptr<MWindow>
	{
		const auto* ViewportData = GetPlatformViewportData(Viewport);
		return ViewportData != nullptr ? ViewportData->Window : nullptr;
	}

	static auto ShouldIgnoreWindowPosEvent(ImGuiViewport* Viewport) -> bool
	{
		const auto* ViewportData = GetPlatformViewportData(Viewport);
		return ViewportData != nullptr && ImGui::GetFrameCount() <= ViewportData->IgnoreWindowPosEventFrame + 1;
	}

	static auto ShouldIgnoreWindowSizeEvent(ImGuiViewport* Viewport) -> bool
	{
		const auto* ViewportData = GetPlatformViewportData(Viewport);
		return ViewportData != nullptr && ImGui::GetFrameCount() <= ViewportData->IgnoreWindowSizeEventFrame + 1;
	}

	static auto GetMainMonaWindow() -> std::shared_ptr<MWindow>
	{
		if (auto* ViewportData = GetPlatformViewportData(ImGui::GetMainViewport()))
		{
			return ViewportData->Window;
		}

		auto& App = Mona::FMonaApplication::Get();
		const auto& Windows = App.GetWindows();
		if (Windows.empty())
		{
			return nullptr;
		}

		return Windows.front();
	}

	// ---- Mouse Cursor Mapping ------------------------------------------

	static auto FromImGuiMouseCursor(ImGuiMouseCursor Cursor) -> EMouseCursor
	{
		switch (Cursor)
		{
		case ImGuiMouseCursor_None: return EMouseCursor::None;
		case ImGuiMouseCursor_Arrow: return EMouseCursor::Arrow;
		case ImGuiMouseCursor_TextInput: return EMouseCursor::TextInput;
		case ImGuiMouseCursor_ResizeAll: return EMouseCursor::ResizeAll;
		case ImGuiMouseCursor_ResizeNS: return EMouseCursor::ResizeNS;
		case ImGuiMouseCursor_ResizeEW: return EMouseCursor::ResizeEW;
		case ImGuiMouseCursor_ResizeNESW: return EMouseCursor::ResizeNESW;
		case ImGuiMouseCursor_ResizeNWSE: return EMouseCursor::ResizeNWSE;
		case ImGuiMouseCursor_Hand: return EMouseCursor::Hand;
		case ImGuiMouseCursor_NotAllowed: return EMouseCursor::NotAllowed;
		case ImGuiMouseCursor_Wait:
		case ImGuiMouseCursor_Progress:
		default:
			return EMouseCursor::Arrow;
		}
	}

	static auto GetImGuiPlatformHandle(const std::shared_ptr<FGenericWindow>& NativeWindow) -> void*
	{
		return NativeWindow != nullptr ? NativeWindow.get() : nullptr;
	}

	// ---- Viewport / Window Helpers -------------------------------------

	static auto UpdateViewportFromWindow(ImGuiViewport* Viewport, const std::shared_ptr<MWindow>& Window) -> void
	{
		if (!Window)
		{
			return;
		}

		const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow();
		if (!NativeWindow)
		{
			return;
		}

		const FIntPoint WindowPositionInt = NativeWindow->GetWindowPosition();
		const FVector2f ViewportSize = Window->GetViewportSize();
		const FVector2f WindowPosition(static_cast<float>(WindowPositionInt.x), static_cast<float>(WindowPositionInt.y));
		const FVector2f WindowSize = Window->GetWindowSize();
		Window->SetCachedScreenPosition(WindowPosition);
		Window->SetCachedViewportSize(ViewportSize);

		Viewport->PlatformHandle = GetImGuiPlatformHandle(NativeWindow);
		Viewport->PlatformHandleRaw = NativeWindow->GetOSNativeWindowHandle();
		Viewport->Pos = ImVec2(WindowPosition.x, WindowPosition.y);
		Viewport->Size = ImVec2(WindowSize.x, WindowSize.y);
		Viewport->WorkPos = Viewport->Pos;
		Viewport->WorkSize = Viewport->Size;
		Viewport->DpiScale = NativeWindow->GetDpiScale();

		if (Viewport == ImGui::GetMainViewport())
		{
			ImGuiIO& IO = ImGui::GetIO();
			IO.DisplaySize = ImVec2(WindowSize.x, WindowSize.y);
			IO.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
			if (WindowSize.x > 0.0f && WindowSize.y > 0.0f && ViewportSize.x > 0.0f && ViewportSize.y > 0.0f)
			{
				IO.DisplayFramebufferScale = ImVec2(ViewportSize.x / WindowSize.x, ViewportSize.y / WindowSize.y);
			}
		}
	}

	static auto SyncAllViewportsFrameStates() -> void
	{
		ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
		for (ImGuiViewport* Viewport : PlatformIO.Viewports)
		{
			if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
			{
				UpdateViewportFromWindow(Viewport, Window);
			}
		}
	}

#if defined(_WIN32)
	static LRESULT CALLBACK ImGuiMonaImpl_WndProc(HWND WindowHandle, UINT Message, WPARAM WParam, LPARAM LParam)
	{
		ImGuiViewport* Viewport = reinterpret_cast<ImGuiViewport*>(::GetPropA(WindowHandle, MonaImGuiViewportProp));
		WNDPROC PrevWndProc = reinterpret_cast<WNDPROC>(::GetPropA(WindowHandle, MonaImGuiPrevWndProcProp));

		if (Message == WM_NCHITTEST && Viewport != nullptr && (Viewport->Flags & ImGuiViewportFlags_NoInputs) != 0)
		{
			return HTTRANSPARENT;
		}

		if (PrevWndProc != nullptr)
		{
			return ::CallWindowProcW(PrevWndProc, WindowHandle, Message, WParam, LParam);
		}
		return ::DefWindowProcW(WindowHandle, Message, WParam, LParam);
	}

	static auto HookViewportWindow(ImGuiViewport* Viewport) -> void
	{
		if (Viewport == nullptr || Viewport->PlatformHandleRaw == nullptr)
		{
			return;
		}

		auto* ViewportData = GetPlatformViewportData(Viewport);
		if (ViewportData == nullptr || ViewportData->PrevWndProc != nullptr)
		{
			return;
		}

		HWND WindowHandle = static_cast<HWND>(Viewport->PlatformHandleRaw);
		ViewportData->PrevWndProc = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(WindowHandle, GWLP_WNDPROC));
		check(ViewportData->PrevWndProc != nullptr);
		::SetPropA(WindowHandle, MonaImGuiViewportProp, Viewport);
		::SetPropA(WindowHandle, MonaImGuiPrevWndProcProp, reinterpret_cast<HANDLE>(ViewportData->PrevWndProc));
		::SetWindowLongPtrW(WindowHandle, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ImGuiMonaImpl_WndProc));
	}
#endif

	static auto BindViewportToWindow(ImGuiViewport* Viewport, const std::shared_ptr<MWindow>& Window) -> void
	{
		if (!Viewport || !Window)
		{
			return;
		}

		CreatePlatformViewportData(Viewport, Window);
		UpdateViewportFromWindow(Viewport, Window);
#if defined(_WIN32)
		HookViewportWindow(Viewport);
#endif
	}

	static auto BindMainViewportToWindowInternal(const std::shared_ptr<MWindow>& MainWindow) -> void
	{
		if (!MainWindow)
		{
			return;
		}

		ImGuiViewport* MainViewport = ImGui::GetMainViewport();
		if (MainViewport == nullptr)
		{
			return;
		}

		BindViewportToWindow(MainViewport, MainWindow);
		MainViewport->Flags |= ImGuiViewportFlags_OwnedByApp;
	}

	// ---- Monitors ------------------------------------------------------

	static auto UpdateMonitors() -> void
	{
		ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
		PlatformIO.Monitors.resize(0);

		for (const FMonitorInfo& MonitorInfo : EnumerateMonitors())
		{
			PlatformIO.Monitors.push_back(ImGuiPlatformMonitor());
			ImGuiPlatformMonitor& Monitor = PlatformIO.Monitors.back();
			Monitor.MainPos = ImVec2(static_cast<float>(MonitorInfo.MainPosition.x), static_cast<float>(MonitorInfo.MainPosition.y));
			Monitor.MainSize = ImVec2(static_cast<float>(MonitorInfo.MainSize.x), static_cast<float>(MonitorInfo.MainSize.y));
			Monitor.WorkPos = ImVec2(static_cast<float>(MonitorInfo.WorkPosition.x), static_cast<float>(MonitorInfo.WorkPosition.y));
			Monitor.WorkSize = ImVec2(static_cast<float>(MonitorInfo.WorkSize.x), static_cast<float>(MonitorInfo.WorkSize.y));
			Monitor.DpiScale = MonitorInfo.DpiScale;
			Monitor.PlatformHandle = MonitorInfo.NativeHandle;
		}
	}

	// ---- Mouse Cursor --------------------------------------------------

	static auto UpdateMouseCursor() -> void
	{
		const ImGuiIO& IO = ImGui::GetIO();
		if ((IO.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) != 0)
		{
			return;
		}

		const EMouseCursor DesiredCursor = FromImGuiMouseCursor(ImGui::GetMouseCursor());

		ImGuiViewport* HoveredViewport = nullptr;
		if (IO.MouseHoveredViewport != 0)
		{
			HoveredViewport = ImGui::FindViewportByID(IO.MouseHoveredViewport);
		}

		if (HoveredViewport)
		{
			FGenericWindow* NativeWindow = static_cast<FGenericWindow*>(HoveredViewport->PlatformHandle);
			if (NativeWindow)
			{
				if (DesiredCursor == EMouseCursor::None || IO.MouseDrawCursor)
				{
					NativeWindow->SetCursor(EMouseCursor::None);
				}
				else
				{
					NativeWindow->SetCursor(DesiredCursor);
				}
			}
		}
	}

	static auto UpdateMouseData() -> void
	{
		ImGuiIO& IO = ImGui::GetIO();
		ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();

		ImGuiViewport* HoveredViewport = nullptr;
		ImGuiViewport* FocusedViewport = nullptr;
		for (ImGuiViewport* Viewport : PlatformIO.Viewports)
		{
			const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport);
			if (Window == nullptr)
			{
				continue;
			}

			const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow();
			if (NativeWindow == nullptr || NativeWindow->IsMinimized())
			{
				continue;
			}

			if (NativeWindow->IsHovered() && (Viewport->Flags & ImGuiViewportFlags_NoInputs) == 0)
			{
				HoveredViewport = Viewport;
			}

			if (FocusedViewport == nullptr && NativeWindow->IsFocused())
			{
				FocusedViewport = Viewport;
			}
		}

		if (GMonaImGuiMouseState.MouseWindow.expired() && FocusedViewport != nullptr)
		{
			const std::shared_ptr<MWindow> Window = GetViewportWindow(FocusedViewport);
			const std::shared_ptr<FGenericWindow> NativeWindow = Window != nullptr ? Window->GetNativeWindow() : nullptr;
			if (NativeWindow != nullptr)
			{
				FVector2d CursorPos = NativeWindow->GetCursorPosition();
				if ((IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
				{
					const FIntPoint WindowPosition = NativeWindow->GetWindowPosition();
					CursorPos.x += WindowPosition.x;
					CursorPos.y += WindowPosition.y;
				}
				GMonaImGuiMouseState.LastValidMousePos = ImVec2(static_cast<float>(CursorPos.x), static_cast<float>(CursorPos.y));
				IO.AddMousePosEvent(static_cast<float>(CursorPos.x), static_cast<float>(CursorPos.y));
			}
		}

		if ((IO.BackendFlags & ImGuiBackendFlags_HasMouseHoveredViewport) != 0)
		{
			IO.AddMouseViewportEvent(HoveredViewport != nullptr ? HoveredViewport->ID : 0);
		}
	}

	// ---- Window Style --------------------------------------------------

	static auto ApplyViewportWindowStyle(ImGuiViewport* Viewport, const std::shared_ptr<MWindow>& Window) -> void
	{
		if (Window == nullptr)
		{
			return;
		}

		const bool bDecorated = (Viewport->Flags & ImGuiViewportFlags_NoDecoration) == 0;
		Window->SetWindowDecorated(bDecorated);
		if (!bDecorated && Window->GetTitle() != "")
		{
			Window->SetTitle("");
		}

		if (const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
		{
			NativeWindow->SetMousePassthrough((Viewport->Flags & ImGuiViewportFlags_NoInputs) != 0);
			NativeWindow->SetFocusOnShow((Viewport->Flags & ImGuiViewportFlags_NoFocusOnAppearing) == 0);
		}
	}

	// ---- Platform Callbacks (ImGuiPlatformIO) --------------------------

	static auto ImGuiMonaImpl_CreateWindow(ImGuiViewport* Viewport) -> void
	{
		auto Window = std::make_shared<MWindow>();
		Window->SetTitleBarDarkMode(GetColorTheme() == EColorTheme::Dark);
		Window->SetTitle("ImGui");
		Window->SetViewportPresentModePolicy(EViewportPresentModePolicy::ImGuiDetachedViewport);
		Window->ReshapeWindow({Viewport->Pos.x, Viewport->Pos.y}, {Viewport->Size.x, Viewport->Size.y});

		std::shared_ptr<MWindow> ParentWindow = nullptr;
		if (Viewport->ParentViewport != nullptr)
		{
			ParentWindow = GetViewportWindow(Viewport->ParentViewport);
		}
		else
		{
			ParentWindow = GetMainMonaWindow();
		}

		if (ParentWindow != nullptr && ParentWindow != Window)
		{
			ParentWindow->AddChildWindow(Window);
		}

		ApplyViewportWindowStyle(Viewport, Window);

		auto& App = Mona::FMonaApplication::Get();
		App.AddWindow(Window, false);
		Window->HideWindow();

		BindViewportToWindow(Viewport, Window);
	}

	static auto ImGuiMonaImpl_DestroyWindow(ImGuiViewport* Viewport) -> void
	{
		auto* ViewportData = GetPlatformViewportData(Viewport);
		if (ViewportData != nullptr && ViewportData->Window != nullptr && (Viewport->Flags & ImGuiViewportFlags_OwnedByApp) == 0)
		{
			ViewportData->Window->RequestDestroyWindow();
		}

		DestroyPlatformViewportData(Viewport);
	}

	static auto ImGuiMonaImpl_ShowWindow(ImGuiViewport* Viewport) -> void
	{
		if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
		{
			ApplyViewportWindowStyle(Viewport, Window);
			Window->ShowWindow();
		}
	}

	static auto ImGuiMonaImpl_SetWindowPos(ImGuiViewport* Viewport, ImVec2 Position) -> void
	{
		if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
		{
			if (auto* ViewportData = GetPlatformViewportData(Viewport))
			{
				ViewportData->IgnoreWindowPosEventFrame = ImGui::GetFrameCount();
			}
			Window->MoveWindowTo({Position.x, Position.y});
		}
	}

	static auto ImGuiMonaImpl_GetWindowPos(ImGuiViewport* Viewport) -> ImVec2
	{
		if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
		{
			if (const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
			{
				const FIntPoint PositionInt = NativeWindow->GetWindowPosition();
				const FVector2f Position(static_cast<float>(PositionInt.x), static_cast<float>(PositionInt.y));
				Window->SetCachedScreenPosition(Position);
				return {Position.x, Position.y};
			}
		}
		return {};
	}

	static auto ImGuiMonaImpl_SetWindowSize(ImGuiViewport* Viewport, ImVec2 Size) -> void
	{
		if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
		{
			if (auto* ViewportData = GetPlatformViewportData(Viewport))
			{
				ViewportData->IgnoreWindowSizeEventFrame = ImGui::GetFrameCount();
			}
			Window->ResizeWindow({Size.x, Size.y});
		}
	}

	static auto ImGuiMonaImpl_GetWindowSize(ImGuiViewport* Viewport) -> ImVec2
	{
		if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
		{
			if (const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
			{
				const FIntPoint SizeInt = NativeWindow->GetWindowSize();
				const FVector2f Size(static_cast<float>(SizeInt.x), static_cast<float>(SizeInt.y));
				Window->SetCachedSize(Size);
				return {Size.x, Size.y};
			}
		}
		return {};
	}

	static auto ImGuiMonaImpl_SetWindowFocus(ImGuiViewport* Viewport) -> void
	{
		if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
		{
			if (const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
			{
				NativeWindow->Focus();
			}
		}
	}

	static auto ImGuiMonaImpl_GetWindowFocus(ImGuiViewport* Viewport) -> bool
	{
		if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
		{
			if (const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
			{
				return NativeWindow->IsFocused();
			}
		}
		return false;
	}

	static auto ImGuiMonaImpl_GetWindowMinimized(ImGuiViewport* Viewport) -> bool
	{
		if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
		{
			return Window->IsMinimized();
		}
		return false;
	}

	static auto ImGuiMonaImpl_SetWindowTitle(ImGuiViewport* Viewport, const char* Title) -> void
	{
		if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
		{
			const bool bDecorated = (Viewport->Flags & ImGuiViewportFlags_NoDecoration) == 0;
			Window->SetTitle(bDecorated && Title != nullptr ? Title : "");
		}
	}

	static auto ImGuiMonaImpl_SetWindowAlpha(ImGuiViewport* Viewport, float Alpha) -> void
	{
		if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
		{
			if (const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
			{
				NativeWindow->SetOpacity(Alpha);
			}
		}
	}

	static auto ImGuiMonaImpl_GetWindowDpiScale(ImGuiViewport* Viewport) -> float
	{
		if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
		{
			if (const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
			{
				return NativeWindow->GetDpiScale();
			}
		}
		return 1.0f;
	}

	static auto ImGuiMonaImpl_GetWindowFramebufferScale(ImGuiViewport* Viewport) -> ImVec2
	{
		if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
		{
			if (const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
			{
				const FIntPoint WindowSize = NativeWindow->GetWindowSize();
				const FIntPoint ViewportSize = NativeWindow->GetViewportSize();
				Window->SetCachedViewportSize(FVector2f(static_cast<float>(ViewportSize.x), static_cast<float>(ViewportSize.y)));
				if (WindowSize.x > 0 && WindowSize.y > 0 && ViewportSize.x > 0 && ViewportSize.y > 0)
				{
					return {
						static_cast<float>(ViewportSize.x) / static_cast<float>(WindowSize.x),
						static_cast<float>(ViewportSize.y) / static_cast<float>(WindowSize.y)
					};
				}
			}
		}
		return {1.0f, 1.0f};
	}

	static auto ImGuiMonaImpl_UpdateWindow(ImGuiViewport* Viewport) -> void
	{
		if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
		{
			ApplyViewportWindowStyle(Viewport, Window);

			if (const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
			{
				if (NativeWindow->ShouldClose() && (Viewport->Flags & ImGuiViewportFlags_OwnedByApp) == 0)
				{
					Viewport->PlatformRequestClose = true;
					NativeWindow->SetShouldClose(false);
				}

				const FIntPoint WindowPositionInt = NativeWindow->GetWindowPosition();
				const FIntPoint WindowSizeInt = NativeWindow->GetWindowSize();
				const ImVec2 PlatformPos(static_cast<float>(WindowPositionInt.x), static_cast<float>(WindowPositionInt.y));
				const ImVec2 PlatformSize(static_cast<float>(WindowSizeInt.x), static_cast<float>(WindowSizeInt.y));

				if ((PlatformPos.x != Viewport->Pos.x || PlatformPos.y != Viewport->Pos.y) && !ShouldIgnoreWindowPosEvent(Viewport))
				{
					Viewport->PlatformRequestMove = true;
				}

				if ((PlatformSize.x != Viewport->Size.x || PlatformSize.y != Viewport->Size.y) && !ShouldIgnoreWindowSizeEvent(Viewport))
				{
					Viewport->PlatformRequestResize = true;
				}
			}
		}
	}

	// ---- ImGui Configuration & Fonts -----------------------------------

	static auto ConfigureDefaultImGuiBehavior() -> void
	{
		ImGuiIO& IO = ImGui::GetIO();
		IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		IO.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
		IO.ConfigDockingTransparentPayload = true;
		IO.ConfigViewportsNoAutoMerge = false;
		IO.ConfigViewportsNoDefaultParent = false;
		IO.ConfigViewportsNoTaskBarIcon = true;
		IO.ConfigViewportsNoDecoration = true;

		ImGuiStyle& Style = ImGui::GetStyle();
		if ((IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
		{
			Style.WindowRounding = 0.0f;
			Style.Colors[ImGuiCol_WindowBg].w = 1.0f;
		}
	}

	static auto InitFonts() -> void
	{
		ImGuiIO& IO = ImGui::GetIO();
		IO.Fonts->AddFontDefaultVector();

		const ImWchar* ChineseGlyphRanges = IO.Fonts->GetGlyphRangesChineseSimplifiedCommon();
		std::string FontDir = FPaths::EngineDir() + "Content/ImGuiFonts/";
		std::string FontPath_DroidSans = FontDir + "DroidSans.ttf";
		std::string FontPath_NotoSansSC = FontDir + "NotoSansSC-Regular.ttf";
		std::string FontPath_FontAwesome = FontDir + "FontAwesome/FontAwesome7Free-Solid-900.otf";

		ImFont* FallbackLatinFont = IO.Fonts->AddFontFromFileTTF(FontPath_DroidSans.c_str(), 20.0f);
		ImFont* ChineseFont = IO.Fonts->AddFontFromFileTTF(FontPath_NotoSansSC.c_str(), 20.0f, nullptr, ChineseGlyphRanges);
		ImFontConfig IconFontConfig;
		IconFontConfig.MergeMode = true;
		IconFontConfig.PixelSnapH = true;
		IconFontConfig.GlyphMinAdvanceX = 16.0f;
		static constexpr ImWchar IconGlyphRanges[]{0xe000, 0xf8ff, 0};
		IO.Fonts->AddFontFromFileTTF(FontPath_FontAwesome.c_str(), 16.0f, &IconFontConfig, IconGlyphRanges);
		if (ChineseFont)
		{
			IO.FontDefault = ChineseFont;
		}
		else if (FallbackLatinFont)
		{
			IO.FontDefault = FallbackLatinFont;
		}
	}

	// ---- Event Handler: Key Translation --------------------------------

	// clang-format off
	static auto ConvertKeyToImGuiType(EKey Key) -> ImGuiKey
	{
		switch (Key)
		{
		// Control keys
		case EKey::Escape:		return ImGuiKey_Escape;
		case EKey::CapsLock:	return ImGuiKey_CapsLock;
		case EKey::LShift:		return ImGuiKey_LeftShift;
		case EKey::RShift:		return ImGuiKey_RightShift;
		case EKey::LAlt:		return ImGuiKey_LeftAlt;
		case EKey::RAlt:		return ImGuiKey_RightAlt;
		case EKey::LControl:	return ImGuiKey_LeftCtrl;
		case EKey::RControl:	return ImGuiKey_RightCtrl;

		// Whitespace keys
		case EKey::Tab:			return ImGuiKey_Tab;
		case EKey::Space:		return ImGuiKey_Space;
		case EKey::Enter:		return ImGuiKey_Enter;
		case EKey::Backspace:	return ImGuiKey_Backspace;

		// Arrow keys
		case EKey::Left:		return ImGuiKey_LeftArrow;
		case EKey::Right:		return ImGuiKey_RightArrow;
		case EKey::Up:			return ImGuiKey_UpArrow;
		case EKey::Down:		return ImGuiKey_DownArrow;

		// Navigation keys
		case EKey::PageUp:		return ImGuiKey_PageUp;
		case EKey::PageDown:	return ImGuiKey_PageDown;
		case EKey::Home:		return ImGuiKey_Home;
		case EKey::End:			return ImGuiKey_End;
		case EKey::Insert:		return ImGuiKey_Insert;
		case EKey::Delete:		return ImGuiKey_Delete;

		// Function keys
		case EKey::F1:			return ImGuiKey_F1;
		case EKey::F2:			return ImGuiKey_F2;
		case EKey::F3:			return ImGuiKey_F3;
		case EKey::F4:			return ImGuiKey_F4;
		case EKey::F5:			return ImGuiKey_F5;
		case EKey::F6:			return ImGuiKey_F6;
		case EKey::F7:			return ImGuiKey_F7;
		case EKey::F8:			return ImGuiKey_F8;
		case EKey::F9:			return ImGuiKey_F9;
		case EKey::F10:			return ImGuiKey_F10;
		case EKey::F11:			return ImGuiKey_F11;
		case EKey::F12:			return ImGuiKey_F12;

		// Letters A-Z (both EKey and ImGuiKey are sequential)
		case EKey::A: case EKey::B: case EKey::C: case EKey::D: case EKey::E:
		case EKey::F: case EKey::G: case EKey::H: case EKey::I: case EKey::J:
		case EKey::K: case EKey::L: case EKey::M: case EKey::N: case EKey::O:
		case EKey::P: case EKey::Q: case EKey::R: case EKey::S: case EKey::T:
		case EKey::U: case EKey::V: case EKey::W: case EKey::X: case EKey::Y:
		case EKey::Z:
			return static_cast<ImGuiKey>(ImGuiKey_A + (static_cast<int>(Key) - static_cast<int>(EKey::A)));

		// Punctuation
		case EKey::Comma:			return ImGuiKey_Comma;
		case EKey::Period:			return ImGuiKey_Period;
		case EKey::Apostrophe:		return ImGuiKey_Apostrophe;
		case EKey::Semicolon:		return ImGuiKey_Semicolon;
		case EKey::Slash:			return ImGuiKey_Slash;
		case EKey::Backslash:		return ImGuiKey_Backslash;
		case EKey::LeftBracket:		return ImGuiKey_LeftBracket;
		case EKey::RightBracket:	return ImGuiKey_RightBracket;
		case EKey::GraveAccent:		return ImGuiKey_GraveAccent;

		// Main keyboard numbers and symbols
		case EKey::Num0: case EKey::Num1: case EKey::Num2: case EKey::Num3: case EKey::Num4:
		case EKey::Num5: case EKey::Num6: case EKey::Num7: case EKey::Num8: case EKey::Num9:
			return static_cast<ImGuiKey>(ImGuiKey_0 + (static_cast<int>(Key) - static_cast<int>(EKey::Num0)));
		case EKey::Minus:			return ImGuiKey_Minus;
		case EKey::Equal:			return ImGuiKey_Equal;

		// Keypad
		case EKey::Keypad0: case EKey::Keypad1: case EKey::Keypad2: case EKey::Keypad3: case EKey::Keypad4:
		case EKey::Keypad5: case EKey::Keypad6: case EKey::Keypad7: case EKey::Keypad8: case EKey::Keypad9:
			return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (static_cast<int>(Key) - static_cast<int>(EKey::Keypad0)));
		case EKey::KeypadDecimal:	return ImGuiKey_KeypadDecimal;
		case EKey::KeypadDivide:	return ImGuiKey_KeypadDivide;
		case EKey::KeypadMultiply:	return ImGuiKey_KeypadMultiply;
		case EKey::KeypadPlus:		return ImGuiKey_KeypadAdd;
		case EKey::KeypadMinus:		return ImGuiKey_KeypadSubtract;
		case EKey::KeypadEquals:	return ImGuiKey_KeypadEqual;

		default: return ImGuiKey_None;
		}
	}
	// clang-format on

	static auto ConvertMouseButtonToImGuiType(EMouseButton Button) -> int32
	{
		switch (Button)
		{
		case EMouseButton::Left: return ImGuiMouseButton_Left;
		case EMouseButton::Right: return ImGuiMouseButton_Right;
		case EMouseButton::Middle: return ImGuiMouseButton_Middle;
		default:
			return -1;
		}
	}

	static auto UpdateKeyModifiers(ImGuiIO& IO, EKeyModFlags Mods) -> void
	{
		IO.AddKeyEvent(ImGuiMod_Shift, EnumHasAnyFlags(Mods, EKeyModFlags::Shift));
		IO.AddKeyEvent(ImGuiMod_Alt, EnumHasAnyFlags(Mods, EKeyModFlags::Alt));
		IO.AddKeyEvent(ImGuiMod_Ctrl, EnumHasAnyFlags(Mods, EKeyModFlags::Control));
		IO.AddKeyEvent(ImGuiMod_Super, EnumHasAnyFlags(Mods, EKeyModFlags::Super));
	}

	static auto GetImGuiIO(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> ImGuiIO&
	{
		return ImGui::GetIO(GMonaImGuiContext);
	}

	static auto ConvertMousePositionToImGuiSpace(const std::shared_ptr<FGenericWindow>& InPlatformWindow, FVector2d CursorPos) -> FVector2d
	{
		ImGuiIO& IO = GetImGuiIO(InPlatformWindow);
		if ((IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0)
		{
			return CursorPos;
		}

		if (const std::shared_ptr<MWindow> Window = Mona::FMonaApplication::Get().FindWindowByPlatformWindow(InPlatformWindow))
		{
			FVector2f WindowPosition = Window->GetScreenPosition();
			return {CursorPos.x + WindowPosition.x, CursorPos.y + WindowPosition.y};
		}

		return CursorPos;
	}

	static auto GetPlatformHandleForImGui(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> void*
	{
		return InPlatformWindow != nullptr ? InPlatformWindow.get() : nullptr;
	}

	static auto UpdateHoveredViewport(ImGuiIO& IO, const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> void
	{
		if ((IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0)
		{
			return;
		}

		if (ImGuiViewport* Viewport = ImGui::FindViewportByPlatformHandle(GetPlatformHandleForImGui(InPlatformWindow)))
		{
			IO.AddMouseViewportEvent(Viewport->ID);
		}
	}

	static auto GetPlatformViewport(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> ImGuiViewport*
	{
		return ImGui::FindViewportByPlatformHandle(GetPlatformHandleForImGui(InPlatformWindow));
	}

	// ---- Event Handler Method Implementations ------------------------------

	bool FMonaImGuiEventHandler::OnWindowCloseRequested(const std::shared_ptr<FGenericWindow>& InPlatformWindow)
	{
		if (InPlatformWindow == nullptr)
		{
			return false;
		}

		ImGuiViewport* Viewport = GetPlatformViewport(InPlatformWindow);
		if (Viewport == nullptr || (Viewport->Flags & ImGuiViewportFlags_OwnedByApp) != 0)
		{
			return false;
		}

		Viewport->PlatformRequestClose = true;

		// Turn an OS-level close into an ImGui-managed close request
		InPlatformWindow->SetShouldClose(false);
		return true;
	}

	void FMonaImGuiEventHandler::OnWindowFocused(const std::shared_ptr<FGenericWindow>& InPlatformWindow, bool bFocused)
	{
		auto& IO = GetImGuiIO(InPlatformWindow);
		IO.AddFocusEvent(bFocused);
	}

	bool FMonaImGuiEventHandler::OnWindowResized(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InWidth, int32 InHeight, bool bInWasMinimized)
	{
		if (bInWasMinimized)
		{
			return false;
		}

		if (ImGuiViewport* Viewport = GetPlatformViewport(InPlatformWindow))
		{
			if (!ShouldIgnoreWindowSizeEvent(Viewport))
			{
				Viewport->PlatformRequestResize = true;
			}
			return true;
		}
		return false;
	}

	bool FMonaImGuiEventHandler::OnWindowViewportResized(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InWidth, int32 InHeight, bool bInWasMinimized)
	{
		if (bInWasMinimized)
		{
			return false;
		}

		if (ImGuiViewport* Viewport = GetPlatformViewport(InPlatformWindow))
		{
			if (!ShouldIgnoreWindowSizeEvent(Viewport))
			{
				Viewport->PlatformRequestResize = true;
			}
			return true;
		}
		return false;
	}

	bool FMonaImGuiEventHandler::OnWindowMoved(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InX, int32 InY)
	{
		if (ImGuiViewport* Viewport = GetPlatformViewport(InPlatformWindow))
		{
			if (!ShouldIgnoreWindowPosEvent(Viewport))
			{
				Viewport->PlatformRequestMove = true;
			}
			return true;
		}
		return false;
	}

	bool FMonaImGuiEventHandler::OnKeyDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods, bool IsRepeat)
	{
		auto& IO = GetImGuiIO(InPlatformWindow);
		IO.AddKeyEvent(ConvertKeyToImGuiType(Key), true);
		UpdateKeyModifiers(IO, Mods);
		return IO.WantCaptureKeyboard;
	}

	bool FMonaImGuiEventHandler::OnKeyUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EKey Key, EKeyModFlags Mods)
	{
		auto& IO = GetImGuiIO(InPlatformWindow);
		IO.AddKeyEvent(ConvertKeyToImGuiType(Key), false);
		UpdateKeyModifiers(IO, Mods);
		return IO.WantCaptureKeyboard;
	}

	bool FMonaImGuiEventHandler::OnKeyChar(const std::shared_ptr<FGenericWindow>& InPlatformWindow, uint32 Codepoint)
	{
		auto& IO = GetImGuiIO(InPlatformWindow);
		IO.AddInputCharacter(Codepoint);
		return IO.WantTextInput;
	}

	auto FMonaImGuiEventHandler::OnMouseMove(const std::shared_ptr<FGenericWindow>& InPlatformWindow, FVector2d CursorPos) -> bool
	{
		auto& IO = GetImGuiIO(InPlatformWindow);
		const FVector2d ImGuiCursorPos = ConvertMousePositionToImGuiSpace(InPlatformWindow, CursorPos);
		GMonaImGuiMouseState.LastValidMousePos = ImVec2(static_cast<float>(ImGuiCursorPos.x), static_cast<float>(ImGuiCursorPos.y));
		UpdateHoveredViewport(IO, InPlatformWindow);
		IO.AddMousePosEvent(static_cast<float>(ImGuiCursorPos.x), static_cast<float>(ImGuiCursorPos.y));
		return IO.WantCaptureMouse;
	}

	auto FMonaImGuiEventHandler::OnMouseEnter(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> void
	{
		auto& IO = GetImGuiIO(InPlatformWindow);
		GMonaImGuiMouseState.MouseWindow = InPlatformWindow;
		IO.AddMousePosEvent(GMonaImGuiMouseState.LastValidMousePos.x, GMonaImGuiMouseState.LastValidMousePos.y);
	}

	auto FMonaImGuiEventHandler::OnMouseLeave(const std::shared_ptr<FGenericWindow>& InPlatformWindow) -> void
	{
		if (const std::shared_ptr<FGenericWindow> MouseWindow = GMonaImGuiMouseState.MouseWindow.lock())
		{
			if (MouseWindow == InPlatformWindow)
			{
				auto& IO = GetImGuiIO(InPlatformWindow);
				GMonaImGuiMouseState.LastValidMousePos = IO.MousePos;
				GMonaImGuiMouseState.MouseWindow.reset();
				IO.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
			}
		}
	}

	auto FMonaImGuiEventHandler::OnMouseDown(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool
	{
		auto& IO = GetImGuiIO(InPlatformWindow);
		const FVector2d ImGuiCursorPos = ConvertMousePositionToImGuiSpace(InPlatformWindow, CursorPos);
		GMonaImGuiMouseState.LastValidMousePos = ImVec2(static_cast<float>(ImGuiCursorPos.x), static_cast<float>(ImGuiCursorPos.y));
		UpdateHoveredViewport(IO, InPlatformWindow);
		IO.AddMousePosEvent(static_cast<float>(ImGuiCursorPos.x), static_cast<float>(ImGuiCursorPos.y));
		IO.AddMouseButtonEvent(ConvertMouseButtonToImGuiType(Button), true);
		return IO.WantCaptureMouse;
	}

	auto FMonaImGuiEventHandler::OnMouseUp(const std::shared_ptr<FGenericWindow>& InPlatformWindow, EMouseButton Button, FVector2d CursorPos) -> bool
	{
		auto& IO = GetImGuiIO(InPlatformWindow);
		const FVector2d ImGuiCursorPos = ConvertMousePositionToImGuiSpace(InPlatformWindow, CursorPos);
		GMonaImGuiMouseState.LastValidMousePos = ImVec2(static_cast<float>(ImGuiCursorPos.x), static_cast<float>(ImGuiCursorPos.y));
		UpdateHoveredViewport(IO, InPlatformWindow);
		IO.AddMousePosEvent(static_cast<float>(ImGuiCursorPos.x), static_cast<float>(ImGuiCursorPos.y));
		IO.AddMouseButtonEvent(ConvertMouseButtonToImGuiType(Button), false);
		return IO.WantCaptureMouse;
	}

	bool FMonaImGuiEventHandler::OnMouseWheel(const std::shared_ptr<FGenericWindow>& InPlatformWindow, double DeltaX, double DeltaY)
	{
		auto& IO = GetImGuiIO(InPlatformWindow);
		IO.AddMouseWheelEvent(static_cast<float>(DeltaX), static_cast<float>(DeltaY));
		return IO.WantCaptureMouse;
	}

	// ---- Public API --------------------------------------------------------

	auto ImGuiMonaImpl_Init() -> void
	{
		ConfigureDefaultImGuiBehavior();

		ImGuiIO& IO = ImGui::GetIO();
		IO.BackendPlatformName = "Mona";
		IO.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports;
		IO.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
		IO.BackendFlags |= ImGuiBackendFlags_HasMouseHoveredViewport;
		IO.BackendFlags |= ImGuiBackendFlags_HasParentViewport;

		ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
		PlatformIO.Platform_CreateWindow = ImGuiMonaImpl_CreateWindow;
		PlatformIO.Platform_DestroyWindow = ImGuiMonaImpl_DestroyWindow;
		PlatformIO.Platform_ShowWindow = ImGuiMonaImpl_ShowWindow;
		PlatformIO.Platform_SetWindowPos = ImGuiMonaImpl_SetWindowPos;
		PlatformIO.Platform_GetWindowPos = ImGuiMonaImpl_GetWindowPos;
		PlatformIO.Platform_SetWindowSize = ImGuiMonaImpl_SetWindowSize;
		PlatformIO.Platform_GetWindowSize = ImGuiMonaImpl_GetWindowSize;
		PlatformIO.Platform_SetWindowFocus = ImGuiMonaImpl_SetWindowFocus;
		PlatformIO.Platform_GetWindowFocus = ImGuiMonaImpl_GetWindowFocus;
		PlatformIO.Platform_GetWindowMinimized = ImGuiMonaImpl_GetWindowMinimized;
		PlatformIO.Platform_SetWindowTitle = ImGuiMonaImpl_SetWindowTitle;
		PlatformIO.Platform_SetWindowAlpha = ImGuiMonaImpl_SetWindowAlpha;
		PlatformIO.Platform_GetWindowDpiScale = ImGuiMonaImpl_GetWindowDpiScale;
		PlatformIO.Platform_GetWindowFramebufferScale = ImGuiMonaImpl_GetWindowFramebufferScale;
		PlatformIO.Platform_UpdateWindow = ImGuiMonaImpl_UpdateWindow;

		UpdateMonitors();
		InitFonts();

		auto& App = Mona::FMonaApplication::Get();
		check(Mona::FMonaApplication::IsInitialized());
		App.SetMonaEventHandler(std::make_unique<FMonaBackendEventHandler>());
	}

	auto ImGuiMonaImpl_Shutdown() -> void
	{
		auto& App = Mona::FMonaApplication::Get();
		App.SetMonaEventHandler(nullptr);
		GMonaImGuiMouseState = {};

		if (ImGuiViewport* MainViewport = ImGui::GetMainViewport())
		{
			DestroyPlatformViewportData(MainViewport);
		}

		ImGuiIO& IO = ImGui::GetIO();
		IO.BackendPlatformName = nullptr;
		IO.BackendFlags &= ~(ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_HasMouseHoveredViewport | ImGuiBackendFlags_HasParentViewport);

		ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
		PlatformIO.ClearPlatformHandlers();
	}

	auto ImGuiMonaImpl_NewFrame() -> void
	{
		const std::shared_ptr<MWindow> MainWindow = GetMainMonaWindow();
		if (!MainWindow)
		{
			return;
		}

		static double LastTime = FTime::Seconds();
		const double CurrentTime = FTime::Seconds();
		ImGuiIO& IO = ImGui::GetIO();
		IO.DeltaTime = static_cast<float>(CurrentTime - LastTime);
		LastTime = CurrentTime;

		SyncAllViewportsFrameStates();
		UpdateMonitors();
		UpdateMouseData();
		UpdateMouseCursor();
	}

	auto ImGuiMonaImpl_BindMainViewport(const std::shared_ptr<MWindow>& Window) -> void
	{
		ImGui::SetCurrentContext(GMonaImGuiContext);
		BindMainViewportToWindowInternal(Window);
	}

	auto ImGuiMonaImpl_GetViewportWindow(ImGuiViewport* Viewport) -> std::shared_ptr<MWindow>
	{
		return GetViewportWindow(Viewport);
	}
}
