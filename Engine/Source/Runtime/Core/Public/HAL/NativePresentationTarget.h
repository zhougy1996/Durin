#pragma once

#include "CoreAPI.h"

namespace Durin
{
	// Immutable platform presentation metadata. The platform owns the native window.
	// Retaining this object does not extend native-window lifetime.
	class CORE_API FNativePresentationTarget
	{
	public:
		explicit FNativePresentationTarget(void* InWindow);
		virtual ~FNativePresentationTarget();
		auto GetNativeWindowHandle() const -> void* { return Window; }
	private:
		void* const Window;
	};
}
