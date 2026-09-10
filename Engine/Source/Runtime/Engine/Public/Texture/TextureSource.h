#pragma once

#include "Asset/EditorBulkData.h"
#include "EngineAPI.h"
#include "Texture/TextureSourceFormat.h"
#include "Image/Image.h"

#include "TextureSource.gen.h"

#include <mutex>

namespace Durin
{
	class DTexture;
	inline constexpr uint32 TextureSourceSchemaVersion = 3;
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

		auto IsValid() const -> bool
		{
			return ImageInfo.IsValid() && PayloadSize > 0;
		}
	};

	// Owns authoritative editor source art independently from family recipes.
	DSTRUCT()
	struct FTextureSource
	{
		GENERATED_BODY()

		// Short-lived read-only decoded mip-chain handle. Family adapters consume it
		// synchronously while the source remains stable, then pass typed owned input
		// to asynchronous builders.
		class FMipData
		{
		public:
			FMipData() = default;
			auto IsValid() const -> bool { return Source && !MipData.IsEmpty(); }
			auto GetData() const -> FSharedByteBuffer { return MipData; }
			ENGINE_API auto GetMipData(uint32 BlockIndex, uint32 LayerIndex,
				uint32 MipIndex) const -> FSharedByteBuffer;
			ENGINE_API auto GetMipImage(uint32 BlockIndex, uint32 LayerIndex,
				uint32 MipIndex) const -> Image::FImageView;

		private:
			friend struct FTextureSource;
			FMipData(const FTextureSource& InSource, FSharedByteBuffer InMipData)
				: Source(&InSource), MipData(std::move(InMipData))
			{
			}

			const FTextureSource* Source = nullptr;
			FSharedByteBuffer MipData;
		};

	private:
		struct FMipDataState
		{
			std::mutex Mutex;
			FSharedByteBuffer LockedMipData;
		};

		DPROPERTY()
		FEditorBulkData Payload;

		// Non-persistent decoded source residency, shared by read-only mip handles.
		mutable std::shared_ptr<FMipDataState> MipDataState =
			std::make_shared<FMipDataState>();

		DPROPERTY()
		uint8 SourceChannelCount = 0;
		DPROPERTY()
		ETextureSourceKind Kind = ETextureSourceKind::Texture2D;
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
			return Blocks.empty() ? 0 : Blocks[0].Width;
		}
		auto GetHeight() const -> uint32
		{
			return Blocks.empty() ? 0 : Blocks[0].Height;
		}
		auto GetDepth() const -> uint32
		{
			return Blocks.empty() ? 1 : Blocks[0].Depth;
		}
		auto GetNumSlices() const -> uint32
		{
			return Blocks.empty() ? 1 : Blocks[0].NumSlices;
		}
		auto GetSourceChannelCount() const -> uint8 { return SourceChannelCount; }
		auto GetFormat() const -> ETextureSourceFormat
		{
			return Layers.empty() ? ETextureSourceFormat::Invalid : Layers[0].Format;
		}
		auto GetKind() const -> ETextureSourceKind { return Kind; }
		auto HasTransparency() const -> bool { return TransparencyMask != 0; }
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
			FByteView DecodedPayload,
			uint8 InSourceChannelCount = 0, uint8 InTransparencyMask = 0,
			ETextureSourceCompression PreferredCompression = ETextureSourceCompression::Raw) -> bool;
		ENGINE_API auto Reset() -> void;
		ENGINE_API auto GetIdentity() const -> FXxHash128;
		ENGINE_API auto GetMipInfo(uint32 BlockIndex, uint32 LayerIndex,
			uint32 MipIndex) const -> FTextureSourceMipInfo;
		ENGINE_API auto GetMipData() const -> FMipData;
		auto ReadPayloadAsync() const -> FPackageResourceRequest
		{
			return Payload.GetPayload();
		}
		ENGINE_API auto ReleaseSourceMemory() const -> void;
		auto GetOwner() -> DTexture* { return Owner; }
		auto GetOwner() const -> const DTexture* { return Owner; }

	private:
		friend class DTexture;
		auto BindOwner(DTexture* InOwner) -> void { Owner = InOwner; }
		ENGINE_API auto InitLayeredImpl(ETextureSourceKind InKind,
			std::span<const FTextureSourceBlock> InBlocks,
			std::span<const FTextureSourceLayer> InLayers,
			ETextureSourceGammaSpace InGammaSpace,
			FByteView DecodedPayload,
			uint8 InSourceChannelCount, uint8 InTransparencyMask,
			ETextureSourceCompression PreferredCompression) -> bool;
		auto InvalidateMipData() const -> void
		{
			const std::shared_ptr<FMipDataState> State = MipDataState;
			std::lock_guard Lock(State->Mutex);
			State->LockedMipData = {};
		}
		// Non-owning runtime back-reference; reflection and source identity exclude it.
		DTexture* Owner = nullptr;
	};
} // namespace Durin
