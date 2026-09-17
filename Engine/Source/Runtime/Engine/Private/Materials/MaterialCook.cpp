#include "Materials/Material.h"
#include "MaterialCompileRetryQueue.h"

#include "Asset/Asset.h"
#include "Asset/AssetCook.h"
#include "Asset/CookDependencies.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "Materials/MaterialCookedProgram.h"

namespace Durin
{
	auto DMaterialInterface::LoadCookedProgram() -> FMaterialOperationResult
	{
		auto FailCooked = [&](FMaterialError Error) -> FMaterialOperationResult {
			MaterialCookDiagnostic = Error;
			return {std::move(Error)};
		};
		auto Read = CookedProgramData.AcquireRead();
		if (!Read)
		{
			FMaterialError Error(EMaterialCookError::PayloadReadFailed);
			Error.BulkStatus = Read.Status;
			Error.ResourceStatus = Read.Error.Status;
			return FailCooked(std::move(Error));
		}
		const FByteView Bytes = Read.Lock.GetBytes();

		FMaterialStaticProperties PayloadProperties;
		std::shared_ptr<const FMaterialCompilerResult> ProgramCandidate;
		const auto Decoded = DecodeMaterialCookedProgram(
			Bytes,
			ECookTargetPlatform::Win64,
			ECookTargetProfile::Game,
			PayloadProperties, ProgramCandidate);
		if (!Decoded)
		{
			return FailCooked(Decoded.Error);
		}
		auto ExpectedProperties = GetStaticProperties();
		ExpectedProperties.OpacityMaskThreshold = CanonicalizeMaterialShaderProperties(ExpectedProperties).OpacityMaskThreshold;
		if (PayloadProperties != ExpectedProperties)
		{
			return FailCooked(
				EMaterialCookError::StaticPropertiesMismatch);
		}
		for (const auto& Parameter : ProgramCandidate->ActiveParameters)
		{
			const auto* Definition = FindParameterDefinition(Parameter.Id);
			if (!Definition || Definition->Type != Parameter.Type)
			{
				return FailCooked(EMaterialCookError::ParameterContractMismatch);
			}
		}
		Read.Lock.Reset();

		CompilationOwner.RenderLayer.CompiledProgram = std::move(ProgramCandidate);
		CompilationOwner.RenderLayer.StaticProperties = PayloadProperties;
		Private::GetMaterialCompileRetryQueue().Remove(FWeakObjectPtr(this));
		CompilationOwner.MaterialCompileStatus.State = EMaterialCompileState::Ready;
		CompilationOwner.MaterialCompileStatus.ResultCategory =
			EMaterialCompileResultCategory::None;
		CompilationOwner.MaterialCompileStatus.CacheOutcome =
			EMaterialCompileCacheOutcome::None;
		CompilationOwner.MaterialCompileStatus.RequestGeneration = 1;
		CompilationOwner.MaterialCompileStatus.RequestedIdentity =
			CompilationOwner.RenderLayer.CompiledProgram->Identity;
		CompilationOwner.MaterialCompileStatus.CompiledIdentity =
			CompilationOwner.RenderLayer.CompiledProgram->Identity;
		CompilationOwner.MaterialCompileStatus.Target = CompilationOwner.RenderLayer.CompiledProgram->Target;
		CompilationOwner.MaterialCompileDiagnostics.clear();
		MaterialCookDiagnostic = {};
		PublishMaterialRenderProxyState();
		return {};
	}

	auto DMaterialInterface::SerializeCooked(FArchive& Ar) -> void
	{
		Super::SerializeCooked(Ar);
		if (Ar.GetTarget().Platform != "Win64" || Ar.GetTarget().Profile != "Game")
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				"Material cooked program data requires the Win64 Game target.");
			return;
		}
		FBulkData Projection;
		FBulkData* FieldValue = &CookedProgramData;
		if (Ar.IsSaving())
		{
			if (!CompilationOwner.MaterialCompileStatus.IsCurrent() || !CompilationOwner.RenderLayer.CompiledProgram
				|| (!GetAssetRuntimeConfiguration().RequiresCookedPayload()
					&& CompilationOwner.MaterialCompileStatus.DependencyRevision != GetShaderReloadGeneration())
				|| !CompilationOwner.RenderLayer.StaticProperties
				|| CanonicalizeMaterialShaderProperties(GetStaticProperties())
					!= CanonicalizeMaterialShaderProperties(*CompilationOwner.RenderLayer.StaticProperties))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"Material cooked program data is unavailable.");
				return;
			}
			FByteBuffer Bytes;
			const auto Encoded = EncodeMaterialCookedProgram(
				*CompilationOwner.RenderLayer.CompiledProgram, GetRenderableStaticProperties(),
				ECookTargetPlatform::Win64, ECookTargetProfile::Game, Bytes);
			if (!Encoded)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, FormatMaterialError(Encoded.Error));
				return;
			}
			std::string BulkError;
			if (!FBulkData::TryCreateDetached(Bytes, Projection, &BulkError))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, BulkError);
				return;
			}
			FieldValue = &Projection;
		}
		auto Field = EnterArchiveField(Ar, {FName("Durin::DMaterialInterface"),
			FName("ProgramData"), FArchiveLogicalTypeDescriptor::BulkData()});
		FieldValue->Serialize(Ar, {.Alignment = EditorBulkDataExternalAlignment,
			.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
	}

	auto DMaterialInterface::ContributeToCook(
		FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game)
			return Fail(std::format(
				"Material '{}' supports only the Win64 game cook target.",
				GetObjectPath()), &OutError);
		if (!CompilationOwner.MaterialCompileStatus.IsCurrent() || !CompilationOwner.RenderLayer.CompiledProgram
			|| CompilationOwner.MaterialCompileStatus.DependencyRevision != GetShaderReloadGeneration()
			|| !CompilationOwner.RenderLayer.StaticProperties
			|| CanonicalizeMaterialShaderProperties(GetStaticProperties())
				!= CanonicalizeMaterialShaderProperties(*CompilationOwner.RenderLayer.StaticProperties))
			return Fail(std::format(
				"Material '{}' cannot cook because authored revision {} does not have a complete latest target result.",
				GetObjectPath(), CompilationOwner.MaterialCompileStatus.AuthoredRevision), &OutError);
		if (CompilationOwner.RenderLayer.CompiledProgram->Target
			!= CompilationOwner.MaterialCompileStatus.Target
			|| CompilationOwner.RenderLayer.CompiledProgram->PassContractVersion
				!= CurrentMaterialPassContractVersion)
			return Fail(std::format(
				"Material '{}' compiled target or pass contract is incompatible with Cook.",
				GetObjectPath()), &OutError);
		if (!AreMaterialFunctionOwnersCurrent(CompilationOwner.RequestedFunctionOwners))
			return Fail(std::format("Material '{}' has stale function dependencies at Cook capture.", GetObjectPath()), &OutError);

		return Context.AddPackage(
			std::string(VirtualPackagePath), GetPackage(), &OutError);
	}
}
