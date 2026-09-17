#include "Renderers/MaterialBindingResolution.h"

#include "CoreGlobals.h"

namespace Durin::RendererPrivate
{
	auto ResolvePreparedMaterialBinding(
		const FMaterialRenderData& Material,
		FMaterialRenderBinding& OutBinding,
		std::string_view DiagnosticResource
	) -> bool
	{
		FMaterialRenderValidationDiagnostic Diagnostic;
		if (TryGetMaterialRenderBinding(
				Material.Representation, OutBinding, Diagnostic))
			return true;

		DURIN_ERROR("Renderer material binding failed: context={}, identity=prepared-material, message={}",
			DiagnosticResource, FormatMaterialError(Diagnostic.Error));
		return false;
	}

	auto ResolveMaterialBinding(
		FMaterialRenderData& Material,
		FMaterialRenderBinding& OutBinding,
		std::string_view DiagnosticResource
	) -> bool
	{
		FMaterialRenderValidationDiagnostic Diagnostic;
		if (TryGetMaterialRenderBinding(
				Material.Representation, OutBinding, Diagnostic))
		{
			return true;
		}

		RecordMaterialFallbackReason(EMaterialFallbackReason::UnsupportedLayout);
		const FMaterialRenderLayoutIdentity RejectedIdentity =
			Material.Representation.GetLayout().Identity;
		DURIN_ERROR("Renderer material binding failed: context={}, layout-version={}, layout-id={}, message={}. ErrorMaterial was selected.",
			DiagnosticResource, RejectedIdentity.Version, RejectedIdentity.Id.ToString(),
			FormatMaterialError(Diagnostic.Error));

		Material = GetErrorMaterialRenderData();
		FMaterialRenderValidationDiagnostic ErrorDiagnostic;
		if (TryGetMaterialRenderBinding(
				Material.Representation, OutBinding, ErrorDiagnostic))
		{
			return true;
		}

		checkf(
			false,
			"ErrorMaterial must satisfy the compiled layout binding contract: %s",
			Durin::FormatMaterialError(ErrorDiagnostic.Error).c_str());
		return false;
	}
}
