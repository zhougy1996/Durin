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
			return FailCooked(FMaterialError::FromBulkRead(Read.Status, Read.Error));
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
		if (Ar.IsError() || IsDynamicInstance()) return;
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
			if (const auto Bulk = FBulkData::TryCreateDetached(Bytes, Projection); !Bulk)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, FormatBulkDataError(Bulk.Error));
				return;
			}
			FieldValue = &Projection;
		}
		auto Field = EnterArchiveField(Ar, {FName("Durin::DMaterialInterface"),
			FName("ProgramData"), FArchiveLogicalTypeDescriptor::BulkData()});
		FieldValue->Serialize(Ar, {.Alignment = EditorBulkDataExternalAlignment,
			.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
	}

	auto DMaterialInterface::ContributeToCook(FCookContext& Context,
		std::string_view VirtualPackagePath) -> FCookContributionResult
	{
		auto Reject = [&](ECookContributionError Error) -> FCookContributionResult {
			return {.Error = Error, .ObjectPath = GetObjectPath(), .VirtualPath = std::string(VirtualPackagePath),
				.TargetPlatform = Context.GetTargetPlatform(), .TargetProfile = Context.GetTargetProfile()};
		};
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game) return Reject(ECookContributionError::Target);
		if (!CompilationOwner.MaterialCompileStatus.IsCurrent() || !CompilationOwner.RenderLayer.CompiledProgram
			|| CompilationOwner.MaterialCompileStatus.DependencyRevision != GetShaderReloadGeneration()
			|| !CompilationOwner.RenderLayer.StaticProperties
			|| CanonicalizeMaterialShaderProperties(GetStaticProperties())
				!= CanonicalizeMaterialShaderProperties(*CompilationOwner.RenderLayer.StaticProperties))
		{
			auto Result = Reject(ECookContributionError::Revision);
			Result.AuthoredRevision = CompilationOwner.MaterialCompileStatus.AuthoredRevision;
			return Result;
		}
		if (CompilationOwner.RenderLayer.CompiledProgram->Target != CompilationOwner.MaterialCompileStatus.Target
			|| CompilationOwner.RenderLayer.CompiledProgram->PassContractVersion != CurrentMaterialPassContractVersion)
			return Reject(ECookContributionError::Contract);
		if (!AreMaterialFunctionOwnersCurrent(CompilationOwner.RequestedFunctionOwners))
			return Reject(ECookContributionError::FunctionDependencies);
		const auto Added = Context.AddPackage(std::string(VirtualPackagePath), GetPackage());
		if (!Added)
		{
			auto Result = Reject(ECookContributionError::Plan);
			Result.PlanCause = Added.Error;
			return Result;
		}
		return {};
	}

}
