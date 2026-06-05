#pragma once

#include "MonaImGuiBackendAPI.h"
#include "MonaUIBackend.h"
#include "RHIResources.h"
#include "ThirdParty/ImGui/imgui.h"

struct ImGuiContext;

namespace Durin::Mona
{
	class MWindow;
	extern MONAIMGUIBACKEND_API ImGuiContext* GMonaImGuiContext;

	class FMonaImGuiBackend : public IMonaUIBackend
	{
	public:
		MONAIMGUIBACKEND_API auto Initialize() -> void override;
		MONAIMGUIBACKEND_API auto Shutdown() -> void override;
		MONAIMGUIBACKEND_API auto NewFrame() -> void override;
		MONAIMGUIBACKEND_API auto Render() -> void override;

		MONAIMGUIBACKEND_API auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void;
		MONAIMGUIBACKEND_API auto ShowDemoWindow() -> void;
		MONAIMGUIBACKEND_API auto GetTextureID(const FTextureRHIRef& Texture) const -> ImTextureID;
	};
}
