#include "MonaImGuiBackend.h"

#include "Application/MonaApplication.h"
#include "ImGuiRHIImpl.h"
#include "MonaCoreGlobals.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "RenderingThread.h"

namespace Durin::MonaImGui
{
	static auto ShouldRenderMainViewportWithImGui() -> bool
	{
		return true;
	}

	auto FMonaImGuiBackend::Initialize() -> void
	{
		check(GDynamicRHI);
		GMonaImGuiContext = ImGui::CreateContext();
		ImGui::SetCurrentContext(GMonaImGuiContext);
		static std::string GImGuiIniPath = FPaths::LaunchConfigsDir() + "imgui.ini";
		ImGui::GetIO().IniFilename = GImGuiIniPath.c_str();

		ImGuiRHIImpl_Init();   // sets RendererHasTextures before InitFonts->Build()
		ImGuiMonaImpl_Init();
	}

	auto FMonaImGuiBackend::Shutdown() -> void
	{
		ImGui::SetCurrentContext(GMonaImGuiContext);

		// Render commands retain non-owning pointers into viewport draw
		// snapshots, render buffers, and texture backend data. Drain all
		// previously accepted work before destroying those owners.
		FlushRenderingCommands();

		ImGui::DestroyPlatformWindows();

		ImGuiMonaImpl_Shutdown();

		ImGuiRHIImpl_Shutdown();
		GMonaImGuiContext = nullptr;
		ImGui::DestroyContext();
	}

	auto FMonaImGuiBackend::NewFrame() -> void
	{
		ImGui::SetCurrentContext(GMonaImGuiContext);

		ImGuiMonaImpl_NewFrame();
		ImGui::NewFrame();

		auto& App = Mona::FMonaApplication::Get();
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
				ImGuiRHIImpl_RenderMainViewport(MainViewport);
			}
		}

		if ((IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
		{
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
		}

		ImGuiRHIImpl_RetireUnregisteredTextures();
	}

	auto FMonaImGuiBackend::RegisterTexture(const FTextureRHIRef& Texture) -> void
	{
		ImGuiRHIImpl_RegisterTexture(Texture);
	}

	auto FMonaImGuiBackend::UnregisterTexture(const FTextureRHIRef& Texture) -> void
	{
		ImGuiRHIImpl_UnregisterTexture(Texture);
	}

	auto FMonaImGuiBackend::IsTextureRegistered(const FRHITexture* InTexture) -> bool
	{
		return ImGuiRHIImpl_GetTextureID(InTexture) != ImTextureID_Invalid;
	}

	auto FMonaImGuiBackend::DrawImage(const FRHITexture* InTexture, const FVector2f& Size) -> bool
	{
		if (InTexture == nullptr)
		{
			return false;
		}

		const ImTextureID TextureID = ImGuiRHIImpl_GetTextureID(InTexture);
		if (TextureID == ImTextureID_Invalid)
		{
			return false;
		}

		ImGui::Image(TextureID, {Size.x, Size.y});
		return true;
	}

	auto FMonaImGuiBackend::Get() -> FMonaImGuiBackend&
	{
		check(Mona::GActiveUIBackend);
		return static_cast<FMonaImGuiBackend&>(*Mona::GActiveUIBackend);
	}

	auto FMonaImGuiBackend::BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void
	{
		ImGuiMonaImpl_BindMainViewport(Window);
		ImGuiRHIImpl_EnsureMainViewportData(ImGui::GetMainViewport());
	}

}
