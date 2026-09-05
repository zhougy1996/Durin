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

		auto ToTextureFormat(Image::ERawImageFormat Format) -> ETextureSourceFormat
		{
			switch (Format)
			{
			case Image::ERawImageFormat::G8: return ETextureSourceFormat::R8_UNORM;
			case Image::ERawImageFormat::G16: return ETextureSourceFormat::G16_UNORM;
			case Image::ERawImageFormat::RG8: return ETextureSourceFormat::RG8_UNORM;
			case Image::ERawImageFormat::RGBA8: return ETextureSourceFormat::RGBA8;
			case Image::ERawImageFormat::RGBA16: return ETextureSourceFormat::RGBA16_UNORM;
			case Image::ERawImageFormat::R16F: return ETextureSourceFormat::R16_FLOAT;
			case Image::ERawImageFormat::RGBA16F: return ETextureSourceFormat::RGBA16_FLOAT;
			case Image::ERawImageFormat::R32F: return ETextureSourceFormat::R32_FLOAT;
			case Image::ERawImageFormat::RGBA32F: return ETextureSourceFormat::RGBA32_FLOAT;
			default: return ETextureSourceFormat::Invalid;
			}
		}

		auto ToTextureGamma(Image::EImageGammaSpace Gamma) -> ETextureSourceGammaSpace
		{
			switch (Gamma)
			{
			case Image::EImageGammaSpace::Linear: return ETextureSourceGammaSpace::Linear;
			case Image::EImageGammaSpace::SRGB: return ETextureSourceGammaSpace::SRGB;
			default: return ETextureSourceGammaSpace::Unknown;
			}
		}

		auto EncodeRunLength(std::span<const std::byte> Bytes) -> FByteArray
		{
			FByteArray Result;
			Result.reserve(Bytes.size());
			for (size_t Offset = 0; Offset < Bytes.size();)
			{
				uint8 Count = 1;
				while (Offset + Count < Bytes.size() && Count < 255
					&& Bytes[Offset + Count] == Bytes[Offset]) ++Count;
				Result.push_back(static_cast<std::byte>(Count));
				Result.push_back(Bytes[Offset]);
				Offset += Count;
			}
			return Result;
		}

		auto DecodeRunLength(std::span<const std::byte> Bytes, uint64 DecodedSize,
			FByteArray& OutBytes) -> bool
		{
			if ((Bytes.size() & 1u) != 0 || DecodedSize > MaximumTextureSourceBytes)
				return false;
			FByteArray Result;
			Result.reserve(static_cast<size_t>(DecodedSize));
			for (size_t Offset = 0; Offset < Bytes.size(); Offset += 2)
			{
				const uint8 Count = std::to_integer<uint8>(Bytes[Offset]);
				if (Count == 0 || Count > DecodedSize - Result.size()) return false;
				Result.insert(Result.end(), Count, Bytes[Offset + 1]);
			}
			if (Result.size() != DecodedSize) return false;
			OutBytes = std::move(Result);
			return true;
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
		if (SchemaVersion != DescriptorTextureSourceSchemaVersion
			&& SchemaVersion != TextureSourceSchemaVersion) return false;
		FTextureSourceMipInfo Ignored;
		uint64 Total = 0;
		if (!ResolveMipInfo(Kind, GammaSpace, Blocks, Layers, 0, 0, 0,
			Ignored, &Total, nullptr)) return false;
		if (SourceChannelCount > 4) return false;
		if (SchemaVersion == DescriptorTextureSourceSchemaVersion)
			return Total == Payload.GetPayloadSize();
		if (DecodedPayloadSize != Total || (CanonicalPayloadHashLow == 0
			&& CanonicalPayloadHashHigh == 0)) return false;
		if (Compression == ETextureSourceCompression::Raw)
			return Payload.GetPayloadSize() == Total;
		return Compression == ETextureSourceCompression::RunLength
			&& Payload.GetPayloadSize() > 0 && Payload.GetPayloadSize() < Total;
	}

	auto FTextureSource::InitLayered(ETextureSourceKind InKind,
		std::span<const FTextureSourceBlock> InBlocks,
		std::span<const FTextureSourceLayer> InLayers,
		ETextureSourceGammaSpace InGammaSpace,
		std::span<const std::byte> DecodedPayload,
		uint8 InSourceChannelCount, uint8 InTransparencyMask,
		ETextureSourceCompression PreferredCompression) -> bool
	{
		return InitLayeredImpl(InKind, InBlocks, InLayers, InGammaSpace,
			DecodedPayload, InSourceChannelCount, InTransparencyMask,
			PreferredCompression);
	}

		auto FTextureSource::InitLayeredImpl(ETextureSourceKind InKind,
		std::span<const FTextureSourceBlock> InBlocks,
		std::span<const FTextureSourceLayer> InLayers,
		ETextureSourceGammaSpace InGammaSpace,
		std::span<const std::byte> DecodedPayload,
		uint8 InSourceChannelCount, uint8 InTransparencyMask,
		ETextureSourceCompression PreferredCompression) -> bool
	{
		if (PreferredCompression != ETextureSourceCompression::Raw
			&& PreferredCompression != ETextureSourceCompression::RunLength)
			return false;
		FTextureSource NewSource;
		NewSource.Kind = InKind;
		NewSource.GammaSpace = InGammaSpace;
		NewSource.Blocks.assign(InBlocks.begin(), InBlocks.end());
		NewSource.Layers.assign(InLayers.begin(), InLayers.end());
		if (!NewSource.Blocks.empty())
		{
			NewSource.Width = NewSource.Blocks[0].Width;
			NewSource.Height = NewSource.Blocks[0].Height;
			NewSource.Depth = NewSource.Blocks[0].Depth;
			NewSource.NumSlices = static_cast<uint8>(std::min<uint32>(
				NewSource.Blocks[0].NumSlices, std::numeric_limits<uint8>::max()));
		}
		if (!NewSource.Layers.empty()) NewSource.Format = NewSource.Layers[0].Format;
		NewSource.SourceChannelCount = InSourceChannelCount;
		NewSource.TransparencyMask = InTransparencyMask;
		NewSource.bHasTransparency = InTransparencyMask != 0;
		NewSource.SchemaVersion = TextureSourceSchemaVersion;
		NewSource.DecodedPayloadSize = DecodedPayload.size();
		const FXxHash128 PayloadHash = FXxHash128::HashBuffer(DecodedPayload);
		NewSource.CanonicalPayloadHashLow = PayloadHash.HashLow;
		NewSource.CanonicalPayloadHashHigh = PayloadHash.HashHigh;
		FByteArray Stored;
		if (PreferredCompression == ETextureSourceCompression::RunLength)
			Stored = EncodeRunLength(DecodedPayload);
		if (PreferredCompression == ETextureSourceCompression::RunLength
			&& !Stored.empty() && Stored.size() < DecodedPayload.size())
		{
			NewSource.Compression = ETextureSourceCompression::RunLength;
			if (!NewSource.Payload.UpdatePayload(Stored)) return false;
		}
		else
		{
			NewSource.Compression = ETextureSourceCompression::Raw;
			if (!NewSource.Payload.UpdatePayload(DecodedPayload)) return false;
		}
		if (!NewSource.IsValid()) return false;
		DTexture* PreviousOwner = Owner;
		*this = std::move(NewSource);
		Owner = PreviousOwner;
		return true;
	}

	auto FTextureSource::Init2D(Image::FImageView Image, uint8 InSourceChannelCount,
		uint8 InTransparencyMask, ETextureSourceCompression PreferredCompression) -> bool
	{
		if (!Image.IsValid() || Image.GetInfo().Depth != 1
			|| Image.GetInfo().SliceCount != 1) return false;
		const FTextureSourceBlock Block{.Width = Image.GetInfo().Width,
			.Height = Image.GetInfo().Height};
		const FTextureSourceLayer Layer{.Format = ToTextureFormat(Image.GetInfo().Format)};
		return InitLayeredImpl(ETextureSourceKind::Texture2D,
			std::span(&Block, 1), std::span(&Layer, 1),
			ToTextureGamma(Image.GetInfo().GammaSpace), Image.GetPixels(),
			InSourceChannelCount, InTransparencyMask, PreferredCompression);
	}

	auto FTextureSource::InitCube(std::span<const Image::FImageView> Faces,
		uint8 InSourceChannelCount, uint8 InTransparencyMask,
		ETextureSourceCompression PreferredCompression) -> bool
	{
		if (Faces.size() != 6 || !Faces[0].IsValid()) return false;
		const Image::FImageInfo Info = Faces[0].GetInfo();
		if (Info.Width != Info.Height || Info.Depth != 1 || Info.SliceCount != 1)
			return false;
		FByteArray Bytes;
		uint64 FaceSize = 0;
		if (!Info.GetByteSize(FaceSize) || FaceSize > MaximumTextureSourceBytes / 6)
			return false;
		Bytes.reserve(static_cast<size_t>(FaceSize * 6));
		for (const Image::FImageView& Face : Faces)
		{
			if (!Face.IsValid() || Face.GetInfo() != Info) return false;
			const auto Pixels = Face.GetPixels();
			Bytes.insert(Bytes.end(), Pixels.begin(), Pixels.end());
		}
		const FTextureSourceBlock Block{.Width = Info.Width, .Height = Info.Height,
			.NumSlices = 6};
		const FTextureSourceLayer Layer{.Format = ToTextureFormat(Info.Format)};
		return InitLayeredImpl(ETextureSourceKind::TextureCube,
			std::span(&Block, 1), std::span(&Layer, 1), ToTextureGamma(Info.GammaSpace),
			Bytes, InSourceChannelCount, InTransparencyMask, PreferredCompression);
	}

	auto FTextureSource::InitVolume(Image::FImageView Image,
		ETextureSourceCompression PreferredCompression) -> bool
	{
		if (!Image.IsValid() || Image.GetInfo().Depth == 0
			|| Image.GetInfo().SliceCount != 1) return false;
		const FTextureSourceBlock Block{.Width = Image.GetInfo().Width,
			.Height = Image.GetInfo().Height, .Depth = Image.GetInfo().Depth};
		const FTextureSourceLayer Layer{.Format = ToTextureFormat(Image.GetInfo().Format)};
		return InitLayeredImpl(ETextureSourceKind::Volume,
			std::span(&Block, 1), std::span(&Layer, 1),
			ToTextureGamma(Image.GetInfo().GammaSpace), Image.GetPixels(), 0, 0,
			PreferredCompression);
	}

	auto FTextureSource::InitLongLatCube(Image::FImageView Image,
		uint8 InSourceChannelCount, uint8 InTransparencyMask,
		ETextureSourceCompression PreferredCompression) -> bool
	{
		if (!Image.IsValid() || Image.GetInfo().Depth != 1
			|| Image.GetInfo().SliceCount != 1) return false;
		const FTextureSourceBlock Block{.Width = Image.GetInfo().Width,
			.Height = Image.GetInfo().Height};
		const FTextureSourceLayer Layer{.Format = ToTextureFormat(Image.GetInfo().Format)};
		return InitLayeredImpl(ETextureSourceKind::LongLatCube,
			std::span(&Block, 1), std::span(&Layer, 1),
			ToTextureGamma(Image.GetInfo().GammaSpace), Image.GetPixels(),
			InSourceChannelCount, InTransparencyMask, PreferredCompression);
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
		if (SchemaVersion == LegacyTextureSourceSchemaVersion)
		{
			Migrated.Blocks = {{.Width = Width, .Height = Height, .Depth = Depth,
				.NumSlices = NumSlices}};
			Migrated.Layers = {{.Format = Format, .NumMips = 1}};
			Migrated.GammaSpace = (Kind == ETextureSourceKind::Volume)
				? ETextureSourceGammaSpace::Linear : ETextureSourceGammaSpace::Unknown;
		}
		const FPackageResourceReadResult Read = Payload.GetPayload().Wait();
		if (!Read) return false;
		Migrated.DecodedPayloadSize = Read.Buffer.GetSize();
		const FXxHash128 Hash = FXxHash128::HashBuffer(Read.Buffer.GetBytes());
		Migrated.CanonicalPayloadHashLow = Hash.HashLow;
		Migrated.CanonicalPayloadHashHigh = Hash.HashHigh;
		Migrated.Compression = ETextureSourceCompression::Raw;
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
		const uint64 CanonicalSize = SchemaVersion == TextureSourceSchemaVersion
			? DecodedPayloadSize : Payload.GetPayloadSize();
		const FXxHash128 CanonicalHash = SchemaVersion == TextureSourceSchemaVersion
			? FXxHash128{.HashLow = CanonicalPayloadHashLow,
				.HashHigh = CanonicalPayloadHashHigh}
			: Payload.GetPayloadId();
		Builder.UpdateValue(CanonicalSize);
		Builder.UpdateValue(CanonicalHash);
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
		FSharedByteBuffer Decoded = Read.Buffer;
		if (SchemaVersion == TextureSourceSchemaVersion
			&& Compression == ETextureSourceCompression::RunLength)
		{
			FByteArray Bytes;
			if (!DecodeRunLength(Read.Buffer.GetBytes(), DecodedPayloadSize, Bytes))
				return SetError("Texture source compressed payload is corrupt.", OutError);
			Decoded = FSharedByteBuffer::Take(std::move(Bytes));
		}
		if (SchemaVersion == TextureSourceSchemaVersion
			&& FXxHash128::HashBuffer(Decoded.GetBytes()) != FXxHash128{
				.HashLow = CanonicalPayloadHashLow, .HashHigh = CanonicalPayloadHashHigh})
			return SetError("Texture source decoded payload identity does not match.", OutError);
		FTextureSourceSnapshot NewSnapshot{.Blocks = Blocks, .Layers = Layers,
			.Payload = Decoded, .Kind = Kind, .GammaSpace = GammaSpace,
			.Compression = ETextureSourceCompression::Raw,
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
		if (Compression != ETextureSourceCompression::Raw)
			return SetError("Texture source snapshots must expose decoded payload bytes.", OutError);
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
