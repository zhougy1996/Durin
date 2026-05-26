#pragma once

#include "ApplicationCoreAPI.h"

namespace Durin
{
	class FGenericWindow;
	class FGenericApplicationMessageHandler;

	struct FMonitorInfo
	{
		FIntPoint MainPosition = {0, 0};
		FIntPoint MainSize = {0, 0};
		FIntPoint WorkPosition = {0, 0};
		FIntPoint WorkSize = {0, 0};
		float DpiScale = 1.0f;
		void* NativeHandle = nullptr;
	};

	APPLICATIONCORE_API auto EnumerateMonitors() -> std::vector<FMonitorInfo>;

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
		FGenericApplicationMessageHandler* MessageHandler = nullptr;
	};

	APPLICATIONCORE_API auto MakePlatformWindow() -> std::shared_ptr<FGenericWindow>;

} // namespace Durin