#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"

namespace Durin
{
	// Reports invalid source geometry, incomplete bytes, and packed size overflow.
	enum class ERHITextureUploadDataError : uint8
	{
		InvalidRegion,
		SourceTooSmall,
		FootprintOverflow,
	};
	RHI_API auto ToString(ERHITextureUploadDataError Error) -> std::string_view;

	// Validated source footprint and tightly packed destination layout.
	struct FRHITextureUploadLayout final
	{
		uint64 SourceOffset = 0;
		uint64 RequiredSourceBytes = 0;
		uint32 SourceRowPitch = 0, SourceDepthPitch = 0;
		uint32 RowPitch = 0, DepthPitch = 0, NumRows = 0, NumSlices = 0;
		uint32 Size = 0;
	};
	RHI_API auto GetTextureUploadDesc(const FRHITexture& Texture) -> FRHITextureDesc;
	RHI_API auto ComputeTextureUploadLayout(EPixelFormat Format,
		const FUpdateTextureRegion3D& Region, uint32 SourceRowPitch,
		uint32 SourceDepthPitch, size_t SourceSize)
		-> std::expected<FRHITextureUploadLayout, ERHITextureUploadDataError>;
	RHI_API auto CopyTextureUploadData(const FRHITextureUploadLayout& Layout,
		FByteView Source, FMutableByteView Destination) -> void;

	class FRHIBufferUploadAccounting;
	// Immutable packed CPU bytes shared by graph declarations and recorded commands.
	class FRHITextureUploadData final
	{
	public:
		RHI_API static auto Pack2D(const FRHITextureDesc& Desc, uint32 Mip, uint32 Layer,
			const FUpdateTextureRegion2D& Region, uint32 SourcePitch, FByteView Source)
			-> std::expected<std::shared_ptr<const FRHITextureUploadData>, ERHITextureUploadDataError>;
		RHI_API static auto Pack3D(const FRHITextureDesc& Desc, uint32 Mip,
			const FUpdateTextureRegion3D& Region, uint32 RowPitch, uint32 DepthPitch, FByteView Source)
			-> std::expected<std::shared_ptr<const FRHITextureUploadData>, ERHITextureUploadDataError>;
		auto GetData() const -> FByteView { return {Bytes.get(), Layout.Size}; }
		auto GetLayout() const -> const FRHITextureUploadLayout& { return Layout; }
		auto GetRegion3D() const -> const FUpdateTextureRegion3D& { return Region; }
		auto GetRegion2D() const -> FUpdateTextureRegion2D
		{ return {Region.DestX, Region.DestY, 0, 0, Region.Width, Region.Height}; }
		auto GetFormat() const -> EPixelFormat { return Format; }
		auto GetMip() const -> uint32 { return Mip; }
		auto GetLayer() const -> uint32 { return Layer; }
		auto IsVolume() const -> bool { return bVolume; }
		auto IsFullOverwrite() const -> bool { return bFullOverwrite; }
	private:
		static auto Pack(EPixelFormat Format, uint32 Mip, uint32 Layer,
			FUpdateTextureRegion3D Region, uint32 RowPitch, uint32 DepthPitch,
			FByteView Source, bool bVolume, bool bFullOverwrite)
			-> std::expected<std::shared_ptr<const FRHITextureUploadData>, ERHITextureUploadDataError>;
		FRHITextureUploadData() = default;
		std::shared_ptr<const FRHIBufferUploadAccounting> Reservation;
		std::unique_ptr<std::byte[]> Bytes;
		FRHITextureUploadLayout Layout;
		FUpdateTextureRegion3D Region;
		EPixelFormat Format = EPixelFormat::Unknown;
		uint32 Mip = 0, Layer = 0;
		bool bVolume = false, bFullOverwrite = false;
	};
}
