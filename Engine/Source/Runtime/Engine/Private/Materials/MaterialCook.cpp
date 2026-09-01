#include "Materials/Material.h"

#include "Asset/Asset.h"
#include "Asset/AssetCook.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "Materials/MaterialCookedProgram.h"

namespace Durin
{
	auto DMaterial::LoadCookedProgram(std::string& OutError) -> bool
	{
		auto FailCooked = [&](std::string Message) {
			MaterialCookDiagnostic = std::format(
				"Cooked Material '{}': {}", GetObjectPath(), Message);
			OutError = MaterialCookDiagnostic;
			return false;
		};
		std::span<const std::byte> Bytes;
		if (!CookedProgramData.LockReadOnly(Bytes, &OutError))
			return FailCooked(OutError);

		FMaterialStaticProperties PayloadProperties;
		std::shared_ptr<const FMaterialCompilerResult> ProgramCandidate;
		if (!DecodeMaterialCookedProgram(
			Bytes,
			ECookTargetPlatform::Win64,
			ECookTargetProfile::Game,
			PayloadProperties, ProgramCandidate, OutError))
		{
			CookedProgramData.UnlockReadOnly();
			return FailCooked(OutError);
		}
		if (PayloadProperties != StaticProperties)
		{
			CookedProgramData.UnlockReadOnly();
			return FailCooked(
				"payload static properties do not match package metadata.");
		}
		if (!CookedProgramData.UnlockReadOnly(&OutError))
			return FailCooked(OutError);

		AcceptedCompiledProgram = std::move(ProgramCandidate);
		AcceptedCompiledStaticProperties = PayloadProperties;
		MaterialCompileStatus.State = EMaterialCompileState::Ready;
		MaterialCompileStatus.ResultCategory =
			EMaterialCompileResultCategory::None;
		MaterialCompileStatus.CacheOutcome =
			EMaterialCompileCacheOutcome::None;
		MaterialCompileStatus.RequestGeneration = 1;
		MaterialCompileStatus.CompiledAuthoredRevision =
			MaterialCompileStatus.AuthoredRevision;
		MaterialCompileStatus.RequestedIdentity =
			AcceptedCompiledProgram->Identity;
		MaterialCompileStatus.CompiledIdentity =
			AcceptedCompiledProgram->Identity;
		MaterialCompileStatus.Target = AcceptedCompiledProgram->Target;
		MaterialCompileStatus.bHasLastKnownGood = true;
		MaterialCompileStatus.bLastKnownGoodDisplayed = false;
		MaterialCompileDiagnostics.clear();
		MaterialCookDiagnostic = std::format(
			"Loaded cooked material program {} for '{}'.",
			AcceptedCompiledProgram->Identity.ToString(), GetObjectPath());
		PublishMaterialRenderProxyState();
		OutError.clear();
		return true;
	}

	auto DMaterial::SerializeCooked(FArchive& Ar) -> void
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
			if (!AcceptedCompiledProgram)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"Material cooked program data is unavailable.");
				return;
			}
			FByteArray Bytes;
			std::string Error;
			if (!EncodeMaterialCookedProgram(
					*AcceptedCompiledProgram, AcceptedCompiledStaticProperties,
					ECookTargetPlatform::Win64,
					ECookTargetProfile::Game, Bytes, Error)
				|| !FBulkData::TryCreateDetached(Bytes, Projection, &Error))
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, std::move(Error));
				return;
			}
			FieldValue = &Projection;
		}
		auto Field = EnterArchiveField(Ar, {FName("Durin::DMaterial"),
			FName("ProgramData"), FArchiveLogicalTypeDescriptor::BulkData()});
		FieldValue->Serialize(Ar, {.Alignment = EditorBulkDataExternalAlignment,
			.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
	}

	auto DMaterial::ContributeToCook(
		FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game)
			return Fail(std::format(
				"Material '{}' supports only the Win64 game cook target.",
				GetObjectPath()), &OutError);
		if (!MaterialCompileStatus.IsCurrent() || !AcceptedCompiledProgram)
			return Fail(std::format(
				"Material '{}' cannot cook because authored revision {} does not have a complete latest target result.",
				GetObjectPath(), MaterialCompileStatus.AuthoredRevision), &OutError);
		if (AcceptedCompiledProgram->Target
			!= MaterialCompileStatus.Target
			|| AcceptedCompiledProgram->PassContractVersion
				!= CurrentMaterialPassContractVersion)
			return Fail(std::format(
				"Material '{}' compiled target or pass contract is incompatible with Cook.",
				GetObjectPath()), &OutError);

		return Context.AddPackage(
			std::string(VirtualPackagePath), GetPackage(), &OutError);
	}
}
