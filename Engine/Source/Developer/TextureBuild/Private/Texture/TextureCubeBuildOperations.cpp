#include "Texture/TextureCubeBuildOperations.h"

#include "DerivedDataObjectStore.h"
#include "Hash/XxHash.h"
#include "Serialization/Archive.h"
#include "Texture/TextureBuilder.h"
#include "Texture/TextureCubeBuilder.h"
#include "Texture/TextureCubeDerivedData.h"

namespace Durin::AssetBuild
{
	namespace
	{
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};

		auto MakeSourceFile(std::string_view Path, const FXxHash128& Hash)
			-> FTextureSourceFile
		{
			return {{.Path = std::string(Path)}, Hash.HashLow, Hash.HashHigh};
		}

		auto ValidateCubeSourceData(
			const FTextureCubeSourceData& SourceData,
			std::string& OutError) -> bool
		{
			const FTextureSourceData& Reference = SourceData.Faces[0];
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				const FTextureSourceData& Face = SourceData.Faces[Index];
				if (!Face.IsValid())
				{
					OutError = std::format("{} face source data is invalid.", FaceNames[Index]);
					return false;
				}
				if (Face.Width != Face.Height)
				{
					OutError = std::format("{} face must be square, but is {}x{}.",
						FaceNames[Index], Face.Width, Face.Height);
					return false;
				}
				if (Face.Width != Reference.Width || Face.Height != Reference.Height
					|| Face.SourceChannelCount != Reference.SourceChannelCount)
				{
					OutError = std::format("{} face source layout must be identical to PositiveX.",
						FaceNames[Index]);
					return false;
				}
			}
			return true;
		}

		auto BuildCubePlatformData(
			const FTextureCubeSourceData& SourceData,
			bool bSRGB,
			FTextureCubePlatformData& OutPlatformData,
			std::string& OutError) -> bool
		{
			if (!ValidateCubeSourceData(SourceData, OutError)) return false;
			const bool bHasTransparency = std::ranges::any_of(
				SourceData.Faces, [](const FTextureSourceData& Face) {
					return Face.bHasTransparency;
				});
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				FTextureSourceData BuildSource = SourceData.Faces[Index];
				BuildSource.bHasTransparency = bHasTransparency;
				if (!TextureBuilder::BuildMipChain(
					BuildSource, ETextureUsage::Color, bSRGB,
					OutPlatformData.Faces[Index], OutError))
				{
					OutError = std::format(
						"{} face platform build failed: {}", FaceNames[Index], OutError);
					return false;
				}
			}
			OutPlatformData.PixelFormat = OutPlatformData.Faces[0].PixelFormat;
			if (OutPlatformData.IsValid()) return true;
			OutError = "Cube texture platform data is inconsistent.";
			return false;
		}

		auto StoreDerivedData(
			std::string_view Key,
			const FTextureCubePlatformData& PlatformData,
			std::string& OutError) -> bool
		{
			std::vector<uint8> Bytes;
			FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
			const_cast<FTextureCubePlatformData&>(PlatformData).Serialize(Ar, {
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game});
			if (Ar.HasError())
			{
				OutError = Ar.GetFailure()->Message;
				return false;
			}
			return Asset::FDerivedDataObjectStore(
				"TextureCube/Objects", MaximumTexturePayloadBytes).Write(
					Key, Bytes, &OutError);
		}

		auto FinishPanoramaProduct(
			FTextureCubeSourceData SourceData,
			uint32 SourceWidth,
			uint32 SourceHeight,
			const FXxHash128& Hash,
			const FTextureCubePanoramaImportSettings& Settings,
			FTextureCubeBuildProduct& OutProduct,
			std::string& OutError) -> bool
		{
			auto PlatformData = std::make_unique<FTextureCubePlatformData>();
			if (!BuildCubePlatformData(SourceData, true, *PlatformData, OutError)) return false;
			std::string Key = BuildTextureCubeDerivedDataKey({
				.SourceLayout = ETextureCubeBuildSourceLayout::EquirectangularPanorama,
				.PanoramaContentHash = Hash,
				.FaceDimension = Settings.FaceDimension,
				.ExposureEV = Settings.ExposureEV,
				.bSRGB = true,
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game}, OutError);
			if (Key.empty() || !StoreDerivedData(Key, *PlatformData, OutError)) return false;
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
		const FTextureCubePanoramaImportSettings& Settings,
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
		std::vector<uint8> Bytes;
		const Asset::FDerivedDataObjectReadResult Read = Asset::FDerivedDataObjectStore(
			"TextureCube/Objects", MaximumTexturePayloadBytes).Read(Key, Bytes);
		if (!Read)
		{
			OutStatus = Read.Status == Asset::EDerivedDataObjectReadStatus::Missing
				? ETextureDerivedDataStatus::Missing : ETextureDerivedDataStatus::Corrupt;
			OutMessage = Read.Message;
			return false;
		}
		auto Candidate = std::make_unique<FTextureCubePlatformData>();
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload);
		Candidate->Serialize(Ar, {
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (Ar.HasError())
		{
			OutStatus = Ar.GetFailure()->Code == EArchiveFailureCode::UnsupportedVersion
				? ETextureDerivedDataStatus::Incompatible : ETextureDerivedDataStatus::Corrupt;
			OutMessage = Ar.GetFailure()->Message;
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
		const FTextureCubePanoramaImportSettings& Settings,
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
		const FTextureCubeImportSettings& Settings,
		FTextureCubeBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		auto PlatformData = std::make_unique<FTextureCubePlatformData>();
		if (!BuildCubePlatformData(SourceData, Settings.bSRGB, *PlatformData, OutError)) return false;
		std::string Key = BuildTextureCubeDerivedDataKey({
			.SourceLayout = ETextureCubeBuildSourceLayout::SixFaces,
			.FaceContentHashes = Hashes,
			.bSRGB = Settings.bSRGB,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game}, OutError);
		if (Key.empty() || !StoreDerivedData(Key, *PlatformData, OutError)) return false;
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
			|| !Product.SourceData.Faces[0].IsValid() || Product.DerivedDataKey.empty())
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
		Texture.PublishAuthoringCandidate(
			Product.SourceLayout, std::move(Provenance), Product.PanoramaFaceDimension,
			Product.PanoramaExposureEV, Product.SourceWidth, Product.SourceHeight,
			Product.bSRGB,
			std::make_unique<FTextureCubeSourceData>(std::move(Product.SourceData)),
			std::move(Product.PlatformData), std::move(Product.DerivedDataKey),
			{.Status = ETextureDerivedDataStatus::Rebuilt,
				.Key = DiagnosticKey,
				.Message = bPanorama
					? "Built TextureCube panorama candidate from normalized pixels."
					: "Built six-face TextureCube candidate from normalized pixels.",
				.bSourceDecoderInvoked = true});
		OutError.clear();
		return true;
	}

}
