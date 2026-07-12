#pragma once

#include "MonaImGuiAPI.h"
#include "Math/MathFwd.h"
#include "RHIResources.h"
// ReSharper disable once CppUnusedIncludeDirective
#include "ThirdParty/ImGui/ImGuiCommon.h"

namespace Durin
{
	class MWindow;

	namespace MonaImGui
	{
		enum class EColorTheme : uint8
		{
			Dark,
			Light,
		};

		MONAIMGUI_API auto DrawTexture(const FRHITexture* Texture, const FVector2f& Size) -> void;
		MONAIMGUI_API auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void;
		MONAIMGUI_API auto SetGlobalUIScale(float Scale) -> void;
		MONAIMGUI_API auto SetColorTheme(EColorTheme Theme) -> void;
		MONAIMGUI_API auto GetColorTheme() -> EColorTheme;
	} // namespace MonaImGui
} // namespace Durin
