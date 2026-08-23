#include "Renderers/MaterialBindingResolution.h"

#include "Renderers/RendererResourceDiagnostics.h"

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

		FRenderResourceCreateDiagnostic ResourceDiagnostic;
		ResourceDiagnostic.Error = MakeRendererResourceCreateError(
			ERenderResourceCreateErrorCategory::ShaderBinding,
			std::string(DiagnosticResource), "prepared-material",
			Diagnostic.Message,
			ERenderResourceGenerationDependency::Manual);
		ReportRendererResourceCreateDiagnostic(ResourceDiagnostic);
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
		FRenderResourceCreateDiagnostic ResourceDiagnostic;
		ResourceDiagnostic.Error = MakeRendererResourceCreateError(
			ERenderResourceCreateErrorCategory::ShaderBinding,
			std::string(DiagnosticResource),
			std::format(
				"layout-version={},layout-id={}",
				RejectedIdentity.Version,
				RejectedIdentity.Id.ToString()),
			std::format("{} ErrorMaterial was selected.", Diagnostic.Message),
			ERenderResourceGenerationDependency::Manual);
		ReportRendererResourceCreateDiagnostic(ResourceDiagnostic);

		Material = GetErrorMaterialRenderData();
		FMaterialRenderValidationDiagnostic ErrorDiagnostic;
		if (TryGetMaterialRenderBinding(
				Material.Representation, OutBinding, ErrorDiagnostic))
		{
			return true;
		}

		checkf(
			false,
			"ErrorMaterial must satisfy the exact v3 binding contract: %s",
			ErrorDiagnostic.Message.c_str());
		return false;
	}
}
