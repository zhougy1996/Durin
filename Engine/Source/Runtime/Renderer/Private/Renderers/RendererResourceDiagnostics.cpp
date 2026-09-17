#include "Renderers/RendererResourceDiagnostics.h"

#include "CoreGlobals.h"

namespace Durin
{
	auto MakeRendererResourceCreateError(
		ERenderResourceCreateErrorCategory Category,
		std::string Context,
		std::string Identity,
		ERenderResourceCreateErrorReason Reason,
		ERenderResourceGenerationDependency RetryDependencies,
		FRenderResourceCreateCause Cause)
		-> FRenderResourceCreateError
	{
		if (std::holds_alternative<std::monostate>(Cause)
			&& (Reason == ERenderResourceCreateErrorReason::ShaderCreationFailed
				|| Reason == ERenderResourceCreateErrorReason::ResourceCreationFailed
				|| Reason == ERenderResourceCreateErrorReason::PipelineCreationFailed
				|| Reason == ERenderResourceCreateErrorReason::SamplerCreationFailed))
			Cause = FRHICreationError{.Failure = ERHIResourceCreationFailure::Unknown,
				.Source = ERHICreationFailureSource::BackendReturnedNull};
		return {
			.Category = Category,
			.Reason = Reason,
			.Context = std::move(Context),
			.Identity = std::move(Identity),
			.Cause = std::move(Cause),
			.RetryDependencies = RetryDependencies,
		};
	}

	auto ReportRendererResourceCreateDiagnostic(
		const FRenderResourceCreateDiagnostic& Diagnostic) -> void
	{
		if (!Diagnostic.Error)
		{
			return;
		}

		const FRenderResourceCreateError& Error = *Diagnostic.Error;
		if (Diagnostic.Kind == ERenderResourceCreateDiagnosticKind::Recovery)
		{
			DURIN_INFO(
				"Recovered renderer resource: context={}, identity={}",
				Error.Context,
				Error.Identity);
			return;
		}

		DURIN_ERROR(
			"Renderer resource creation failed: category={}, context={}, "
			"identity={}, generation={}/{}/{}, retained={}, message={}",
			static_cast<uint8>(Error.Category),
			Error.Context,
			Error.Identity,
			Error.AttemptedGeneration.Shader,
			Error.AttemptedGeneration.Device,
			Error.AttemptedGeneration.Manual,
			Error.bRetainedFallback,
			FormatRenderResourceCreateError(Error));
	}

	auto ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable(
		const FRenderResourceCreateDiagnostic& Diagnostic) -> void
	{
		if (Diagnostic.Error
			&& Diagnostic.Error->Reason
				== ERenderResourceCreateErrorReason::GlobalShaderUnavailable)
		{
			return;
		}
		ReportRendererResourceCreateDiagnostic(Diagnostic);
	}
} // namespace Durin
