#include "Materials/MaterialRenderTypes.h"
#include "RenderingThread.h"

namespace Durin
{
	auto FUniformExpressionCache::UpdateUniformBuffer(FRHICommandListBase& Commands, const FMaterialRenderRepresentation& Material,
		std::span<FRHIResource* const> Resources, std::span<const FRHIUniformBufferResource> Members)
		-> bool
	{
		CheckRenderingThread();
		// Commands already prepared from another publication retain its exact RHI buffer.
		auto Target = UniformBuffer ? UniformBuffer : FUniformBuffer::Create();
		if (const auto* Current = Target->GetRHI(); Current && CachedRepresentation != Material.GetRecordId()
			&& (!std::ranges::equal(Target->GetData(), Material.GetUniformPayload())
				|| !std::ranges::equal(Current->GetResourceTable(), Resources,
					[](const auto& A, auto* B) { return A.GetReference() == B; })))
			Target = FUniformBuffer::Create();
		if (!Target->Update(Commands, Material.GetUniformPayload(), Resources, Members)) return false;
		UniformBuffer = std::move(Target);
		CachedRepresentation = Material.GetRecordId();
		return true;
	}
	auto FUniformExpressionCache::GetParameterCollectionUniformBuffer(const std::shared_ptr<const FMaterialProgram>& Program,
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
			OutDiagnostic.Message = "Material render layouts require compiled version 5; migrate and recompile older assets.";
			return false;
		}
		const auto Result = ValidateCompiledMaterialLayout(Layout);
		if (Result) return true;
		OutDiagnostic.Message = std::format("{}{} [index {}]", GetMaterialLayoutErrorText(Result.Error),
			Result.ParameterId.IsValid() ? std::format(" [parameter {}]", Result.ParameterId.ToString()) : "", Result.FieldIndex);
		return false;
	}
}
