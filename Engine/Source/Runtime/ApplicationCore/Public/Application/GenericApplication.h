#pragma once

namespace Doge
{
	class FGenericWindow;

	class APPLICATIONCORE_API FGenericApplication
	{
	public:
		virtual ~FGenericApplication() = default;

		virtual auto Tick() -> void;

		virtual auto ProcessDeferredEvents() -> void;

		virtual auto FindWindowByNativeWindowHandle(void* InNativeWindowHandle) -> TSharedPtr<FGenericWindow>;
	};
}