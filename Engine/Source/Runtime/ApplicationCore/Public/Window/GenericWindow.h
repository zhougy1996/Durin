#pragma once

#include "ApplicationCoreAPI.h"
#include "Window/GenericWindowDefinition.h"
#include "Input/InputCoreTypes.h"

namespace Durin
{
	class FGenericApplication;

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

		APPLICATIONCORE_API virtual auto IsFocused() const -> bool;

		APPLICATIONCORE_API virtual auto SetTitle(const std::string& InTitle) -> void;

		APPLICATIONCORE_API virtual auto SetOpacity(float InOpacity) -> void;

		APPLICATIONCORE_API virtual auto SetWindowDecorated(bool bDecorated) -> void;

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

		// Non-owning handle supplied by the platform windowing implementation.
		void* OSNativeWindowHandle = nullptr;
	};
}
