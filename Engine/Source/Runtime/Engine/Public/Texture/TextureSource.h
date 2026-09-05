#pragma once

#include "Asset/EditorBulkData.h"
#include "EngineAPI.h"
#include "Image/Image.h"

#include "TextureSource.gen.h"

namespace Durin
{
	class DTexture;
	inline constexpr uint32 TextureSourceSchemaVersion = 2;
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

	struct FTextureSourceInitData
	{
		std::vector<FTextureSourceBlock> Blocks;
		std::vector<FTextureSourceLayer> Layers;
		FEditorBulkData Payload;
		ETextureSourceKind Kind = ETextureSourceKind::Texture2D;
		ETextureSourceGammaSpace GammaSpace = ETextureSourceGammaSpace::Unknown;
		uint8 SourceChannelCount = 0;
		uint8 TransparencyMask = 0;
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

	public:
		auto GetWidth() const -> uint32 { return Width; }
		auto GetHeight() const -> uint32 { return Height; }
		auto GetDepth() const -> uint32 { return Depth; }
		auto GetNumSlices() const -> uint32
		{
			return Blocks.empty() ? NumSlices : Blocks[0].NumSlices;
		}
		auto GetSourceChannelCount() const -> uint8 { return SourceChannelCount; }
		auto GetFormat() const -> ETextureSourceFormat { return Format; }
		auto GetKind() const -> ETextureSourceKind { return Kind; }
		auto HasTransparency() const -> bool { return bHasTransparency; }
		auto GetTransparencyMask() const -> uint8 { return TransparencyMask; }
		auto GetBlocks() const -> std::span<const FTextureSourceBlock> { return Blocks; }
		auto GetLayers() const -> std::span<const FTextureSourceLayer> { return Layers; }
		auto GetGammaSpace() const -> ETextureSourceGammaSpace { return GammaSpace; }
		auto GetSchemaVersion() const -> uint32 { return SchemaVersion; }
		auto GetBulkData() const -> const FEditorBulkData& { return Payload; }

		ENGINE_API auto IsValid() const -> bool;
		ENGINE_API auto Validate(std::string* OutError = nullptr) const -> bool;
		ENGINE_API static auto TryCreate(FTextureSourceInitData InitData,
			FTextureSource& OutSource, std::string* OutError = nullptr) -> bool;
		ENGINE_API static auto TryCreate(FTextureSourceInitData InitData,
			std::span<const std::byte> Bytes, FTextureSource& OutSource,
			std::string* OutError = nullptr) -> bool;
		ENGINE_API auto Reset() -> void;
		ENGINE_API auto MigrateLegacy(std::string* OutError = nullptr) -> bool;
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
		auto BindOwner(DTexture* InOwner) -> void { Owner = InOwner; }
		// Non-owning runtime back-reference; reflection and source identity exclude it.
		DTexture* Owner = nullptr;
	};
} // namespace Durin
