#include "MonaImGuiBackend.h"

#include "ThirdParty/ImGui/imgui_threaded_rendering.h"

#include "Application/MonaApplication.h"
#include "ImGuiRHIImpl.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "Rendering/MonaRenderer.h"
#include "Rendering/RenderingCommon.h"
#include "RenderingThread.h"
#include "Widgets/MWindow.h"

namespace Durin::Mona
{
	namespace
	{
		std::unordered_map<FRHITexture*, FTextureRHIRef*> GExternalTextureRefs;
	}

	struct FMonaImGuiRendererViewportData
	{
		FImGuiRHIImpl_WindowRenderBuffers RenderBuffers;
		std::array<ImDrawDataSnapshot, 2> DrawDataSnapshots;
		FViewportRHIRef ViewportRHI;
		uint32 NextSnapshotIndex = 0;
	};

	namespace
	{
		auto GetRendererViewportData(ImGuiViewport* Viewport) -> FMonaImGuiRendererViewportData*
		{
			return Viewport != nullptr ? static_cast<FMonaImGuiRendererViewportData*>(Viewport->RendererUserData) : nullptr;
		}

		auto CreateRendererViewportData(ImGuiViewport* Viewport) -> FMonaImGuiRendererViewportData*
		{
			if (Viewport == nullptr)
			{
				return nullptr;
			}

			auto* ViewportData = GetRendererViewportData(Viewport);
			if (ViewportData == nullptr)
			{
				ViewportData = new FMonaImGuiRendererViewportData();
				Viewport->RendererUserData = ViewportData;
			}

			return ViewportData;
		}

		auto DestroyRendererViewportData(ImGuiViewport* Viewport) -> void
		{
			if (Viewport == nullptr)
			{
				return;
			}

			if (auto* ViewportData = GetRendererViewportData(Viewport))
			{
				ViewportData->RenderBuffers.Clear();
				ViewportData->ViewportRHI = nullptr;
				delete ViewportData;
			}
			Viewport->RendererUserData = nullptr;
		}

		auto ShouldRenderMainViewportWithImGui() -> bool
		{
			return true;
		}

		auto GetViewportRenderBuffers(ImGuiViewport* Viewport) -> FImGuiRHIImpl_WindowRenderBuffers*
		{
			auto* ViewportData = GetRendererViewportData(Viewport);
			return ViewportData != nullptr ? &ViewportData->RenderBuffers : nullptr;
		}

		auto SnapshotViewportDrawData(ImGuiViewport* Viewport, ImDrawData* DrawData) -> ImDrawData*
		{
			auto* ViewportData = GetRendererViewportData(Viewport);
			if (ViewportData == nullptr || DrawData == nullptr)
			{
				return nullptr;
			}

			const uint32 SnapshotIndex = ViewportData->NextSnapshotIndex;
			ImDrawDataSnapshot& Snapshot = ViewportData->DrawDataSnapshots[SnapshotIndex];
			Snapshot.SnapUsingSwap(DrawData, FTime::Seconds());
			ViewportData->NextSnapshotIndex = (SnapshotIndex + 1) % ViewportData->DrawDataSnapshots.size();
			return &Snapshot.DrawData;
		}

		auto GetTextureID(FRHITexture* Texture) -> ImTextureID
		{
			if (Texture == nullptr)
			{
				return ImTextureID_Invalid;
			}

			if (auto It = GExternalTextureRefs.find(Texture); It != GExternalTextureRefs.end())
			{
				return reinterpret_cast<ImTextureID>(It->second);
			}

			auto* Ref = new FTextureRHIRef(Texture);
			GExternalTextureRefs[Texture] = Ref;
			return reinterpret_cast<ImTextureID>(Ref);
		}

		auto PruneExternalTextureRefs() -> void
		{
			for (auto It = GExternalTextureRefs.begin(); It != GExternalTextureRefs.end(); )
			{
				if (It->second->GetRefCount() == 1)
				{
					delete It->second;
					It = GExternalTextureRefs.erase(It);
				}
				else
				{
					++It;
				}
			}
		}

		auto RenderViewport(ImGuiViewport* Viewport, ImDrawData* DrawData, FMonaImGuiRendererViewportData& ViewportData, bool bPresent) -> void
		{
			if (DrawData == nullptr)
			{
				ViewportData.ViewportRHI = nullptr;
				return;
			}

			auto& App = FMonaApplication::Get();
			FMonaRenderer* Renderer = App.GetRenderer();
			const std::shared_ptr<MWindow> Window = ImGuiMonaImpl_GetViewportWindow(Viewport);
			if (Renderer == nullptr || Window == nullptr || Window->IsMinimized())
			{
				ViewportData.ViewportRHI = nullptr;
				return;
			}

			ViewportData.ViewportRHI = Renderer->GetRHIViewport(*Window);
			if (ViewportData.ViewportRHI == nullptr)
			{
				return;
			}

			ImDrawData* SnapshotDrawData = SnapshotViewportDrawData(Viewport, DrawData);
			if (SnapshotDrawData == nullptr)
			{
				return;
			}

			ImGuiRHIImpl_RenderDrawData(ViewportData.ViewportRHI, SnapshotDrawData, &ViewportData.RenderBuffers, bPresent);
		}

		// ---- Renderer Callbacks (ImGuiPlatformIO) --------------------------

		auto ImGuiMona_RendererCreateWindow(ImGuiViewport* Viewport) -> void
		{
			CreateRendererViewportData(Viewport);

			auto& App = FMonaApplication::Get();
			FMonaRenderer* Renderer = App.GetRenderer();
			const std::shared_ptr<MWindow> Window = ImGuiMonaImpl_GetViewportWindow(Viewport);
			if (Renderer != nullptr && Window != nullptr)
			{
				Renderer->CreateViewport(Window);
			}
		}

		auto ImGuiMona_RendererDestroyWindow(ImGuiViewport* Viewport) -> void
		{
			DestroyRendererViewportData(Viewport);
		}

		auto ImGuiMona_RendererSetWindowSize(ImGuiViewport* Viewport, ImVec2 Size) -> void
		{
			auto& App = FMonaApplication::Get();
			FMonaRenderer* Renderer = App.GetRenderer();
			const std::shared_ptr<MWindow> Window = ImGuiMonaImpl_GetViewportWindow(Viewport);
			if (Renderer == nullptr || Window == nullptr)
			{
				return;
			}

			uint32 Width, Height;
			if (const std::shared_ptr<FGenericWindow> NativeWindow = Window->GetNativeWindow())
			{
				const FIntPoint ViewportSize = NativeWindow->GetViewportSize();
				Width = static_cast<uint32>(FMath::Max(8, ViewportSize.x));
				Height = static_cast<uint32>(FMath::Max(8, ViewportSize.y));
			}
			else
			{
				Width = static_cast<uint32>(FMath::Max(8.0f, Size.x));
				Height = static_cast<uint32>(FMath::Max(8.0f, Size.y));
			}
			Renderer->RequestResize(Window, Width, Height);
			Renderer->GetRHIViewport(*Window);
		}

		auto ImGuiMona_RendererRenderWindow(ImGuiViewport* Viewport, void* RenderArg) -> void
		{
			auto* ViewportData = GetRendererViewportData(Viewport);
			if (ViewportData == nullptr)
			{
				return;
			}

			RenderViewport(Viewport, Viewport->DrawData, *ViewportData, false);
		}

		auto ImGuiMona_RendererSwapBuffers(ImGuiViewport* Viewport, void* RenderArg) -> void
		{
			if (auto* ViewportData = GetRendererViewportData(Viewport))
			{
				if (ViewportData->ViewportRHI != nullptr)
				{
					ImGuiRHIImpl_PresentViewport(ViewportData->ViewportRHI);
				}
			}
		}
	}

	auto FMonaImGuiBackend::Initialize() -> void
	{
		check(GDynamicRHI);
		GMonaImGuiContext = ImGui::CreateContext();
		ImGui::SetCurrentContext(GMonaImGuiContext);
		static std::string GImGuiIniPath = FPaths::LaunchDir() + "imgui.ini";
		ImGui::GetIO().IniFilename = GImGuiIniPath.c_str();

		ImGuiRHIImpl_Init();   // sets RendererHasTextures before InitFonts->Build()
		ImGuiMonaImpl_Init();

		// Register renderer callbacks (coordinator bridges platform and renderer)
		ImGuiIO& IO = ImGui::GetIO();
		IO.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;

		ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
		PlatformIO.Renderer_CreateWindow = ImGuiMona_RendererCreateWindow;
		PlatformIO.Renderer_DestroyWindow = ImGuiMona_RendererDestroyWindow;
		PlatformIO.Renderer_SetWindowSize = ImGuiMona_RendererSetWindowSize;
		PlatformIO.Renderer_RenderWindow = ImGuiMona_RendererRenderWindow;
		PlatformIO.Renderer_SwapBuffers = ImGuiMona_RendererSwapBuffers;
	}

	auto FMonaImGuiBackend::Shutdown() -> void
	{
		ImGui::SetCurrentContext(GMonaImGuiContext);

		ImGui::DestroyPlatformWindows();
		if (ImGuiViewport* MainViewport = ImGui::GetMainViewport())
		{
			DestroyRendererViewportData(MainViewport);
		}

		ImGuiMonaImpl_Shutdown();

		ImGuiIO& IO = ImGui::GetIO();
		IO.BackendFlags &= ~ImGuiBackendFlags_RendererHasViewports;

		ImGuiPlatformIO& PlatformIO = ImGui::GetPlatformIO();
		PlatformIO.ClearRendererHandlers();

		ImGuiRHIImpl_Shutdown();
		GMonaImGuiContext = nullptr;

		FlushRenderingCommands();
		for (auto& [_, Ref] : GExternalTextureRefs)
		{
			delete Ref;
		}
		GExternalTextureRefs.clear();
		ImGui::DestroyContext();
	}

	auto FMonaImGuiBackend::NewFrame() -> void
	{
		ImGui::SetCurrentContext(GMonaImGuiContext);

		PruneExternalTextureRefs();
		ImGuiMonaImpl_NewFrame();
		ImGuiRHIImpl_NewFrame();
		ImGui::NewFrame();

		auto& App = FMonaApplication::Get();
		App.DrawWindows();
	}

	auto FMonaImGuiBackend::Render() -> void
	{
		ImGui::SetCurrentContext(GMonaImGuiContext);
		ImGui::Render();

		ImGuiIO& IO = ImGui::GetIO();

		if (ImGuiViewport* MainViewport = ImGui::GetMainViewport())
		{
			if (ShouldRenderMainViewportWithImGui())
			{
				if (auto* RenderBuffers = GetViewportRenderBuffers(MainViewport))
				{
					// Ensure main viewport swapchain matches current window framebuffer size.
					// The main viewport is skipped by UpdatePlatformWindows() (index 0),
					// so its swapchain must be resized here.
					auto& App = FMonaApplication::Get();
					FMonaRenderer* Renderer = App.GetRenderer();
					if (const std::shared_ptr<MWindow> MainWindow = ImGuiMonaImpl_GetViewportWindow(MainViewport))
					{
						if (const std::shared_ptr<FGenericWindow> NativeWindow = MainWindow->GetNativeWindow())
						{
							const FIntPoint ViewportSize = NativeWindow->GetViewportSize();
							Renderer->RequestResize(MainWindow, ViewportSize.x, ViewportSize.y);
						}
					}

					if (auto* ViewportData = GetRendererViewportData(MainViewport))
					{
						RenderViewport(MainViewport, MainViewport->DrawData, *ViewportData, true);
					}
				}
			}
		}

		if ((IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
		{
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
		}
	}

	auto FMonaImGuiBackend::DrawTexture(FRHITexture* Texture, const FVector2f& Size) -> bool
	{
		const ImTextureID TextureID = GetTextureID(Texture);
		if (TextureID == ImTextureID_Invalid)
		{
			return false;
		}

		ImGui::Image(TextureID, {Size.x, Size.y});
		return true;
	}

	auto FMonaImGuiBackend::BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void
	{
		ImGuiMonaImpl_BindMainViewport(Window);
		CreateRendererViewportData(ImGui::GetMainViewport());
	}

	auto FMonaImGuiBackend::ShowDemoWindow() -> void
	{
		ImGui::SetCurrentContext(GMonaImGuiContext);
		ImGui::ShowDemoWindow();
	}
}
