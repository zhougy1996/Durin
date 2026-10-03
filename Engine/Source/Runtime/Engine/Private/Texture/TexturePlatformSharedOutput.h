#pragma once

#include "CoreMinimal.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildOutput.h"
#include "Serialization/BinaryFormat.h"
#include "TexturePlatformFormat.h"
#include "Texture/TextureCubeData.h"
#include "Texture/VolumeTextureData.h"

namespace Durin::TexturePrivate
{
	struct FTexturePlatformOutputLayout
	{
		struct FBlock
		{
			uint32 Width = 0, Height = 0, Depth = 1, RowPitch = 0, DepthPitch = 0;
			const DerivedData::FValue* Value = nullptr;
		};
		EPixelFormat Format = EPixelFormat::Unknown;
		uint32 MipCount = 0;
		std::array<FBlock, TextureCubeFaceCount * MaximumTextureMipCount> Blocks;
	};

	inline auto TexturePlatformValueId(bool Cube, uint32 Face, uint32 Mip) -> DerivedData::FValueId
	{ return DerivedData::FValueId::FromName(Cube ? "Durin.TextureCube.Block" : "Durin.VolumeTexture.Mip").MakeIndexed(Cube ? Face * MaximumTextureMipCount + Mip : Mip); }

	// Reads descriptors only, with no temporary family product or source acquisition.
	inline auto ReadTexturePlatformOutputLayout(const DerivedData::FBuildOutput& Output,
		bool Cube, ECookTargetPlatform Platform, ECookTargetProfile Profile)
		-> std::expected<FTexturePlatformOutputLayout, std::string>
	{
		if (Platform != ECookTargetPlatform::Win64
			|| (Profile != ECookTargetProfile::Game && Profile != ECookTargetProfile::EditorValidation)
			|| Output.GetSchema() != (Cube ? "TextureCube.Output" : "VolumeTexture.Output") || Output.GetSchemaVersion() != 2)
			return std::unexpected("Texture output schema or target is unsupported.");
		const uint32 Faces = Cube ? TextureCubeFaceCount : 1;
		const auto Metadata = DerivedData::GetBuildMetadataPayload(Output);
		FBinaryReader Reader(Metadata.GetBytes(), {.MaximumTotalBytes = 16 + Faces * MaximumTextureMipCount * (Cube ? 20u : 28u)});
		uint32 StoredPlatform = 0, StoredProfile = 0, Format = 0;
		FTexturePlatformOutputLayout Result;
		if (!Reader.ReadU32(StoredPlatform) || !Reader.ReadU32(StoredProfile)
			|| !Reader.ReadU32(Format) || !Reader.ReadU32(Result.MipCount)
			|| StoredPlatform != static_cast<uint32>(Platform) || StoredProfile != static_cast<uint32>(Profile)
			|| !(Cube ? FromStablePixelFormat(Format, Result.Format) : FromVolumeStableFormat(Format, Result.Format))
			|| !Result.MipCount || Result.MipCount > MaximumTextureMipCount
			|| Output.GetValues().size() != Faces * Result.MipCount)
			return std::unexpected("Texture output header is invalid.");
		uint64 Total = Metadata.GetSize();
		for (uint32 Face = 0; Face < Faces; ++Face)
			for (uint32 Mip = 0; Mip < Result.MipCount; ++Mip)
			{
				auto& Block = Result.Blocks[Face * Result.MipCount + Mip];
				uint64 Size = 0;
				if (!Reader.ReadU32(Block.Width) || !Reader.ReadU32(Block.Height)
					|| (!Cube && !Reader.ReadU32(Block.Depth)) || !Reader.ReadU32(Block.RowPitch)
					|| (!Cube && !Reader.ReadU32(Block.DepthPitch)) || !Reader.ReadU64(Size))
					return std::unexpected("Texture output block descriptor is truncated.");
				const uint32 Limit = Cube ? MaximumTextureCubeDimension : MaximumVolumeTextureDimension;
				if (!Block.Width || !Block.Height || !Block.Depth || Block.Width > Limit || Block.Height > Limit
					|| Block.Depth > Limit || (Cube && Block.Width != Block.Height)
					|| Total > MaximumTexturePayloadBytes || Size > MaximumTexturePayloadBytes - Total)
					return std::unexpected("Texture output block extent exceeds its bound.");
				Total += Size;
				const auto Slice = GetPixelFormatLayout(Result.Format, Block.Width, Block.Height);
				Block.Value = Output.FindValue(TexturePlatformValueId(Cube, Face, Mip));
				if (!Block.Value || !Slice.DataSize || Slice.RowPitch != Block.RowPitch
					|| (!Cube && Slice.DataSize != Block.DepthPitch)
					|| Slice.DataSize > MaximumTexturePayloadBytes / Block.Depth
					|| Size != Slice.DataSize * Block.Depth || Block.Value->GetRawSize() != Size)
					return std::unexpected("Texture output block does not match its format and pitches.");
				if (Mip)
				{
					const auto& Previous = Result.Blocks[Face * Result.MipCount + Mip - 1];
					if ((Previous.Width == 1 && Previous.Height == 1 && Previous.Depth == 1)
						|| Block.Width != std::max(1u, Previous.Width / 2)
						|| Block.Height != std::max(1u, Previous.Height / 2) || Block.Depth != std::max(1u, Previous.Depth / 2))
						return std::unexpected("Texture output mip progression is invalid.");
				}
				if (Face && (Block.Width != Result.Blocks[Mip].Width || Block.Height != Result.Blocks[Mip].Height))
					return std::unexpected("Texture output cube faces disagree.");
				if (Mip + 1 == Result.MipCount && (Block.Width != 1 || Block.Height != 1 || Block.Depth != 1))
					return std::unexpected("Texture output mip chain is incomplete.");
			}
		if (!Reader.IsAtEnd()) return std::unexpected("Texture output has trailing metadata.");
		return Result;
	}

	inline auto MakeTextureCubeSharedOutput(const FTextureCubePlatformData& Product,
		ECookTargetPlatform Platform, ECookTargetProfile Profile) -> std::expected<DerivedData::FBuildOutput, std::string>
	{
		ETextureStablePixelFormat Format;
		if (!Product.IsValid() || Product.Faces[0].Mips.size() > MaximumTextureMipCount || !ToStablePixelFormat(Product.PixelFormat, Format))
			return std::unexpected("Cube product cannot form a complete output.");
		FBinaryWriter Metadata({.MaximumTotalBytes = 16 + TextureCubeFaceCount * MaximumTextureMipCount * 20});
		Metadata.WriteU32(static_cast<uint32>(Platform)); Metadata.WriteU32(static_cast<uint32>(Profile));
		Metadata.WriteU32(static_cast<uint32>(Format)); Metadata.WriteU32(static_cast<uint32>(Product.Faces[0].Mips.size()));
		DerivedData::FBuildOutputBuilder Output("TextureCube.Output", 2, {.MaximumTotalBytes = MaximumTexturePayloadBytes});
		for (uint32 Face = 0; Face < TextureCubeFaceCount; ++Face)
			for (uint32 Index = 0; Index < Product.Faces[Face].Mips.size(); ++Index)
			{
				const auto& Mip = Product.Faces[Face].Mips[Index];
				Metadata.WriteU32(Mip.Width); Metadata.WriteU32(Mip.Height); Metadata.WriteU32(Mip.RowPitch); Metadata.WriteU64(Mip.Pixels.GetSize());
				Output.AddValue(TexturePlatformValueId(true, Face, Index), Mip.Pixels);
			}
		if (Metadata.HasError()) return std::unexpected("Cube output metadata exceeds its bound.");
		auto Meta = DerivedData::MakeBuildMetadata(FSharedByteBuffer::Take(Metadata.TakeBytes()));
		if (!Meta || !Output.AddMeta(DerivedData::FValueId::FromName("Metadata"), std::move(*Meta))) return std::unexpected("Cube output metadata is invalid.");
		auto Built = std::move(Output).Build(); if (!Built) return Built;
		if (auto Layout = ReadTexturePlatformOutputLayout(*Built, true, Platform, Profile); !Layout) return std::unexpected(std::move(Layout.error()));
		return Built;
	}

	inline auto MakeVolumeTextureSharedOutput(const FVolumeTexturePlatformData& Product,
		ECookTargetPlatform Platform, ECookTargetProfile Profile) -> std::expected<DerivedData::FBuildOutput, std::string>
	{
		ETextureStablePixelFormat Format;
		if (!Product.IsValid() || Product.Mips.size() > MaximumTextureMipCount || !ToVolumeStableFormat(Product.PixelFormat, Format))
			return std::unexpected("Volume product cannot form a complete output.");
		FBinaryWriter Metadata({.MaximumTotalBytes = 16 + MaximumTextureMipCount * 28});
		Metadata.WriteU32(static_cast<uint32>(Platform)); Metadata.WriteU32(static_cast<uint32>(Profile));
		Metadata.WriteU32(static_cast<uint32>(Format)); Metadata.WriteU32(static_cast<uint32>(Product.Mips.size()));
		DerivedData::FBuildOutputBuilder Output("VolumeTexture.Output", 2, {.MaximumTotalBytes = MaximumTexturePayloadBytes});
		for (uint32 Index = 0; Index < Product.Mips.size(); ++Index)
		{
			const auto& Mip = Product.Mips[Index];
			Metadata.WriteU32(Mip.Width); Metadata.WriteU32(Mip.Height); Metadata.WriteU32(Mip.Depth);
			Metadata.WriteU32(Mip.RowPitch); Metadata.WriteU32(Mip.DepthPitch); Metadata.WriteU64(Mip.Voxels.GetSize());
			Output.AddValue(TexturePlatformValueId(false, 0, Index), Mip.Voxels);
		}
		if (Metadata.HasError()) return std::unexpected("Volume output metadata exceeds its bound.");
		auto Meta = DerivedData::MakeBuildMetadata(FSharedByteBuffer::Take(Metadata.TakeBytes()));
		if (!Meta || !Output.AddMeta(DerivedData::FValueId::FromName("Metadata"), std::move(*Meta))) return std::unexpected("Volume output metadata is invalid.");
		auto Built = std::move(Output).Build(); if (!Built) return Built;
		if (auto Layout = ReadTexturePlatformOutputLayout(*Built, false, Platform, Profile); !Layout) return std::unexpected(std::move(Layout.error()));
		return Built;
	}

	inline auto AssembleTextureCubeSharedOutput(const DerivedData::FBuildOutput& Output,
		ECookTargetPlatform Platform, ECookTargetProfile Profile) -> std::expected<std::unique_ptr<FTextureCubePlatformData>, std::string>
	{
		auto Layout = ReadTexturePlatformOutputLayout(Output, true, Platform, Profile);
		if (!Layout) return std::unexpected(std::move(Layout.error()));
		auto Product = std::make_unique<FTextureCubePlatformData>(); Product->PixelFormat = Layout->Format;
		for (uint32 Face = 0; Face < TextureCubeFaceCount; ++Face)
		{
			Product->Faces[Face].PixelFormat = Layout->Format;
			for (uint32 Index = 0; Index < Layout->MipCount; ++Index)
			{
				const auto& Block = Layout->Blocks[Face * Layout->MipCount + Index];
				Product->Faces[Face].Mips.push_back({.Pixels = Block.Value->GetData(),
					.Width = Block.Width, .Height = Block.Height, .RowPitch = Block.RowPitch});
			}
		}
		return Product;
	}
	inline auto AssembleVolumeTextureSharedOutput(const DerivedData::FBuildOutput& Output,
		ECookTargetPlatform Platform, ECookTargetProfile Profile) -> std::expected<std::unique_ptr<FVolumeTexturePlatformData>, std::string>
	{
		auto Layout = ReadTexturePlatformOutputLayout(Output, false, Platform, Profile);
		if (!Layout) return std::unexpected(std::move(Layout.error()));
		auto Product = std::make_unique<FVolumeTexturePlatformData>(); Product->PixelFormat = Layout->Format;
		for (uint32 Index = 0; Index < Layout->MipCount; ++Index)
		{
			const auto& Block = Layout->Blocks[Index];
			Product->Mips.push_back({.Voxels = Block.Value->GetData(), .Width = Block.Width, .Height = Block.Height,
				.Depth = Block.Depth, .RowPitch = Block.RowPitch, .DepthPitch = Block.DepthPitch});
		}
		return Product;
	}
}
#endif
