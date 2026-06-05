#pragma once

#include "MonaImGuiAPI.h"
#include "MonaUIInterface.h"

struct ImGuiContext;

namespace Durin
{
	class MWindow;
}

namespace Durin::Mona
{
	extern MONAIMGUI_API ImGuiContext* GMonaImGuiContext;

	class FMonaImGui : public IMonaUIInterface
	{
	public:
		MONAIMGUI_API auto Initialize() -> void override;
		MONAIMGUI_API auto Shutdown() -> void override;
		MONAIMGUI_API auto NewFrame() -> void override;
		MONAIMGUI_API auto Render() -> void override;

		MONAIMGUI_API auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void;
		MONAIMGUI_API auto ShowDemoWindow() -> void;
		MONAIMGUI_API auto DrawTexture(FRHITexture* Texture, const FVector2f& Size) -> bool override;
	};
}
