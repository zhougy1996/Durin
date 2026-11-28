#include "Renderers/MaterialBindingResolution.h"

#include "CoreGlobals.h"

namespace Durin::RendererPrivate
{
	auto IsPreparedMaterialReady(
		const FMaterialRenderData& Material,
		std::string_view DiagnosticResource
	) -> bool
	{
		if (Material.IsReadyForRendering())
			return true;

		DURIN_ERROR("Renderer material binding failed: context={}, identity=prepared-material, pending={}",
			DiagnosticResource, Material.bPending);
		return false;
	}

	auto SelectMeshMaterial(
		FMaterialRenderData& Material,
		std::string_view DiagnosticResource
	) -> bool
	{
		if (Material.IsReadyForRendering())
		{
			return true;
		}

		RecordMaterialFallbackReason(EMaterialFallbackReason::UnsupportedLayout);
		const FMaterialRenderLayoutIdentity RejectedIdentity =
			Material.Representation.GetLayout().Identity;
		DURIN_ERROR("Renderer material binding failed: context={}, layout-version={}, layout-id={}, pending={}. ErrorMaterial was selected.",
			DiagnosticResource, RejectedIdentity.Version, RejectedIdentity.Id.ToString(),
			Material.bPending);

		Material = GetErrorMaterialRenderData();
		if (Material.IsReadyForRendering())
		{
			return true;
		}

		checkf(
			false,
			"ErrorMaterial must satisfy the compiled layout binding contract");
		return false;
	}
}
