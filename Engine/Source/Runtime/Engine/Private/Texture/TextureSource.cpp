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

	auto FTextureSource::Validate(std::string* OutError) const -> bool
	{
		if (SchemaVersion == LegacyTextureSourceSchemaVersion)
		{
			const uint32 ByteWidth = BytesPerTexel(Format);
			if (ByteWidth == 0 || Width == 0 || Height == 0 || Depth == 0 || NumSlices == 0)
				return SetError("Legacy texture source metadata is invalid.", OutError);
			if (Kind == ETextureSourceKind::Texture2D
				&& (Depth != 1 || NumSlices != 1 || Format != ETextureSourceFormat::RGBA8))
				return SetError("Legacy Texture2D source is not canonical RGBA8.", OutError);
			if (Kind == ETextureSourceKind::TextureCube
				&& (Depth != 1 || NumSlices != 6 || Width != Height
					|| Format != ETextureSourceFormat::RGBA8))
				return SetError("Legacy TextureCube source is not six canonical RGBA8 faces.", OutError);
			if (Kind == ETextureSourceKind::Volume && NumSlices != 1)
				return SetError("Legacy volume source must use one volume slice.", OutError);
			const uint64 Texels = static_cast<uint64>(Width) * Height * Depth * NumSlices;
			if (Texels > MaximumTextureSourceBytes / ByteWidth
				|| Payload.GetPayloadSize() != Texels * ByteWidth)
				return SetError("Legacy texture source payload size is invalid.", OutError);
			if (OutError) OutError->clear();
			return true;
		}
		if (SchemaVersion != TextureSourceSchemaVersion)
			return SetError("Texture source schema version is unsupported.", OutError);
		FTextureSourceMipInfo Ignored;
		uint64 Total = 0;
		if (!ResolveMipInfo(Kind, GammaSpace, Blocks, Layers, 0, 0, 0,
			Ignored, &Total, OutError)) return false;
		if (Total != Payload.GetPayloadSize())
			return SetError("Texture source descriptors do not exactly cover the payload.", OutError);
		if (SourceChannelCount > 4)
			return SetError("Texture source channel provenance is out of range.", OutError);
		if (OutError) OutError->clear();
		return true;
	}

	auto FTextureSource::IsValid() const -> bool { return Validate(); }

	auto FTextureSource::TryCreate(FTextureSourceInitData InitData,
		std::span<const std::byte> Bytes, FTextureSource& OutSource,
		std::string* OutError) -> bool
	{
		if (Bytes.size() > MaximumTextureSourceBytes
			|| !InitData.Payload.UpdatePayload(Bytes))
			return SetError("Texture source payload exceeds the supported limit.", OutError);
		return TryCreate(std::move(InitData), OutSource, OutError);
	}

	auto FTextureSource::TryCreate(FTextureSourceInitData InitData,
		FTextureSource& OutSource, std::string* OutError) -> bool
	{
		FTextureSource Candidate;
		Candidate.Payload = std::move(InitData.Payload);
		Candidate.Kind = InitData.Kind;
		Candidate.GammaSpace = InitData.GammaSpace;
		Candidate.Blocks = std::move(InitData.Blocks);
		Candidate.Layers = std::move(InitData.Layers);
		Candidate.SourceChannelCount = InitData.SourceChannelCount;
		Candidate.TransparencyMask = InitData.TransparencyMask;
		Candidate.bHasTransparency = InitData.TransparencyMask != 0;
		Candidate.SchemaVersion = TextureSourceSchemaVersion;
		if (!Candidate.Blocks.empty())
		{
			Candidate.Width = Candidate.Blocks[0].Width;
			Candidate.Height = Candidate.Blocks[0].Height;
			Candidate.Depth = Candidate.Blocks[0].Depth;
			Candidate.NumSlices = static_cast<uint8>(std::min(Candidate.Blocks[0].NumSlices, 255u));
		}
		if (!Candidate.Layers.empty()) Candidate.Format = Candidate.Layers[0].Format;
		if (!Candidate.Validate(OutError)) return false;
		OutSource = std::move(Candidate);
		if (OutError) OutError->clear();
		return true;
	}

	auto FTextureSource::Reset() -> void
	{
		DTexture* PreviousOwner = Owner;
		*this = {};
		Owner = PreviousOwner;
	}

	auto FTextureSource::MigrateLegacy(std::string* OutError) -> bool
	{
		if (SchemaVersion == TextureSourceSchemaVersion) return Validate(OutError);
		if (!Validate(OutError)) return false;
		FTextureSource Candidate = *this;
		Candidate.Blocks = {{.Width = Width, .Height = Height, .Depth = Depth,
			.NumSlices = NumSlices}};
		Candidate.Layers = {{.Format = Format, .NumMips = 1}};
		Candidate.GammaSpace = (Kind == ETextureSourceKind::Volume)
			? ETextureSourceGammaSpace::Linear : ETextureSourceGammaSpace::Unknown;
		Candidate.SchemaVersion = TextureSourceSchemaVersion;
		if (!Candidate.Validate(OutError)) return false;
		*this = std::move(Candidate);
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
		if (!Validate(OutError)) return false;
		const FPackageResourceReadResult Read = Payload.GetPayload().Wait();
		if (!Read)
			return SetError(Read.Message.empty() ? "Texture source payload read failed."
				: Read.Message, OutError);
		FTextureSourceSnapshot Candidate{.Blocks = Blocks, .Layers = Layers,
			.Payload = Read.Buffer, .Kind = Kind, .GammaSpace = GammaSpace,
			.SourceChannelCount = SourceChannelCount,
			.TransparencyMask = TransparencyMask, .Identity = GetIdentity(),
			.Generation = Generation};
		if (!Candidate.IsValid(OutError)) return false;
		OutSnapshot = std::move(Candidate);
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
