#include "Texture/TextureCubeBuildOperations.h"

#include "DerivedDataCache/DerivedDataBuildSession.h"
#include "Hash/XxHash.h"
#include "Texture/TextureBuildFunctionRegistry.h"
#include "Texture/TextureBuildFunctions.h"
#include "Texture/TextureCubeBuilder.h"
#include "Texture/TextureCubeDerivedData.h"

namespace Durin::Asset
{
	using namespace ::Durin::DerivedData;

	namespace
	{
		auto TryLoadCubeBuild(
			const FTextureCubeBuildKeyInput& KeyInput,
			std::string& OutKey,
			std::unique_ptr<FTextureCubePlatformData>& OutPlatformData,
			std::string& OutError) -> bool
		{
			OutKey = BuildTextureCubeDerivedDataKey(KeyInput, OutError);
			if (OutKey.empty()) return false;
			ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::Missing;
			std::string Message;
			if (LoadTextureCubeDerivedData(
				OutKey, OutPlatformData, Status, Message))
			{
				OutError.clear();
				return true;
			}
			OutError.clear();
			return false;
		}

		auto ExecuteCubeBuild(FTextureCubeSourceData& SourceData,
			const FTextureCubeBuildKeyInput& KeyInput, std::string& OutKey,
			std::unique_ptr<FTextureCubePlatformData>& OutPlatformData,
			bool bQueryCache, bool& OutCacheHit,
			std::string& OutError) -> bool
		{
			if (!EnsureTextureBuildFunctions(&OutError)) return false;
			const FByteArray KeyBytes =
				BuildTextureCubeDerivedDataKeyBytes(KeyInput, OutError);
			OutKey = KeyBytes.empty()
				? std::string{} : FXxHash128::HashBuffer(KeyBytes).ToString();
			if (OutKey.empty()) return false;
			FBuildDefinition Definition;
			FBuildDefinitionBuilder Builder(
				Private::TextureCubeFunctionName,
				std::string(Private::TextureCubeValueName));
			Builder.SetKey(FBuildKey::FromString(OutKey), KeyBytes)
				.AddTargetFact("Platform", "Win64")
				.AddTargetFact("Profile", "Game")
				.AddTargetFact("SRGB", KeyInput.bSRGB ? "1" : "0")
				.AddTargetFact("Dimension", std::to_string(SourceData.Faces[0].Width))
				.AddInput(FBuildValue::FromOwned(
					std::string(Private::TextureCubeInputName),
					Private::EncodeTextureCubeLocalInput(SourceData)));
			if (!Builder.Build(Definition, &OutError)) return false;
			const FBuildOutput Output = FBuildSession().Build(Definition, {
				.bQueryCache = bQueryCache, .bAllowLocalBuild = true,
				.bStoreBuildResult = true});
			if (!Output.Succeeded())
			{
				OutError = Output.Diagnostic;
				return false;
			}
			auto Candidate = std::make_unique<FTextureCubePlatformData>();
			if (!Private::DecodeTextureCubePlatformValue(
				Output.Value, *Candidate, OutError)) return false;
			OutPlatformData = std::move(Candidate);
			OutCacheHit = Output.Status == EBuildStatus::CacheHit;
			OutError = Output.StoreDiagnostic;
			return true;
		}

		auto FinishPanoramaProduct(
			FTextureCubeSourceData SourceData,
			uint32 SourceWidth,
			uint32 SourceHeight,
			const FXxHash128& Hash,
			const FTextureCubePanoramaBuildSettings& Settings,
			FTextureCubeBuildProduct& OutProduct,
			std::string& OutError) -> bool
		{
			(void)Hash;
			FTextureCubeImportedData Imported;
			if (!Imported.SetSourceData(SourceData))
			{
				OutError = "TextureCube canonical imported faces are invalid.";
				return false;
			}
			const FXxHash128 CanonicalHash = Imported.GetIdentity();
			const FTextureCubeBuildKeyInput KeyInput{
				.SourceLayout = ETextureCubeBuildSourceLayout::SixFaces,
				.FaceContentHashes = {CanonicalHash, CanonicalHash, CanonicalHash,
					CanonicalHash, CanonicalHash, CanonicalHash},
				.bSRGB = true,
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game};
			std::string Key;
			std::unique_ptr<FTextureCubePlatformData> PlatformData;
			bool bCacheHit = false;
			if (!ExecuteCubeBuild(
				SourceData, KeyInput, Key, PlatformData, true, bCacheHit, OutError))
				return false;
			const std::string PersistenceDiagnostic = OutError;
			OutProduct = {
				.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama,
				.SourceData = std::move(SourceData),
				.PlatformData = std::move(PlatformData),
				.DerivedDataKey = std::move(Key),
				.SourceWidth = SourceWidth,
				.SourceHeight = SourceHeight,
				.PanoramaFaceDimension = Settings.FaceDimension,
				.PanoramaExposureEV = Settings.ExposureEV,
				.bSRGB = true,
				.bLoadedFromDerivedDataCache = bCacheHit,
				.PersistenceDiagnostic = PersistenceDiagnostic};
			OutError.clear();
			return true;
		}
	}

	auto BuildTextureCubePanorama(
		TextureCubeBuilder::FTexturePanoramaImage Panorama,
		const FXxHash128& SourceHash,
		const FTextureCubePanoramaBuildSettings& Settings,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		FTextureCubeSourceData SourceData;
		if (!TextureCubeBuilder::ProjectEquirectangularTextureCube(
			Panorama, {Settings.FaceDimension, Settings.ExposureEV}, SourceData, OutError))
			return false;
		return FinishPanoramaProduct(std::move(SourceData), Panorama.Width,
			Panorama.Height, SourceHash, Settings, OutProduct, OutError);
	}

	auto MakeTextureCubeDerivedDataKey(
		const DTextureCube& Texture,
		std::string& OutError) -> std::string
	{
		const FXxHash128 CanonicalHash = Texture.GetImportedDataIdentity();
		if (CanonicalHash.IsZero())
		{
			OutError = "TextureCube canonical imported-data identity is missing or invalid.";
			return {};
		}
		FTextureCubeBuildKeyInput Input{
			.SourceLayout = ETextureCubeBuildSourceLayout::SixFaces,
			.FaceContentHashes = {CanonicalHash, CanonicalHash, CanonicalHash,
				CanonicalHash, CanonicalHash, CanonicalHash},
			.bSRGB = Texture.IsSRGB(),
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game};
		return BuildTextureCubeDerivedDataKey(Input, OutError);
	}

	auto LoadTextureCubeDerivedData(
		std::string_view Key,
		std::unique_ptr<FTextureCubePlatformData>& OutPlatformData,
		ETextureDerivedDataStatus& OutStatus,
		std::string& OutMessage) -> bool
	{
		std::string Error;
		if (!EnsureTextureBuildFunctions(&Error))
		{
			OutStatus = ETextureDerivedDataStatus::Corrupt;
			OutMessage = Error;
			return false;
		}
		FBuildDefinition Definition;
		FBuildDefinitionBuilder Builder(
			Private::TextureCubeFunctionName,
			std::string(Private::TextureCubeValueName));
		Builder.SetKey(FBuildKey::FromString(Key))
			.AddTargetFact("Platform", "Win64")
			.AddTargetFact("Profile", "Game");
		if (!Builder.Build(Definition, &Error))
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
		auto Candidate = std::make_unique<FTextureCubePlatformData>();
		if (!Private::DecodeTextureCubePlatformValue(
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

	auto BuildTextureCubePanorama(
		TextureCubeBuilder::FTexturePanoramaFloatImage Panorama,
		const FXxHash128& SourceHash,
		const FTextureCubePanoramaBuildSettings& Settings,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		FTextureCubeSourceData SourceData;
		if (!TextureCubeBuilder::ProjectEquirectangularTextureCube(
			Panorama, {Settings.FaceDimension, Settings.ExposureEV}, SourceData, OutError))
			return false;
		return FinishPanoramaProduct(std::move(SourceData), Panorama.Width,
			Panorama.Height, SourceHash, Settings, OutProduct, OutError);
	}

	auto BuildTextureCubeFaces(
		FTextureCubeSourceData SourceData,
		const std::array<FXxHash128, TextureCubeFaceCount>& Hashes,
		const FTextureCubeFacesBuildSettings& Settings,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		(void)Hashes;
		FTextureCubeImportedData Imported;
		if (!Imported.SetSourceData(SourceData))
		{
			OutError = "TextureCube canonical imported faces are invalid.";
			return false;
		}
		const FXxHash128 CanonicalHash = Imported.GetIdentity();
		const FTextureCubeBuildKeyInput KeyInput{
			.SourceLayout = ETextureCubeBuildSourceLayout::SixFaces,
			.FaceContentHashes = {CanonicalHash, CanonicalHash, CanonicalHash,
				CanonicalHash, CanonicalHash, CanonicalHash},
			.bSRGB = Settings.bSRGB,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game};
		std::string Key;
		std::unique_ptr<FTextureCubePlatformData> PlatformData;
		bool bCacheHit = false;
		if (!ExecuteCubeBuild(
			SourceData, KeyInput, Key, PlatformData, true, bCacheHit, OutError))
			return false;
		const std::string PersistenceDiagnostic = OutError;
		const uint32 SourceWidth = SourceData.Faces[0].Width;
		const uint32 SourceHeight = SourceData.Faces[0].Height;
		OutProduct = {
			.SourceLayout = ETextureCubeSourceLayout::SixFaces,
			.SourceData = std::move(SourceData),
			.PlatformData = std::move(PlatformData),
			.DerivedDataKey = std::move(Key),
			.SourceWidth = SourceWidth,
			.SourceHeight = SourceHeight,
			.bSRGB = Settings.bSRGB,
			.bLoadedFromDerivedDataCache = bCacheHit,
			.PersistenceDiagnostic = PersistenceDiagnostic};
		OutError.clear();
		return true;
	}

	auto PublishTextureCubeProduct(
		DTextureCube& Texture,
		FTextureCubeBuildProduct Product,
		const FTextureCubePublicationContext& Context,
		std::string& OutError) -> bool
	{
		(void)Context;
		if (!Product.PlatformData || !Product.PlatformData->IsValid()
			|| !Product.SourceData.IsValid()
			|| Product.DerivedDataKey.empty())
		{
			OutError = "TextureCube publication product is incomplete.";
			return false;
		}
		const std::string DiagnosticKey = Product.DerivedDataKey;
		const bool bPanorama = Product.SourceLayout
			== ETextureCubeSourceLayout::EquirectangularPanorama;
		auto SourceData = std::make_unique<FTextureCubeSourceData>(
			std::move(Product.SourceData));
		Texture.PublishBuildProduct(
			Product.SourceLayout, Product.PanoramaFaceDimension,
			Product.PanoramaExposureEV, Product.SourceWidth, Product.SourceHeight,
			Product.bSRGB,
			std::move(SourceData),
			std::move(Product.PlatformData), std::move(Product.DerivedDataKey),
			{.Status = Product.bLoadedFromDerivedDataCache
					? ETextureDerivedDataStatus::Hit
					: ETextureDerivedDataStatus::Rebuilt,
				.Key = DiagnosticKey,
				.Message = Product.bLoadedFromDerivedDataCache
					? "Loaded TextureCube build candidate from DDC."
					: !Product.PersistenceDiagnostic.empty()
						? std::format("Built TextureCube from canonical faces; DDC persistence was best effort: {}",
							Product.PersistenceDiagnostic)
					: bPanorama
						? "Built TextureCube panorama candidate from normalized pixels."
						: "Built six-face TextureCube candidate from normalized pixels.",
				.bSourceDecoderInvoked = true});
		OutError.clear();
		return true;
	}

	auto BuildTextureCubePanoramaInto(
		DTextureCube& Texture,
		TextureCubeBuilder::FTexturePanoramaImage Panorama,
		const FXxHash128& SourceHash,
		const FTextureCubePanoramaBuildSettings& Settings,
		std::string& OutError) -> bool
	{
		FTextureCubeBuildProduct Product;
		return BuildTextureCubePanorama(
			std::move(Panorama), SourceHash, Settings, Product, OutError)
			&& PublishTextureCubeProduct(
				Texture, std::move(Product), {.PanoramaHash = SourceHash}, OutError);
	}

	auto BuildTextureCubePanoramaInto(
		DTextureCube& Texture,
		TextureCubeBuilder::FTexturePanoramaFloatImage Panorama,
		const FXxHash128& SourceHash,
		const FTextureCubePanoramaBuildSettings& Settings,
		std::string& OutError) -> bool
	{
		FTextureCubeBuildProduct Product;
		return BuildTextureCubePanorama(
			std::move(Panorama), SourceHash, Settings, Product, OutError)
			&& PublishTextureCubeProduct(
				Texture, std::move(Product), {.PanoramaHash = SourceHash}, OutError);
	}

	auto BuildTextureCubeFacesInto(
		DTextureCube& Texture,
		FTextureCubeSourceData SourceData,
		const std::array<FXxHash128, TextureCubeFaceCount>& SourceHashes,
		const FTextureCubeFacesBuildSettings& Settings,
		std::string& OutError) -> bool
	{
		FTextureCubeBuildProduct Product;
		return BuildTextureCubeFaces(
			std::move(SourceData), SourceHashes, Settings, Product, OutError)
			&& PublishTextureCubeProduct(
				Texture, std::move(Product), {.FaceHashes = SourceHashes}, OutError);
	}
}
