#pragma once

#include <format>
#include <source_location>
#include <string_view>
#include <utility>

#if defined(_WIN32)
	#if defined(CORE_EXPORTS)
		#define DURIN_ASSERTION_API __declspec(dllexport)
	#else
		#define DURIN_ASSERTION_API __declspec(dllimport)
	#endif
#else
	#define DURIN_ASSERTION_API __attribute__((visibility("default")))
#endif

namespace Durin::Private
{
	// Reports an assertion without relying on module logging and then terminates.
	[[noreturn]] DURIN_ASSERTION_API auto ReportAssertionFailure(
		std::string_view Expression,
		std::source_location Location,
		std::string_view Context = {}) noexcept -> void;

	template<typename... Args>
	[[noreturn]] auto ReportFormattedAssertionFailure(
		std::string_view Expression,
		std::source_location Location,
		std::format_string<Args...> Format,
		Args&&... Arguments) noexcept -> void
	{
		try
		{
			ReportAssertionFailure(
				Expression,
				Location,
				std::format(Format, std::forward<Args>(Arguments)...));
		}
		catch (...)
		{
			ReportAssertionFailure(
				Expression,
				Location,
				"Assertion context formatting failed.");
		}
	}
} // namespace Durin::Private

#undef DURIN_ASSERTION_API
