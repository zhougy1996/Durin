#pragma once

#include "ImGuiMonaImpl.h"
#include "MonaImGui.h"
#include "MonaUIBackend.h"

namespace Durin::MonaImGui
{
	// Connects the Mona UI backend contract to ImGui platform and RHI adapters.
	class FMonaImGuiBackend : public Mona::IMonaUIBackend
	{
	public:
		MONAIMGUI_API auto Initialize() -> void override;
		MONAIMGUI_API auto Shutdown() -> void override;
		MONAIMGUI_API auto NewFrame() -> void override;
		MONAIMGUI_API auto Render() -> void override;
		MONAIMGUI_API auto RegisterTexture(const FTextureRHIRef& Texture) -> void override;
		MONAIMGUI_API auto UnregisterTexture(const FTextureRHIRef& Texture) -> void override;
		MONAIMGUI_API auto IsTextureRegistered(const FRHITexture* InTexture) -> bool override;
		MONAIMGUI_API auto DrawImage(const FRHITexture* InTexture, const FVector2f& Size) -> bool override;

		MONAIMGUI_API static auto Get() -> FMonaImGuiBackend&;

		MONAIMGUI_API auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void;
	};
}
