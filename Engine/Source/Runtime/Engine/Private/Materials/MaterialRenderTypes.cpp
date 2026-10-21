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
			OutDiagnostic.Message = "Material render layouts require compiled version 4; migrate and recompile older assets.";
			return false;
		}
		const auto Result = ValidateCompiledMaterialLayout(Layout);
		if (Result) return true;
		OutDiagnostic.Message = std::format("{}{} [index {}]", GetMaterialLayoutErrorText(Result.Error),
			Result.ParameterId.IsValid() ? std::format(" [parameter {}]", Result.ParameterId.ToString()) : "", Result.FieldIndex);
		return false;
	}
}
