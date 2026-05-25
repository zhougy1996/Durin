#include "MonaImGuiBackend.h"

#include "ThirdParty/ImGui/imgui_threaded_rendering.h"

#include "RHI.h"
#include "RenderingThread.h"
#include "Rendering/MonaRHIRenderer.h"
#include "Application/MonaApplication.h"
#include "Widgets/MWindow.h"
#include "ImGuiRHIImpl.h"
#include "MonaImGuiEventHandler.h"
#include "Misc/Paths.h"

namespace Durin::Mona
{
	ImGuiContext* GMonaImGuiContext = nullptr;

	struct FMonaImGuiViewportData
	{
		std::shared_ptr<MWindow> Window;
		FImGuiRHIImpl_WindowRenderBuffers RenderBuffers;
		std::array<ImDrawDataSnapshot, 2> DrawDataSnapshots;
	};

	namespace
	{
		auto GetViewportData(ImGuiViewport* Viewport) -> FMonaImGuiViewportData*
		{
			return static_cast<FMonaImGuiViewportData*>(Viewport->PlatformUserData);
		}

		auto GetMainMonaWindow() -> std::shared_ptr<MWindow>
		{
			if (auto* ViewportData = GetViewportData(ImGui::GetMainViewport()))
			{
				return ViewportData->Window;
			}

			auto& App = FMonaApplication::Get();
			const auto& Windows = App.GetWindows();
			if (Windows.empty())
			{
				return nullptr;
			}

			return Windows.front();
		}

		auto GetViewportWindow(ImGuiViewport* Viewport) -> std::shared_ptr<MWindow>
		{
			const auto* ViewportData = GetViewportData(Viewport);
			return ViewportData != nullptr ? ViewportData->Window : nullptr;
		}

		auto GetImGuiPlatformHandle(const std::shared_ptr<FGenericWindow>& NativeWindow) -> void*
		{
			return NativeWindow != nullptr ? NativeWindow.get() : nullptr;
		}

		auto GetViewportRenderBuffers(ImGuiViewport* Viewport) -> FImGuiRHIImpl_WindowRenderBuffers*
		{
			auto* ViewportData = GetViewportData(Viewport);
			return ViewportData != nullptr ? &ViewportData->RenderBuffers : nullptr;
		}

		auto GetViewportSnapshots(ImGuiViewport* Viewport) -> std::array<ImDrawDataSnapshot, 2>*
		{
			auto* ViewportData = GetViewportData(Viewport);
			return ViewportData != nullptr ? &ViewportData->DrawDataSnapshots : nullptr;
		}

		auto UpdateViewportFromWindow(ImGuiViewport* Viewport, const std::shared_ptr<MWindow>& Window) -> void
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
			const FIntPoint WindowSizeInt = NativeWindow->GetWindowSize();
			const FVector2f ViewportSize = Window->GetViewportSize();
			const FVector2f WindowPosition(static_cast<float>(WindowPositionInt.x), static_cast<float>(WindowPositionInt.y));
			const FVector2f WindowSize(static_cast<float>(WindowSizeInt.x), static_cast<float>(WindowSizeInt.y));
			Window->SetCachedScreenPosition(WindowPosition);
			Window->SetCachedSize(WindowSize);

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

		auto SyncMainViewportFrameState(const std::shared_ptr<MWindow>& MainWindow) -> void
		{
			UpdateViewportFromWindow(ImGui::GetMainViewport(), MainWindow);
		}

		auto BindViewportToWindow(ImGuiViewport* Viewport, const std::shared_ptr<MWindow>& Window) -> void
		{
			if (!Viewport || !Window)
			{
				return;
			}

			auto* ViewportData = GetViewportData(Viewport);
			if (ViewportData == nullptr)
			{
				ViewportData = new FMonaImGuiViewportData();
				Viewport->PlatformUserData = ViewportData;
			}

			ViewportData->Window = Window;
			UpdateViewportFromWindow(Viewport, Window);
		}

		auto BindMainViewportToWindowInternal(const std::shared_ptr<MWindow>& MainWindow) -> void
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

		auto UpdateMonitors() -> void
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

		auto RenderViewport(ImGuiViewport* Viewport, ImDrawData* DrawData, FImGuiRHIImpl_WindowRenderBuffers& RenderBuffers) -> void
		{
			if (DrawData == nullptr)
			{
				return;
			}

			auto& App = FMonaApplication::Get();
			auto* Renderer = dynamic_cast<FMonaRHIRenderer*>(App.GetRenderer());
			const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport);
			if (Renderer == nullptr || Window == nullptr || Window->IsMinimized())
			{
				return;
			}

			FViewportRHIRef ViewportRHI = Renderer->GetRHIViewport(*Window);
			if (ViewportRHI == nullptr)
			{
				return;
			}

			std::array<ImDrawDataSnapshot, 2>* Snapshots = GetViewportSnapshots(Viewport);
			check(Snapshots != nullptr);

			ImDrawDataSnapshot& Snapshot = (*Snapshots)[GFrameCounter % 2];
			Snapshot.SnapUsingSwap(DrawData, FTime::Seconds());

			ImGuiRHIImpl_RenderDrawData(ViewportRHI, &Snapshot.DrawData, &RenderBuffers);
		}

		auto ApplyViewportWindowStyle(ImGuiViewport* Viewport, const std::shared_ptr<MWindow>& Window) -> void
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

		auto ImGuiMona_CreateWindow(ImGuiViewport* Viewport) -> void
		{
			auto Window = std::make_shared<MWindow>();
			Window->SetTitle("ImGui");
			Window->ReshapeWindow({Viewport->Pos.x, Viewport->Pos.y}, {Viewport->Size.x, Viewport->Size.y});
			ApplyViewportWindowStyle(Viewport, Window);

			auto& App = FMonaApplication::Get();
			App.AddWindow(Window, false);
			Window->HideWindow();

			auto* ViewportData = new FMonaImGuiViewportData();
			Viewport->PlatformUserData = ViewportData;
			BindViewportToWindow(Viewport, Window);
		}

		auto ImGuiMona_DestroyWindow(ImGuiViewport* Viewport) -> void
		{
			auto* ViewportData = GetViewportData(Viewport);
			if (ViewportData != nullptr && ViewportData->Window != nullptr && (Viewport->Flags & ImGuiViewportFlags_OwnedByApp) == 0)
			{
				ViewportData->Window->RequestDestroyWindow();
			}

			delete ViewportData;
			Viewport->PlatformUserData = nullptr;
			Viewport->PlatformHandle = nullptr;
			Viewport->PlatformHandleRaw = nullptr;
		}

		auto ImGuiMona_ShowWindow(ImGuiViewport* Viewport) -> void
		{
			if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
			{
				ApplyViewportWindowStyle(Viewport, Window);
				Window->ShowWindow();
			}
		}

		auto ImGuiMona_SetWindowPos(ImGuiViewport* Viewport, ImVec2 Position) -> void
		{
			if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
			{
				Window->MoveWindowTo({Position.x, Position.y});
			}
		}

		auto ImGuiMona_GetWindowPos(ImGuiViewport* Viewport) -> ImVec2
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

		auto ImGuiMona_SetWindowSize(ImGuiViewport* Viewport, ImVec2 Size) -> void
		{
			if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
			{
				Window->ResizeWindow({Size.x, Size.y});
			}
		}

		auto ImGuiMona_GetWindowSize(ImGuiViewport* Viewport) -> ImVec2
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

		auto ImGuiMona_SetWindowFocus(ImGuiViewport* Viewport) -> void
		{
			if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
			{
				if (const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
				{
					NativeWindow->Focus();
				}
			}
		}

		auto ImGuiMona_GetWindowFocus(ImGuiViewport* Viewport) -> bool
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

		auto ImGuiMona_GetWindowMinimized(ImGuiViewport* Viewport) -> bool
		{
			if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
			{
				return Window->IsMinimized();
			}
			return false;
		}

		auto ImGuiMona_SetWindowTitle(ImGuiViewport* Viewport, const char* Title) -> void
		{
			if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
			{
				const bool bDecorated = (Viewport->Flags & ImGuiViewportFlags_NoDecoration) == 0;
				Window->SetTitle(bDecorated && Title != nullptr ? Title : "");
			}
		}

		auto ImGuiMona_SetWindowAlpha(ImGuiViewport* Viewport, float Alpha) -> void
		{
			if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
			{
				if (const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
				{
					NativeWindow->SetOpacity(Alpha);
				}
			}
		}

		auto ImGuiMona_GetWindowDpiScale(ImGuiViewport* Viewport) -> float
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

		auto ImGuiMona_UpdateWindow(ImGuiViewport* Viewport) -> void
		{
			if (const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport))
			{
				ApplyViewportWindowStyle(Viewport, Window);

				if (const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
				{
					const FIntPoint WindowPositionInt = NativeWindow->GetWindowPosition();
					const FIntPoint WindowSizeInt = NativeWindow->GetWindowSize();
					const ImVec2 PlatformPos(static_cast<float>(WindowPositionInt.x), static_cast<float>(WindowPositionInt.y));
					const ImVec2 PlatformSize(static_cast<float>(WindowSizeInt.x), static_cast<float>(WindowSizeInt.y));

					if (PlatformPos.x != Viewport->Pos.x || PlatformPos.y != Viewport->Pos.y)
					{
						Viewport->PlatformRequestMove = true;
					}

					if (PlatformSize.x != Viewport->Size.x || PlatformSize.y != Viewport->Size.y)
					{
						Viewport->PlatformRequestResize = true;
					}
				}
			}
		}

		auto ImGuiMona_RendererCreateWindow(ImGuiViewport* Viewport) -> void
		{
			auto& App = FMonaApplication::Get();
			auto* Renderer = dynamic_cast<FMonaRHIRenderer*>(App.GetRenderer());
			const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport);
			if (Renderer != nullptr && Window != nullptr)
			{
				Renderer->CreateViewport(Window);
			}
		}

		auto ImGuiMona_RendererDestroyWindow(ImGuiViewport* Viewport) -> void
		{
			if (auto* RenderBuffers = GetViewportRenderBuffers(Viewport))
			{
				RenderBuffers->Clear();
			}
		}

		auto ImGuiMona_RendererSetWindowSize(ImGuiViewport* Viewport, ImVec2 Size) -> void
		{
			auto& App = FMonaApplication::Get();
			auto* Renderer = dynamic_cast<FMonaRHIRenderer*>(App.GetRenderer());
			const std::shared_ptr<MWindow> Window = GetViewportWindow(Viewport);
			if (Renderer != nullptr && Window != nullptr)
			{
				const uint32 Width = static_cast<uint32>(FMath::Max(8.0f, Size.x));
				const uint32 Height = static_cast<uint32>(FMath::Max(8.0f, Size.y));
				Renderer->RequestResize(Window, Width, Height);
			}
		}

		auto ImGuiMona_RendererRenderWindow(ImGuiViewport* Viewport, void* RenderArg) -> void
		{
			if (Viewport == ImGui::GetMainViewport())
			{
				return;
			}

			auto* RenderBuffers = GetViewportRenderBuffers(Viewport);
			if (RenderBuffers == nullptr)
			{
				return;
			}

			RenderViewport(Viewport, Viewport->DrawData, *RenderBuffers);
		}

		auto ImGuiMona_RendererSwapBuffers(ImGuiViewport* Viewport, void* RenderArg) -> void
		{
		}

		auto InitializePlatformInterface() -> void
		{
			ImGuiIO& IO = ImGui::GetIO();
			IO.BackendPlatformName = "Mona";
			IO.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports;
			IO.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
			IO.BackendFlags |= ImGuiBackendFlags_HasMouseHoveredViewport;

			ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
			PlatformIO.Platform_CreateWindow = ImGuiMona_CreateWindow;
			PlatformIO.Platform_DestroyWindow = ImGuiMona_DestroyWindow;
			PlatformIO.Platform_ShowWindow = ImGuiMona_ShowWindow;
			PlatformIO.Platform_SetWindowPos = ImGuiMona_SetWindowPos;
			PlatformIO.Platform_GetWindowPos = ImGuiMona_GetWindowPos;
			PlatformIO.Platform_SetWindowSize = ImGuiMona_SetWindowSize;
			PlatformIO.Platform_GetWindowSize = ImGuiMona_GetWindowSize;
			PlatformIO.Platform_SetWindowFocus = ImGuiMona_SetWindowFocus;
			PlatformIO.Platform_GetWindowFocus = ImGuiMona_GetWindowFocus;
			PlatformIO.Platform_GetWindowMinimized = ImGuiMona_GetWindowMinimized;
			PlatformIO.Platform_SetWindowTitle = ImGuiMona_SetWindowTitle;
			PlatformIO.Platform_SetWindowAlpha = ImGuiMona_SetWindowAlpha;
			PlatformIO.Platform_GetWindowDpiScale = ImGuiMona_GetWindowDpiScale;
			PlatformIO.Platform_UpdateWindow = ImGuiMona_UpdateWindow;
			PlatformIO.Renderer_CreateWindow = ImGuiMona_RendererCreateWindow;
			PlatformIO.Renderer_DestroyWindow = ImGuiMona_RendererDestroyWindow;
			PlatformIO.Renderer_SetWindowSize = ImGuiMona_RendererSetWindowSize;
			PlatformIO.Renderer_RenderWindow = ImGuiMona_RendererRenderWindow;
			PlatformIO.Renderer_SwapBuffers = ImGuiMona_RendererSwapBuffers;

			UpdateMonitors();
		}

		auto ConfigureDefaultImGuiBehavior() -> void
		{
			ImGuiIO& IO = ImGui::GetIO();
			IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
			IO.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
			IO.ConfigViewportsNoAutoMerge = false;
			IO.ConfigViewportsNoTaskBarIcon = true;
			IO.ConfigViewportsNoDecoration = true;

			ImGuiStyle& Style = ImGui::GetStyle();
			if ((IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
			{
				Style.WindowRounding = 0.0f;
				Style.Colors[ImGuiCol_WindowBg].w = 1.0f;
			}
		}
	} // namespace

	static auto InitFonts() -> void
	{
		ImGuiIO& IO = ImGui::GetIO();
		IO.Fonts->AddFontDefaultVector();

		const ImWchar* ChineseGlyphRanges = IO.Fonts->GetGlyphRangesChineseSimplifiedCommon();
		std::string FontDir = FPaths::EngineDir() + "Content/ImGuiFonts/";
		std::string FontPath_DroidSans = FontDir + "DroidSans.ttf";
		std::string FontPath_NotoSansSC = FontDir + "NotoSansSC-Regular.ttf";

		ImFont* FallbackLatinFont = IO.Fonts->AddFontFromFileTTF(FontPath_DroidSans.c_str(), 20.0f);
		ImFont* ChineseFont = IO.Fonts->AddFontFromFileTTF(FontPath_NotoSansSC.c_str(), 20.0f, nullptr, ChineseGlyphRanges);
		if (ChineseFont)
		{
			IO.FontDefault = ChineseFont;
		}
		else if (FallbackLatinFont)
		{
			IO.FontDefault = FallbackLatinFont;
		}
		IO.Fonts->Build();
	}

	auto FMonaImGuiBackend::Initialize() -> void
	{
		check(GDynamicRHI);
		GMonaImGuiContext = ImGui::CreateContext();
		ImGui::SetCurrentContext(GMonaImGuiContext);

		ConfigureDefaultImGuiBehavior();
		ImGuiRHIImpl_Init();
		InitializePlatformInterface();
		InitFonts();

		// Set the Mona event handler to the application. This will allow us to receive input events and forward them to ImGui.
		auto& App = FMonaApplication::Get();
		check(FMonaApplication::IsInitialized());
		App.SetMonaEventHandler(std::make_unique<FMonaBackendEventHandler>());
	}

	auto FMonaImGuiBackend::Shutdown() -> void
	{
		ImGui::SetCurrentContext(GMonaImGuiContext);

		auto& App = FMonaApplication::Get();

		ImGui::DestroyPlatformWindows();
		App.SetMonaEventHandler(nullptr);
		ImGuiIO& IO = ImGui::GetIO();
		IO.BackendPlatformName = nullptr;
		IO.BackendFlags &= ~(ImGuiBackendFlags_PlatformHasViewports | ImGuiBackendFlags_HasMouseHoveredViewport);

		ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
		PlatformIO.ClearPlatformHandlers();
		ImGuiRHIImpl_Shutdown();
		GMonaImGuiContext = nullptr;

		FlushRenderingCommands();
		ImGui::DestroyContext();
	}

	auto FMonaImGuiBackend::NewFrame() -> void
	{
		ImGui::SetCurrentContext(GMonaImGuiContext);

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

		SyncMainViewportFrameState(MainWindow);
		UpdateMonitors();

		ImGuiRHIImpl_NewFrame();
		ImGui::NewFrame();

		// Test content
		if (!MainWindow || MainWindow->IsMinimized())
		{
			return;
		}
		ImGui::ShowDemoWindow();
	}


	auto FMonaImGuiBackend::Render() -> void
	{
		ImGui::SetCurrentContext(GMonaImGuiContext);
		ImGui::Render();

		ImGuiIO& IO = ImGui::GetIO();
		if ((IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
		{
			ImGui::UpdatePlatformWindows();
		}

		if (ImGuiViewport* MainViewport = ImGui::GetMainViewport())
		{
			if (auto* RenderBuffers = GetViewportRenderBuffers(MainViewport))
			{
				RenderViewport(MainViewport, MainViewport->DrawData, *RenderBuffers);
			}
		}

		if ((IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
		{
			ImGui::RenderPlatformWindowsDefault();
		}
	}

	auto FMonaImGuiBackend::BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void
	{
		ImGui::SetCurrentContext(GMonaImGuiContext);
		BindMainViewportToWindowInternal(Window);
	}

}
