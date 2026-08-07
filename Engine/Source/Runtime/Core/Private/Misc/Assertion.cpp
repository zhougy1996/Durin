#include "Misc/Assertion.h"

#include <cstdio>
#include <cstdlib>

#include "HAL/Platform.h"

namespace Durin::Private
{
	[[noreturn]] auto ReportAssertionFailure(
		std::string_view Expression,
		std::source_location Location,
		std::string_view Context) noexcept -> void
	{
		std::fprintf(
			stderr,
			"Assertion failed: %.*s\nSource: %s:%u\nFunction: %s\n",
			static_cast<int>(Expression.size()),
			Expression.data(),
			Location.file_name(),
			Location.line(),
			Location.function_name());
		if (!Context.empty())
		{
			std::fprintf(
				stderr,
				"Context: %.*s\n",
				static_cast<int>(Context.size()),
				Context.data());
		}
		std::fflush(stderr);

		PLATFORM_BREAK();
		std::abort();
	}
} // namespace Durin::Private
