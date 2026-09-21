#include "MonaImGui.h"

#include "Backend/MonaImGuiBackend.h"
#include "Widgets/MWindow.h"

namespace Durin::MonaImGui
{
	auto DrawTexture(const FRHITexture* Texture, const FVector2f& Size) -> void
	{
		ImGui::Image(reinterpret_cast<ImTextureID>(Texture), {Size.x, Size.y});
	}

	auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void
	{
		Window->SetTitleBarDarkMode(GetColorTheme() == EColorTheme::Dark);
		FMonaImGuiBackend::Get().BindMainViewportToWindow(Window);
	}

} // namespace Durin::MonaImGui
