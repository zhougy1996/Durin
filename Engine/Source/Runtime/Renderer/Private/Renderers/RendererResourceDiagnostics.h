#pragma once

#include "RenderResourceCreation.h"

namespace Durin
{
	auto MakeRendererResourceCreateError(
		ERenderResourceCreateErrorCategory Category,
		std::string Context, std::string Identity,
		ERenderResourceCreateErrorReason Reason,
		ERenderResourceGenerationDependency RetryDependencies,
		FRenderResourceCreateCause Cause = {}) -> FRenderResourceCreateError;

	inline auto MakeRendererResourceCreateError(
		ERenderResourceCreateErrorCategory Category,
		std::string Context, std::string Identity, FShaderError Cause,
		ERenderResourceGenerationDependency RetryDependencies) -> FRenderResourceCreateError
	{
		return MakeRendererResourceCreateError(Category, std::move(Context), std::move(Identity),
			ERenderResourceCreateErrorReason::ShaderFailure, RetryDependencies, std::move(Cause));
	}

	auto ReportRendererResourceCreateDiagnostic(
		const FRenderResourceCreateDiagnostic& Diagnostic) -> void;

	// A containing renderer slot may mirror an already-reported global-set
	// failure solely to preserve its own retry state. Suppress that wrapper
	// transition while continuing to report all locally owned failures.
	auto ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable(
		const FRenderResourceCreateDiagnostic& Diagnostic) -> void;
} // namespace Durin
