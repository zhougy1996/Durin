#include "Texture/TextureSource.h"

#include "Hash/XxHash.h"

namespace Durin
{
	namespace
	{
		auto SetError(std::string_view Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = Message;
			return false;
		}

		auto BytesPerTexel(ETextureSourceFormat Format) -> uint32
		{
			switch (Format)
			{
			case ETextureSourceFormat::R8_UNORM: return 1;
			case ETextureSourceFormat::RG8_UNORM:
			case ETextureSourceFormat::R16_FLOAT:
			case ETextureSourceFormat::G16_UNORM: return 2;
			case ETextureSourceFormat::RGBA8:
			case ETextureSourceFormat::R32_FLOAT: return 4;
			case ETextureSourceFormat::RGBA16_FLOAT:
			case ETextureSourceFormat::RGBA16_UNORM: return 8;
			case ETextureSourceFormat::RGBA32_FLOAT: return 16;
			default: return 0;
			}
		}

		auto ToImageFormat(ETextureSourceFormat Format) -> Image::ERawImageFormat
		{
			switch (Format)
			{
			case ETextureSourceFormat::R8_UNORM: return Image::ERawImageFormat::G8;
			case ETextureSourceFormat::G16_UNORM: return Image::ERawImageFormat::G16;
			case ETextureSourceFormat::RG8_UNORM: return Image::ERawImageFormat::RG8;
			case ETextureSourceFormat::RGBA8: return Image::ERawImageFormat::RGBA8;
			case ETextureSourceFormat::RGBA16_UNORM: return Image::ERawImageFormat::RGBA16;
			case ETextureSourceFormat::R16_FLOAT: return Image::ERawImageFormat::R16F;
			case ETextureSourceFormat::RGBA16_FLOAT: return Image::ERawImageFormat::RGBA16F;
			case ETextureSourceFormat::R32_FLOAT: return Image::ERawImageFormat::R32F;
			case ETextureSourceFormat::RGBA32_FLOAT: return Image::ERawImageFormat::RGBA32F;
			default: return Image::ERawImageFormat::Invalid;
			}
		}

		auto ToImageGamma(ETextureSourceGammaSpace Gamma) -> Image::EImageGammaSpace
		{
			switch (Gamma)
			{
			case ETextureSourceGammaSpace::Linear: return Image::EImageGammaSpace::Linear;
			case ETextureSourceGammaSpace::SRGB: return Image::EImageGammaSpace::SRGB;
			default: return Image::EImageGammaSpace::Unknown;
			}
		}

		auto IsKindValid(ETextureSourceKind Kind) -> bool
		{
			return Kind >= ETextureSourceKind::Texture2D
				&& Kind <= ETextureSourceKind::LongLatCube;
		}

		auto ResolveMipInfo(ETextureSourceKind Kind,
			ETextureSourceGammaSpace GammaSpace,
			std::span<const FTextureSourceBlock> Blocks,
			std::span<const FTextureSourceLayer> Layers,
			uint32 WantedBlock, uint32 WantedLayer, uint32 WantedMip,
			FTextureSourceMipInfo& OutInfo, uint64* OutTotal,
			std::string* OutError) -> bool
		{
			OutInfo = {};
			if (!IsKindValid(Kind) || GammaSpace > ETextureSourceGammaSpace::SRGB
				|| Blocks.empty() || Layers.empty())
				return SetError("Texture source descriptors are empty or invalid.", OutError);
			uint64 Offset = 0;
			for (uint32 BlockIndex = 0; BlockIndex < Blocks.size(); ++BlockIndex)
			{
				const FTextureSourceBlock& Block = Blocks[BlockIndex];
				if (Block.Width == 0 || Block.Height == 0 || Block.Depth == 0
					|| Block.NumSlices == 0)
					return SetError("Texture source block dimensions and slice count must be nonzero.", OutError);
				for (uint32 LayerIndex = 0; LayerIndex < Layers.size(); ++LayerIndex)
				{
					const FTextureSourceLayer& Layer = Layers[LayerIndex];
					if (BytesPerTexel(Layer.Format) == 0 || Layer.NumMips == 0)
						return SetError("Texture source layer format or mip count is invalid.", OutError);
					for (uint32 MipIndex = 0; MipIndex < Layer.NumMips; ++MipIndex)
					{
						const uint32 Width = std::max(1u, Block.Width >> std::min(MipIndex, 31u));
						const uint32 Height = std::max(1u, Block.Height >> std::min(MipIndex, 31u));
						const uint32 Depth = Kind == ETextureSourceKind::Volume
							? std::max(1u, Block.Depth >> std::min(MipIndex, 31u)) : Block.Depth;
						Image::FImageInfo ImageInfo{.Width = Width, .Height = Height,
							.Depth = Depth, .SliceCount = Block.NumSlices,
							.Format = ToImageFormat(Layer.Format),
							.GammaSpace = ToImageGamma(GammaSpace)};
						uint64 Size = 0;
						if (!ImageInfo.GetByteSize(Size) || Offset > MaximumTextureSourceBytes
							|| Size > MaximumTextureSourceBytes - Offset)
							return SetError("Texture source layout exceeds the bounded payload size.", OutError);
						if (BlockIndex == WantedBlock && LayerIndex == WantedLayer
							&& MipIndex == WantedMip)
							OutInfo = {.ImageInfo = ImageInfo, .PayloadOffset = Offset,
								.PayloadSize = Size};
						Offset += Size;
					}
				}
			}
			if (OutTotal) *OutTotal = Offset;
			if (WantedBlock >= Blocks.size() || WantedLayer >= Layers.size()
				|| WantedMip >= Layers[WantedLayer].NumMips)
				return SetError("Texture source subresource index is out of range.", OutError);
			if (OutError) OutError->clear();
			return true;
		}
	}

	auto FTextureSource::IsValid() const -> bool
	{
		if (SchemaVersion == LegacyTextureSourceSchemaVersion)
		{
			const uint32 ByteWidth = BytesPerTexel(Format);
			if (ByteWidth == 0 || Width == 0 || Height == 0 || Depth == 0 || NumSlices == 0)
				return false;
			if (!IsKindValid(Kind)) return false;
			const Image::FImageInfo Info{.Width = Width, .Height = Height,
				.Depth = Depth, .SliceCount = NumSlices,
				.Format = ToImageFormat(Format)};
			uint64 ExpectedSize = 0;
			return Info.GetByteSize(ExpectedSize)
				&& ExpectedSize == Payload.GetPayloadSize();
		}
		if (SchemaVersion != TextureSourceSchemaVersion) return false;
		FTextureSourceMipInfo Ignored;
		uint64 Total = 0;
		if (!ResolveMipInfo(Kind, GammaSpace, Blocks, Layers, 0, 0, 0,
			Ignored, &Total, nullptr)) return false;
		return Total == Payload.GetPayloadSize() && SourceChannelCount <= 4;
	}

	auto FTextureSource::Reset() -> void
	{
		DTexture* PreviousOwner = Owner;
		*this = {};
		Owner = PreviousOwner;
	}

	auto FTextureSource::MigrateLegacy() -> bool
	{
		if (SchemaVersion == TextureSourceSchemaVersion) return IsValid();
		if (!IsValid()) return false;
		FTextureSource Migrated = *this;
		Migrated.Blocks = {{.Width = Width, .Height = Height, .Depth = Depth,
			.NumSlices = NumSlices}};
		Migrated.Layers = {{.Format = Format, .NumMips = 1}};
		Migrated.GammaSpace = (Kind == ETextureSourceKind::Volume)
			? ETextureSourceGammaSpace::Linear : ETextureSourceGammaSpace::Unknown;
		Migrated.SchemaVersion = TextureSourceSchemaVersion;
		if (!Migrated.IsValid()) return false;
		*this = std::move(Migrated);
		return true;
	}

	auto FTextureSource::GetIdentity() const -> FXxHash128
	{
		if (!IsValid()) return {};
		FXxHash128Builder Builder;
		Builder.UpdateValue(SchemaVersion);
		Builder.UpdateValue(Kind);
		Builder.UpdateValue(GammaSpace);
		Builder.UpdateValue(SourceChannelCount);
		Builder.UpdateValue(TransparencyMask);
		if (SchemaVersion == LegacyTextureSourceSchemaVersion)
		{
			Builder.UpdateValue(Width); Builder.UpdateValue(Height);
			Builder.UpdateValue(Depth); Builder.UpdateValue(NumSlices);
			Builder.UpdateValue(Format);
		}
		for (const FTextureSourceBlock& Block : Blocks)
		{
			Builder.UpdateValue(Block.Width); Builder.UpdateValue(Block.Height);
			Builder.UpdateValue(Block.Depth); Builder.UpdateValue(Block.NumSlices);
		}
		for (const FTextureSourceLayer& Layer : Layers)
		{
			Builder.UpdateValue(Layer.Format); Builder.UpdateValue(Layer.NumMips);
		}
		Builder.UpdateValue(Payload.GetPayloadSize());
		Builder.UpdateValue(Payload.GetPayloadId());
		return Builder.Finalize();
	}

	auto FTextureSource::GetMipInfo(uint32 BlockIndex, uint32 LayerIndex,
		uint32 MipIndex, FTextureSourceMipInfo& OutInfo,
		std::string* OutError) const -> bool
	{
		if (SchemaVersion != TextureSourceSchemaVersion)
			return SetError("Legacy texture source must be migrated before subresource access.", OutError);
		return ResolveMipInfo(Kind, GammaSpace, Blocks, Layers, BlockIndex,
			LayerIndex, MipIndex, OutInfo, nullptr, OutError);
	}

	auto FTextureSource::CreateSnapshotBlocking(uint64 Generation,
		FTextureSourceSnapshot& OutSnapshot, std::string* OutError) const -> bool
	{
		OutSnapshot = {};
		if (!IsValid()) return SetError("Texture source is invalid.", OutError);
		const FPackageResourceReadResult Read = Payload.GetPayload().Wait();
		if (!Read)
			return SetError(Read.Message.empty() ? "Texture source payload read failed."
				: Read.Message, OutError);
		FTextureSourceSnapshot NewSnapshot{.Blocks = Blocks, .Layers = Layers,
			.Payload = Read.Buffer, .Kind = Kind, .GammaSpace = GammaSpace,
			.SourceChannelCount = SourceChannelCount,
			.TransparencyMask = TransparencyMask, .Identity = GetIdentity(),
			.Generation = Generation};
		if (!NewSnapshot.IsValid(OutError)) return false;
		OutSnapshot = std::move(NewSnapshot);
		return true;
	}

	auto FTextureSourceSnapshot::IsValid(std::string* OutError) const -> bool
	{
		FTextureSourceMipInfo Ignored;
		uint64 Total = 0;
		if (Identity.IsZero() || !ResolveMipInfo(Kind, GammaSpace, Blocks, Layers,
			0, 0, 0, Ignored, &Total, OutError)) return false;
		if (Total != Payload.GetSize())
			return SetError("Texture source snapshot payload size is invalid.", OutError);
		if (OutError) OutError->clear();
		return true;
	}

	auto FTextureSourceSnapshot::GetMipInfo(uint32 BlockIndex, uint32 LayerIndex,
		uint32 MipIndex, FTextureSourceMipInfo& OutInfo,
		std::string* OutError) const -> bool
	{
		return ResolveMipInfo(Kind, GammaSpace, Blocks, Layers, BlockIndex,
			LayerIndex, MipIndex, OutInfo, nullptr, OutError);
	}

	auto FTextureSourceSnapshot::GetMipImage(uint32 BlockIndex, uint32 LayerIndex,
		uint32 MipIndex, Image::FImageView& OutImage, std::string* OutError) const -> bool
	{
		OutImage = {};
		FTextureSourceMipInfo Info;
		if (!GetMipInfo(BlockIndex, LayerIndex, MipIndex, Info, OutError)) return false;
		OutImage = Image::FImageView(Info.ImageInfo, Payload, Info.PayloadOffset);
		if (!OutImage.IsValid()) return SetError("Texture source mip view is invalid.", OutError);
		if (OutError) OutError->clear();
		return true;
	}
} // namespace Durin
