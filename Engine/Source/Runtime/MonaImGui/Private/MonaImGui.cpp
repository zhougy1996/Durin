#include "MonaImGui.h"

#include "MonaImGuiBackend.h"

namespace Durin::MonaImGui
{
	auto DrawTexture(const FRHITexture* Texture, const FVector2f& Size) -> void
	{
		ImGui::Image(reinterpret_cast<ImTextureID>(Texture), {Size.x, Size.y});
	}

	auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void
	{
		FMonaImGuiBackend::Get().BindMainViewportToWindow(Window);
	}

	auto SetGlobalUIScale(float Scale) -> void
	{
		Scale = std::clamp(Scale, 0.75f, 2.0f);
		static const ImGuiStyle BaseStyle = ImGui::GetStyle();
		ImGuiStyle& Style = ImGui::GetStyle();
		Style = BaseStyle;
		Style.ScaleAllSizes(Scale);
		Style.FontScaleMain = Scale;
	}
}
