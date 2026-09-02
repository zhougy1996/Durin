#include "Texture/VolumeTextureBuildOperations.h"

#include "DerivedDataCache/DerivedDataBuildSession.h"
#include "Hash/XxHash.h"
#include "Texture/TextureBuildFunctionRegistry.h"
#include "Texture/TextureBuildFunctions.h"
#include "Texture/VolumeTextureDerivedData.h"

namespace Durin
{
	using namespace ::Durin::DerivedData;

	namespace
	{
		auto MakeKeyInput(const FVolumeTextureSourceData& Source,
			const FVolumeTextureBuildSettings& Settings) -> FVolumeTextureBuildKeyInput
		{
			return {.CanonicalSourceIdentity = Source.GetIdentity(),
				.Width = Source.Width, .Height = Source.Height, .Depth = Source.Depth,
				.Settings = Settings,
				.SourcePayloadSchemaVersion = Source.PayloadSchemaVersion,
				.TargetPlatform = ECookTargetPlatform::Win64,
				.TargetProfile = ECookTargetProfile::Game};
		}

		auto MakeDefinition(std::string_view Key,
			std::span<const std::byte> KeyBytes,
			const FVolumeTextureSourceData* Source,
			const FVolumeTextureBuildSettings* Settings,
			FBuildDefinition& OutDefinition, std::string& OutError) -> bool
		{
			FBuildDefinitionBuilder Builder(AssetPrivate::VolumeTextureFunctionName,
				std::string(AssetPrivate::VolumeTextureValueName));
			Builder.SetKey(FBuildKey::FromString(Key), KeyBytes)
				.AddTargetFact("Platform", "Win64")
				.AddTargetFact("Profile", "Game");
			if (Source && Settings)
				Builder.AddInput(FBuildValue::FromOwned(
					std::string(AssetPrivate::VolumeTextureInputName),
					AssetPrivate::EncodeVolumeTextureLocalInput(*Source, *Settings)));
			return Builder.Build(OutDefinition, &OutError);
		}
	}

	auto BuildVolumeTexture(const FVolumeTextureSourceData& SourceData,
		const FVolumeTextureBuildSettings& Settings,
		FVolumeTextureBuildProduct& OutProduct,
		std::string& OutError,
		bool bPersistDerivedData) -> bool
	{
		if (!SourceData.IsValid() || SourceData.Format != Settings.OutputFormat)
		{
			OutError = "Volume texture build source and settings are incompatible.";
			return false;
		}
		if (!EnsureTextureBuildFunctions(&OutError)) return false;
		const FVolumeTextureBuildKeyInput KeyInput = MakeKeyInput(SourceData, Settings);
		const FByteArray KeyBytes = BuildVolumeTextureDerivedDataKeyBytes(
			KeyInput, OutError);
		const std::string Key = KeyBytes.empty()
			? std::string{} : FXxHash128::HashBuffer(KeyBytes).ToString();
		if (Key.empty()) return false;
		FBuildDefinition CacheDefinition;
		if (!MakeDefinition(Key, KeyBytes, nullptr, nullptr,
			CacheDefinition, OutError)) return false;
		FBuildOutput Output = FBuildSession().Build(CacheDefinition, {
			.bQueryCache = true, .bAllowLocalBuild = false,
			.bStoreBuildResult = false});
		if (!Output.Succeeded())
		{
			FBuildDefinition LocalDefinition;
			if (!MakeDefinition(Key, KeyBytes, &SourceData, &Settings,
				LocalDefinition, OutError)) return false;
			Output = FBuildSession().Build(LocalDefinition, {
				.bQueryCache = false, .bAllowLocalBuild = true,
				.bStoreBuildResult = bPersistDerivedData});
		}
		if (!Output.Succeeded())
		{
			OutError = Output.Diagnostic;
			return false;
		}
		auto PlatformData = std::make_unique<FVolumeTexturePlatformData>();
		if (!AssetPrivate::DecodeVolumeTexturePlatformValue(
			Output.Value, *PlatformData, OutError)) return false;
		OutProduct = {.PlatformData = std::move(PlatformData),
			.DerivedDataKey = Key,
			.PersistenceDiagnostic = Output.StoreDiagnostic,
			.Origin = Output.Status == EBuildStatus::CacheHit
				? EVolumeTextureBuildProductOrigin::CacheHit
				: EVolumeTextureBuildProductOrigin::Rebuilt};
		OutError.clear();
		return true;
	}
}
