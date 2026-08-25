#include "Texture/TextureCubeBuildOperations.h"

#include "DerivedDataCache/DerivedDataBuildSession.h"
#include "Hash/XxHash.h"
#include "Texture/TextureBuildFunctionRegistry.h"
#include "Texture/TextureBuildFunctions.h"
#include "Texture/TextureCubeBuilder.h"
#include "Texture/TextureCubeDerivedData.h"

namespace Durin::Asset::Build
{
	using namespace ::Durin::DerivedData;

	namespace
	{
		auto MakeSourceFile(std::string_view Path, const FXxHash128& Hash)
			-> FTextureSourceFile
		{
			return {{.Path = std::string(Path)}, Hash.HashLow, Hash.HashHigh};
		}

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

		template<typename TPanorama>
		auto TryLoadPanoramaBuild(
			const TPanorama& Panorama,
			const FXxHash128& SourceHash,
			const FTextureCubePanoramaBuildSettings& Settings,
			FTextureCubeBuildProduct& OutProduct,
			std::string& OutError) -> bool
		{
			const FTextureCubeBuildKeyInput KeyInput{
				.SourceLayout = ETextureCubeBuildSourceLayout::EquirectangularPanorama,
				.PanoramaContentHash = SourceHash,
				.FaceDimension = Settings.FaceDimension,
				.ExposureEV = Settings.ExposureEV,
				.bSRGB = true,
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game};
			std::string Key;
			std::unique_ptr<FTextureCubePlatformData> PlatformData;
			if (!TryLoadCubeBuild(KeyInput, Key, PlatformData, OutError)) return false;
			OutProduct = {
				.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama,
				.PlatformData = std::move(PlatformData),
				.DerivedDataKey = std::move(Key),
				.SourceWidth = Panorama.Width,
				.SourceHeight = Panorama.Height,
				.PanoramaFaceDimension = Settings.FaceDimension,
				.PanoramaExposureEV = Settings.ExposureEV,
				.bSRGB = true,
				.bLoadedFromDerivedDataCache = true};
			return true;
		}

		auto ExecuteCubeBuild(FTextureCubeSourceData& SourceData,
			const FTextureCubeBuildKeyInput& KeyInput, std::string& OutKey,
			std::unique_ptr<FTextureCubePlatformData>& OutPlatformData,
			bool bQueryCache,
			std::string& OutError) -> bool
		{
			if (!EnsureTextureBuildFunctions(&OutError)) return false;
			const std::vector<std::byte> KeyBytes =
				BuildTextureCubeDerivedDataKeyBytes(KeyInput, OutError);
			OutKey = KeyBytes.empty()
				? std::string{} : FXxHash128::HashBuffer(KeyBytes).ToString();
			if (OutKey.empty()) return false;
			FBuildDefinition Definition;
			FBuildDefinitionBuilder Builder(
				Private::TextureCubeFunctionIdentity,
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
				.bStoreBuildResult = true, .bRequireStoreSuccess = true,
				.bReturnData = true});
			if (!Output.Succeeded())
			{
				OutError = Output.Diagnostic;
				return false;
			}
			auto Candidate = std::make_unique<FTextureCubePlatformData>();
			if (!Private::DecodeTextureCubePlatformValue(
				Output.Value, *Candidate, OutError)) return false;
			OutPlatformData = std::move(Candidate);
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
			const FTextureCubeBuildKeyInput KeyInput{
				.SourceLayout = ETextureCubeBuildSourceLayout::EquirectangularPanorama,
				.PanoramaContentHash = Hash,
				.FaceDimension = Settings.FaceDimension,
				.ExposureEV = Settings.ExposureEV,
				.bSRGB = true,
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game};
			std::string Key;
			std::unique_ptr<FTextureCubePlatformData> PlatformData;
			if (!ExecuteCubeBuild(
				SourceData, KeyInput, Key, PlatformData, false, OutError))
				return false;
			OutProduct = {
				.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama,
				.SourceData = std::move(SourceData),
				.PlatformData = std::move(PlatformData),
				.DerivedDataKey = std::move(Key),
				.SourceWidth = SourceWidth,
				.SourceHeight = SourceHeight,
				.PanoramaFaceDimension = Settings.FaceDimension,
				.PanoramaExposureEV = Settings.ExposureEV,
				.bSRGB = true};
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
		if (TryLoadPanoramaBuild(
			Panorama, SourceHash, Settings, OutProduct, OutError)) return true;
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
		const FTextureCubeSourceImportData& Source = Texture.GetSourceImportData();
		if (!Source.HasSource())
		{
			OutError = "TextureCube has no persisted source identity.";
			return {};
		}
		FTextureCubeBuildKeyInput Input{
			.SourceLayout = Source.SourceLayout == ETextureCubeSourceLayout::SixFaces
				? ETextureCubeBuildSourceLayout::SixFaces
				: ETextureCubeBuildSourceLayout::EquirectangularPanorama,
			.FaceDimension = Texture.GetPanoramaFaceDimension(),
			.ExposureEV = Texture.GetPanoramaExposureEV(),
			.bSRGB = Texture.IsSRGB(),
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game};
		if (Source.SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				const FTextureSourceFile& Face =
					Source.GetFace(static_cast<ETextureCubeFace>(Index));
				if (!Face.HasSource() || !Face.HasContentHash())
				{
					OutError = "TextureCube face source provenance is incomplete.";
					return {};
				}
				Input.FaceContentHashes[Index] = {
					.HashLow = Face.SourceContentHashLow,
					.HashHigh = Face.SourceContentHashHigh};
			}
		}
		else
		{
			if (!Source.Panorama.HasSource() || !Source.Panorama.HasContentHash())
			{
				OutError = "TextureCube panorama source provenance is incomplete.";
				return {};
			}
			Input.PanoramaContentHash = {
				.HashLow = Source.Panorama.SourceContentHashLow,
				.HashHigh = Source.Panorama.SourceContentHashHigh};
		}
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
			Private::TextureCubeFunctionIdentity,
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
			.bStoreBuildResult = false, .bReturnData = true});
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
		if (TryLoadPanoramaBuild(
			Panorama, SourceHash, Settings, OutProduct, OutError)) return true;
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
		const FTextureCubeBuildKeyInput KeyInput{
			.SourceLayout = ETextureCubeBuildSourceLayout::SixFaces,
			.FaceContentHashes = Hashes,
			.bSRGB = Settings.bSRGB,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game};
		std::string Key;
		std::unique_ptr<FTextureCubePlatformData> PlatformData;
		if (!ExecuteCubeBuild(
			SourceData, KeyInput, Key, PlatformData, true, OutError))
			return false;
		const uint32 SourceWidth = SourceData.Faces[0].Width;
		const uint32 SourceHeight = SourceData.Faces[0].Height;
		OutProduct = {
			.SourceLayout = ETextureCubeSourceLayout::SixFaces,
			.SourceData = std::move(SourceData),
			.PlatformData = std::move(PlatformData),
			.DerivedDataKey = std::move(Key),
			.SourceWidth = SourceWidth,
			.SourceHeight = SourceHeight,
			.bSRGB = Settings.bSRGB};
		OutError.clear();
		return true;
	}

	auto PublishTextureCubeProduct(
		DTextureCube& Texture,
		FTextureCubeBuildProduct Product,
		const FTextureCubePublicationContext& Context,
		std::string& OutError) -> bool
	{
		if (!Product.PlatformData || !Product.PlatformData->IsValid()
			|| (!Product.bLoadedFromDerivedDataCache
				&& !Product.SourceData.Faces[0].IsValid())
			|| Product.DerivedDataKey.empty())
		{
			OutError = "TextureCube publication product is incomplete.";
			return false;
		}
		FTextureCubeSourceImportData Provenance;
		Provenance.SourceLayout = Product.SourceLayout;
		Provenance.DecoderId = Context.DecoderId;
		Provenance.DecoderVersion = Context.DecoderVersion;
		Provenance.ProjectionVersion = TextureCubeProjectionVersion;
		if (Product.SourceLayout == ETextureCubeSourceLayout::SixFaces)
		{
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
				Provenance.GetMutableFace(static_cast<ETextureCubeFace>(Index)) =
					MakeSourceFile(Context.FacePaths[Index].Path, Context.FaceHashes[Index]);
		}
		else
		{
			Provenance.Panorama = MakeSourceFile(
				Context.PanoramaPath.Path, Context.PanoramaHash);
		}
		const std::string DiagnosticKey = Product.DerivedDataKey;
		const bool bPanorama = Product.SourceLayout
			== ETextureCubeSourceLayout::EquirectangularPanorama;
		std::unique_ptr<FTextureCubeSourceData> SourceData;
		if (!Product.bLoadedFromDerivedDataCache)
			SourceData = std::make_unique<FTextureCubeSourceData>(
				std::move(Product.SourceData));
		Texture.PublishAuthoringCandidate(
			Product.SourceLayout, std::move(Provenance), Product.PanoramaFaceDimension,
			Product.PanoramaExposureEV, Product.SourceWidth, Product.SourceHeight,
			Product.bSRGB,
			std::move(SourceData),
			std::move(Product.PlatformData), std::move(Product.DerivedDataKey),
			{.Status = Product.bLoadedFromDerivedDataCache
					? ETextureDerivedDataStatus::Hit
					: ETextureDerivedDataStatus::Rebuilt,
				.Key = DiagnosticKey,
				.Message = Product.bLoadedFromDerivedDataCache
					? "Loaded TextureCube authoring candidate from DDC."
					: bPanorama
						? "Built TextureCube panorama candidate from normalized pixels."
						: "Built six-face TextureCube candidate from normalized pixels.",
				.bSourceDecoderInvoked = true});
		OutError.clear();
		return true;
	}
}
