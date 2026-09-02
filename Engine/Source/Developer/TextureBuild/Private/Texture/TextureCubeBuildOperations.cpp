#include "Texture/TextureCubeBuildOperations.h"

#include "DerivedDataCache/DerivedDataBuildSession.h"
#include "Hash/XxHash.h"
#include "Texture/TextureBuildFunctionRegistry.h"
#include "Texture/TextureBuildFunctions.h"
#include "Texture/TextureCubeBuilder.h"
#include "Texture/TextureCubeDerivedData.h"

namespace Durin
{
	using namespace ::Durin::DerivedData;

	namespace
	{
		auto MakeDefinition(std::string_view Key, std::span<const std::byte> KeyBytes,
			const FTextureCubeSourceData* SourceData,
			const FTextureCubeBuildKeyInput& KeyInput,
			FBuildDefinition& OutDefinition, std::string& OutError) -> bool
		{
			FBuildDefinitionBuilder Builder(AssetPrivate::TextureCubeFunctionName,
				std::string(AssetPrivate::TextureCubeValueName));
			Builder.SetKey(FBuildKey::FromString(Key), KeyBytes)
				.AddTargetFact("Platform", "Win64")
				.AddTargetFact("Profile", "Game")
				.AddTargetFact("SRGB", KeyInput.bSRGB ? "1" : "0");
			if (SourceData)
				Builder.AddTargetFact("Dimension",
					std::to_string(SourceData->Faces[0].Width))
					.AddInput(FBuildValue::FromOwned(
						std::string(AssetPrivate::TextureCubeInputName),
						AssetPrivate::EncodeTextureCubeLocalInput(*SourceData)));
			return Builder.Build(OutDefinition, &OutError);
		}

		auto ExecuteCubeBuild(const FTextureCubeImportedData& ImportedData,
			bool bSRGB, bool bPersistDerivedData,
			FTextureCubeBuildProduct& OutProduct, std::string& OutError) -> bool
		{
			if (!ImportedData.IsValid())
			{
				OutError = "TextureCube canonical imported faces are invalid.";
				return false;
			}
			if (!EnsureTextureBuildFunctions(&OutError)) return false;
			const FXxHash128 CanonicalHash = ImportedData.GetIdentity();
			const FTextureCubeBuildKeyInput KeyInput{
				.SourceLayout = ETextureCubeBuildSourceLayout::SixFaces,
				.FaceContentHashes = {CanonicalHash, CanonicalHash, CanonicalHash,
					CanonicalHash, CanonicalHash, CanonicalHash},
				.bSRGB = bSRGB,
				.TargetPlatform = ECookTargetPlatform::Win64,
				.TargetProfile = ECookTargetProfile::Game};
			const FByteArray KeyBytes = BuildTextureCubeDerivedDataKeyBytes(
				KeyInput, OutError);
			const std::string Key = KeyBytes.empty()
				? std::string{} : FXxHash128::HashBuffer(KeyBytes).ToString();
			if (Key.empty()) return false;

			FBuildDefinition CacheDefinition;
			if (!MakeDefinition(Key, KeyBytes, nullptr, KeyInput,
				CacheDefinition, OutError)) return false;
			FBuildOutput Output = FBuildSession().Build(CacheDefinition, {
				.bQueryCache = true, .bAllowLocalBuild = false,
				.bStoreBuildResult = false});
			if (!Output.Succeeded())
			{
				const FTextureCubeSourceData SourceData = ImportedData.ToSourceData();
				if (!SourceData.IsValid())
				{
					OutError = "TextureCube canonical source pixels are invalid.";
					return false;
				}
				FBuildDefinition LocalDefinition;
				if (!MakeDefinition(Key, KeyBytes, &SourceData, KeyInput,
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
			auto PlatformData = std::make_unique<FTextureCubePlatformData>();
			if (!AssetPrivate::DecodeTextureCubePlatformValue(
				Output.Value, *PlatformData, OutError)) return false;
			OutProduct = {.PlatformData = std::move(PlatformData),
				.DerivedDataKey = Key,
				.PersistenceDiagnostic = Output.StoreDiagnostic,
				.Origin = Output.Status == EBuildStatus::CacheHit
					? ETextureCubeBuildProductOrigin::CacheHit
					: ETextureCubeBuildProductOrigin::Rebuilt};
			OutError.clear();
			return true;
		}

		template <typename PanoramaType>
		auto BuildPanorama(const PanoramaType& Panorama,
			const FTextureCubePanoramaBuildSettings& Settings,
			bool bPersistDerivedData,
			FTextureCubeCanonicalBuildInput& OutCanonicalInput,
			FTextureCubeBuildProduct& OutProduct,
			std::string& OutError) -> bool
		{
			FTextureCubeSourceData SourceData;
			if (!TextureCubeBuilder::ProjectEquirectangularTextureCube(
				Panorama, {Settings.FaceDimension, Settings.ExposureEV},
				SourceData, OutError)) return false;
			FTextureCubeImportedData ImportedData;
			if (!ImportedData.SetSourceData(SourceData))
			{
				OutError = "TextureCube canonical imported faces are invalid.";
				return false;
			}
			OutCanonicalInput = {
				.ImportedData = ImportedData,
				.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama,
				.OriginalSourceWidth = Panorama.Width,
				.OriginalSourceHeight = Panorama.Height,
				.PanoramaFaceDimension = Settings.FaceDimension,
				.PanoramaExposureEV = Settings.ExposureEV,
				.bSRGB = true};
			return ExecuteCubeBuild(ImportedData, true, bPersistDerivedData,
				OutProduct, OutError);
		}
	}

	auto BuildTextureCube(const FTextureCubeBuildRequest& Request,
		FTextureCubeCanonicalBuildInput& OutCanonicalInput,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutCanonicalInput = {};
		OutProduct = {};
		if (Request.TargetPlatform != ECookTargetPlatform::Win64
			|| Request.TargetProfile != ECookTargetProfile::Game)
		{
			OutError = "TextureCube build target is unsupported.";
			return false;
		}
		if (const auto* Faces = std::get_if<FTextureCubeFacesBuildInput>(
			&Request.Input))
		{
			OutCanonicalInput = {.ImportedData = Faces->ImportedData,
				.SourceLayout = Faces->SourceLayout,
				.OriginalSourceWidth = Faces->OriginalSourceWidth,
				.OriginalSourceHeight = Faces->OriginalSourceHeight,
				.PanoramaFaceDimension = Faces->PanoramaFaceDimension,
				.PanoramaExposureEV = Faces->PanoramaExposureEV,
				.bSRGB = Faces->Settings.bSRGB};
			return ExecuteCubeBuild(Faces->ImportedData, Faces->Settings.bSRGB,
				Request.bPersistDerivedData, OutProduct, OutError);
		}
		const auto& Panorama = std::get<FTextureCubePanoramaBuildInput>(Request.Input);
		return std::visit([&](const auto& Image) {
			return BuildPanorama(Image, Panorama.Settings,
				Request.bPersistDerivedData, OutCanonicalInput, OutProduct, OutError);
		}, Panorama.Image);
	}
}
