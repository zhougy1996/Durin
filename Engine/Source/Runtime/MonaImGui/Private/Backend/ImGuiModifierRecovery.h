#pragma once

#include "CoreMinimal.h"

#include "Input/InputCoreTypes.h"
#include "ThirdParty/ImGui/imgui.h"

namespace Durin::MonaImGui
{
	// Recover releases swallowed by system overlays without synthesizing presses
	// for shortcuts started outside the editor. Flags use physical OS key meanings.
	inline auto RecoverReleasedModifiers(ImGuiIO& IO, EKeyModFlags PhysicalMods) -> void
	{
		auto Release = [&](EKeyModFlags Flag, ImGuiKey Mod, ImGuiKey Left, ImGuiKey Right) {
			if (EnumHasAnyFlags(PhysicalMods, Flag)) return;
			IO.AddKeyEvent(Left, false);
			IO.AddKeyEvent(Right, false);
			IO.AddKeyEvent(Mod, false);
		};
		Release(EKeyModFlags::Shift, ImGuiMod_Shift, ImGuiKey_LeftShift, ImGuiKey_RightShift);
		Release(EKeyModFlags::Control, ImGuiMod_Ctrl, ImGuiKey_LeftCtrl, ImGuiKey_RightCtrl);
		Release(EKeyModFlags::Alt, ImGuiMod_Alt, ImGuiKey_LeftAlt, ImGuiKey_RightAlt);
		Release(EKeyModFlags::Super, ImGuiMod_Super, ImGuiKey_LeftSuper, ImGuiKey_RightSuper);
	}

#if defined(__APPLE__)
	auto GetMacOSPhysicalModifiers() -> EKeyModFlags;
#endif
}
