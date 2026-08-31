#pragma once

#include "CoreFwd.h"

namespace Durin
{
	enum class EDiagnosticSeverity : uint8
	{
		Info,
		Warning,
		Error
	};

	// Domain-neutral, owning diagnostic suitable for logs, tools, and UI.
	// Domain-specific result enums remain authoritative for programmatic control flow.
	struct FDiagnostic
	{
		std::string Domain;
		std::string Code;
		EDiagnosticSeverity Severity = EDiagnosticSeverity::Error;
		std::string Message;
		std::string Context;

		auto IsError() const -> bool { return Severity == EDiagnosticSeverity::Error; }
		auto operator==(const FDiagnostic&) const -> bool = default;
	};
}
