#include "Materials/MaterialRenderTypes.h"

namespace Durin
{
	auto MakeErrorMaterialRenderLayout() -> FMaterialRenderLayout
	{
		static const FMaterialRenderLayout Layout = CompileMaterialLayout({}).Layout;
		return Layout;
	}

	auto ValidateMaterialRenderLayout(const FMaterialRenderLayout& Layout,
		FMaterialRenderValidationDiagnostic& OutDiagnostic) -> bool
	{
		OutDiagnostic = {};
		if (Layout.Identity.Version != CompiledMaterialRenderLayoutVersion)
		{
			OutDiagnostic.Failure = EMaterialRenderValidationFailure::UnsupportedVersion;
			OutDiagnostic.Message = "Material render layouts require compiled version 4; migrate and recompile older assets.";
			return false;
		}
		const auto Result = ValidateCompiledMaterialLayout(Layout);
		if (Result) return true;
		OutDiagnostic.Failure = EMaterialRenderValidationFailure::InvalidField;
		OutDiagnostic.FieldIndex = Result.FieldIndex;
		OutDiagnostic.Message = GetMaterialLayoutErrorText(Result.Error);
		return false;
	}
}
