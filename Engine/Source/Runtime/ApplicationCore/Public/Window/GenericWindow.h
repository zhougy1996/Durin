#pragma once

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

	class APPLICATIONCORE_API FGenericWindow
	{
	public:
		FGenericWindow();

		virtual ~FGenericWindow();

		virtual auto Initialize(FGenericApplication* InApplication, const TSharedPtr<FGenericWindowDefinition>& InDefinition) -> void;

		virtual auto PollEvents() const -> void;

		virtual auto ReshapeWindow(int32 X, int32 Y, int32 Width, int32 Height) -> void;

		virtual auto MoveWindowTo(int32 X, int32 Y) -> void;

		virtual auto GetWindowMode() const -> EWindowMode;

		virtual auto SetWindowMode(EWindowMode WindowMode) -> void;

		virtual auto GetOSNativeWindowHandle() const -> void*;

		virtual auto ShouldClose() const -> bool;

		virtual auto Close() -> void;

		virtual auto GetViewportSize() const -> FIntPoint;

		virtual auto CreateVulkanSurface(void* InVulkanInstance) const -> void* { return nullptr; }

	protected:
		TSharedPtr<FGenericWindowDefinition> Definition_;

		void* OSNativeWindowHandle = nullptr;
	};
}