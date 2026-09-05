#pragma once

#include "Asset/EditorBulkData.h"
#include "EngineAPI.h"
#include "Image/Image.h"

#include "TextureSource.gen.h"

namespace Durin
{
	class DTexture;
	class DTexture2D;
	class DTextureCube;
	class DVolumeTexture;
	inline constexpr uint32 TextureSourceSchemaVersion = 3;
	inline constexpr uint32 DescriptorTextureSourceSchemaVersion = 2;
	inline constexpr uint32 LegacyTextureSourceSchemaVersion = 1;
	inline constexpr uint64 MaximumTextureSourceBytes = 512ull * 1024ull * 1024ull;

	DENUM()
	enum class ETextureSourceKind : uint8
	{
		Texture2D,
		TextureCube,
		Volume,
		TextureArray,
		LongLatCube,
	};

	DENUM()
	enum class ETextureSourceFormat : uint8
	{
		Invalid,
		RGBA8,
		R8_UNORM,
		RG8_UNORM,
		R16_FLOAT,
		RGBA16_FLOAT,
		G16_UNORM,
		RGBA16_UNORM,
		R32_FLOAT,
		RGBA32_FLOAT,
	};

	DENUM()
	enum class ETextureSourceGammaSpace : uint8
	{
		Unknown,
		Linear,
		SRGB,
	};

	DENUM()
	enum class ETextureSourceCompression : uint8
	{
		Raw,
		RunLength,
	};

	DSTRUCT()
	struct FTextureSourceBlock
	{
		GENERATED_BODY()

		DPROPERTY()
		uint32 Width = 0;

		DPROPERTY()
		uint32 Height = 0;

		DPROPERTY()
		uint32 Depth = 1;

		DPROPERTY()
		uint32 NumSlices = 1;

		auto operator==(const FTextureSourceBlock&) const -> bool = default;
	};

	DSTRUCT()
	struct FTextureSourceLayer
	{
		GENERATED_BODY()

		DPROPERTY()
		ETextureSourceFormat Format = ETextureSourceFormat::Invalid;

		DPROPERTY()
		uint32 NumMips = 1;

		auto operator==(const FTextureSourceLayer&) const -> bool = default;
	};

	struct FTextureSourceMipInfo
	{
		Image::FImageInfo ImageInfo;
		uint64 PayloadOffset = 0;
		uint64 PayloadSize = 0;
	};

	struct FTextureSourceSnapshot
	{
		std::vector<FTextureSourceBlock> Blocks;
		std::vector<FTextureSourceLayer> Layers;
		FSharedByteBuffer Payload;
		ETextureSourceKind Kind = ETextureSourceKind::Texture2D;
		ETextureSourceGammaSpace GammaSpace = ETextureSourceGammaSpace::Unknown;
		ETextureSourceCompression Compression = ETextureSourceCompression::Raw;
		uint8 SourceChannelCount = 0;
		uint8 TransparencyMask = 0;
		FXxHash128 Identity;
		uint64 Generation = 0;

		ENGINE_API auto IsValid(std::string* OutError = nullptr) const -> bool;
		ENGINE_API auto GetMipInfo(uint32 BlockIndex, uint32 LayerIndex,
			uint32 MipIndex, FTextureSourceMipInfo& OutInfo,
			std::string* OutError = nullptr) const -> bool;
		ENGINE_API auto GetMipImage(uint32 BlockIndex, uint32 LayerIndex,
			uint32 MipIndex, Image::FImageView& OutImage,
			std::string* OutError = nullptr) const -> bool;
	};

	// Owns authoritative editor source art independently from family recipes.
	DSTRUCT()
	struct FTextureSource
	{
		GENERATED_BODY()

	private:
		DPROPERTY()
		FEditorBulkData Payload;

		// Legacy v1 mirrors remain reflected so existing DAST v9 packages load.
		DPROPERTY()
		uint32 Width = 0;
		DPROPERTY()
		uint32 Height = 0;
		DPROPERTY()
		uint32 Depth = 1;
		DPROPERTY()
		uint8 NumSlices = 1;
		DPROPERTY()
		uint8 SourceChannelCount = 0;
		DPROPERTY()
		ETextureSourceFormat Format = ETextureSourceFormat::Invalid;
		DPROPERTY()
		ETextureSourceKind Kind = ETextureSourceKind::Texture2D;
		DPROPERTY()
		bool bHasTransparency = false;
		DPROPERTY()
		uint8 TransparencyMask = 0;

		DPROPERTY()
		std::vector<FTextureSourceBlock> Blocks;

		DPROPERTY()
		std::vector<FTextureSourceLayer> Layers;

		DPROPERTY()
		ETextureSourceGammaSpace GammaSpace = ETextureSourceGammaSpace::Unknown;

		DPROPERTY()
		uint32 SchemaVersion = TextureSourceSchemaVersion;

		DPROPERTY()
		ETextureSourceCompression Compression = ETextureSourceCompression::Raw;

		DPROPERTY()
		uint64 DecodedPayloadSize = 0;

		DPROPERTY()
		uint64 CanonicalPayloadHashLow = 0;

		DPROPERTY()
		uint64 CanonicalPayloadHashHigh = 0;

	public:
		auto GetWidth() const -> uint32
		{
			return Blocks.empty() ? Width : Blocks[0].Width;
		}
		auto GetHeight() const -> uint32
		{
			return Blocks.empty() ? Height : Blocks[0].Height;
		}
		auto GetDepth() const -> uint32
		{
			return Blocks.empty() ? Depth : Blocks[0].Depth;
		}
		auto GetNumSlices() const -> uint32
		{
			return Blocks.empty() ? NumSlices : Blocks[0].NumSlices;
		}
		auto GetSourceChannelCount() const -> uint8 { return SourceChannelCount; }
		auto GetFormat() const -> ETextureSourceFormat
		{
			return Layers.empty() ? Format : Layers[0].Format;
		}
		auto GetKind() const -> ETextureSourceKind { return Kind; }
		auto HasTransparency() const -> bool { return bHasTransparency; }
		auto GetTransparencyMask() const -> uint8 { return TransparencyMask; }
		auto GetBlocks() const -> std::span<const FTextureSourceBlock> { return Blocks; }
		auto GetLayers() const -> std::span<const FTextureSourceLayer> { return Layers; }
		auto GetGammaSpace() const -> ETextureSourceGammaSpace { return GammaSpace; }
		auto GetCompression() const -> ETextureSourceCompression { return Compression; }
		auto GetDecodedPayloadSize() const -> uint64 { return DecodedPayloadSize; }
		auto GetSchemaVersion() const -> uint32 { return SchemaVersion; }
		auto GetBulkData() const -> const FEditorBulkData& { return Payload; }

		ENGINE_API auto IsValid() const -> bool;
		ENGINE_API auto Init2D(Image::FImageView Image, uint8 InSourceChannelCount,
			uint8 InTransparencyMask = 0,
			ETextureSourceCompression PreferredCompression = ETextureSourceCompression::Raw) -> bool;
		ENGINE_API auto InitCube(std::span<const Image::FImageView> Faces,
			uint8 InSourceChannelCount, uint8 InTransparencyMask = 0,
			ETextureSourceCompression PreferredCompression = ETextureSourceCompression::Raw) -> bool;
		ENGINE_API auto InitVolume(Image::FImageView Image,
			ETextureSourceCompression PreferredCompression = ETextureSourceCompression::Raw) -> bool;
		ENGINE_API auto InitLongLatCube(Image::FImageView Image,
			uint8 InSourceChannelCount, uint8 InTransparencyMask = 0,
			ETextureSourceCompression PreferredCompression = ETextureSourceCompression::Raw) -> bool;
		ENGINE_API auto InitLayered(ETextureSourceKind InKind,
			std::span<const FTextureSourceBlock> InBlocks,
			std::span<const FTextureSourceLayer> InLayers,
			ETextureSourceGammaSpace InGammaSpace,
			std::span<const std::byte> DecodedPayload,
			uint8 InSourceChannelCount = 0, uint8 InTransparencyMask = 0,
			ETextureSourceCompression PreferredCompression = ETextureSourceCompression::Raw) -> bool;
		ENGINE_API auto Reset() -> void;
		ENGINE_API auto MigrateLegacy() -> bool;
		ENGINE_API auto GetIdentity() const -> FXxHash128;
		ENGINE_API auto GetMipInfo(uint32 BlockIndex, uint32 LayerIndex,
			uint32 MipIndex, FTextureSourceMipInfo& OutInfo,
			std::string* OutError = nullptr) const -> bool;
		auto ReadPayloadAsync() const -> FPackageResourceRequest
		{
			return Payload.GetPayload();
		}
		ENGINE_API auto CreateSnapshotBlocking(uint64 Generation,
			FTextureSourceSnapshot& OutSnapshot,
			std::string* OutError = nullptr) const -> bool;
		auto GetOwner() -> DTexture* { return Owner; }
		auto GetOwner() const -> const DTexture* { return Owner; }

	private:
		friend class DTexture;
		friend class DTexture2D;
		friend class DTextureCube;
		friend class DVolumeTexture;
		auto BindOwner(DTexture* InOwner) -> void { Owner = InOwner; }
		ENGINE_API auto InitLayeredImpl(ETextureSourceKind InKind,
			std::span<const FTextureSourceBlock> InBlocks,
			std::span<const FTextureSourceLayer> InLayers,
			ETextureSourceGammaSpace InGammaSpace,
			std::span<const std::byte> DecodedPayload,
			uint8 InSourceChannelCount, uint8 InTransparencyMask,
			ETextureSourceCompression PreferredCompression) -> bool;
		// Non-owning runtime back-reference; reflection and source identity exclude it.
		DTexture* Owner = nullptr;
	};
} // namespace Durin
