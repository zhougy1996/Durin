#include "Texture/TextureDerivedData.h"
#include "TexturePlatformFormat.h"
#include "TextureDerivedDataKey.h"

#include "Serialization/Archive.h"
#include "Texture/TextureCube.h"
#include "Texture/TexturePayloadContainer.h"

#if DURIN_WITH_EDITOR
#include "DerivedDataCache/DerivedDataCache.h"
#include "DerivedDataBuildDefinition.h"
#include "TextureCubeBuildFunction.h"
#endif

namespace Durin
{
	namespace
	{
		auto InvalidBuildKey(FArchiveFailure* OutFailure,
			EArchiveFailureCode Code, std::string_view Message) -> bool
		{
			if (OutFailure) *OutFailure = {.Code = Code, .Message = std::string(Message)};
			return false;
		}

		auto IsSupportedTarget(ECookTargetPlatform Platform, ECookTargetProfile Profile) -> bool
		{
			return Platform == ECookTargetPlatform::Win64
				&& (Profile == ECookTargetProfile::Game
					|| Profile == ECookTargetProfile::EditorValidation);
		}

		auto IsCompleteMipChain(const FTexturePlatformData& PlatformData) -> bool
		{
			return PlatformData.IsValid()
				&& PlatformData.Mips.size() <= MaximumTextureMipCount
				&& PlatformData.Mips.front().Width <= MaximumTexture2DDimension
				&& PlatformData.Mips.front().Height <= MaximumTexture2DDimension
				&& PlatformData.Mips.back().Width == 1
				&& PlatformData.Mips.back().Height == 1;
		}

		auto IsCompleteCubeMipChain(const FTextureCubePlatformData& PlatformData) -> bool
		{
			if (!PlatformData.IsValid()) return false;
			const FTexturePlatformData& Reference = PlatformData.Faces[0];
			if (Reference.Mips.size() > MaximumTextureMipCount
				|| Reference.Mips.front().Width > MaximumTextureCubeDimension
				|| Reference.Mips.front().Height > MaximumTextureCubeDimension
				|| Reference.Mips.back().Width != 1
				|| Reference.Mips.back().Height != 1) return false;
			for (const FTexturePlatformData& Face : PlatformData.Faces)
			{
				for (size_t MipIndex = 0; MipIndex < Face.Mips.size(); ++MipIndex)
				{
					const FTexture2DMipData& Mip = Face.Mips[MipIndex];
					if (!Mip.IsValid(PlatformData.PixelFormat)) return false;
					if (MipIndex > 0)
					{
						const FTexture2DMipData& Previous = Face.Mips[MipIndex - 1];
						if (Mip.Width != std::max(Previous.Width / 2, 1u)
							|| Mip.Height != std::max(Previous.Height / 2, 1u)) return false;
					}
				}
			}
			return true;
		}

	}

	auto FTexture2DBuildKeyInput::IsValid(FArchiveFailure* OutFailure) const -> bool
	{
		if (OutFailure) *OutFailure = {};
		if (!IsSupportedTarget(TargetPlatform, TargetProfile)
			|| !IsValidTextureUsage(Usage)
			|| !IsValidTextureCompressionQuality(CompressionQuality)
			|| !IsValidTextureAlphaMipMode(AlphaMipMode)
			|| !IsValidTextureAlphaCoverageThreshold(AlphaCoverageThreshold))
		{
			return InvalidBuildKey(OutFailure, EArchiveFailureCode::InvalidData,
				"Texture2D derived-data key input is invalid.");
		}
		return true;
	}

	auto FTextureCubeBuildKeyInput::IsValid(FArchiveFailure* OutFailure) const -> bool
	{
		if (OutFailure) *OutFailure = {};

		if (!IsSupportedTarget(TargetPlatform, TargetProfile))
		{
			return InvalidBuildKey(OutFailure, EArchiveFailureCode::InvalidData,
				"TextureCube derived-data target is unsupported.");
		}
		if (!std::isfinite(ExposureEV)
			|| std::bit_cast<uint32>(ExposureEV) == 0x80000000u
			|| ExposureEV < -32.0f || ExposureEV > 32.0f)
		{
			return InvalidBuildKey(OutFailure, EArchiveFailureCode::InvalidData,
				"TextureCube panorama exposure is not canonical.");
		}
		if (FaceDimension > MaximumTextureCubeDimension)
		{
			return InvalidBuildKey(OutFailure, EArchiveFailureCode::LimitExceeded,
				"TextureCube requested face dimension exceeds the supported limit.");
		}
		if (SourceLayout != ETextureCubeBuildSourceLayout::SixFaces
			&& SourceLayout != ETextureCubeBuildSourceLayout::EquirectangularPanorama)
		{
			return InvalidBuildKey(OutFailure, EArchiveFailureCode::InvalidData,
				"TextureCube source layout is unsupported.");
		}
		return true;
	}

	auto FVolumeTextureBuildKeyInput::IsValid(FArchiveFailure* OutFailure) const -> bool
	{
		if (OutFailure) *OutFailure = {};
		if (Width == 0 || Height == 0 || Depth == 0
			|| Width > MaximumVolumeTextureDimension
			|| Height > MaximumVolumeTextureDimension
			|| Depth > MaximumVolumeTextureDimension
			|| SourcePayloadSchemaVersion != VolumeTextureSourcePayloadSchemaVersion
			|| !IsSupportedTarget(TargetPlatform, TargetProfile))
		{
			return InvalidBuildKey(OutFailure, EArchiveFailureCode::InvalidData,
				"Volume texture derived-data key input is invalid.");
		}
		return true;
	}

#if DURIN_WITH_EDITOR
	auto MakeTexture2DBuildAction(const FTexture2DBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildAction, DerivedData::FBuildDefinitionError>
	{
		using namespace DerivedData;
		if (!Input.IsValid())
			return std::unexpected(FBuildDefinitionError{EBuildDefinitionError::InvalidConstant, "Texture2D"});
		auto Definition = FBuildDefinition::TryCreate("Durin.Texture2D",
			{{"Usage", uint64(Input.Usage)}, {"SRGB", Input.bSRGB},
			 {"Quality", uint64(Input.CompressionQuality)}, {"AlphaMode", uint64(Input.AlphaMipMode)},
			 {"MaximumResolution", uint64(Input.MaximumResolution)}, {"AlphaThreshold", Input.AlphaCoverageThreshold},
			 {"TargetPlatform", uint64(Input.TargetPlatform)}, {"TargetProfile", uint64(Input.TargetProfile)}},
			{{"Source", "CapturedSource"}});
		if (!Definition) return std::unexpected(std::move(Definition.error()));
		return FBuildAction::TryCreate(*Definition, {"Durin.Texture2D", Input.BuilderVersion, 1,
			"Texture2D.Output", Input.OutputSchemaVersion, FCacheBucket::FromString(Texture2DCacheBucket)},
			{{"Source", Input.SourceIdentity, "TextureSource", TextureSourceSchemaVersion, "Texture2D.RGBA8", 1}});
	}
	auto MakeTextureCubeBuildAction(const FTextureCubeBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildAction, DerivedData::FBuildDefinitionError>
	{
		using namespace DerivedData;
		auto Definition = TexturePrivate::MakeTextureCubeSessionDefinition(Input);
		if (!Definition) return std::unexpected(std::move(Definition.error()));
		const bool Panorama = Input.SourceLayout == ETextureCubeBuildSourceLayout::EquirectangularPanorama;
		return FBuildAction::TryCreate(*Definition, {"Durin.TextureCube", Input.BuilderVersion, 2,
			"TextureCube.Output", Input.OutputSchemaVersion, FCacheBucket::FromString(TextureCubeCacheBucket)},
			{{"Source", Input.CanonicalSourceIdentity, "TextureSource", TextureSourceSchemaVersion,
				Panorama ? "Panorama.RGBA32F" : "Cube.RGBA8", 1}});
	}

	auto MakeVolumeTextureBuildAction(const FVolumeTextureBuildKeyInput& Input)
		-> std::expected<DerivedData::FBuildAction, DerivedData::FBuildDefinitionError>
	{
		using namespace DerivedData;
		if (!Input.IsValid())
			return std::unexpected(FBuildDefinitionError{EBuildDefinitionError::InvalidConstant, "VolumeTexture"});
		auto Definition = FBuildDefinition::TryCreate("Durin.VolumeTexture",
			{{"Width", uint64(Input.Width)}, {"Height", uint64(Input.Height)}, {"Depth", uint64(Input.Depth)},
			 {"Format", uint64(Input.Settings.OutputFormat)}, {"MipFilter", uint64(Input.Settings.MipFilter)},
			 {"SourceSchema", uint64(Input.SourcePayloadSchemaVersion)},
			 {"TargetPlatform", uint64(Input.TargetPlatform)}, {"TargetProfile", uint64(Input.TargetProfile)}},
			{{"Source", "CapturedSource"}});
		if (!Definition) return std::unexpected(std::move(Definition.error()));
		return FBuildAction::TryCreate(*Definition, {"Durin.VolumeTexture", Input.BuilderVersion, 1,
			"VolumeTexture.Output", 1, FCacheBucket::FromString(VolumeTextureCacheBucket)},
			{{"Source", Input.CanonicalSourceIdentity, "TextureSource", TextureSourceSchemaVersion, "Volume.Voxels", 1}});
	}

#endif

	auto BuildTexture2DDerivedDataKeyBytes(const FTexture2DBuildKeyInput& Input) -> FByteBuffer
	{
#if DURIN_WITH_EDITOR
		auto Definition = MakeTexture2DBuildAction(Input);
		if (Definition)
			return FByteBuffer(Definition->GetCanonicalBytes().begin(), Definition->GetCanonicalBytes().end());
#endif
		return {};
	}

	auto BuildTexture2DDerivedDataKey(const FTexture2DBuildKeyInput& Input) -> FCacheKeyProxy
	{
#if DURIN_WITH_EDITOR
		auto Definition = MakeTexture2DBuildAction(Input);
		if (Definition) return FCacheKeyProxy(Definition->GetKey());
#endif
		return {};
	}

	auto BuildTextureCubeDerivedDataKeyBytes(const FTextureCubeBuildKeyInput& Input, std::string& OutError) -> FByteBuffer
	{
		OutError.clear();
#if DURIN_WITH_EDITOR
		auto Definition = MakeTextureCubeBuildAction(Input);
		if (Definition) return FByteBuffer(Definition->GetCanonicalBytes().begin(), Definition->GetCanonicalBytes().end());
#endif
		OutError = "Invalid TextureCube build definition.";
		return {};
	}

	auto BuildTextureCubeDerivedDataKey(const FTextureCubeBuildKeyInput& Input, std::string& OutError) -> FCacheKeyProxy
	{
		OutError.clear();
#if DURIN_WITH_EDITOR
		auto Definition = MakeTextureCubeBuildAction(Input);
		if (Definition) return FCacheKeyProxy(Definition->GetKey());
#endif
		OutError = "Invalid TextureCube build definition.";
		return {};
	}

	auto BuildVolumeTextureDerivedDataKeyBytes(const FVolumeTextureBuildKeyInput& Input, std::string& OutError) -> FByteBuffer
	{
		OutError.clear();
#if DURIN_WITH_EDITOR
		auto Definition = MakeVolumeTextureBuildAction(Input);
		if (Definition) return FByteBuffer(Definition->GetCanonicalBytes().begin(), Definition->GetCanonicalBytes().end());
#endif
		OutError = "Invalid VolumeTexture build definition.";
		return {};
	}

	auto BuildVolumeTextureDerivedDataKey(const FVolumeTextureBuildKeyInput& Input, std::string& OutError) -> FCacheKeyProxy
	{
		OutError.clear();
#if DURIN_WITH_EDITOR
		auto Definition = MakeVolumeTextureBuildAction(Input);
		if (Definition) return FCacheKeyProxy(Definition->GetKey());
#endif
		OutError = "Invalid VolumeTexture build definition.";
		return {};
	}

	auto FTexturePlatformData::Serialize(FArchive& Ar) -> void
	{
		TexturePayloadContainer::FTargetContext Context;
		if (!TexturePayloadContainer::ResolveContext(Ar, Context)) return;
		auto Reject = [&](EArchiveFailureCode Code, std::string_view Message) { Ar.Fail(Code, Message); };
		if (Ar.IsError()) return;
		TexturePayloadContainer::FDescriptor Descriptor{
			.ProducerVersion = Texture2DPayloadProducerVersion, .TargetPlatform = Context.TargetPlatform,
			.TargetProfile = Context.TargetProfile, .Dimension = ETexturePayloadDimension::Texture2D,
			.SliceCount = 1};
		std::vector<TexturePayloadContainer::FPayloadRecord> Records;
		if (Ar.IsSaving())
		{
			if (!IsCompleteMipChain(*this))
				return Reject(EArchiveFailureCode::InvalidData, "Texture2D payload requires complete bounded mip chains.");
			if (!TexturePrivate::ToStablePixelFormat(PixelFormat, Descriptor.StableFormat))
				return Reject(EArchiveFailureCode::UnsupportedType, "Texture pixel format has no stable identifier.");
			Descriptor.MipCount = static_cast<uint32>(Mips.size());
			Records.reserve(Mips.size());
			for (uint32 MipIndex = 0; MipIndex < Mips.size(); ++MipIndex)
			{
				const FTexture2DMipData& Mip = Mips[MipIndex];
				Records.push_back({
					.Record = {
						.Coordinate = 0,
						.MipIndex = MipIndex,
						.Width = Mip.Width,
						.Height = Mip.Height,
						.RowPitch = Mip.RowPitch},
					.Data = FByteView(Mip.Pixels)});
			}
		}
		TexturePayloadContainer::Serialize(Ar, Descriptor, Records);
		if (Ar.IsError() || Ar.IsSaving()) return;
		if (Descriptor.Dimension != ETexturePayloadDimension::Texture2D)
			return Reject(EArchiveFailureCode::InvalidData, "Texture2D payload dimension is invalid.");
		EPixelFormat PixelFormat = EPixelFormat::Unknown;
		if (!TexturePrivate::FromStablePixelFormat(static_cast<uint32>(Descriptor.StableFormat), PixelFormat))
			return Reject(EArchiveFailureCode::UnsupportedType,
				"Texture payload pixel format identifier is unsupported.");

		Mips.clear();
		this->PixelFormat = PixelFormat;
		Mips.reserve(Descriptor.MipCount);
		for (uint32 MipIndex = 0; MipIndex < Descriptor.MipCount; ++MipIndex)
		{
			const TexturePayloadContainer::FRecord& Record = Records[MipIndex].Record;
			if (Record.Coordinate != 0 || Record.MipIndex != MipIndex || Record.LayerPitch != 0
				|| Record.Width == 0 || Record.Height == 0
				|| Record.Width > MaximumTexture2DDimension
				|| Record.Height > MaximumTexture2DDimension)
				return Reject(EArchiveFailureCode::InvalidData,
					"Texture payload subresource identity or dimensions are invalid.");
			if (MipIndex > 0)
			{
				const FTexture2DMipData& PreviousMip = Mips.back();
				if (Record.Width != std::max(PreviousMip.Width / 2, 1u)
					|| Record.Height != std::max(PreviousMip.Height / 2, 1u))
					return Reject(EArchiveFailureCode::InvalidData,
						"Texture payload mip dimensions are not a complete progression.");
			}
			const FPixelFormatLayout Layout = GetPixelFormatLayout(
				PixelFormat, Record.Width, Record.Height);
			if (Record.RowPitch != Layout.RowPitch || Record.ByteCount != Layout.DataSize)
				return Reject(EArchiveFailureCode::InvalidData,
					"Texture payload subresource layout does not match its format.");

			FTexture2DMipData& Mip = Mips.emplace_back();
			Mip.Width = Record.Width;
			Mip.Height = Record.Height;
			Mip.RowPitch = Record.RowPitch;
			const FByteView Data = Records[MipIndex].Data;
			Mip.Pixels = FSharedByteBuffer::Copy(Data);
		}
		if (!IsCompleteMipChain(*this))
			return Reject(EArchiveFailureCode::InvalidData,
				"Texture payload mip chain is incomplete or invalid.");
	}


	auto FTextureCubePlatformData::Serialize(FArchive& Ar) -> void
	{
		TexturePayloadContainer::FTargetContext Context;
		if (!TexturePayloadContainer::ResolveContext(Ar, Context)) return;
		auto Reject = [&](EArchiveFailureCode Code, std::string_view Message) { Ar.Fail(Code, Message); };
		if (Ar.IsError()) return;
		TexturePayloadContainer::FDescriptor Descriptor{
			.ProducerVersion = TextureCubeBuilderVersion, .TargetPlatform = Context.TargetPlatform,
			.TargetProfile = Context.TargetProfile, .Dimension = ETexturePayloadDimension::TextureCube,
			.SliceCount = TextureCubeFaceCount};
		std::vector<TexturePayloadContainer::FPayloadRecord> Records;
		if (Ar.IsSaving())
		{
			if (!IsCompleteCubeMipChain(*this))
				return Reject(EArchiveFailureCode::InvalidData, "TextureCube payload requires complete bounded mip chains.");
			if (!TexturePrivate::ToStablePixelFormat(PixelFormat, Descriptor.StableFormat))
				return Reject(EArchiveFailureCode::UnsupportedType, "Texture pixel format has no stable identifier.");
			Descriptor.MipCount = static_cast<uint32>(Faces[0].Mips.size());
			const uint32 MipCount = static_cast<uint32>(Faces[0].Mips.size());
			const uint32 RecordCount = static_cast<uint32>(TextureCubeFaceCount) * MipCount;
			Records.reserve(RecordCount);
			for (uint32 Slice = 0; Slice < TextureCubeFaceCount; ++Slice)
			{
				for (uint32 MipIndex = 0; MipIndex < MipCount; ++MipIndex)
				{
					const FTexture2DMipData& Mip = Faces[Slice].Mips[MipIndex];
					Records.push_back({
						.Record = {
							.Coordinate = Slice,
							.MipIndex = MipIndex,
							.Width = Mip.Width,
							.Height = Mip.Height,
							.RowPitch = Mip.RowPitch},
						.Data = FByteView(Mip.Pixels)});
				}
			}
		}
		TexturePayloadContainer::Serialize(Ar, Descriptor, Records);
		if (Ar.IsError() || Ar.IsSaving()) return;
		if (Descriptor.Dimension != ETexturePayloadDimension::TextureCube)
			return Reject(EArchiveFailureCode::InvalidData, "TextureCube payload dimension is invalid.");
		EPixelFormat PixelFormat = EPixelFormat::Unknown;
		if (!TexturePrivate::FromStablePixelFormat(static_cast<uint32>(Descriptor.StableFormat), PixelFormat))
			return Reject(EArchiveFailureCode::UnsupportedType,
				"Texture payload pixel format identifier is unsupported.");

		for (auto& Face : Faces) Face.Mips.clear();
		this->PixelFormat = PixelFormat;
		for (uint32 RecordIndex = 0; RecordIndex < Records.size(); ++RecordIndex)
		{
			const uint32 ExpectedSlice = RecordIndex / Descriptor.MipCount;
			const uint32 ExpectedMip = RecordIndex % Descriptor.MipCount;
			const TexturePayloadContainer::FRecord& Record = Records[RecordIndex].Record;
			if (Record.Coordinate != ExpectedSlice || Record.MipIndex != ExpectedMip
				|| Record.LayerPitch != 0 || Record.Width == 0 || Record.Height == 0
				|| Record.Width != Record.Height || Record.Width > MaximumTextureCubeDimension)
				return Reject(EArchiveFailureCode::InvalidData,
					"TextureCube payload subresource identity or dimensions are invalid.");

			FTexturePlatformData& Face = Faces[ExpectedSlice];
			Face.PixelFormat = PixelFormat;
			if (ExpectedMip > 0)
			{
				const FTexture2DMipData& PreviousMip = Face.Mips.back();
				if (Record.Width != std::max(PreviousMip.Width / 2, 1u)
					|| Record.Height != std::max(PreviousMip.Height / 2, 1u))
					return Reject(EArchiveFailureCode::InvalidData,
						"TextureCube payload mip dimensions are not a complete progression.");
			}
			else if (ExpectedSlice > 0)
			{
				const FTexture2DMipData& Reference = Faces[0].Mips[0];
				if (Record.Width != Reference.Width || Record.Height != Reference.Height)
					return Reject(EArchiveFailureCode::InvalidData,
						"TextureCube payload face dimensions do not match.");
			}
			const FPixelFormatLayout Layout = GetPixelFormatLayout(
				PixelFormat, Record.Width, Record.Height);
			if (Record.RowPitch != Layout.RowPitch || Record.ByteCount != Layout.DataSize)
				return Reject(EArchiveFailureCode::InvalidData,
					"Texture payload subresource layout does not match its format.");
			if (ExpectedSlice > 0)
			{
				const FTexture2DMipData& Reference = Faces[0].Mips[ExpectedMip];
				if (Record.Width != Reference.Width || Record.Height != Reference.Height
					|| Record.RowPitch != Reference.RowPitch
					|| Record.ByteCount != Reference.Pixels.size())
					return Reject(EArchiveFailureCode::InvalidData,
						"TextureCube payload faces have incompatible mip layouts.");
			}

			FTexture2DMipData& Mip = Face.Mips.emplace_back();
			Mip.Width = Record.Width;
			Mip.Height = Record.Height;
			Mip.RowPitch = Record.RowPitch;
			const FByteView Data = Records[RecordIndex].Data;
			Mip.Pixels = FSharedByteBuffer::Copy(Data);
		}
		if (!IsCompleteCubeMipChain(*this))
			return Reject(EArchiveFailureCode::InvalidData,
				"TextureCube payload mip chains are incomplete or invalid.");
	}

}
