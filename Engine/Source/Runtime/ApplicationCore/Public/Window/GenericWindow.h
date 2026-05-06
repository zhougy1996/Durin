#pragma once

#include "ApplicationCoreAPI.h"
#include "Window/GenericWindowDefinition.h"

namespace Doge
{
	class FGenericApplication;

	enum class EWindowMode : uint32
	{
		Fullscreen,			// Fullscreen with a window border
		WindowedFullScreen, // Fullscreen without a window border
		Windowed,			// Stretch the window to the size of the monitor
	};

	// Enumeration specifying the type of mouse cursor to display.
	enum class EMouseCursor : uint8
	{
		None = 0,
		Arrow,
		TextInput,
		ResizeAll,
		ResizeNS,	// top to bottom.
		ResizeEW,	// left to right.
		ResizeNESW, // top-right to the bottom-left.
		ResizeNWSE, // top-left to the bottom-right.
		Hand,
		NotAllowed
	};

	class FGenericWindow
	{
	public:
		APPLICATIONCORE_API FGenericWindow();

		APPLICATIONCORE_API virtual ~FGenericWindow();

		APPLICATIONCORE_API virtual auto Initialize(const std::shared_ptr<FGenericWindowDefinition>& InDefinition) -> void;

		APPLICATIONCORE_API virtual auto PollEvents() const -> void;

		APPLICATIONCORE_API virtual auto ReshapeWindow(int32 X, int32 Y, int32 Width, int32 Height) -> void;

		APPLICATIONCORE_API virtual auto MoveWindowTo(int32 X, int32 Y) -> void;

		APPLICATIONCORE_API virtual auto GetWindowMode() const -> EWindowMode;

		APPLICATIONCORE_API virtual auto SetWindowMode(EWindowMode WindowMode) -> void;

		APPLICATIONCORE_API virtual auto GetOSNativeWindowHandle() const -> void*;

		APPLICATIONCORE_API virtual auto ShouldClose() const -> bool;

		APPLICATIONCORE_API virtual auto Close() -> void;

		APPLICATIONCORE_API virtual auto GetViewportSize() const -> FIntPoint;

		APPLICATIONCORE_API virtual auto CreateVulkanSurface(void* InVulkanInstance) const -> void*;

		APPLICATIONCORE_API virtual auto IsMinimized() const -> bool;

		APPLICATIONCORE_API virtual auto SetCursor(EMouseCursor Cursor) -> void;

	protected:
		std::shared_ptr<FGenericWindowDefinition> Definition;

		void* OSNativeWindowHandle = nullptr;
	};
}