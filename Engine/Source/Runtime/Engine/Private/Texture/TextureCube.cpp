#include "Texture/TextureCube.h"
#include "Logging/LogMacros.h"
#include "Texture/TextureCookedData.h"

#include "DObject/Package.h"

#include "Asset/AssetCook.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DynamicRHI.h"
#include "Hash/XxHash.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"
#include "Texture/TextureCubeBuildProvider.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Texture/TextureDerivedData.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};

		auto MakeTextureCubeImportedData(const FTextureSource& Source)
			-> FTextureCubeImportedData
		{
			FTextureCubeImportedData Result;
			if (!Source.IsValid() || Source.GetKind() != ETextureSourceKind::TextureCube)
				return Result;
			const FTextureSource::FMipData Mips = Source.GetMipData();
			if (!Mips.IsValid()
				|| !Result.Pixels.UpdatePayload(Mips.GetData())) return {};
			Result.FaceDimension = Source.GetWidth();
			Result.SourceChannelCount = Source.GetSourceChannelCount();
			Result.TransparencyMask = Source.GetTransparencyMask();
			Result.CanonicalSourceIdentity = Source.GetIdentity();
			return Result.IsValid() ? std::move(Result) : FTextureCubeImportedData{};
		}

		auto MakeTextureCubeBuildRequest(const DTextureCube& Texture,
			FTextureCubeBuildRequest& OutRequest, std::string& OutError) -> bool
		{
			const FTextureSource& Source = Texture.GetSource();
			if (Source.GetKind() == ETextureSourceKind::TextureCube)
			{
				FTextureCubeImportedData Imported = MakeTextureCubeImportedData(Source);
				if (!Imported.IsValid()) return false;
				OutRequest.Input = FTextureCubeFacesBuildInput{
					.ImportedData = std::move(Imported),
					.SourceLayout = ETextureCubeSourceLayout::SixFaces,
					.OriginalSourceWidth = Texture.GetOriginalSourceWidth(),
					.OriginalSourceHeight = Texture.GetOriginalSourceHeight(),
					.Settings = {.bSRGB = Texture.IsSRGB()}};
				return true;
			}
			if (Source.GetKind() != ETextureSourceKind::LongLatCube) return false;
			const FTextureSource::FMipData Mips = Source.GetMipData();
			const Image::FImageView View = Mips.GetMipImage(0, 0, 0);
			if (!View.IsValid())
			{
				OutError = "TextureCube panorama source payload could not be loaded.";
				return false;
			}
			const auto& Info = View.GetInfo();
			FTextureCubePanoramaBuildInput Panorama;
			Panorama.Settings = {.FaceDimension = Texture.GetPanoramaFaceDimension(),
				.ExposureEV = Texture.GetPanoramaExposureEV()};
			if (Info.Format == Image::ERawImageFormat::RGBA32F)
			{
				FTextureCubePanoramaFloatImage Image;
				Image.Width = Info.Width;
				Image.Height = Info.Height;
				const size_t PixelCount = static_cast<size_t>(Info.Width) * Info.Height;
				Image.Pixels.resize(PixelCount * 3);
				for (size_t Index = 0; Index < PixelCount; ++Index)
					std::memcpy(Image.Pixels.data() + Index * 3,
						View.GetPixels().data() + Index * 4 * sizeof(float),
						3 * sizeof(float));
				Panorama.Image = std::move(Image);
			}
			else if (Info.Format == Image::ERawImageFormat::RGBA8)
			{
				FTextureCubePanoramaImage Image;
				Image.Width = Info.Width;
				Image.Height = Info.Height;
				Image.SourceChannelCount = Source.GetSourceChannelCount();
				Image.bHasTransparency = Source.HasTransparency();
				Image.Pixels.assign(View.GetPixels().begin(), View.GetPixels().end());
				Panorama.Image = std::move(Image);
			}
			else
			{
				OutError = "TextureCube panorama source format is unsupported by the projection recipe.";
				return false;
			}
			OutRequest.Input = std::move(Panorama);
			return true;
		}

		auto ValidateCubeSourceData(const FTextureCubeSourceData& SourceData, std::string& OutError) -> bool
		{
			if ((SourceData.TransparencyMask & ~0x3fu) != 0
				|| SourceData.SourceChannelCounts[0] == 0 || SourceData.SourceChannelCounts[0] > 4)
			{
				OutError = "Cube face import metadata is invalid.";
				return false;
			}
			const Image::FImage& Reference = SourceData.Faces[0];
			for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			{
				const Image::FImage& Face = SourceData.Faces[FaceIndex];
				if (!Face.IsValid() || Face.GetInfo().Format != Image::ERawImageFormat::RGBA8
					|| Face.GetInfo().Depth != 1 || Face.GetInfo().SliceCount != 1
					|| Face.GetInfo().Width > 16384 || Face.GetInfo().Height > 16384
					|| Face.GetInfo().GammaSpace != Image::EImageGammaSpace::Unknown)
				{
					OutError = std::format("{} face source data is invalid.", FaceNames[FaceIndex]);
					return false;
				}
				if (Face.GetInfo().Width != Face.GetInfo().Height)
				{
					OutError = std::format("{} face must be square, but is {}x{}.",
						FaceNames[FaceIndex], Face.GetInfo().Width, Face.GetInfo().Height);
					return false;
				}
				if (Face.GetInfo().Width != Reference.GetInfo().Width || Face.GetInfo().Height != Reference.GetInfo().Height)
				{
					OutError = std::format("{} face dimensions {}x{} do not match PositiveX {}x{}; all faces must be identical.",
						FaceNames[FaceIndex], Face.GetInfo().Width, Face.GetInfo().Height, Reference.GetInfo().Width, Reference.GetInfo().Height);
					return false;
				}
				if (SourceData.SourceChannelCounts[FaceIndex] != SourceData.SourceChannelCounts[0])
				{
					OutError = std::format("{} face source channel count {} does not match PositiveX {}; all faces must use an identical source format.",
						FaceNames[FaceIndex], SourceData.SourceChannelCounts[FaceIndex], SourceData.SourceChannelCounts[0]);
					return false;
				}
			}
			return true;
		}

	}

	auto FTextureCubeSourceData::IsValid() const -> bool
	{
		std::string Error;
		return ValidateCubeSourceData(*this, Error);
	}

	auto FTextureCubeImportedData::IsValid() const -> bool
	{
		const uint64 ExpectedByteCount = static_cast<uint64>(FaceDimension)
			* FaceDimension * 4ull * TextureCubeFaceCount;
		return SchemaVersion == TextureCubeImportedDataSchemaVersion
			&& FaceDimension > 0 && FaceDimension <= 16384
			&& SourceChannelCount > 0 && SourceChannelCount <= 4
			&& ExpectedByteCount == Pixels.GetPayloadSize()
			&& ExpectedByteCount <= MaximumTextureCubeImportedPixelBytes
			&& (TransparencyMask & ~0x3fu) == 0;
	}

	auto FTextureCubeImportedData::SetSourceData(
		const FTextureCubeSourceData& Source) -> bool
	{
		if (!Source.IsValid()) return false;
		FByteBuffer Bytes;
		const uint64 TotalBytes = static_cast<uint64>(Source.Faces[0].GetPixels().size())
			* TextureCubeFaceCount;
		if (TotalBytes > MaximumTextureCubeImportedPixelBytes) return false;
		Bytes.reserve(static_cast<size_t>(TotalBytes));
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			const Image::FImage& Face = Source.Faces[Index];
			Bytes.insert(Bytes.end(), Face.GetPixels().begin(), Face.GetPixels().end());
		}
		if (!Pixels.UpdatePayload(Bytes)) return false;
		FaceDimension = Source.Faces[0].GetInfo().Width;
		SourceChannelCount = Source.SourceChannelCounts[0];
		TransparencyMask = Source.TransparencyMask;
		SchemaVersion = TextureCubeImportedDataSchemaVersion;
		FTextureSource Canonical;
		const FTextureSourceBlock Block{.Width = FaceDimension,
			.Height = FaceDimension, .NumSlices = TextureCubeFaceCount};
		const FTextureSourceLayer Layer{.Format = ETextureSourceFormat::RGBA8};
		if (!Canonical.InitLayered(ETextureSourceKind::TextureCube,
			std::span(&Block, 1), std::span(&Layer, 1),
			ETextureSourceGammaSpace::Unknown, Bytes, SourceChannelCount,
			TransparencyMask, ETextureSourceCompression::Raw)) return false;
		CanonicalSourceIdentity = Canonical.GetIdentity();
		return IsValid();
	}

	auto FTextureCubeImportedData::ToSourceData() const -> FTextureCubeSourceData
	{
		FTextureCubeSourceData Result;
		if (!IsValid()) return Result;
		const FPackageResourceReadResult Read = Pixels.GetPayload().Wait();
		if (!Read || Read.Buffer.GetSize() != Pixels.GetPayloadSize()) return {};
		const uint64 FaceBytes = static_cast<uint64>(FaceDimension) * FaceDimension * 4;
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			if (!Image::FImage::TryCreate({.Width = FaceDimension, .Height = FaceDimension,
				.Format = Image::ERawImageFormat::RGBA8},
				Read.Buffer.MakeView(Index * FaceBytes, FaceBytes), Result.Faces[Index])) return {};
		}
		Result.SourceChannelCounts.fill(SourceChannelCount);
		Result.TransparencyMask = TransparencyMask;
		return Result;
	}

	auto FTextureCubeImportedData::GetIdentity() const -> FXxHash128
	{
		if (!IsValid()) return {};
		if (!CanonicalSourceIdentity.IsZero()) return CanonicalSourceIdentity;
		FXxHash128Builder Builder;
		Builder.UpdateValue(SchemaVersion);
		Builder.UpdateValue(FaceDimension);
		Builder.UpdateValue(SourceChannelCount);
		Builder.UpdateValue(TransparencyMask);
		Builder.UpdateValue(Pixels.GetPayloadId());
		return Builder.Finalize();
	}

	auto FTextureCubePlatformData::IsValid() const -> bool
	{
		if (PixelFormat == EPixelFormat::Unknown) return false;
		const FTexturePlatformData& Reference = Faces[0];
		if (!Reference.IsValid() || Reference.PixelFormat != PixelFormat || Reference.Mips.front().Width != Reference.Mips.front().Height)
			return false;
		for (const FTexturePlatformData& Face : Faces)
		{
			if (!Face.IsValid() || Face.PixelFormat != PixelFormat || Face.Mips.size() != Reference.Mips.size()) return false;
			for (size_t MipIndex = 0; MipIndex < Face.Mips.size(); ++MipIndex)
			{
				if (Face.Mips[MipIndex].Width != Reference.Mips[MipIndex].Width
					|| Face.Mips[MipIndex].Height != Reference.Mips[MipIndex].Height
					|| Face.Mips[MipIndex].RowPitch != Reference.Mips[MipIndex].RowPitch) return false;
			}
		}
		return true;
	}

	DTextureCube::DTextureCube(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{}

	DTextureCube::~DTextureCube() = default;

	auto DTextureCube::SerializeCooked(FArchive& Ar) -> void
	{
		Super::SerializeCooked(Ar);
		TexturePrivate::SerializeCookedPlatformData(Ar, GetMutableCookedPlatformData(),
			PlatformData.get(), FName("Durin::DTextureCube"), "TextureCube");
	}

	auto DTextureCube::GetBuiltFaceDimension() const -> uint32
	{
		if (PlatformData && !PlatformData->Faces[0].Mips.empty())
			return PlatformData->Faces[0].Mips[0].Width;
		return 0;
	}

	auto DTextureCube::GetBuiltMipCount() const -> uint32
	{
		return PlatformData && PlatformData->IsValid()
			? static_cast<uint32>(PlatformData->Faces[0].Mips.size()) : 0;
	}

	auto DTextureCube::GetBuiltPixelFormat() const -> EPixelFormat
	{
		return PlatformData && PlatformData->IsValid() ? PlatformData->PixelFormat : EPixelFormat::Unknown;
	}

	auto DTextureCube::SetPlatformData(
		std::unique_ptr<FTextureCubePlatformData> Data) -> void
	{
		CheckGameThread();
		PlatformData = std::move(Data);
	}

	auto DTextureCube::CreateRenderResourceCandidate(
		FTextureReference* TextureReference)
		-> std::unique_ptr<FTextureResource>
	{
		check(PlatformData && PlatformData->IsValid());
		return std::make_unique<FTextureCubeResource>(
			TextureReference,
			std::make_shared<const FTextureCubePlatformData>(*PlatformData));
	}

	auto DTextureCube::RebuildPlatformData() -> bool
	{
		std::string Error;
		FTextureCubeBuildRequest Request;
		if (!MakeTextureCubeBuildRequest(*this, Request, Error))
		{
			DURIN_ERROR("RebuildPlatformData '{}': {}", GetObjectPath(), Error);
			return false;
		}
		const auto Result = BuildTextureCubeSynchronously(*this, Request, {});
		if (!Result) DURIN_ERROR("RebuildPlatformData '{}': {}", GetObjectPath(), Result.Diagnostic);
		return static_cast<bool>(Result);
	}

	auto DTextureCube::PostLoad() -> void
	{
		std::string Error;
		BindTextureSourceOwner();
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (GetCookedPlatformData().GetMetadata().LogicalSize == 0)
			{
				Error = std::format(
					"Cooked TextureCube '{}': required PlatformData field is missing.",
					GetObjectPath());
				DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
				return;
			}
			PlatformData.reset();
			return;
		}
		if (GetSource().GetSchemaVersion() != TextureSourceSchemaVersion)
		{
			FTextureSource Migrated = GetSource();
			if (!Migrated.MigrateLegacy())
			{
				Error = "Texture source migration failed.";
				DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
				return;
			}
			if (!SetSource(std::move(Migrated), Error))
			{
				DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
				return;
			}
		}
		FTextureCubeBuildRequest Request;
		if (!MakeTextureCubeBuildRequest(*this, Request, Error))
		{
			if (Error.empty()) Error = "TextureCube source data is missing or invalid.";
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
			return;
		}
		auto BuildResult = BuildTextureCubeSynchronously(*this, Request, {.bMarkPackageDirty = false, .bSourceDecoderInvoked = false});
		Error = BuildResult.Diagnostic;
		if (!BuildResult)
		{
			DURIN_ERROR("PostLoad '{}': {}", GetObjectPath(), Error);
		}
	}

	auto DTextureCube::LoadCookedPlatformData(std::string& OutError) -> bool
	{
		return TexturePrivate::LoadCookedPlatformData<FTextureCubePlatformData>(
			*this, GetMutableCookedPlatformData(), "TextureCube", OutError);
	}

	auto DTextureCube::ContributeToCook(
		FCookContext& Context,
		std::string_view VirtualPackagePath,
		std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game)
		{
			OutError = std::format(
				"TextureCube '{}' supports only the Win64 game cook target.", GetObjectPath());
			return false;
		}
		if (!HasPlatformData()) PostLoad();
		if (!HasPlatformData())
		{
			OutError = std::format("Failed to cook TextureCube '{}': platform data is unavailable.",
				GetObjectPath());
			return false;
		}
		return Context.AddPackage(
			std::string(VirtualPackagePath), GetPackage(), &OutError);
	}

	auto DTextureCube::SetSourceData(
		const FTextureCubeImportedData& Value, std::string& OutError) -> bool
	{
		CheckGameThread();
		if (!Value.IsValid())
		{
			OutError = "TextureCube source data is invalid.";
			return false;
		}
		const FPackageResourceReadResult Read = Value.Pixels.GetPayload().Wait();
		if (!Read)
		{
			OutError = "TextureCube source payload could not be read.";
			return false;
		}
		FTextureSource NewSource;
		const FTextureSourceBlock Block{.Width = Value.FaceDimension,
			.Height = Value.FaceDimension, .NumSlices = TextureCubeFaceCount};
		const FTextureSourceLayer Layer{.Format = ETextureSourceFormat::RGBA8};
		if (!NewSource.InitLayeredImpl(ETextureSourceKind::TextureCube,
			std::span(&Block, 1), std::span(&Layer, 1),
			ETextureSourceGammaSpace::Unknown, Read.Buffer.GetBytes(),
			Value.SourceChannelCount, Value.TransparencyMask,
			ETextureSourceCompression::Raw))
		{
			OutError = "TextureCube source data could not be initialized.";
			return false;
		}
		return SetSource(std::move(NewSource), OutError);
	}

	auto DTextureCube::SetPanoramaSourceData(Image::FImageView Value,
		uint8 SourceChannelCount, uint8 TransparencyMask,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		FTextureSource NewSource;
		if (!NewSource.InitLongLatCube(Value, SourceChannelCount,
			TransparencyMask, ETextureSourceCompression::Raw))
		{
			OutError = "TextureCube panorama source data could not be initialized.";
			return false;
		}
		return SetSource(std::move(NewSource), OutError);
	}

	auto DTextureCube::ValidateSettingsAfterImportOrEdit(
		const FTextureSource& ProposedSource) const -> bool
	{
		const auto Blocks = ProposedSource.GetBlocks();
		const auto Layers = ProposedSource.GetLayers();
		if (Blocks.size() != 1 || Layers.size() != 1) return false;
		const bool bFaces = ProposedSource.GetKind() == ETextureSourceKind::TextureCube
			&& Blocks[0].Width == Blocks[0].Height
			&& Blocks[0].NumSlices == TextureCubeFaceCount
			&& Layers[0].Format == ETextureSourceFormat::RGBA8;
		const bool bPanorama = ProposedSource.GetKind() == ETextureSourceKind::LongLatCube
			&& Blocks[0].NumSlices == 1
			&& (Layers[0].Format == ETextureSourceFormat::RGBA8
				|| Layers[0].Format == ETextureSourceFormat::RGBA32_FLOAT);
		return (bFaces || bPanorama)
			&& Blocks[0].Depth == 1
			&& Layers[0].NumMips == 1
			&& (SourceLayout == ETextureCubeSourceLayout::SixFaces
				|| SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
			&& std::isfinite(PanoramaExposureEV)
			&& PanoramaExposureEV >= MinimumTextureCubePanoramaExposureEV
			&& PanoramaExposureEV <= MaximumTextureCubePanoramaExposureEV;
	}

	auto DTextureCube::SetBuildSettings(
		ETextureCubeSourceLayout InSourceLayout,
		uint32 InPanoramaFaceDimension,
		float InPanoramaExposureEV,
		uint32 InOriginalSourceWidth,
		uint32 InOriginalSourceHeight,
		bool bInSRGB,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		if ((InSourceLayout != ETextureCubeSourceLayout::SixFaces
				&& InSourceLayout != ETextureCubeSourceLayout::EquirectangularPanorama)
			|| !std::isfinite(InPanoramaExposureEV)
			|| InPanoramaExposureEV < MinimumTextureCubePanoramaExposureEV
			|| InPanoramaExposureEV > MaximumTextureCubePanoramaExposureEV
			|| InOriginalSourceWidth == 0 || InOriginalSourceHeight == 0)
		{
			OutError = "TextureCube authored build settings are invalid.";
			return false;
		}
		SourceLayout = InSourceLayout;
		PanoramaFaceDimension = InPanoramaFaceDimension;
		PanoramaExposureEV = InPanoramaExposureEV;
		OriginalSourceWidth = InOriginalSourceWidth;
		OriginalSourceHeight = InOriginalSourceHeight;
		bSRGB = bInSRGB;
		InvalidateAuthoredBuild();
		OutError.clear();
		return true;
	}
}
