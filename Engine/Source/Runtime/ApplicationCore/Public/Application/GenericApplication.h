#pragma once

#include "ApplicationCoreAPI.h"

namespace Durin
{
	class FGenericWindow;
	class FGenericApplicationMessageHandler;

	// Captures monitor geometry and DPI data in platform screen coordinates.
	struct FMonitorInfo
	{
		// Main bounds include system-reserved areas; work bounds exclude them.
		FIntPoint MainPosition = {0, 0};
		FIntPoint MainSize = {0, 0};
		FIntPoint WorkPosition = {0, 0};
		FIntPoint WorkSize = {0, 0};
		float DpiScale = 1.0f;
		// Non-owning platform monitor handle, valid only while the monitor exists.
		void* NativeHandle = nullptr;
	};

	APPLICATIONCORE_API auto EnumerateMonitors() -> std::vector<FMonitorInfo>;

	// Provides the platform-neutral application loop and window lookup boundary.
	class FGenericApplication
	{
	public:
		FGenericApplication() = default;
		virtual ~FGenericApplication() = default;

		APPLICATIONCORE_API virtual auto Tick() -> void;

		APPLICATIONCORE_API virtual auto ProcessDeferredEvents() -> void;

		APPLICATIONCORE_API virtual auto FindWindowByNativeWindowHandle(void* InNativeWindowHandle) -> std::shared_ptr<FGenericWindow>;

		auto GetMessageHandler() const -> FGenericApplicationMessageHandler* { return MessageHandler; }

		auto SetMessageHandler(FGenericApplicationMessageHandler* InMessageHandler) -> void { MessageHandler = InMessageHandler; }

		DURIN_NONCOPYABLE(FGenericApplication)

	protected:
		// Non-owning handler installed by the application layer.
		FGenericApplicationMessageHandler* MessageHandler = nullptr;
	};

	APPLICATIONCORE_API auto MakePlatformWindow() -> std::shared_ptr<FGenericWindow>;

} // namespace Durin
