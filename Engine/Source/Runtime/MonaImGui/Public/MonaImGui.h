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
		MONAIMGUI_API auto DrawTexture(const FRHITexture* Texture, const FVector2f& Size) -> void;
		MONAIMGUI_API auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void;
		MONAIMGUI_API auto SetGlobalUIScale(float Scale) -> void;
	} // namespace MonaImGui
} // namespace Durin
