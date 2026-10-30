#include "Texture/VolumeTexture.h"
#include "Texture/TexturePlatformCache.h"
#include "Logging/LogMacros.h"
#include "Texture/TextureCookedData.h"

#include "DObject/Package.h"

#include "DObject/DurinPropertyTypes.h"
#include "Serialization/Archive.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/VolumeTextureRenderResource.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
#if DURIN_WITH_EDITORONLY_DATA
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

#endif

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

#if DURIN_WITH_EDITORONLY_DATA
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

	auto FVolumeTextureSourceData::SetVoxelBytes(FByteView Bytes) -> bool
	{
		if (!Voxels.UpdatePayload(Bytes)) return false;
		FTextureSource Canonical;
		const FTextureSourceBlock Block{.Width = Width, .Height = Height, .Depth = Depth};
		const FTextureSourceLayer Layer{.Format = ToTextureSourceFormat(Format)};
		if (Canonical.InitLayered(ETextureSourceKind::Volume,
			std::span(&Block, 1), std::span(&Layer, 1),
			ETextureSourceGammaSpace::Linear, Bytes, 0, 0,
			ETextureSourceCompression::Zstd))
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

#endif

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


	auto DVolumeTexture::SetPlatformData(
		std::unique_ptr<FVolumeTexturePlatformData> Data) -> void
	{
		CheckGameThread();
		InvalidateRenderResource();
		PlatformData = std::move(Data);
	}

	auto DVolumeTexture::SerializeCooked(FArchive& Ar) -> void
	{
		Super::SerializeCooked(Ar);
		TexturePrivate::SerializeCookedPlatformData(Ar, GetMutableCookedPlatformData(),
			PlatformData.get(), FName("Durin::DVolumeTexture"), "VolumeTexture");
	}

	auto DVolumeTexture::CreateResource(
		FTextureReference* TextureReference)
		-> std::unique_ptr<FTextureResource>
	{
		check(PlatformData && PlatformData->IsValid());
		return std::make_unique<FVolumeTextureResource>(TextureReference,
			PlatformData);
	}

	auto DVolumeTexture::LoadCookedPlatformData(bool bAsync) -> void
	{
		TexturePrivate::LoadCookedPlatformData<FVolumeTexturePlatformData>(*this, bAsync);
	}

#if DURIN_WITH_EDITORONLY_DATA
	auto PrepareVolumeTextureSource(
		const FVolumeTextureSourceData& Value) -> std::expected<FTextureSource, std::string>
	{
		if (!Value.IsValid())
		{
			return std::unexpected(std::string("VolumeTexture source data is invalid."));
		}
		const FPackageResourceReadResult Read = Value.Voxels.GetPayload().Wait();
		if (!Read)
		{
			return std::unexpected(std::format("VolumeTexture source payload could not be read: {}", FormatPackageResourceReadError(Read)));
		}
		FTextureSource NewSource;
		const FTextureSourceBlock Block{.Width = Value.Width, .Height = Value.Height,
			.Depth = Value.Depth};
		const FTextureSourceLayer Layer{.Format = ToTextureSourceFormat(Value.Format)};
		if (!NewSource.InitLayered(ETextureSourceKind::Volume,
			std::span(&Block, 1), std::span(&Layer, 1),
			ETextureSourceGammaSpace::Linear, Read->GetBytes(), 0, 0,
			ETextureSourceCompression::Zstd))
		{
			return std::unexpected(std::string("VolumeTexture source data could not be initialized."));
		}
		return NewSource;
	}
#endif

#if DURIN_WITH_EDITORONLY_DATA
	auto DVolumeTexture::SetBuildSettings(
		FVolumeTextureBuildSettings Value) -> void
	{
		CheckGameThread();
		PrepareTextureForMutation(*this);
		BuildSettings = Value;
		InvalidateAuthoredBuild();
	}
#endif

}
