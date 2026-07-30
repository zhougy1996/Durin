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
} // namespace Durin
