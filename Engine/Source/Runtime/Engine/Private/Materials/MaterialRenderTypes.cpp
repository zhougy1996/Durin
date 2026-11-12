#include "Materials/MaterialRenderTypes.h"
#include "RenderingThread.h"

namespace Durin
{
	auto FUniformExpressionCache::UpdateUniformBuffer(FRHICommandListBase& Commands, const FMaterialRenderRepresentation& Material)
		-> bool
	{
		CheckRenderingThread();
		if (CachedRepresentation == Material.GetRecordId() && UniformBuffer->GetRange().Buffer) return true;
		auto Result = UniformBuffer->Update(Commands, Material.GetUniformPayload());
		if (Result) CachedRepresentation = Material.GetRecordId();
		return Result;
	}
	auto FUniformExpressionCache::GetParameterCollectionUniformBuffer(const std::shared_ptr<const FMaterialCompilerResult>& Program,
		FGuid Collection) -> FUniformBuffer&
	{
		CheckRenderingThread();
		auto& Buffer = ParameterCollections[{Program, Collection}];
		if (!Buffer) Buffer = FUniformBuffer::Create();
		return *Buffer;
	}
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
