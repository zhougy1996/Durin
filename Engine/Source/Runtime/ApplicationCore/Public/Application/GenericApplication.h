#pragma once

#include "ApplicationCoreAPI.h"

namespace Durin
{
	class FGenericWindow;
	class FGenericApplicationMessageHandler;

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
}