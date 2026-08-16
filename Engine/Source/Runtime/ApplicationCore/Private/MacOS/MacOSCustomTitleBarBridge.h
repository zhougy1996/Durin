#pragma once

#include "Window/GenericWindow.h"

namespace Durin
{
	struct FMacOSCustomTitleBarBridge;

	// Installs AppKit custom-title-bar composition without replacing GLFW's window delegate.
	auto CreateMacOSCustomTitleBarBridge(void* NativeWindow) -> FMacOSCustomTitleBarBridge*;

	auto DestroyMacOSCustomTitleBarBridge(FMacOSCustomTitleBarBridge*& Bridge) -> void;

	auto PublishMacOSCustomTitleBarLayout(
		FMacOSCustomTitleBarBridge* Bridge,
		const FWindowTitleBarLayout& Layout) -> void;

	auto GetMacOSCustomTitleBarPlatformMetrics(
		const FMacOSCustomTitleBarBridge* Bridge) -> FWindowTitleBarPlatformMetrics;

	auto SetMacOSCustomTitleBarDarkMode(
		FMacOSCustomTitleBarBridge* Bridge,
		bool bDarkMode) -> void;
}
