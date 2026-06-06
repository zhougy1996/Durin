#pragma once

#include "MonaImGui.h"
#include "MonaUIBackend.h"

struct ImGuiContext;

namespace Durin::Mona
{
	extern ImGuiContext* GMonaImGuiContext;

	class FMonaImGuiBackend : public IMonaUIBackend
	{
	public:
		MONAIMGUI_API auto Initialize() -> void override;
		MONAIMGUI_API auto Shutdown() -> void override;
		MONAIMGUI_API auto NewFrame() -> void override;
		MONAIMGUI_API auto Render() -> void override;
		MONAIMGUI_API auto RenderWindowRefresh(void* NativeWindowHandle) -> bool override;
		MONAIMGUI_API auto DrawTexture(FRHITexture* Texture, const FVector2f& Size) -> bool override;

		MONAIMGUI_API auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void;
		MONAIMGUI_API auto ShowDemoWindow() -> void;
	};
}
