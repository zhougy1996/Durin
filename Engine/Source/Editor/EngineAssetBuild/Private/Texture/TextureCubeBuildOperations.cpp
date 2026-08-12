#include "Texture/TextureCubeBuildOperations.h"

#include "DerivedDataObjectStore.h"
#include "Hash/XxHash.h"
#include "ImageDecoder.h"
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
					OutError = std::format("{} face source layout does not match PositiveX.",
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

		auto PublishPanoramaProduct(
			DTextureCube& Texture,
			FTextureCubeSourceData SourceData,
			uint32 SourceWidth,
			uint32 SourceHeight,
			const FXxHash128& Hash,
			const FSourcePath& SourcePath,
			const FTextureCubePanoramaImportSettings& Settings,
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
			FTextureCubeSourceImportData Provenance{
				.SourceLayout = ETextureCubeSourceLayout::EquirectangularPanorama,
				.Panorama = MakeSourceFile(SourcePath.Path, Hash),
				.DecoderId = "DurinImage",
				.DecoderVersion = 1,
				.ProjectionVersion = TextureCubeProjectionVersion};
			const std::string DiagnosticKey = Key;
			Texture.PublishAuthoringCandidate(
				ETextureCubeSourceLayout::EquirectangularPanorama,
				std::move(Provenance), Settings.FaceDimension, Settings.ExposureEV,
				SourceWidth, SourceHeight, true,
				std::make_unique<FTextureCubeSourceData>(std::move(SourceData)),
				std::move(PlatformData), std::move(Key),
				{.Status = ETextureDerivedDataStatus::Rebuilt,
					.Key = DiagnosticKey,
					.Message = "Built TextureCube panorama candidate from normalized pixels.",
					.bSourceDecoderInvoked = true});
			OutError.clear();
			return true;
		}
	}

	auto BuildTextureCubePanorama(
		DTextureCube& Texture,
		Asset::FDecodedImage Panorama,
		const FXxHash128& SourceHash,
		const FSourcePath& SourcePath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		FTextureCubeSourceData SourceData;
		if (!TextureCubeBuilder::ProjectEquirectangularTextureCube(
			Panorama, {Settings.FaceDimension, Settings.ExposureEV}, SourceData, OutError))
			return false;
		return PublishPanoramaProduct(Texture, std::move(SourceData), Panorama.Width,
			Panorama.Height, SourceHash, SourcePath, Settings, OutError);
	}

	auto BuildTextureCubePanorama(
		DTextureCube& Texture,
		Asset::FDecodedFloatImage Panorama,
		const FXxHash128& SourceHash,
		const FSourcePath& SourcePath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		FTextureCubeSourceData SourceData;
		if (!TextureCubeBuilder::ProjectEquirectangularTextureCube(
			Panorama, {Settings.FaceDimension, Settings.ExposureEV}, SourceData, OutError))
			return false;
		return PublishPanoramaProduct(Texture, std::move(SourceData), Panorama.Width,
			Panorama.Height, SourceHash, SourcePath, Settings, OutError);
	}

	auto BuildTextureCubePanoramaFromEncodedBytes(
		DTextureCube& Texture,
		std::span<const uint8> EncodedBytes,
		std::string_view ExtensionHint,
		const FSourcePath& SourcePath,
		const FTextureCubePanoramaImportSettings& Settings,
		std::string& OutError) -> bool
	{
		if (EncodedBytes.empty() || SourcePath.IsEmpty())
		{
			OutError = "Panorama candidate requires captured bytes and mounted provenance.";
			return false;
		}
		const FXxHash128 Hash = FXxHash128::HashBuffer(EncodedBytes);
		std::string Extension(ExtensionHint);
		std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) {
			return static_cast<char>(std::tolower(Character));
		});
		if (Asset::IsRadianceHDRExtension(Extension))
		{
			Asset::FDecodedFloatImage Panorama;
			Asset::FRadianceHDRDecodeLimits Limits;
			Limits.MaximumDecodedPixels = TextureCubeBuilder::MaximumPanoramaPixels;
			if (!Asset::DecodeRadianceHDRFromMemory(EncodedBytes, Panorama, OutError, Limits)
				) return false;
			return BuildTextureCubePanorama(
				Texture, std::move(Panorama), Hash, SourcePath, Settings, OutError);
		}
		else
		{
			Asset::FDecodedImage Panorama;
			Asset::FImageDecodeLimits Limits;
			Limits.MaximumDecodedPixels = TextureCubeBuilder::MaximumPanoramaPixels;
			if (!Asset::DecodeImageFromMemory(EncodedBytes, Panorama, OutError, Limits)
				) return false;
			return BuildTextureCubePanorama(
				Texture, std::move(Panorama), Hash, SourcePath, Settings, OutError);
		}
	}

	auto BuildTextureCubeFaces(
		DTextureCube& Texture,
		FTextureCubeSourceData SourceData,
		const std::array<FXxHash128, TextureCubeFaceCount>& Hashes,
		const std::array<FSourcePath, TextureCubeFaceCount>& SourcePaths,
		const FTextureCubeImportSettings& Settings,
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
		FTextureCubeSourceImportData Provenance;
		Provenance.SourceLayout = ETextureCubeSourceLayout::SixFaces;
		Provenance.DecoderId = "DurinImage";
		Provenance.DecoderVersion = 1;
		Provenance.ProjectionVersion = TextureCubeProjectionVersion;
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
			Provenance.GetMutableFace(static_cast<ETextureCubeFace>(Index)) =
				MakeSourceFile(SourcePaths[Index].Path, Hashes[Index]);
		const uint32 SourceWidth = SourceData.Faces[0].Width;
		const uint32 SourceHeight = SourceData.Faces[0].Height;
		const std::string DiagnosticKey = Key;
		Texture.PublishAuthoringCandidate(
			ETextureCubeSourceLayout::SixFaces, std::move(Provenance), 0, 0.0f,
			SourceWidth, SourceHeight, Settings.bSRGB,
			std::make_unique<FTextureCubeSourceData>(std::move(SourceData)),
			std::move(PlatformData), std::move(Key),
			{.Status = ETextureDerivedDataStatus::Rebuilt,
				.Key = DiagnosticKey,
				.Message = "Built six-face TextureCube candidate from normalized pixels.",
				.bSourceDecoderInvoked = true});
		OutError.clear();
		return true;
	}

	auto BuildTextureCubeFacesFromEncodedBytes(
		DTextureCube& Texture,
		const std::array<std::span<const uint8>, TextureCubeFaceCount>& EncodedFaces,
		const std::array<FSourcePath, TextureCubeFaceCount>& SourcePaths,
		const FTextureCubeImportSettings& Settings,
		std::string& OutError) -> bool
	{
		auto SourceData = std::make_unique<FTextureCubeSourceData>();
		std::array<FXxHash128, TextureCubeFaceCount> Hashes;
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			if (EncodedFaces[Index].empty() || SourcePaths[Index].IsEmpty()
				|| !TextureBuilder::DecodeRGBA8(
					EncodedFaces[Index], SourceData->Faces[Index], OutError))
			{
				if (OutError.empty())
					OutError = std::format("{} face source is invalid.", FaceNames[Index]);
				return false;
			}
			Hashes[Index] = FXxHash128::HashBuffer(EncodedFaces[Index]);
		}
		return BuildTextureCubeFaces(
			Texture, std::move(*SourceData), Hashes, SourcePaths, Settings, OutError);
	}
}
