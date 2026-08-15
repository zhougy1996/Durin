#pragma once

#include "ApplicationCoreAPI.h"
#include "Window/GenericWindowDefinition.h"
#include "Input/InputCoreTypes.h"

namespace Durin
{
	class FGenericApplication;

	// Identifies the native non-client role of a point in the custom title bar.
	enum class EWindowTitleBarHitTest : uint8
	{
		Client,
		Caption,
		Minimize,
		Maximize,
		Close,
		ResizeLeft,
		ResizeRight,
		ResizeTop,
		ResizeBottom,
		ResizeTopLeft,
		ResizeTopRight,
		ResizeBottomLeft,
		ResizeBottomRight
	};

	// Defines an axis-aligned rectangle in window-client pixels.
	struct FWindowTitleBarRect
	{
		int32 MinX = 0;
		int32 MinY = 0;
		int32 MaxX = 0;
		int32 MaxY = 0;

		auto Contains(FIntPoint Point) const -> bool
		{
			return Point.x >= MinX && Point.x < MaxX && Point.y >= MinY && Point.y < MaxY;
		}
	};

	// Publishes one complete rendered title-bar layout for native hit testing.
	struct FWindowTitleBarLayout
	{
		uint64 Generation = 0;
		bool bValid = false;
		int32 Height = 0;
		int32 MinimumWindowWidth = 0;
		std::vector<FWindowTitleBarRect> DragRegions;
		FWindowTitleBarRect MinimizeRegion;
		FWindowTitleBarRect MaximizeRegion;
		FWindowTitleBarRect CloseRegion;
	};

	// Describes native caption interaction state consumed by the renderer.
	struct FWindowTitleBarInteractionState
	{
		EWindowTitleBarHitTest HoveredPart = EWindowTitleBarHitTest::Client;
		EWindowTitleBarHitTest PressedPart = EWindowTitleBarHitTest::Client;
		bool bFocused = false;
		bool bMaximized = false;
	};

	// Classifies a client point using resize-first custom-title-bar semantics.
	APPLICATIONCORE_API auto HitTestWindowTitleBar(
		const FWindowTitleBarLayout& Layout,
		FIntPoint Point,
		FIntPoint ClientSize,
		int32 ResizeBorderX,
		int32 ResizeBorderY,
		bool bAllowResize) -> EWindowTitleBarHitTest;

	// Selects how a platform window occupies its monitor.
	enum class EWindowMode : uint8
	{
		Fullscreen,			// Fullscreen with a window border
		WindowedFullScreen, // Fullscreen without a window border
		Windowed,			// Stretch the window to the size of the monitor
	};

	// Selects cursor presentation and native pointer ownership independently of cursor shape.
	enum class ECursorMode : uint8
	{
		Free,
		Hidden,
		Captured
	};

	// Defines the platform-independent window contract used by runtime and UI code.
	class FGenericWindow
	{
	public:
		APPLICATIONCORE_API FGenericWindow();

		APPLICATIONCORE_API virtual ~FGenericWindow();

		APPLICATIONCORE_API virtual auto Initialize(const std::shared_ptr<FGenericWindowDefinition>& InDefinition) -> void;

		APPLICATIONCORE_API virtual auto PollEvents() const -> void;

		APPLICATIONCORE_API virtual auto WaitEventsTimeout(double TimeoutSeconds) const -> void;

		APPLICATIONCORE_API virtual auto ReshapeWindow(int32 X, int32 Y, int32 Width, int32 Height) -> void;

		APPLICATIONCORE_API virtual auto ResizeWindow(int32 Width, int32 Height) -> void;

		APPLICATIONCORE_API virtual auto MoveWindowTo(int32 X, int32 Y) -> void;

		APPLICATIONCORE_API virtual auto GetWindowPosition() const -> FIntPoint;

		APPLICATIONCORE_API virtual auto GetWindowSize() const -> FIntPoint;

		APPLICATIONCORE_API virtual auto GetWindowMode() const -> EWindowMode;

		APPLICATIONCORE_API virtual auto SetWindowMode(EWindowMode WindowMode) -> void;

		APPLICATIONCORE_API virtual auto GetOSNativeWindowHandle() const -> void*;

		APPLICATIONCORE_API virtual auto ShouldClose() const -> bool;

		APPLICATIONCORE_API virtual auto SetShouldClose(bool bShouldClose) -> void;

		APPLICATIONCORE_API virtual auto Close() -> void;

		APPLICATIONCORE_API virtual auto Show() -> void;

		APPLICATIONCORE_API virtual auto Hide() -> void;

		APPLICATIONCORE_API virtual auto IsVisible() const -> bool;

		APPLICATIONCORE_API virtual auto Focus() -> void;

		APPLICATIONCORE_API virtual auto GetViewportSize() const -> FIntPoint;

		APPLICATIONCORE_API virtual auto CreateVulkanSurface(void* InVulkanInstance) const -> void*;

		APPLICATIONCORE_API virtual auto IsMinimized() const -> bool;

		APPLICATIONCORE_API virtual auto IsMaximized() const -> bool;

		APPLICATIONCORE_API virtual auto MaximizeWindow() -> void;

		APPLICATIONCORE_API virtual auto RestoreWindow() -> void;

		APPLICATIONCORE_API virtual auto MinimizeWindow() -> void;

		APPLICATIONCORE_API virtual auto IsFocused() const -> bool;

		APPLICATIONCORE_API virtual auto SetTitle(const std::string& InTitle) -> void;

		APPLICATIONCORE_API virtual auto SetOpacity(float InOpacity) -> void;

		APPLICATIONCORE_API virtual auto SetWindowDecorationMode(EWindowDecorationMode Mode) -> void;

		APPLICATIONCORE_API virtual auto GetWindowDecorationMode() const -> EWindowDecorationMode;

		APPLICATIONCORE_API virtual auto GetEffectiveWindowDecorationMode() const -> EWindowDecorationMode;

		APPLICATIONCORE_API virtual auto PublishTitleBarLayout(const FWindowTitleBarLayout& Layout) -> void;

		APPLICATIONCORE_API virtual auto GetTitleBarInteractionState() const -> FWindowTitleBarInteractionState;

		APPLICATIONCORE_API virtual auto SetTitleBarDarkMode(bool bDarkMode) -> void;

		APPLICATIONCORE_API virtual auto SetMousePassthrough(bool bPassthrough) -> void;

		APPLICATIONCORE_API virtual auto SetFocusOnShow(bool bFocusOnShow) -> void;

		APPLICATIONCORE_API virtual auto GetDpiScale() const -> float;

		APPLICATIONCORE_API virtual auto SetCursor(EMouseCursor Cursor) -> void;

		APPLICATIONCORE_API virtual auto SetCursorMode(ECursorMode InCursorMode) -> void;

		auto GetCursorMode() const -> ECursorMode { return CursorMode; }

		APPLICATIONCORE_API virtual auto GetCursorPosition() const -> FVector2d;

		APPLICATIONCORE_API virtual auto SetCursorPosition(FVector2d Position) -> void;

		APPLICATIONCORE_API virtual auto IsHovered() const -> bool;

	protected:
		APPLICATIONCORE_API virtual auto ApplyCursorMode(ECursorMode InCursorMode) -> void;

		// Retained so platform implementations can reapply the requested window policy.
		std::shared_ptr<FGenericWindowDefinition> Definition;

		EWindowMode WindowMode = EWindowMode::Windowed;
		ECursorMode CursorMode = ECursorMode::Free;
		EMouseCursor CursorShape = EMouseCursor::Arrow;
		FVector2d CursorPositionBeforeCapture{0.0};
		bool bHasCursorPositionBeforeCapture = false;
		EWindowDecorationMode EffectiveDecorationMode = EWindowDecorationMode::System;

		// Non-owning handle supplied by the platform windowing implementation.
		void* OSNativeWindowHandle = nullptr;
	};
}
