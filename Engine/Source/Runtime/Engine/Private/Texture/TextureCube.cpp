#include "Texture/TextureCube.h"
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
			Result.Pixels = Source.GetBulkData();
			Result.FaceDimension = Source.GetWidth();
			Result.SourceChannelCount = Source.GetSourceChannelCount();
			Result.TransparencyMask = Source.GetTransparencyMask();
			return Result.IsValid() ? std::move(Result) : FTextureCubeImportedData{};
		}

		auto ValidateCubeSourceData(const FTextureCubeSourceData& SourceData, std::string& OutError) -> bool
		{
			const FTextureSourceData& Reference = SourceData.Faces[0];
			for (size_t FaceIndex = 0; FaceIndex < TextureCubeFaceCount; ++FaceIndex)
			{
				const FTextureSourceData& Face = SourceData.Faces[FaceIndex];
				if (!Face.IsValid())
				{
					OutError = std::format("{} face source data is invalid.", FaceNames[FaceIndex]);
					return false;
				}
				if (Face.Width != Face.Height)
				{
					OutError = std::format("{} face must be square, but is {}x{}.",
						FaceNames[FaceIndex], Face.Width, Face.Height);
					return false;
				}
				if (Face.Width != Reference.Width || Face.Height != Reference.Height)
				{
					OutError = std::format("{} face dimensions {}x{} do not match PositiveX {}x{}; all faces must be identical.",
						FaceNames[FaceIndex], Face.Width, Face.Height, Reference.Width, Reference.Height);
					return false;
				}
				if (Face.SourceChannelCount != Reference.SourceChannelCount)
				{
					OutError = std::format("{} face source channel count {} does not match PositiveX {}; all faces must use an identical source format.",
						FaceNames[FaceIndex], Face.SourceChannelCount, Reference.SourceChannelCount);
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
		FByteArray Bytes;
		const uint64 TotalBytes = static_cast<uint64>(Source.Faces[0].Pixels.size())
			* TextureCubeFaceCount;
		if (TotalBytes > MaximumTextureCubeImportedPixelBytes) return false;
		Bytes.reserve(static_cast<size_t>(TotalBytes));
		uint8 NewTransparencyMask = 0;
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			const FTextureSourceData& Face = Source.Faces[Index];
			Bytes.insert(Bytes.end(), Face.Pixels.begin(), Face.Pixels.end());
			if (Face.bHasTransparency) NewTransparencyMask |= static_cast<uint8>(1u << Index);
		}
		if (!Pixels.UpdatePayload(Bytes)) return false;
		FaceDimension = Source.Faces[0].Width;
		SourceChannelCount = Source.Faces[0].SourceChannelCount;
		TransparencyMask = NewTransparencyMask;
		SchemaVersion = TextureCubeImportedDataSchemaVersion;
		return IsValid();
	}

	auto FTextureCubeImportedData::ToSourceData() const -> FTextureCubeSourceData
	{
		FTextureCubeSourceData Result;
		if (!IsValid()) return Result;
		const FSharedByteBuffer Payload = Pixels.GetPayload().Wait().Buffer;
		const std::span<const std::byte> Bytes = Payload.GetBytes();
		const size_t FaceBytes = static_cast<size_t>(FaceDimension) * FaceDimension * 4;
		for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
		{
			const auto Face = Bytes.subspan(Index * FaceBytes, FaceBytes);
			Result.Faces[Index] = {
				.Pixels = FByteArray(Face.begin(), Face.end()),
				.Width = FaceDimension,
				.Height = FaceDimension,
				.SourceChannelCount = SourceChannelCount,
				.Format = ETextureSourceFormat::RGBA8,
				.bHasTransparency = (TransparencyMask & (1u << Index)) != 0};
		}
		return Result;
	}

	auto FTextureCubeImportedData::GetIdentity() const -> FXxHash128
	{
		if (!IsValid()) return {};
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
		std::unique_ptr<FTextureCubePlatformData> Data,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		if (!Data || !Data->IsValid())
		{
			OutError = "TextureCube platform data must be complete and valid.";
			return false;
		}
		PlatformData = std::move(Data);
		OutError.clear();
		return true;
	}

	auto DTextureCube::CreateRenderResourceCandidate(
		FTextureReference* TextureReference,
		uint64 Revision,
		const std::shared_ptr<FTextureResourceCompletion>& Completion)
		-> std::unique_ptr<FTextureAssetResource>
	{
		check(PlatformData && PlatformData->IsValid());
		return std::make_unique<FTextureCubeResource>(
			TextureReference,
			std::make_shared<const FTextureCubePlatformData>(*PlatformData),
			Revision,
			Completion);
	}

	auto DTextureCube::RebuildPlatformData(std::string& OutError) -> bool
	{
		FTextureCubeImportedData BuildInput =
			MakeTextureCubeImportedData(GetSource());
		if (!BuildInput.IsValid())
		{
			OutError = "TextureCube source data is missing or invalid.";
			return false;
		}
		return BuildTextureCubeSynchronously(*this, {.Input = FTextureCubeFacesBuildInput{
			.ImportedData = std::move(BuildInput), .SourceLayout = SourceLayout,
			.OriginalSourceWidth = OriginalSourceWidth,
			.OriginalSourceHeight = OriginalSourceHeight,
			.PanoramaFaceDimension = PanoramaFaceDimension,
			.PanoramaExposureEV = PanoramaExposureEV,
			.Settings = {.bSRGB = bSRGB}}}, {}, OutError);
	}

	auto DTextureCube::PostLoad(std::string& OutError) -> bool
	{
		BindTextureSourceOwner();
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (GetCookedPlatformData().GetMetadata().LogicalSize == 0)
			{
				OutError = std::format(
					"Cooked TextureCube '{}': required PlatformData field is missing.",
					GetObjectPath());
				return false;
			}
			PlatformData.reset();
			OutError.clear();
			return true;
		}
		if (GetSource().GetSchemaVersion() == LegacyTextureSourceSchemaVersion)
		{
			FTextureSource Migrated = GetSource();
			if (!Migrated.MigrateLegacy()
				|| !SetSource(std::move(Migrated), OutError)) return false;
		}
		FTextureCubeImportedData BuildInput =
			MakeTextureCubeImportedData(GetSource());
		if (!BuildInput.IsValid())
		{
			OutError = "TextureCube source data is missing or invalid.";
			return false;
		}
		return BuildTextureCubeSynchronously(*this, {.Input = FTextureCubeFacesBuildInput{
			.ImportedData = std::move(BuildInput), .SourceLayout = SourceLayout,
			.OriginalSourceWidth = OriginalSourceWidth,
			.OriginalSourceHeight = OriginalSourceHeight,
			.PanoramaFaceDimension = PanoramaFaceDimension,
			.PanoramaExposureEV = PanoramaExposureEV,
			.Settings = {.bSRGB = bSRGB}}}, {
			.bMarkPackageDirty = false, .bSourceDecoderInvoked = false}, OutError);
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
		if (!PlatformData && !PostLoad(OutError))
		{
			OutError = std::format("Failed to cook TextureCube '{}': {}", GetObjectPath(), OutError);
			return false;
		}
		if (!PlatformData)
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
		FTextureSource NewSource;
		NewSource.Payload = Value.Pixels;
		NewSource.Width = Value.FaceDimension;
		NewSource.Height = Value.FaceDimension;
		NewSource.Depth = 1;
		NewSource.NumSlices = TextureCubeFaceCount;
		NewSource.SourceChannelCount = Value.SourceChannelCount;
		NewSource.Format = ETextureSourceFormat::RGBA8;
		NewSource.Kind = ETextureSourceKind::TextureCube;
		NewSource.bHasTransparency = Value.TransparencyMask != 0;
		NewSource.TransparencyMask = Value.TransparencyMask;
		NewSource.Blocks = {{.Width = Value.FaceDimension,
			.Height = Value.FaceDimension, .NumSlices = TextureCubeFaceCount}};
		NewSource.Layers = {{.Format = ETextureSourceFormat::RGBA8}};
		NewSource.GammaSpace = ETextureSourceGammaSpace::Unknown;
		NewSource.SchemaVersion = TextureSourceSchemaVersion;
		return SetSource(std::move(NewSource), OutError);
	}

	auto DTextureCube::ValidateSettingsAfterImportOrEdit(
		const FTextureSource& ProposedSource) const -> bool
	{
		const auto Blocks = ProposedSource.GetBlocks();
		const auto Layers = ProposedSource.GetLayers();
		return ProposedSource.GetKind() == ETextureSourceKind::TextureCube
			&& Blocks.size() == 1 && Layers.size() == 1
			&& Blocks[0].Width == Blocks[0].Height && Blocks[0].Depth == 1
			&& Blocks[0].NumSlices == TextureCubeFaceCount
			&& Layers[0].Format == ETextureSourceFormat::RGBA8
			&& Layers[0].NumMips == 1
			&& MakeTextureCubeImportedData(ProposedSource).IsValid()
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
		AdvanceAuthoredGeneration();
		OutError.clear();
		return true;
	}
}
