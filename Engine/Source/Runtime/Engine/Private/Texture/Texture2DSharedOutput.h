#pragma once

#include "CoreMinimal.h"

#if DURIN_WITH_EDITOR
#include "DerivedDataBuildOutput.h"
#include "TexturePlatformFormat.h"

namespace Durin::TexturePrivate
{
	struct FTexture2DOutputLayout
	{
		struct FMip
		{
			uint32 Width = 0, Height = 0, RowPitch = 0;
			const DerivedData::FValue* Value = nullptr;
		};
		EPixelFormat Format = EPixelFormat::Unknown;
		uint32 Count = 0;
		std::array<FMip, MaximumTextureMipCount> Mips;
	};

	// Reads bounded descriptors only. No source resolution, archive payload or
	// disposable platform product is required for semantic validation.
	template <typename TOutput>
	inline auto ReadTexture2DOutputLayout(const TOutput& Output,
		ECookTargetPlatform Platform, ECookTargetProfile Profile)
		-> std::expected<FTexture2DOutputLayout, std::string>
	{
		if ((Platform != ECookTargetPlatform::Win64
				&& Platform != ECookTargetPlatform::MacOS)
			|| (Profile != ECookTargetProfile::Game && Profile != ECookTargetProfile::EditorValidation)
			|| Output.GetSchema() != "Texture2D.Output" || Output.GetSchemaVersion() != 3)
			return std::unexpected("Texture output schema or target is unsupported.");
		const auto Metadata = Output.FindMeta(DerivedData::FValueId::FromName("Metadata"));
		const auto Format = Metadata.Find("Format").AsUInt64(); const auto Mips = Metadata.Find("Mips").AsArray();
		FTexture2DOutputLayout Layout;
		if (!Metadata.IsValid() || Metadata.Num() != 4 || !Format || *Format > UINT32_MAX
			|| Metadata.Find("Platform").AsUInt64() != static_cast<uint32>(Platform)
			|| Metadata.Find("Profile").AsUInt64() != static_cast<uint32>(Profile)
			|| !FromStablePixelFormat(uint32(*Format), Layout.Format) || !Mips.IsValid() || Mips.Num() == 0
			|| Mips.Num() > MaximumTextureMipCount || Output.GetValues().size() != Mips.Num())
			return std::unexpected("Texture output header is invalid.");
		Layout.Count = Mips.Num(); const auto Fields = Mips.GetFields(); uint64 Total = 0;
		for (const auto& Item : Output.GetMetadata()) Total += Item.Object.GetBytes().GetSize();
		for (uint32 Index = 0; Index < Layout.Count; ++Index)
		{
			auto& Mip = Layout.Mips[Index];
			const auto Descriptor = Fields[Index].AsObject(); const auto Width = Descriptor.Find("Width").AsUInt64();
			const auto Height = Descriptor.Find("Height").AsUInt64(); const auto Pitch = Descriptor.Find("RowPitch").AsUInt64();
			const auto Size = Descriptor.Find("Size").AsUInt64();
			if (Descriptor.Num() != 4 || !Width || !Height || !Pitch || !Size || *Width == 0 || *Height == 0
				|| *Width > MaximumTexture2DDimension || *Height > MaximumTexture2DDimension || *Pitch > UINT32_MAX
				|| Total > MaximumTexturePayloadBytes || *Size > MaximumTexturePayloadBytes - Total)
				return std::unexpected("Texture output mip extent is invalid.");
			Mip.Width = uint32(*Width); Mip.Height = uint32(*Height); Mip.RowPitch = uint32(*Pitch); Total += *Size;
			Mip.Value = Output.FindValue(DerivedData::FValueId::FromName("Durin.Texture2D.Mip").MakeIndexed(Index));
			const auto Expected = GetPixelFormatLayout(Layout.Format, Mip.Width, Mip.Height);
			if (!Mip.Value || Expected.DataSize == 0 || *Size != Expected.DataSize
				|| Mip.Value->GetRawSize() != *Size || Mip.RowPitch != Expected.RowPitch)
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
		if (Layout.Mips[Layout.Count - 1].Width != 1
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
		FCbWriter Metadata, Mips;
		Metadata.AddUInt64(static_cast<uint32>(Platform), "Platform"); Metadata.AddUInt64(static_cast<uint32>(Profile), "Profile");
		Metadata.AddUInt64(static_cast<uint32>(Format), "Format");
		DerivedData::FBuildOutputBuilder Output("Texture2D.Output", 3, {.MaximumTotalBytes = MaximumTexturePayloadBytes});
		for (size_t Index = 0; Index < Product.Mips.size(); ++Index)
		{
			const auto& Mip = Product.Mips[Index];
			FCbWriter Descriptor; Descriptor.AddUInt64(Mip.Width, "Width"); Descriptor.AddUInt64(Mip.Height, "Height");
			Descriptor.AddUInt64(Mip.RowPitch, "RowPitch"); Descriptor.AddUInt64(Mip.Pixels.GetSize(), "Size");
			auto Object = Descriptor.SaveObject(); if (!Object || !Mips.AddObject(std::move(*Object))) return std::unexpected("Texture mip metadata is invalid.");
			Output.AddValue(DerivedData::FValueId::FromName("Durin.Texture2D.Mip").MakeIndexed(Index), Mip.Pixels);
		}
		auto Array = Mips.SaveArray(); if (!Array || !Metadata.AddArray(std::move(*Array), "Mips")) return std::unexpected("Texture mip table is invalid.");
		auto Meta = Metadata.SaveObject();
		if (!Meta || !Output.AddMeta(DerivedData::FValueId::FromName("Metadata"), std::move(*Meta))) return std::unexpected("Texture output metadata is invalid.");
		auto Built = std::move(Output).Build();
		if (!Built) return Built;
		if (auto Layout = ReadTexture2DOutputLayout(*Built, Platform, Profile); !Layout)
			return std::unexpected(std::move(Layout.error()));
		return Built;
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
			Product.Mips.push_back({.Pixels = Mip.Value->GetData(), .Width = Mip.Width,
				.Height = Mip.Height, .RowPitch = Mip.RowPitch});
		}
		return Product;
	}
}
#endif
