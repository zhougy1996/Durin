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
		MONAIMGUI_API auto DrawTexture(const FTextureRHIRef& Texture, const FVector2f& Size) -> bool;
		MONAIMGUI_API auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void;
		MONAIMGUI_API auto ShowDemoWindow() -> void;
	} // namespace MonaImGui
} // namespace Durin
