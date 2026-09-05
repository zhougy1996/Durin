#include "Texture/VolumeTexture.h"
#include "Texture/TextureCookedData.h"

#include "DObject/Package.h"

#include "Asset/AssetCook.h"
#include "DObject/DurinPropertyTypes.h"
#include "Serialization/Archive.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/VolumeTextureBuildProvider.h"
#include "Texture/VolumeTextureRenderResource.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		auto ToTextureSourceFormat(EVolumeTextureFormat Format)
			-> ETextureSourceFormat
		{
			switch (Format)
			{
			case EVolumeTextureFormat::R8_UNORM: return ETextureSourceFormat::R8_UNORM;
			case EVolumeTextureFormat::RG8_UNORM: return ETextureSourceFormat::RG8_UNORM;
			case EVolumeTextureFormat::RGBA8_UNORM: return ETextureSourceFormat::RGBA8;
			case EVolumeTextureFormat::R16_FLOAT: return ETextureSourceFormat::R16_FLOAT;
			case EVolumeTextureFormat::RGBA16_FLOAT: return ETextureSourceFormat::RGBA16_FLOAT;
			default: return ETextureSourceFormat::Invalid;
			}
		}

		auto ToVolumeTextureFormat(ETextureSourceFormat Format)
			-> std::optional<EVolumeTextureFormat>
		{
			switch (Format)
			{
			case ETextureSourceFormat::R8_UNORM: return EVolumeTextureFormat::R8_UNORM;
			case ETextureSourceFormat::RG8_UNORM: return EVolumeTextureFormat::RG8_UNORM;
			case ETextureSourceFormat::RGBA8: return EVolumeTextureFormat::RGBA8_UNORM;
			case ETextureSourceFormat::R16_FLOAT: return EVolumeTextureFormat::R16_FLOAT;
			case ETextureSourceFormat::RGBA16_FLOAT: return EVolumeTextureFormat::RGBA16_FLOAT;
			default: return std::nullopt;
			}
		}

		auto MakeVolumeTextureBuildInput(const FTextureSource& Source)
			-> FVolumeTextureSourceData
		{
			FVolumeTextureSourceData Result;
			const std::optional<EVolumeTextureFormat> Format =
				ToVolumeTextureFormat(Source.GetFormat());
			if (!Source.IsValid() || Source.GetKind() != ETextureSourceKind::Volume
				|| !Format) return Result;
			Result.Width = Source.GetWidth();
			Result.Height = Source.GetHeight();
			Result.Depth = Source.GetDepth();
			Result.Format = *Format;
			Result.CanonicalSourceIdentity = Source.GetIdentity();
			FTextureSourceSnapshot Snapshot;
			if (!Source.CreateSnapshotBlocking(0, Snapshot)
				|| !Result.Voxels.UpdatePayload(Snapshot.Payload)) return {};
			return Result.IsValid() ? std::move(Result) : FVolumeTextureSourceData{};
		}

		auto ToPixelFormat(EVolumeTextureFormat Format) -> EPixelFormat
		{
			switch (Format)
			{
			case EVolumeTextureFormat::R8_UNORM: return EPixelFormat::R8_UNORM;
			case EVolumeTextureFormat::RG8_UNORM: return EPixelFormat::RG8_UNORM;
			case EVolumeTextureFormat::RGBA8_UNORM: return EPixelFormat::RGBA8_UNORM;
			case EVolumeTextureFormat::R16_FLOAT: return EPixelFormat::R16_FLOAT;
			case EVolumeTextureFormat::RGBA16_FLOAT: return EPixelFormat::RGBA16_FLOAT;
			default: return EPixelFormat::Unknown;
			}
		}
	}

	auto FVolumeTextureSourceData::IsValid() const -> bool
	{
		if (PayloadSchemaVersion != VolumeTextureSourcePayloadSchemaVersion
			|| Width == 0 || Height == 0 || Depth == 0
			|| Width > MaximumVolumeTextureDimension
			|| Height > MaximumVolumeTextureDimension
			|| Depth > MaximumVolumeTextureDimension)
			return false;
		const EPixelFormat PixelFormat = ToPixelFormat(Format);
		const FPixelFormatInfo& Info = GetPixelFormatInfo(PixelFormat);
		if (Info.BlockSize != 1 || Info.BytesPerBlock == 0) return false;
		const uint64 Texels = static_cast<uint64>(Width) * Height * Depth;
		return Texels <= MaximumTexturePayloadBytes / Info.BytesPerBlock
			&& Voxels.GetPayloadSize() == Texels * Info.BytesPerBlock;
	}

	auto FVolumeTextureSourceData::SetVoxelBytes(std::span<const std::byte> Bytes) -> bool
	{
		if (!Voxels.UpdatePayload(Bytes)) return false;
		FTextureSource Canonical;
		const FTextureSourceBlock Block{.Width = Width, .Height = Height, .Depth = Depth};
		const FTextureSourceLayer Layer{.Format = ToTextureSourceFormat(Format)};
		if (Canonical.InitLayered(ETextureSourceKind::Volume,
			std::span(&Block, 1), std::span(&Layer, 1),
			ETextureSourceGammaSpace::Linear, Bytes, 0, 0,
			ETextureSourceCompression::Raw))
			CanonicalSourceIdentity = Canonical.GetIdentity();
		return true;
	}

	auto FVolumeTextureSourceData::GetIdentity() const -> FXxHash128
	{
		if (!IsValid()) return {};
		if (!CanonicalSourceIdentity.IsZero()) return CanonicalSourceIdentity;
		FXxHash128Builder Builder;
		Builder.UpdateValue(PayloadSchemaVersion);
		Builder.UpdateValue(Width);
		Builder.UpdateValue(Height);
		Builder.UpdateValue(Depth);
		Builder.UpdateValue(Format);
		Builder.UpdateValue(Voxels.GetPayloadId());
		return Builder.Finalize();
	}

	auto FVolumeTextureMipData::IsValid(EPixelFormat PixelFormat) const -> bool
	{
		if (Width == 0 || Height == 0 || Depth == 0) return false;
		const FPixelFormatLayout Slice = GetPixelFormatLayout(PixelFormat, Width, Height);
		if (Slice.RowPitch != RowPitch || Slice.DataSize != DepthPitch
			|| Depth > std::numeric_limits<uint64>::max() / DepthPitch)
			return false;
		return Voxels.size() == static_cast<uint64>(DepthPitch) * Depth;
	}

	auto FVolumeTexturePlatformData::IsValid() const -> bool
	{
		if (Mips.empty()) return false;
		ETextureStablePixelFormat Stable;
		switch (PixelFormat)
		{
		case EPixelFormat::R8_UNORM: Stable = ETextureStablePixelFormat::R8_UNORM; break;
		case EPixelFormat::RG8_UNORM: Stable = ETextureStablePixelFormat::RG8_UNORM; break;
		case EPixelFormat::RGBA8_UNORM: Stable = ETextureStablePixelFormat::RGBA8_UNORM; break;
		case EPixelFormat::R16_FLOAT: Stable = ETextureStablePixelFormat::R16_FLOAT; break;
		case EPixelFormat::RGBA16_FLOAT: Stable = ETextureStablePixelFormat::RGBA16_FLOAT; break;
		default: return false;
		}
		(void)Stable;
		for (size_t MipIndex = 0; MipIndex < Mips.size(); ++MipIndex)
		{
			const FVolumeTextureMipData& Mip = Mips[MipIndex];
			if (!Mip.IsValid(PixelFormat)) return false;
			if (MipIndex > 0)
			{
				const FVolumeTextureMipData& Previous = Mips[MipIndex - 1];
				if (Mip.Width != std::max(1u, Previous.Width / 2)
					|| Mip.Height != std::max(1u, Previous.Height / 2)
					|| Mip.Depth != std::max(1u, Previous.Depth / 2)) return false;
			}
		}
		return Mips.size() <= MaximumTextureMipCount
			&& Mips.front().Width <= MaximumVolumeTextureDimension
			&& Mips.front().Height <= MaximumVolumeTextureDimension
			&& Mips.front().Depth <= MaximumVolumeTextureDimension
			&& Mips.back().Width == 1 && Mips.back().Height == 1
			&& Mips.back().Depth == 1;
	}

	DVolumeTexture::DVolumeTexture(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	DVolumeTexture::~DVolumeTexture() = default;

	auto DVolumeTexture::CreateBuildInput() const -> FVolumeTextureSourceData
	{
		return MakeVolumeTextureBuildInput(GetSource());
	}

	auto DVolumeTexture::SetPlatformData(
		std::unique_ptr<FVolumeTexturePlatformData> Data,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		if (!Data || !Data->IsValid())
		{
			OutError = "VolumeTexture platform data must be complete and valid.";
			return false;
		}
		PlatformData = std::move(Data);
		OutError.clear();
		return true;
	}

	auto DVolumeTexture::SerializeCooked(FArchive& Ar) -> void
	{
		Super::SerializeCooked(Ar);
		TexturePrivate::SerializeCookedPlatformData(Ar, GetMutableCookedPlatformData(),
			PlatformData.get(), FName("Durin::DVolumeTexture"), "VolumeTexture");
	}

	auto DVolumeTexture::CreateRenderResourceCandidate(
		FTextureReference* TextureReference, uint64 Revision,
		const std::shared_ptr<FTextureResourceCompletion>& Completion)
		-> std::unique_ptr<FTextureAssetResource>
	{
		check(PlatformData && PlatformData->IsValid());
		return std::make_unique<FVolumeTextureResource>(TextureReference,
			std::make_shared<const FVolumeTexturePlatformData>(*PlatformData),
			Revision, Completion);
	}

	auto DVolumeTexture::PostLoad(std::string& OutError) -> bool
	{
		BindTextureSourceOwner();
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			if (GetCookedPlatformData().GetMetadata().LogicalSize == 0)
			{
				OutError = std::format(
					"Cooked volume texture '{}': required PlatformData field is missing.",
					GetObjectPath());
				return false;
			}
			PlatformData.reset();
			OutError.clear();
			return true;
		}
		if (GetSource().GetSchemaVersion() != TextureSourceSchemaVersion)
		{
			FTextureSource Migrated = GetSource();
			if (!Migrated.MigrateLegacy()
				|| !SetSource(std::move(Migrated), OutError)) return false;
		}
		FVolumeTextureSourceData BuildInput =
			MakeVolumeTextureBuildInput(GetSource());
		if (!BuildInput.IsValid())
		{
			OutError = "VolumeTexture source data is missing or invalid.";
			return false;
		}
		return BuildVolumeTextureSynchronously(*this, {
			.SourceData = BuildInput,
			.Settings = BuildSettings}, {
			.bMarkPackageDirty = false,
			.bSourceDecoderInvoked = false}, OutError);
	}

	auto DVolumeTexture::LoadCookedPlatformData(std::string& OutError) -> bool
	{
		return TexturePrivate::LoadCookedPlatformData<FVolumeTexturePlatformData>(
			*this, GetMutableCookedPlatformData(), "volume texture", OutError);
	}

	auto DVolumeTexture::ContributeToCook(FCookContext& Context,
		std::string_view VirtualPackagePath, std::string& OutError) -> bool
	{
		if (Context.GetTargetPlatform() != ECookTargetPlatform::Win64
			|| Context.GetTargetProfile() != ECookTargetProfile::Game)
		{
			OutError = "Volume textures support only the Win64 game cook target.";
			return false;
		}
		if (!PlatformData || !PlatformData->IsValid())
		{
			if (!PostLoad(OutError)) return false;
		}
		return Context.AddPackage(
			std::string(VirtualPackagePath), GetPackage(), &OutError);
	}

	auto DVolumeTexture::SetSourceData(
		const FVolumeTextureSourceData& Value, std::string& OutError) -> bool
	{
		CheckGameThread();
		if (!Value.IsValid())
		{
			OutError = "VolumeTexture source data is invalid.";
			return false;
		}
		const FPackageResourceReadResult Read = Value.Voxels.GetPayload().Wait();
		if (!Read)
		{
			OutError = "VolumeTexture source payload could not be read.";
			return false;
		}
		FTextureSource NewSource;
		const FTextureSourceBlock Block{.Width = Value.Width, .Height = Value.Height,
			.Depth = Value.Depth};
		const FTextureSourceLayer Layer{.Format = ToTextureSourceFormat(Value.Format)};
		if (!NewSource.InitLayeredImpl(ETextureSourceKind::Volume,
			std::span(&Block, 1), std::span(&Layer, 1),
			ETextureSourceGammaSpace::Linear, Read.Buffer.GetBytes(), 0, 0,
			ETextureSourceCompression::Raw))
		{
			OutError = "VolumeTexture source data could not be initialized.";
			return false;
		}
		return SetSource(std::move(NewSource), OutError);
	}

	auto DVolumeTexture::ValidateSettingsAfterImportOrEdit(
		const FTextureSource& ProposedSource) const -> bool
	{
		const auto Blocks = ProposedSource.GetBlocks();
		const auto Layers = ProposedSource.GetLayers();
		return ProposedSource.GetKind() == ETextureSourceKind::Volume
			&& Blocks.size() == 1 && Layers.size() == 1
			&& Blocks[0].NumSlices == 1 && Layers[0].NumMips == 1
			&& MakeVolumeTextureBuildInput(ProposedSource).IsValid()
			&& ToPixelFormat(BuildSettings.OutputFormat) != EPixelFormat::Unknown
			&& BuildSettings.MipFilter == EVolumeTextureMipFilter::Box;
	}

	auto DVolumeTexture::SetBuildSettings(
		FVolumeTextureBuildSettings Value, std::string& OutError) -> bool
	{
		CheckGameThread();
		if (ToPixelFormat(Value.OutputFormat) == EPixelFormat::Unknown
			|| Value.MipFilter != EVolumeTextureMipFilter::Box)
		{
			OutError = "VolumeTexture build settings are invalid.";
			return false;
		}
		BuildSettings = Value;
		AdvanceAuthoredGeneration();
		OutError.clear();
		return true;
	}

}
