#pragma once

#include "CoreMinimal.h"

namespace Durin::Testing
{
	// Fixed-format fixtures select their backend independently of the host default.
	class FScopedRHIBackendOverride final
	{
	public:
		explicit FScopedRHIBackendOverride(const char* Backend)
		{
			if (const char* Value = std::getenv("DURIN_RHI_BACKEND")) Previous = Value;
			Set(Backend);
		}
		~FScopedRHIBackendOverride() { Set(Previous ? Previous->c_str() : nullptr); }
		FScopedRHIBackendOverride(const FScopedRHIBackendOverride&) = delete;
		auto operator=(const FScopedRHIBackendOverride&) -> FScopedRHIBackendOverride& = delete;
	private:
		static auto Set(const char* Backend) -> void
		{
#if defined(_WIN32)
			_putenv_s("DURIN_RHI_BACKEND", Backend ? Backend : "");
#else
			if (Backend) setenv("DURIN_RHI_BACKEND", Backend, 1);
			else unsetenv("DURIN_RHI_BACKEND");
#endif
		}
		std::optional<std::string> Previous;
	};
}
