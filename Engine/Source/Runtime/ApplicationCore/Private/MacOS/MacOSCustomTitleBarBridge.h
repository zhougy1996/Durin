#pragma once

#include "Window/GenericWindow.h"

namespace Durin
{
	struct FMacOSCustomTitleBarBridge;

	enum class EMacOSCustomTitleBarMouseDownAction : uint8
	{
		PassThrough,
		Drag,
		Zoom
	};

	inline auto ResolveMacOSCustomTitleBarMouseDown(
		const FWindowTitleBarLayout& Layout,
		FIntPoint Point,
		uint32 ClickCount) -> EMacOSCustomTitleBarMouseDownAction
	{
		if (!Layout.bValid) return EMacOSCustomTitleBarMouseDownAction::PassThrough;
		for (const FWindowTitleBarRect& Region : Layout.DragRegions)
		{
			if (!Region.Contains(Point)) continue;
			return ClickCount == 2
				? EMacOSCustomTitleBarMouseDownAction::Zoom
				: EMacOSCustomTitleBarMouseDownAction::Drag;
		}
		return EMacOSCustomTitleBarMouseDownAction::PassThrough;
	}

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
