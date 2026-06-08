#include "MonaImGuiBackend.h"

#include "Application/MonaApplication.h"
#include "ImGuiRHIImpl.h"
#include "Misc/Paths.h"
#include "RHI.h"

namespace Durin::Mona
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
		static std::string GImGuiIniPath = FPaths::LaunchDir() + "imgui.ini";
		ImGui::GetIO().IniFilename = GImGuiIniPath.c_str();

		ImGuiRHIImpl_Init();   // sets RendererHasTextures before InitFonts->Build()
		ImGuiMonaImpl_Init();
	}

	auto FMonaImGuiBackend::Shutdown() -> void
	{
		ImGui::SetCurrentContext(GMonaImGuiContext);

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
				ImGuiRHIImpl_RenderMainViewport(MainViewport);
			}
		}

		if ((IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
		{
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
		}
	}

	auto FMonaImGuiBackend::RegisterTexture(const FTextureRHIRef& Texture) -> void
	{
		ImGuiRHIImpl_RegisterTexture(Texture);
	}

	auto FMonaImGuiBackend::UnregisterTexture(const FTextureRHIRef& Texture) -> void
	{
		ImGuiRHIImpl_UnregisterTexture(Texture);
	}

	auto FMonaImGuiBackend::DrawTexture(const FTextureRHIRef& Texture, const FVector2f& Size) -> bool
	{
		const ImTextureID TextureID = ImGuiRHIImpl_GetTextureID(Texture);
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
		ImGuiRHIImpl_EnsureMainViewportData(ImGui::GetMainViewport());
	}

	auto FMonaImGuiBackend::ShowDemoWindow() -> void
	{
		ImGui::SetCurrentContext(GMonaImGuiContext);
		ImGui::ShowDemoWindow();
	}
}
