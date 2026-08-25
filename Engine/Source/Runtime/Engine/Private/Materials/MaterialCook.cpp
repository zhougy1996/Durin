#include "Materials/Material.h"

#include "Asset.h"
#include "AssetCook.h"
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
		if (CookedProgramPayload.PayloadId != MaterialCookedProgramPayloadId
			|| CookedProgramPayload.LocationKind
				!= static_cast<uint32>(
					Asset::ECookedPayloadLocationKind::PackageCompanion)
			|| CookedProgramPayload.PayloadSchemaVersion
				!= MaterialCookedProgramPayloadSchemaVersion
			|| CookedProgramPayload.TargetPlatform
				!= static_cast<uint32>(Asset::ECookTargetPlatform::Win64)
			|| CookedProgramPayload.TargetProfile
				!= static_cast<uint32>(Asset::ECookTargetProfile::Game)
			|| CookedProgramPayload.CompressionMethod
				!= static_cast<uint32>(Asset::ECookedPayloadCompression::None))
			return FailCooked("required DMAT descriptor is missing or incompatible.");
		if (!GetPackage())
			return FailCooked("package companion path could not be resolved.");

		const Asset::FAssetRuntimeConfiguration& Runtime =
			Asset::GetAssetRuntimeConfiguration();
		Asset::FCookedPackagePayload LoadedPayload;
		if (!Asset::LoadCookedPackagePayload(
			Runtime, GetPackage()->GetPackagePath(), CookedProgramPayload,
			Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game, LoadedPayload, &OutError))
			return FailCooked(OutError);

		FMaterialStaticProperties PayloadProperties;
		std::shared_ptr<const FMaterialCompilerResult> ProgramCandidate;
		if (!DecodeMaterialCookedProgram(
			LoadedPayload.Payload,
			Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game,
			PayloadProperties, ProgramCandidate, OutError))
			return FailCooked(OutError);
		if (PayloadProperties != StaticProperties)
			return FailCooked(
				"payload static properties do not match package metadata.");

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

	auto DMaterial::AddToCook(
		Asset::FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != Asset::ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != Asset::ECookTargetProfile::Game)
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

		std::vector<std::byte> PayloadBytes;
		if (!EncodeMaterialCookedProgram(
			*AcceptedCompiledProgram, AcceptedCompiledStaticProperties,
			Context.GetTargetPlatform(), Context.GetTargetProfile(),
			PayloadBytes, OutError))
			return false;
		Asset::FCookedBulkPayload BulkPayload{
			.PayloadId = MaterialCookedProgramPayloadId,
			.Flags = 1,
			.PayloadSchemaVersion = MaterialCookedProgramPayloadSchemaVersion,
			.Compression = Asset::ECookedPayloadCompression::None,
			.Alignment = MaterialCookedProgramPayloadAlignment,
			.Bytes = std::move(PayloadBytes),
		};
		const Asset::FAssetPackageSerializationOptions CookPackageOptions =
			Context.MakePackageSerializationOptions();
		return Context.AddPackage(
			std::string(VirtualPackagePath), {std::move(BulkPayload)},
			[this, CookPackageOptions](
				std::span<const Asset::FCookedPayloadDescriptor> Descriptors,
				std::vector<std::byte>& OutPackageBytes,
				std::string* Error) {
				if (Descriptors.size() != 1
					|| Descriptors.front().PayloadId
						!= MaterialCookedProgramPayloadId)
				{
					if (Error) *Error =
						"Material Cook did not produce its required descriptor.";
					return false;
				}
				FProperty* DescriptorProperty = GetClass()->FindPropertyByName(
					"CookedProgramPayload");
				if (!DescriptorProperty)
				{
					if (Error) *Error =
						"Material CookedProgramPayload reflection is unavailable.";
					return false;
				}
				auto Overrides =
					std::make_shared<Asset::FObjectSaveOverrides>();
				std::string OverrideError;
				if (!Overrides->AddPropertyValue(
					*this, *DescriptorProperty, Descriptors.front(),
					&OverrideError))
				{
					if (Error) *Error = OverrideError;
					return false;
				}
				Asset::FAssetPackageSerializationOptions Options =
					CookPackageOptions;
				Options.SaveOverrides = std::move(Overrides);
				const Asset::FAssetResult Serialized =
					Asset::SerializeAssetPackageBytes(
						GetPackage(), OutPackageBytes, Options);
				if (!Serialized)
				{
					if (Error) *Error = Serialized.Message;
					return false;
				}
				return true;
			}, &OutError);
	}
}
