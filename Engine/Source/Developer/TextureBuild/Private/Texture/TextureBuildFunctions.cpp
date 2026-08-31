#include "Texture/TextureBuildFunctions.h"

#include "Serialization/BinaryFormat.h"
#include "Serialization/Archive.h"
#include "Texture/Texture2DDerivedData.h"
#include "Texture/TextureBuilder.h"
#include "Texture/TextureCubeBuilder.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/VolumeTextureBuilder.h"

namespace Durin::Asset::Private
{
	const FBuildFunctionName Texture2DFunctionName =
		FBuildFunctionName::FromString("Durin.TextureBuild.Texture2D");
	const FBuildFunctionName TextureCubeFunctionName =
		FBuildFunctionName::FromString("Durin.TextureBuild.TextureCube");
	const FBuildFunctionName VolumeTextureFunctionName =
		FBuildFunctionName::FromString("Durin.TextureBuild.VolumeTexture");

	namespace
	{
		constexpr uint64 TextureDerivedDataBudgetBytes = 4ull * 1024ull * 1024ull * 1024ull;
		constexpr uint32 TextureDerivedDataCleanupDeleteLimit = 16;
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};

		auto DecodeTexture2DLocalInput(std::span<const std::byte> Bytes,
			FTextureSourceData& Source, FTexture2DBuildSettings& Settings,
			bool& bSRGB, std::string& OutError) -> bool
		{
			FBinaryReader Reader(Bytes);
			uint32 Usage = 0, Quality = 0, AlphaMode = 0, AlphaThreshold = 0;
			uint64 PixelCount = 0;
			uint8 SourceFormat = 0, HasTransparency = 0, SRGB = 0;
			if (!Reader.ReadU32(Source.Width)
				|| !Reader.ReadU32(Source.Height)
				|| !Reader.ReadU8(Source.SourceChannelCount)
				|| !Reader.ReadU8(SourceFormat)
				|| !Reader.ReadU8(HasTransparency)
				|| !Reader.ReadU8(SRGB)
				|| !Reader.ReadU32(Usage)
				|| !Reader.ReadU32(Quality)
				|| !Reader.ReadU32(AlphaMode)
				|| !Reader.ReadU32(AlphaThreshold)
				|| !Reader.ReadU32(Settings.MaxResolution)
				|| !Reader.ReadU64(PixelCount)
				|| PixelCount != Reader.GetRemainingBytes()
				|| !Reader.ReadBytes(Source.Pixels, PixelCount, Bytes.size())
				|| !Reader.IsAtEnd()) goto Invalid;
			Source.Format = static_cast<ETextureSourceFormat>(SourceFormat);
			Source.bHasTransparency = HasTransparency != 0;
			bSRGB = SRGB != 0;
			Settings.Usage = static_cast<ETextureUsage>(Usage);
			Settings.CompressionQuality = static_cast<ETextureCompressionQuality>(Quality);
			Settings.AlphaMipMode = static_cast<ETextureAlphaMipMode>(AlphaMode);
			Settings.AlphaCoverageThreshold = std::bit_cast<float>(AlphaThreshold);
			Settings.bSRGB = bSRGB;
			if (Source.IsValid()) return true;
		Invalid:
			OutError = "Texture2D local build input is malformed.";
			return false;
		}

		auto ValidateCubeSourceData(
			const FTextureCubeSourceData& SourceData, std::string& OutError) -> bool
		{
			const FTextureSourceData& Reference = SourceData.Faces[0];
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				const FTextureSourceData& Face = SourceData.Faces[Index];
				if (!Face.IsValid())
				{
					OutError = std::format("{} face source data is invalid.", FaceNames[Index]);
					return false;
				}
				if (Face.Width != Face.Height)
				{
					OutError = std::format("{} face must be square, but is {}x{}.",
						FaceNames[Index], Face.Width, Face.Height);
					return false;
				}
				if (Face.Width != Reference.Width || Face.Height != Reference.Height
					|| Face.SourceChannelCount != Reference.SourceChannelCount)
				{
					OutError = std::format(
						"{} face source layout must be identical to PositiveX.", FaceNames[Index]);
					return false;
				}
			}
			return true;
		}

		auto DecodeTextureCubeLocalInput(std::span<const std::byte> Bytes,
			FTextureCubeSourceData& OutSourceData, std::string& OutError) -> bool
		{
			FBinaryReader Reader(Bytes);
			FTextureCubeSourceData Candidate;
			for (FTextureSourceData& Face : Candidate.Faces)
			{
				uint32 Channels = 0, Format = 0, Transparency = 0;
				uint64 ByteCount = 0;
				if (!Reader.ReadU32(Face.Width)
					|| !Reader.ReadU32(Face.Height)
					|| !Reader.ReadU32(Channels)
					|| !Reader.ReadU32(Format)
					|| !Reader.ReadU32(Transparency)
					|| !Reader.ReadU64(ByteCount)
					|| Channels > std::numeric_limits<uint8>::max()
					|| ByteCount > Reader.GetRemainingBytes()
					|| !Reader.ReadBytes(Face.Pixels, ByteCount, Bytes.size()))
				{
					OutError = "TextureCube local build input is malformed.";
					return false;
				}
				Face.SourceChannelCount = static_cast<uint8>(Channels);
				Face.Format = static_cast<ETextureSourceFormat>(Format);
				Face.bHasTransparency = Transparency != 0;
			}
			if (!Reader.IsAtEnd() || !ValidateCubeSourceData(Candidate, OutError))
			{
				if (OutError.empty())
					OutError = "TextureCube local build input has trailing bytes.";
				return false;
			}
			OutSourceData = std::move(Candidate);
			return true;
		}

		auto BuildCubePlatformData(const FTextureCubeSourceData& SourceData,
			bool bSRGB, FTextureCubePlatformData& OutPlatformData,
			std::string& OutError) -> bool
		{
			if (!ValidateCubeSourceData(SourceData, OutError)) return false;
			const bool bHasTransparency = std::ranges::any_of(
				SourceData.Faces, [](const FTextureSourceData& Face) {
					return Face.bHasTransparency;
				});
			for (size_t Index = 0; Index < TextureCubeFaceCount; ++Index)
			{
				FTextureSourceData BuildSource = SourceData.Faces[Index];
				BuildSource.bHasTransparency = bHasTransparency;
				if (!TextureBuilder::BuildMipChain(BuildSource, ETextureUsage::Color,
					bSRGB, OutPlatformData.Faces[Index], OutError))
				{
					OutError = std::format(
						"{} face platform build failed: {}", FaceNames[Index], OutError);
					return false;
				}
			}
			OutPlatformData.PixelFormat = OutPlatformData.Faces[0].PixelFormat;
			if (OutPlatformData.IsValid()) return true;
			OutError = "Cube texture platform data is inconsistent.";
			return false;
		}

		auto EncodeTextureCubePlatformValue(const FTextureCubePlatformData& PlatformData,
			FBuildValue& OutValue, std::string& OutError) -> bool
		{
			FByteArray Bytes;
			FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
			const_cast<FTextureCubePlatformData&>(PlatformData).Serialize(Ar, {
				.TargetPlatform = Asset::ECookTargetPlatform::Win64,
				.TargetProfile = Asset::ECookTargetProfile::Game});
			if (Ar.HasError())
			{
				OutError = Ar.GetFailure()->Message;
				return false;
			}
			OutValue = FBuildValue::FromOwned(
				std::string(TextureCubeValueName), std::move(Bytes));
			OutError.clear();
			return true;
		}

		class FTexture2DBuildFunction final : public IBuildFunction
		{
		public:
			auto GetConfig() const -> FBuildFunctionConfig override
			{
				return {.Version = Texture2DBuilderVersion,
					.CacheBucket = "Textures/Objects",
					.ExpectedValueName = std::string(Texture2DValueName),
					.MaximumValueBytes = MaximumTexturePayloadBytes,
					.CleanupBudgetBytes = TextureDerivedDataBudgetBytes,
					.CleanupDeleteLimit = TextureDerivedDataCleanupDeleteLimit};
			}

			auto Validate(const FBuildDefinition&, const FBuildValue& Value,
				std::string& OutError) const -> bool override
			{
				FTexturePlatformData Data;
				return Value.GetName() == Texture2DValueName
					&& DecodeTexture2DPlatformValue(Value, Data, OutError);
			}

			auto Build(const FBuildContext& Context, FBuildValue& OutValue,
				std::string& OutError) const -> bool override
			{
				const FBuildValue* Input = Context.GetInput(Texture2DInputName);
				FTextureSourceData Source;
				FTexture2DBuildSettings Settings;
				bool bSRGB = true;
				if (!Input || !DecodeTexture2DLocalInput(
					Input->GetBytes(), Source, Settings, bSRGB, OutError)) return false;
				FTexturePlatformData PlatformData;
				const TextureBuilder::FBuildExecutionControl Control{
					.ShouldCancel = [&Context] { return Context.IsCanceled(); }};
				if (!TextureBuilder::BuildMipChain(Source, Settings.Usage, bSRGB,
					PlatformData, OutError, Settings.MaxResolution,
					Settings.CompressionQuality, Settings.AlphaMipMode,
					Settings.AlphaCoverageThreshold, &Control)) return false;
				if (Context.IsCanceled())
				{
					OutError = "Texture2D build was cancelled.";
					return false;
				}
				FByteArray Bytes;
				FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
				PlatformData.Serialize(Ar, {
					.TargetPlatform = Asset::ECookTargetPlatform::Win64,
					.TargetProfile = Asset::ECookTargetProfile::Game});
				if (Ar.HasError())
				{
					OutError = Ar.GetError();
					return false;
				}
				OutValue = FBuildValue::FromOwned(
					std::string(Texture2DValueName), std::move(Bytes));
				return true;
			}
		};

		class FTextureCubeBuildFunction final : public IBuildFunction
		{
		public:
			auto GetConfig() const -> FBuildFunctionConfig override
			{
				return {.Version = TextureCubeBuilderVersion,
					.CacheBucket = "TextureCube/Objects",
					.ExpectedValueName = std::string(TextureCubeValueName),
					.MaximumValueBytes = MaximumTexturePayloadBytes,
					.CleanupBudgetBytes = TextureDerivedDataBudgetBytes,
					.CleanupDeleteLimit = TextureDerivedDataCleanupDeleteLimit};
			}

			auto Validate(const FBuildDefinition& Definition, const FBuildValue& Value,
				std::string& OutError) const -> bool override
			{
				if (Definition.GetTargetFact("Platform")
						!= std::optional<std::string_view>("Win64")
					|| Definition.GetTargetFact("Profile")
						!= std::optional<std::string_view>("Game"))
				{
					OutError = "TextureCube target facts are missing or incompatible.";
					return false;
				}
				FTextureCubePlatformData PlatformData;
				if (!DecodeTextureCubePlatformValue(Value, PlatformData, OutError))
					return false;
				const auto DimensionFact = Definition.GetTargetFact("Dimension");
				uint32 Dimension = 0;
				if (DimensionFact
					&& (!ParseBuildTargetFactUInt32(*DimensionFact, Dimension)
						|| PlatformData.Faces[0].Mips[0].Width != Dimension))
				{
					OutError = "TextureCube payload dimension does not match the definition.";
					return false;
				}
				return true;
			}

			auto Build(const FBuildContext& Context, FBuildValue& OutValue,
				std::string& OutError) const -> bool override
			{
				const FBuildValue* Input = Context.GetInput(TextureCubeInputName);
				FTextureCubeSourceData SourceData;
				if (!Input || !DecodeTextureCubeLocalInput(
					Input->GetBytes(), SourceData, OutError)) return false;
				if (Context.IsCanceled())
				{
					OutError = "TextureCube build was canceled.";
					return false;
				}
				const auto SRGB = Context.GetDefinition().GetTargetFact("SRGB");
				if (!SRGB || (*SRGB != "0" && *SRGB != "1"))
				{
					OutError = "TextureCube sRGB target fact is missing.";
					return false;
				}
				FTextureCubePlatformData PlatformData;
				if (!BuildCubePlatformData(SourceData, *SRGB == "1", PlatformData, OutError))
					return false;
				if (Context.IsCanceled())
				{
					OutError = "TextureCube build was canceled.";
					return false;
				}
				return EncodeTextureCubePlatformValue(PlatformData, OutValue, OutError);
			}
		};

		class FVolumeTextureBuildFunction final : public IBuildFunction
		{
		public:
			auto GetConfig() const -> FBuildFunctionConfig override
			{
				return {.Version = VolumeTextureBuilderVersion,
					.CacheBucket = "VolumeTexture/Objects",
					.ExpectedValueName = std::string(VolumeTextureValueName),
					.MaximumValueBytes = MaximumTexturePayloadBytes,
					.CleanupBudgetBytes = TextureDerivedDataBudgetBytes,
					.CleanupDeleteLimit = TextureDerivedDataCleanupDeleteLimit};
			}

			auto Validate(const FBuildDefinition&, const FBuildValue& Value,
				std::string& OutError) const -> bool override
			{
				FVolumeTexturePlatformData Data;
				return DecodeVolumeTexturePlatformValue(Value, Data, OutError);
			}

			auto Build(const FBuildContext& Context, FBuildValue& OutValue,
				std::string& OutError) const -> bool override
			{
				const FBuildValue* Input = Context.GetInput(VolumeTextureInputName);
				if (!Input)
				{
					OutError = "Volume texture local input is missing.";
					return false;
				}
				FBinaryReader Reader(Input->GetBytes());
				FVolumeTextureSourceData Source;
				FVolumeTextureBuildSettings Settings;
				FByteArray Voxels;
				uint32 Schema = 0, Format = 0, Filter = 0;
				uint64 ByteCount = 0;
				if (!Reader.ReadU32(Schema)
					|| !Reader.ReadU32(Source.Width) || !Reader.ReadU32(Source.Height)
					|| !Reader.ReadU32(Source.Depth) || !Reader.ReadU32(Format)
					|| !Reader.ReadU32(Filter) || !Reader.ReadU64(ByteCount)
					|| ByteCount != Reader.GetRemainingBytes()
					|| !Reader.ReadBytes(Voxels, ByteCount, Input->GetBytes().size())
					|| !Reader.IsAtEnd())
				{
					OutError = "Volume texture local input is malformed.";
					return false;
				}
				Source.PayloadSchemaVersion = Schema;
				if (!Source.SetVoxelBytes(Voxels))
				{
					OutError = "Volume texture local input bulk publication failed.";
					return false;
				}
				Source.Format = static_cast<EVolumeTextureFormat>(Format);
				Settings.OutputFormat = static_cast<EVolumeTextureFormat>(Format);
				Settings.MipFilter = static_cast<EVolumeTextureMipFilter>(Filter);
				FVolumeTexturePlatformData PlatformData;
				if (!VolumeTextureBuilder::BuildMipChain(
					Source, Settings, PlatformData, OutError)) return false;
				FByteArray Bytes;
				FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
				PlatformData.Serialize(Ar, {
					.TargetPlatform = Asset::ECookTargetPlatform::Win64,
					.TargetProfile = Asset::ECookTargetProfile::Game});
				if (Ar.HasError())
				{
					OutError = Ar.GetFailure()->Message;
					return false;
				}
				OutValue = FBuildValue::FromOwned(
					std::string(VolumeTextureValueName), std::move(Bytes));
				return true;
			}
		};
	}

	auto EncodeTexture2DLocalInput(const FTexture2DBuildRequest& Request, bool bSRGB)
		-> FByteArray
	{
		FBinaryWriter Writer;
		Writer.WriteU32(Request.SourceData.Width);
		Writer.WriteU32(Request.SourceData.Height);
		Writer.WriteU8(Request.SourceData.SourceChannelCount);
		Writer.WriteU8(static_cast<uint8>(Request.SourceData.Format));
		Writer.WriteU8(Request.SourceData.bHasTransparency);
		Writer.WriteU8(bSRGB);
		Writer.WriteU32(static_cast<uint32>(Request.Settings.Usage));
		Writer.WriteU32(
			static_cast<uint32>(Request.Settings.CompressionQuality));
		Writer.WriteU32(static_cast<uint32>(Request.Settings.AlphaMipMode));
		Writer.WriteU32(
			std::bit_cast<uint32>(Request.Settings.AlphaCoverageThreshold));
		Writer.WriteU32(Request.Settings.MaxResolution);
		Writer.WriteU64(Request.SourceData.Pixels.size());
		Writer.WriteBytes(Request.SourceData.Pixels);
		return Writer.TakeBytes();
	}

	auto DecodeTexture2DPlatformValue(const FBuildValue& Value,
		FTexturePlatformData& OutData, std::string& OutError) -> bool
	{
		FCanonicalMemoryReader Ar(Value.GetBytes(), EArchivePurpose::DerivedDataPayload);
		OutData.Serialize(Ar, {.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (!Ar.HasError() && RequireArchiveEnd(Ar) && OutData.IsValid()) return true;
		OutError = Ar.GetError().empty() ? "Texture2D payload is invalid." : Ar.GetError();
		return false;
	}

	auto EncodeTextureCubeLocalInput(const FTextureCubeSourceData& SourceData)
		-> FByteArray
	{
		FBinaryWriter Writer;
		for (const FTextureSourceData& Face : SourceData.Faces)
		{
			Writer.WriteU32(Face.Width);
			Writer.WriteU32(Face.Height);
			Writer.WriteU32(Face.SourceChannelCount);
			Writer.WriteU32(static_cast<uint32>(Face.Format));
			Writer.WriteU32(Face.bHasTransparency ? 1u : 0u);
			Writer.WriteU64(Face.Pixels.size());
			Writer.WriteBytes(Face.Pixels);
		}
		return Writer.TakeBytes();
	}

	auto DecodeTextureCubePlatformValue(const FBuildValue& Value,
		FTextureCubePlatformData& OutPlatformData, std::string& OutError) -> bool
	{
		if (Value.GetName() != TextureCubeValueName)
		{
			OutError = "TextureCube build value name is incompatible.";
			return false;
		}
		FTextureCubePlatformData Candidate;
		FCanonicalMemoryReader Ar(Value.GetBytes(), EArchivePurpose::DerivedDataPayload);
		Candidate.Serialize(Ar, {.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (Ar.HasError() || !RequireArchiveEnd(Ar) || !Candidate.IsValid())
		{
			OutError = Ar.GetFailure() ? Ar.GetFailure()->Message
				: "TextureCube payload is invalid or has trailing bytes.";
			return false;
		}
		OutPlatformData = std::move(Candidate);
		OutError.clear();
		return true;
	}

	auto EncodeVolumeTextureLocalInput(const FVolumeTextureSourceData& SourceData,
		const FVolumeTextureBuildSettings& Settings) -> FByteArray
	{
		FBinaryWriter Writer;
		Writer.WriteU32(SourceData.PayloadSchemaVersion);
		Writer.WriteU32(SourceData.Width);
		Writer.WriteU32(SourceData.Height);
		Writer.WriteU32(SourceData.Depth);
		Writer.WriteU32(static_cast<uint32>(Settings.OutputFormat));
		Writer.WriteU32(static_cast<uint32>(Settings.MipFilter));
		Writer.WriteU64(SourceData.GetVoxelBytes().size());
		Writer.WriteBytes(SourceData.GetVoxelBytes());
		return Writer.TakeBytes();
	}

	auto DecodeVolumeTexturePlatformValue(const FBuildValue& Value,
		FVolumeTexturePlatformData& OutData, std::string& OutError) -> bool
	{
		if (Value.GetName() != VolumeTextureValueName)
		{
			OutError = "Volume texture build value name is incompatible.";
			return false;
		}
		FVolumeTexturePlatformData Candidate;
		FCanonicalMemoryReader Ar(Value.GetBytes(), EArchivePurpose::DerivedDataPayload);
		Candidate.Serialize(Ar, {.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game});
		if (Ar.HasError() || !RequireArchiveEnd(Ar) || !Candidate.IsValid())
		{
			OutError = Ar.GetFailure() ? Ar.GetFailure()->Message
				: "Volume texture payload is invalid or has trailing bytes.";
			return false;
		}
		OutData = std::move(Candidate);
		OutError.clear();
		return true;
	}

	auto CreateTexture2DBuildFunction() -> std::shared_ptr<IBuildFunction>
	{
		return std::make_shared<FTexture2DBuildFunction>();
	}

	auto CreateTextureCubeBuildFunction() -> std::shared_ptr<IBuildFunction>
	{
		return std::make_shared<FTextureCubeBuildFunction>();
	}

	auto CreateVolumeTextureBuildFunction() -> std::shared_ptr<IBuildFunction>
	{
		return std::make_shared<FVolumeTextureBuildFunction>();
	}
}
