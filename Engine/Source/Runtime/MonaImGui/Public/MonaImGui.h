#pragma once

#include "MonaImGuiAPI.h"
#include "Style/MonaImGuiStyle.h"
#include "Widgets/MonaImGuiBottomDrawer.h"
#include "Widgets/MonaImGuiPropertyTable.h"
#include "Widgets/MonaImGuiWidgets.h"
#include "Math/MathFwd.h"
#include "RHIResources.h"
// ReSharper disable once CppUnusedIncludeDirective
#include "ImGui/ImGuiCommon.h"

namespace Durin
{
	class MWindow;

	namespace MonaImGui
	{
		MONAIMGUI_API auto DrawTexture(const FRHITexture* Texture, const FVector2f& Size) -> void;
		MONAIMGUI_API auto BindMainViewportToWindow(const std::shared_ptr<MWindow>& Window) -> void;
		// Returns the medium-weight Latin UI font, or the default font when the
		// authored font asset could not be loaded.
		MONAIMGUI_API auto GetMediumUIFont() -> ImFont*;
	} // namespace MonaImGui
} // namespace Durin
