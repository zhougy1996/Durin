#pragma once

#include "RenderResourceCreation.h"

namespace Durin
{
	auto MakeRendererResourceCreateError(
		ERenderResourceCreateErrorCategory Category,
		std::string Context,
		std::string Identity,
		std::string Message,
		ERenderResourceGenerationDependency RetryDependencies)
		-> FRenderResourceCreateError;

	auto ReportRendererResourceCreateDiagnostic(
		const FRenderResourceCreateDiagnostic& Diagnostic) -> void;

	// A containing renderer slot may mirror an already-reported global-set
	// failure solely to preserve its own retry state. Suppress that wrapper
	// transition while continuing to report all locally owned failures.
	auto ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable(
		const FRenderResourceCreateDiagnostic& Diagnostic) -> void;
} // namespace Durin
