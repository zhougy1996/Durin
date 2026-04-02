#pragma once

namespace Doge
{
	class FGenericWindow;

	class FGenericApplicationMessageHandler
	{
	public:
		virtual ~FGenericApplicationMessageHandler() = default;

		APPLICATIONCORE_API virtual auto OnWindowResize(const std::shared_ptr<FGenericWindow>& InPlatformWindow, int32 InWidth, int32 InHeight, bool bInWasMinimized) -> void;
	};
}