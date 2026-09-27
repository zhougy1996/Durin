#pragma once

#if DURIN_WITH_EDITOR
#include "DerivedDataBuildOutput.h"
#include "Serialization/BinaryFormat.h"
#include "TexturePlatformFormat.h"

namespace Durin::TexturePrivate
{
	struct FTexture2DOutputLayout
	{
		struct FMip
		{
			uint32 Width = 0, Height = 0, RowPitch = 0;
			const DerivedData::FBuildValue* Value = nullptr;
		};
		EPixelFormat Format = EPixelFormat::Unknown;
		uint32 Count = 0;
		std::array<FMip, MaximumTextureMipCount> Mips;
	};

	// Reads bounded descriptors only. No source resolution, archive payload or
	// disposable platform product is required for semantic validation.
	inline auto ReadTexture2DOutputLayout(const DerivedData::FBuildOutput& Output,
		ECookTargetPlatform Platform, ECookTargetProfile Profile)
		-> std::expected<FTexture2DOutputLayout, std::string>
	{
		if (Platform != ECookTargetPlatform::Win64
			|| (Profile != ECookTargetProfile::Game && Profile != ECookTargetProfile::EditorValidation)
			|| Output.GetSchema() != "Texture2D.Output" || Output.GetSchemaVersion() != 1)
			return std::unexpected("Texture output schema or target is unsupported.");
		const auto Metadata = Output.GetMetadata();
		FBinaryReader Reader(Metadata.GetBytes(), {.MaximumTotalBytes = 16 + MaximumTextureMipCount * 20});
		uint32 StoredPlatform = 0, StoredProfile = 0, Format = 0;
		FTexture2DOutputLayout Layout;
		if (!Reader.ReadU32(StoredPlatform) || !Reader.ReadU32(StoredProfile)
			|| !Reader.ReadU32(Format) || !Reader.ReadU32(Layout.Count)
			|| StoredPlatform != static_cast<uint32>(Platform) || StoredProfile != static_cast<uint32>(Profile)
			|| !FromStablePixelFormat(Format, Layout.Format) || Layout.Count == 0
			|| Layout.Count > MaximumTextureMipCount || Output.GetValues().size() != Layout.Count)
			return std::unexpected("Texture output header is invalid.");
		uint64 Total = Metadata.GetSize();
		for (uint32 Index = 0; Index < Layout.Count; ++Index)
		{
			auto& Mip = Layout.Mips[Index];
			uint64 Size = 0;
			if (!Reader.ReadU32(Mip.Width) || !Reader.ReadU32(Mip.Height)
				|| !Reader.ReadU32(Mip.RowPitch) || !Reader.ReadU64(Size)
				|| Mip.Width == 0 || Mip.Height == 0 || Mip.Width > MaximumTexture2DDimension
				|| Mip.Height > MaximumTexture2DDimension || Size > MaximumTexturePayloadBytes - Total)
				return std::unexpected("Texture output mip extent is invalid.");
			Total += Size;
			Mip.Value = Output.FindValue(std::format("Mip/{}", Index));
			const auto Expected = GetPixelFormatLayout(Layout.Format, Mip.Width, Mip.Height);
			if (!Mip.Value || Expected.DataSize == 0 || Size != Expected.DataSize
				|| Mip.Value->Data.GetSize() != Size || Mip.RowPitch != Expected.RowPitch)
				return std::unexpected("Texture output mip block does not match its layout.");
			if (Index)
			{
				const auto& Previous = Layout.Mips[Index - 1];
				if ((Previous.Width == 1 && Previous.Height == 1)
					|| Mip.Width != std::max(Previous.Width / 2, 1u)
					|| Mip.Height != std::max(Previous.Height / 2, 1u))
					return std::unexpected("Texture output mip chain is invalid.");
			}
		}
		if (!Reader.IsAtEnd() || Layout.Mips[Layout.Count - 1].Width != 1
			|| Layout.Mips[Layout.Count - 1].Height != 1)
			return std::unexpected("Texture output is incomplete or has trailing metadata.");
		return Layout;
	}

	inline auto MakeTexture2DSharedOutput(const FTexturePlatformData& Product,
		ECookTargetPlatform Platform, ECookTargetProfile Profile)
		-> std::expected<DerivedData::FBuildOutput, std::string>
	{
		ETextureStablePixelFormat Format;
		if (!Product.IsValid() || Product.Mips.size() > MaximumTextureMipCount
			|| !ToStablePixelFormat(Product.PixelFormat, Format))
			return std::unexpected("Texture product cannot form a complete output.");
		FBinaryWriter Metadata({.MaximumTotalBytes = 16 + MaximumTextureMipCount * 20});
		Metadata.WriteU32(static_cast<uint32>(Platform)); Metadata.WriteU32(static_cast<uint32>(Profile));
		Metadata.WriteU32(static_cast<uint32>(Format)); Metadata.WriteU32(static_cast<uint32>(Product.Mips.size()));
		DerivedData::FBuildOutputData Data{.Schema = "Texture2D.Output", .SchemaVersion = 1};
		Data.Values.reserve(Product.Mips.size());
		for (size_t Index = 0; Index < Product.Mips.size(); ++Index)
		{
			const auto& Mip = Product.Mips[Index];
			Metadata.WriteU32(Mip.Width); Metadata.WriteU32(Mip.Height); Metadata.WriteU32(Mip.RowPitch);
			Metadata.WriteU64(Mip.Pixels.GetSize());
			Data.Values.push_back({std::format("Mip/{}", Index), Mip.Pixels});
		}
		if (Metadata.HasError()) return std::unexpected("Texture output metadata exceeds its limit.");
		Data.Metadata = FSharedByteBuffer::Take(Metadata.TakeBytes());
		auto Output = DerivedData::FBuildOutput::TryCreate(std::move(Data), {.MaximumTotalBytes = MaximumTexturePayloadBytes});
		if (!Output) return Output;
		if (auto Layout = ReadTexture2DOutputLayout(*Output, Platform, Profile); !Layout)
			return std::unexpected(std::move(Layout.error()));
		return Output;
	}

	inline auto AssembleTexture2DSharedOutput(const DerivedData::FBuildOutput& Output,
		ECookTargetPlatform Platform, ECookTargetProfile Profile)
		-> std::expected<FTexturePlatformData, std::string>
	{
		auto Layout = ReadTexture2DOutputLayout(Output, Platform, Profile);
		if (!Layout) return std::unexpected(std::move(Layout.error()));
		FTexturePlatformData Product;
		Product.PixelFormat = Layout->Format;
		Product.Mips.reserve(Layout->Count);
		for (uint32 Index = 0; Index < Layout->Count; ++Index)
		{
			const auto& Mip = Layout->Mips[Index];
			Product.Mips.push_back({.Pixels = Mip.Value->Data, .Width = Mip.Width,
				.Height = Mip.Height, .RowPitch = Mip.RowPitch});
		}
		return Product;
	}
}
#endif
