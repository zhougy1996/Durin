#pragma once

#include "ApplicationCoreAPI.h"

namespace Doge
{
	class FGenericWindow;
	class FGenericApplicationMessageHandler;

	class FGenericApplication
	{
	public:
		virtual ~FGenericApplication() = default;

		APPLICATIONCORE_API virtual auto Tick() -> void;

		APPLICATIONCORE_API virtual auto ProcessDeferredEvents() -> void;

		APPLICATIONCORE_API virtual auto FindWindowByNativeWindowHandle(void* InNativeWindowHandle) -> std::shared_ptr<FGenericWindow>;

		auto GetMessageHandler() const -> FGenericApplicationMessageHandler* { return MessageHandler; }

		auto SetMessageHandler(FGenericApplicationMessageHandler* InMessageHandler) -> void { MessageHandler = InMessageHandler; }

	protected:
		FGenericApplicationMessageHandler* MessageHandler = nullptr;
	};
}