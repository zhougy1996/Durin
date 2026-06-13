#include "MonaImGuiBackend.h"

#include "Application/MonaApplication.h"
#include "ImGuiRHIImpl.h"
#include "MonaCoreGlobals.h"
#include "Misc/Paths.h"
#include "RHI.h"

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
		return ImGuiRHIImpl_GetTextureID(const_cast<FRHITexture*>(InTexture)) != ImTextureID_Invalid;
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

	auto FMonaImGuiBackend::ShowDemoWindow() -> void
	{
		ImGui::SetCurrentContext(GMonaImGuiContext);
		ImGui::ShowDemoWindow();
	}
}
