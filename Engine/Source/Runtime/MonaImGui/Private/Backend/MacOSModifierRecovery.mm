#include "Backend/ImGuiModifierRecovery.h"

#import <AppKit/AppKit.h>

namespace Durin::MonaImGui
{
	auto GetMacOSPhysicalModifiers() -> EKeyModFlags
	{
		const NSEventModifierFlags Flags = [NSEvent modifierFlags];
		EKeyModFlags Mods = EKeyModFlags::None;
		if (Flags & NSEventModifierFlagShift) Mods = Mods | EKeyModFlags::Shift;
		if (Flags & NSEventModifierFlagControl) Mods = Mods | EKeyModFlags::Control;
		if (Flags & NSEventModifierFlagOption) Mods = Mods | EKeyModFlags::Alt;
		if (Flags & NSEventModifierFlagCommand) Mods = Mods | EKeyModFlags::Super;
		return Mods;
	}
}
