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
			return {.SourceContentHash = Source.GetIdentity(),
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

	auto BuildVolumeTexture(FVolumeTextureSourceData SourceData,
		const FVolumeTextureBuildSettings& Settings,
		FVolumeTextureBuildProduct& OutProduct,
		std::string& OutError) -> bool
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
		FBuildDefinition Definition;
		if (!MakeDefinition(Key, KeyBytes, &SourceData, &Settings,
			Definition, OutError)) return false;
		const FBuildOutput Output = FBuildSession().Build(Definition, {
			.bQueryCache = true, .bAllowLocalBuild = true,
			.bStoreBuildResult = true});
		if (!Output.Succeeded())
		{
			OutError = Output.Diagnostic;
			return false;
		}
		auto PlatformData = std::make_unique<FVolumeTexturePlatformData>();
		if (!AssetPrivate::DecodeVolumeTexturePlatformValue(
			Output.Value, *PlatformData, OutError)) return false;
		OutProduct = {.SourceData = std::move(SourceData), .Settings = Settings,
			.PlatformData = std::move(PlatformData), .DerivedDataKey = Key,
			.bCacheHit = Output.Status == EBuildStatus::CacheHit,
			.PersistenceDiagnostic = Output.StoreDiagnostic};
		OutError.clear();
		return true;
	}

	auto PublishVolumeTextureProduct(DVolumeTexture& Texture,
		FVolumeTextureBuildProduct Product, std::string& OutError) -> bool
	{
		return Texture.PublishBuiltData(std::move(Product.SourceData), Product.Settings,
			std::move(Product.PlatformData), std::move(Product.DerivedDataKey),
			std::move(Product.PersistenceDiagnostic), OutError);
	}

	auto BuildVolumeTextureInto(
		DVolumeTexture& Texture,
		FVolumeTextureSourceData SourceData,
		const FVolumeTextureBuildSettings& Settings,
		std::string& OutError) -> bool
	{
		FVolumeTextureBuildProduct Product;
		return BuildVolumeTexture(
			std::move(SourceData), Settings, Product, OutError)
			&& PublishVolumeTextureProduct(
				Texture, std::move(Product), OutError);
	}

	auto MakeVolumeTextureDerivedDataKey(const DVolumeTexture& Texture,
		std::string& OutError) -> std::string
	{
		if (!Texture.GetSourceData().IsValid())
		{
			OutError = "Volume texture has no valid normalized source.";
			return {};
		}
		return BuildVolumeTextureDerivedDataKey(
			MakeKeyInput(Texture.GetSourceData(), Texture.GetBuildSettings()), OutError);
	}

	auto LoadVolumeTextureDerivedData(std::string_view Key,
		std::unique_ptr<FVolumeTexturePlatformData>& OutPlatformData,
		ETextureDerivedDataStatus& OutStatus,
		std::string& OutMessage) -> bool
	{
		if (!EnsureTextureBuildFunctions(&OutMessage))
		{
			OutStatus = ETextureDerivedDataStatus::Corrupt;
			return false;
		}
		FBuildDefinition Definition;
		std::string Error;
		if (!MakeDefinition(Key, {}, nullptr, nullptr, Definition, Error))
		{
			OutStatus = ETextureDerivedDataStatus::Incompatible;
			OutMessage = Error;
			return false;
		}
		const FBuildOutput Output = FBuildSession().Build(Definition, {
			.bQueryCache = true, .bAllowLocalBuild = false,
			.bStoreBuildResult = false});
		if (!Output.Succeeded())
		{
			OutStatus = Output.Status == EBuildStatus::CacheMiss
				? ETextureDerivedDataStatus::Missing : ETextureDerivedDataStatus::Corrupt;
			OutMessage = Output.Diagnostic;
			return false;
		}
		auto Candidate = std::make_unique<FVolumeTexturePlatformData>();
		if (!AssetPrivate::DecodeVolumeTexturePlatformValue(
			Output.Value, *Candidate, OutMessage))
		{
			OutStatus = ETextureDerivedDataStatus::Corrupt;
			return false;
		}
		OutPlatformData = std::move(Candidate);
		OutStatus = ETextureDerivedDataStatus::Hit;
		OutMessage.clear();
		return true;
	}
}
