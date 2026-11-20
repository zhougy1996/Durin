#pragma once

#include "CoreMinimal.h"

#include "RenderResourceCreation.h"

namespace Durin
{
	auto MakeRendererResourceCreateError(
		ERenderResourceCreateErrorCategory Category,
		std::string Context, std::string Identity,
		ERenderResourceCreateErrorReason Reason,
		ERenderResourceGenerationDependency RetryDependencies,
		std::string Cause = {}) -> FRenderResourceCreateError;

	inline auto MakeRendererResourceCreateError(
		ERenderResourceCreateErrorCategory Category,
		std::string Context, std::string Identity, std::string Cause,
		ERenderResourceGenerationDependency RetryDependencies) -> FRenderResourceCreateError
	{
		return MakeRendererResourceCreateError(Category, std::move(Context), std::move(Identity),
			ERenderResourceCreateErrorReason::ShaderFailure, RetryDependencies, std::move(Cause));
	}

	auto ReportRendererResourceCreateDiagnostic(
		const FRenderResourceCreateDiagnostic& Diagnostic) -> void;

} // namespace Durin
