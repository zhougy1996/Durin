#include "CoreMinimal.h"
#include "MacOS/MacOSCustomTitleBarBridge.h"

#import <AppKit/AppKit.h>

#include <pthread.h>

namespace Durin
{
	struct FMacOSCustomTitleBarBridge
	{
		NSWindow* Window = nil;
		id EventMonitor = nil;
		FWindowTitleBarLayout Layout;
		NSWindowStyleMask PreviousStyleMask = NSWindowStyleMaskBorderless;
		NSWindowTitleVisibility PreviousTitleVisibility = NSWindowTitleVisible;
		bool bPreviousTitlebarAppearsTransparent = false;
		bool bPreviousMovableByWindowBackground = false;
		void* PreviousAppearance = nullptr;
		bool bAppearanceChanged = false;
		bool bActive = false;
	};

	namespace
	{
		constexpr CGFloat NativeControlPaddingX = 8.0;
		constexpr CGFloat NativeControlPaddingY = 4.0;

		auto IsMainThread() -> bool
		{
			return pthread_main_np() != 0;
		}

		auto MakeClientPoint(NSWindow* Window, NSEvent* Event) -> FIntPoint
		{
			NSView* ContentView = Window.contentView;
			const NSPoint ContentPoint = [ContentView convertPoint:Event.locationInWindow fromView:nil];
			return {
				static_cast<int32>(std::lround(ContentPoint.x)),
				static_cast<int32>(std::lround(NSHeight(ContentView.bounds) - ContentPoint.y))};
		}

		auto IsDragPoint(const FWindowTitleBarLayout& Layout, FIntPoint Point) -> bool
		{
			if (!Layout.bValid) return false;
			return std::ranges::any_of(Layout.DragRegions, [Point](const FWindowTitleBarRect& Region) {
				return Region.Contains(Point);
			});
		}

			auto RestoreWindowProperties(FMacOSCustomTitleBarBridge& Bridge) -> void
		{
			if (Bridge.Window == nil) return;
			if (Bridge.bAppearanceChanged)
			{
				Bridge.Window.appearance = Bridge.PreviousAppearance != nullptr
					? (__bridge NSAppearance*)Bridge.PreviousAppearance : nil;
				Bridge.bAppearanceChanged = false;
			}
			if (Bridge.PreviousAppearance != nullptr)
			{
				CFRelease(Bridge.PreviousAppearance);
				Bridge.PreviousAppearance = nullptr;
			}
			Bridge.Window.movableByWindowBackground = Bridge.bPreviousMovableByWindowBackground;
			Bridge.Window.titlebarAppearsTransparent = Bridge.bPreviousTitlebarAppearsTransparent;
			Bridge.Window.titleVisibility = Bridge.PreviousTitleVisibility;
			Bridge.Window.styleMask = Bridge.PreviousStyleMask;
		}
	}

	auto CreateMacOSCustomTitleBarBridge(void* NativeWindow) -> FMacOSCustomTitleBarBridge*
	{
		if (!IsMainThread() || NativeWindow == nullptr) return nullptr;

		@autoreleasepool
		{
			auto* Bridge = new FMacOSCustomTitleBarBridge();
			Bridge->Window = (__bridge NSWindow*)NativeWindow;
			Bridge->PreviousStyleMask = Bridge->Window.styleMask;
			Bridge->PreviousTitleVisibility = Bridge->Window.titleVisibility;
			Bridge->bPreviousTitlebarAppearsTransparent = Bridge->Window.titlebarAppearsTransparent;
			Bridge->bPreviousMovableByWindowBackground = Bridge->Window.movableByWindowBackground;
			if (Bridge->Window.appearance != nil)
			{
				Bridge->PreviousAppearance = const_cast<void*>(CFRetain(
					(__bridge CFTypeRef)Bridge->Window.appearance));
			}

			Bridge->Window.styleMask |= NSWindowStyleMaskFullSizeContentView;
			Bridge->Window.titleVisibility = NSWindowTitleHidden;
			Bridge->Window.titlebarAppearsTransparent = YES;
			Bridge->Window.movableByWindowBackground = NO;
			Bridge->bActive = true;

			Bridge->EventMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown
				handler:^NSEvent* (NSEvent* Event) {
					if (!Bridge->bActive || Event.window != Bridge->Window) return Event;
					if (!IsDragPoint(Bridge->Layout, MakeClientPoint(Bridge->Window, Event))) return Event;
					[Bridge->Window performWindowDragWithEvent:Event];
					return nil;
				}];
			if (Bridge->EventMonitor == nil)
			{
				Bridge->bActive = false;
				RestoreWindowProperties(*Bridge);
				delete Bridge;
				return nullptr;
			}
			return Bridge;
		}
	}

	auto DestroyMacOSCustomTitleBarBridge(FMacOSCustomTitleBarBridge*& Bridge) -> void
	{
		if (Bridge == nullptr) return;
		if (!IsMainThread())
		{
			DURIN_ERROR("The macOS custom title-bar bridge must be destroyed on the main thread.");
			return;
		}

		@autoreleasepool
		{
			Bridge->bActive = false;
			if (Bridge->EventMonitor != nil)
			{
				[NSEvent removeMonitor:Bridge->EventMonitor];
				Bridge->EventMonitor = nil;
			}
			RestoreWindowProperties(*Bridge);
			Bridge->Window = nil;
			delete Bridge;
			Bridge = nullptr;
		}
	}

	auto PublishMacOSCustomTitleBarLayout(
		FMacOSCustomTitleBarBridge* Bridge,
		const FWindowTitleBarLayout& Layout) -> void
	{
		if (Bridge == nullptr || !Bridge->bActive || !IsMainThread()) return;
		Bridge->Layout = Layout;
	}

	auto GetMacOSCustomTitleBarPlatformMetrics(
		const FMacOSCustomTitleBarBridge* Bridge) -> FWindowTitleBarPlatformMetrics
	{
		FWindowTitleBarPlatformMetrics Metrics;
		if (Bridge == nullptr || !Bridge->bActive || Bridge->Window == nil || !IsMainThread()) return Metrics;
		Metrics.bNativeWindowControls = true;

		@autoreleasepool
		{
			NSView* ContentView = Bridge->Window.contentView;
			NSRect ControlBounds = NSZeroRect;
			bool bHasVisibleControl = false;
			for (const NSWindowButton ButtonKind : {
				NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton})
			{
				NSButton* Button = [Bridge->Window standardWindowButton:ButtonKind];
				if (Button == nil || Button.hidden || Button.superview == nil) continue;
				const NSRect ClientRect = [ContentView convertRect:Button.bounds fromView:Button];
				ControlBounds = bHasVisibleControl ? NSUnionRect(ControlBounds, ClientRect) : ClientRect;
				bHasVisibleControl = true;
			}
			if (!bHasVisibleControl) return Metrics;

			ControlBounds = NSInsetRect(ControlBounds, -NativeControlPaddingX, -NativeControlPaddingY);
			const CGFloat ContentHeight = NSHeight(ContentView.bounds);
			Metrics.NativeControlExclusion = {
				static_cast<int32>(std::floor(std::max<CGFloat>(0.0, NSMinX(ControlBounds)))),
				static_cast<int32>(std::floor(std::max<CGFloat>(0.0, ContentHeight - NSMaxY(ControlBounds)))),
				static_cast<int32>(std::ceil(std::max<CGFloat>(0.0, NSMaxX(ControlBounds)))),
				static_cast<int32>(std::ceil(std::max<CGFloat>(0.0, ContentHeight - NSMinY(ControlBounds))))};
		}
		return Metrics;
	}

	auto SetMacOSCustomTitleBarDarkMode(
		FMacOSCustomTitleBarBridge* Bridge,
		bool bDarkMode) -> void
	{
		if (Bridge == nullptr || Bridge->Window == nil || !IsMainThread()) return;
		Bridge->bAppearanceChanged = true;
		Bridge->Window.appearance = [NSAppearance appearanceNamed:
			bDarkMode ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
	}
}
