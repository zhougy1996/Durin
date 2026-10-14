#include "Backend/RHITextureUpload.h"
#include "Backend/RHIValidation.h"
#include "RHIBufferUploadData.h"
#include "PixelFormat.h"

namespace Durin
{
	auto ToString(ERHITextureUploadDataError Error) -> std::string_view
	{
		switch (Error)
		{
		case ERHITextureUploadDataError::InvalidRegion: return "invalid texture upload region or pitch";
		case ERHITextureUploadDataError::SourceTooSmall: return "texture upload source is incomplete";
		case ERHITextureUploadDataError::FootprintOverflow: return "texture upload footprint overflow";
		}
		return "unknown texture upload error";
	}

	auto GetTextureUploadDesc(const FRHITexture& Texture) -> FRHITextureDesc
	{
		FRHITextureDesc Desc(Texture.GetDimension());
		Desc.Extent = {int32(Texture.GetSizeX()), int32(Texture.GetSizeY())};
		Desc.Depth = uint16(Texture.GetSizeZ());
		Desc.Format = Texture.GetFormat();
		Desc.ArraySize = Texture.GetArraySize();
		Desc.NumMips = Texture.GetNumMips();
		Desc.NumSamples = Texture.GetNumSamples();
		Desc.Flags = Texture.GetFlags();
		return Desc;
	}

	auto ComputeTextureUploadLayout(EPixelFormat Format, const FUpdateTextureRegion3D& Region,
		uint32 SourceRowPitch, uint32 SourceDepthPitch, size_t SourceSize)
		-> std::expected<FRHITextureUploadLayout, ERHITextureUploadDataError>
	{
		if (Format >= EPixelFormat::Count) return std::unexpected(ERHITextureUploadDataError::InvalidRegion);
		const auto& Info = GetPixelFormatInfo(Format);
		if (!Info.BlockSize || !Info.BytesPerBlock || !Region.Width || !Region.Height || !Region.Depth
			|| Region.SrcX < 0 || Region.SrcY < 0 || Region.SrcZ < 0)
			return std::unexpected(ERHITextureUploadDataError::InvalidRegion);
		const auto Slice = GetPixelFormatLayout(Format, Region.Width, Region.Height);
		if (!Slice.BlocksHigh || !Slice.DataSize || Slice.RowPitch > UINT32_MAX / Slice.BlocksHigh
			|| Slice.DataSize > UINT32_MAX / Region.Depth)
			return std::unexpected(ERHITextureUploadDataError::FootprintOverflow);
		if (uint32(Region.SrcX) % Info.BlockSize || uint32(Region.SrcY) % Info.BlockSize
			|| SourceRowPitch < uint64(Region.SrcX / Info.BlockSize) * Info.BytesPerBlock + Slice.RowPitch
			|| ((Region.Depth > 1 || Region.SrcZ > 0)
				&& uint64(Region.SrcY / Info.BlockSize) + Slice.BlocksHigh > SourceDepthPitch / SourceRowPitch))
			return std::unexpected(ERHITextureUploadDataError::InvalidRegion);
		uint64 Required = 0;
		auto Consume = [&](uint64 Bytes) {
			if (Bytes > SourceSize - Required) return false;
			Required += Bytes;
			return true;
		};
		if (!Consume(uint64(Region.SrcZ) * SourceDepthPitch)
			|| !Consume(uint64(Region.SrcY / Info.BlockSize) * SourceRowPitch)
			|| !Consume(uint64(Region.SrcX / Info.BlockSize) * Info.BytesPerBlock))
			return std::unexpected(ERHITextureUploadDataError::SourceTooSmall);
		const uint64 Offset = Required;
		if (!Consume(uint64(Region.Depth - 1) * SourceDepthPitch)
			|| !Consume((Slice.BlocksHigh - 1) * SourceRowPitch) || !Consume(Slice.RowPitch))
			return std::unexpected(ERHITextureUploadDataError::SourceTooSmall);
		return FRHITextureUploadLayout{Offset, Required, SourceRowPitch, SourceDepthPitch,
			uint32(Slice.RowPitch), uint32(Slice.DataSize), uint32(Slice.BlocksHigh), Region.Depth,
			uint32(Slice.DataSize * Region.Depth)};
	}

	auto CopyTextureUploadData(const FRHITextureUploadLayout& Layout,
		FByteView Source, FMutableByteView Destination) -> void
	{
		require(Source.size() >= Layout.RequiredSourceBytes && Destination.size() == Layout.Size);
		const auto* Start = Source.data() + Layout.SourceOffset;
		if (Layout.SourceRowPitch == Layout.RowPitch
			&& (Layout.NumSlices == 1 || Layout.SourceDepthPitch == Layout.DepthPitch))
		{
			std::memcpy(Destination.data(), Start, Layout.Size);
			return;
		}
		for (uint32 Z = 0; Z < Layout.NumSlices; ++Z)
			for (uint32 Row = 0; Row < Layout.NumRows; ++Row)
				std::memcpy(Destination.data() + uint64(Z) * Layout.DepthPitch + uint64(Row) * Layout.RowPitch,
					Start + uint64(Z) * Layout.SourceDepthPitch + uint64(Row) * Layout.SourceRowPitch, Layout.RowPitch);
	}

	auto FRHITextureUploadData::Pack(EPixelFormat Format, uint32 Mip, uint32 Layer,
		FUpdateTextureRegion3D Region, uint32 RowPitch, uint32 DepthPitch, FByteView Source,
		bool bVolume, bool bFullOverwrite)
		-> std::expected<std::shared_ptr<const FRHITextureUploadData>, ERHITextureUploadDataError>
	{
		const auto Layout = ComputeTextureUploadLayout(Format, Region, RowPitch, DepthPitch, Source.size());
		if (!Layout) return std::unexpected(Layout.error());
		auto Result = std::shared_ptr<FRHITextureUploadData>(new FRHITextureUploadData);
		Result->Reservation = FRHIBufferUploadAccounting::Track(Layout->Size);
		Result->Bytes = std::make_unique_for_overwrite<std::byte[]>(Layout->Size);
		CopyTextureUploadData(*Layout, Source, {Result->Bytes.get(), Layout->Size});
		Result->Layout = *Layout;
		Result->Layout.SourceOffset = 0;
		Result->Layout.RequiredSourceBytes = Layout->Size;
		Result->Layout.SourceRowPitch = Layout->RowPitch;
		Result->Layout.SourceDepthPitch = Layout->DepthPitch;
		Region.SrcX = Region.SrcY = Region.SrcZ = 0;
		Result->Region = Region;
		Result->Format = Format;
		Result->Mip = Mip;
		Result->Layer = Layer;
		Result->bVolume = bVolume;
		Result->bFullOverwrite = bFullOverwrite;
		return Result;
	}

	auto FRHITextureUploadData::Pack2D(const FRHITextureDesc& Desc, uint32 Mip, uint32 Layer,
		const FUpdateTextureRegion2D& Region, uint32 SourcePitch, FByteView Source)
		-> std::expected<std::shared_ptr<const FRHITextureUploadData>, ERHITextureUploadDataError>
	{
		if (Desc.Dimension == ETextureDimension::Texture3D
			|| Desc.Format >= EPixelFormat::Count || Mip >= 32 || !ValidateTexture2DUpdate(Desc, Mip, Layer, Region, SourcePitch))
			return std::unexpected(ERHITextureUploadDataError::InvalidRegion);
		const bool bFull = Region.DestX == 0 && Region.DestY == 0
			&& Region.Width == std::max(1u, uint32(Desc.Extent.x) >> Mip)
			&& Region.Height == std::max(1u, uint32(Desc.Extent.y) >> Mip);
		return Pack(Desc.Format, Mip, Layer,
			{Region.DestX, Region.DestY, 0, Region.SrcX, Region.SrcY, 0, Region.Width, Region.Height, 1},
			SourcePitch, 0, Source, false, bFull);
	}

	auto FRHITextureUploadData::Pack3D(const FRHITextureDesc& Desc, uint32 Mip,
		const FUpdateTextureRegion3D& Region, uint32 RowPitch, uint32 DepthPitch, FByteView Source)
		-> std::expected<std::shared_ptr<const FRHITextureUploadData>, ERHITextureUploadDataError>
	{
		if (Desc.Format >= EPixelFormat::Count || Mip >= 32 || !ValidateTexture3DUpdate(Desc, Mip, Region, RowPitch, DepthPitch))
			return std::unexpected(ERHITextureUploadDataError::InvalidRegion);
		const bool bFull = Region.DestX == 0 && Region.DestY == 0 && Region.DestZ == 0
			&& Region.Width == std::max(1u, uint32(Desc.Extent.x) >> Mip)
			&& Region.Height == std::max(1u, uint32(Desc.Extent.y) >> Mip)
			&& Region.Depth == std::max(1u, uint32(Desc.Depth) >> Mip);
		return Pack(Desc.Format, Mip, 0, Region, RowPitch, DepthPitch, Source, true, bFull);
	}
}
