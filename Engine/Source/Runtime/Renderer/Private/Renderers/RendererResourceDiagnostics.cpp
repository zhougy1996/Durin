#include "Renderers/RendererResourceDiagnostics.h"

#include "CoreGlobals.h"

namespace Durin
{
	auto MakeRendererResourceCreateError(
		ERenderResourceCreateErrorCategory Category,
		std::string Context,
		std::string Identity,
		std::string Message,
		ERenderResourceGenerationDependency RetryDependencies)
		-> FRenderResourceCreateError
	{
		return {
			.Category = Category,
			.Context = std::move(Context),
			.Identity = std::move(Identity),
			.Message = std::move(Message),
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
			Error.Message);
	}

	auto ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable(
		const FRenderResourceCreateDiagnostic& Diagnostic) -> void
	{
		if (Diagnostic.Error
			&& Diagnostic.Error->Message == "Global shader set is unavailable.")
		{
			return;
		}
		ReportRendererResourceCreateDiagnostic(Diagnostic);
	}
} // namespace Durin
