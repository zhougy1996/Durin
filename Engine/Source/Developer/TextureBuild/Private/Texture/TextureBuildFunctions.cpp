#include "Texture/TextureBuildFunctions.h"

#include "Serialization/Archive.h"
#include "Texture/TextureBuildCodec.h"
#include "Texture/TextureBuilder.h"
#include "Texture/TextureCubeBuilder.h"
#include "Texture/TextureDerivedData.h"

namespace Durin::Asset::Build::Private
{
	const FBuildFunctionIdentity Texture2DFunctionIdentity{
		"Durin.TextureBuild.Texture2D", 1};
	const FBuildFunctionIdentity TextureCubeFunctionIdentity{
		"Durin.TextureBuild.TextureCube", 1};

	namespace
	{
		constexpr uint64 TextureDerivedDataBudgetBytes = 4ull * 1024ull * 1024ull * 1024ull;
		constexpr uint32 TextureDerivedDataCleanupDeleteLimit = 16;
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};

		auto DecodeTexture2DLocalInput(std::span<const uint8> Bytes,
			FTextureSourceData& Source, FTexture2DBuildSettings& Settings,
			bool& bSRGB, std::string& OutError) -> bool
		{
			size_t Offset = 0;
			uint32 Usage = 0, Quality = 0, AlphaMode = 0, AlphaThreshold = 0;
			uint64 PixelCount = 0;
			if (!ReadLittleEndianU32(Bytes, Offset, Source.Width)
				|| !ReadLittleEndianU32(Bytes, Offset, Source.Height)
				|| Offset > Bytes.size() || Bytes.size() - Offset < 4) goto Invalid;
			Source.SourceChannelCount = Bytes[Offset++];
			Source.Format = static_cast<ETextureSourceFormat>(Bytes[Offset++]);
			Source.bHasTransparency = Bytes[Offset++] != 0;
			bSRGB = Bytes[Offset++] != 0;
			if (!ReadLittleEndianU32(Bytes, Offset, Usage)
				|| !ReadLittleEndianU32(Bytes, Offset, Quality)
				|| !ReadLittleEndianU32(Bytes, Offset, AlphaMode)
				|| !ReadLittleEndianU32(Bytes, Offset, AlphaThreshold)
				|| !ReadLittleEndianU32(Bytes, Offset, Settings.MaxResolution)
				|| !ReadLittleEndianU64(Bytes, Offset, PixelCount)
				|| Offset > Bytes.size() || PixelCount > Bytes.size() - Offset
				|| PixelCount != Bytes.size() - Offset) goto Invalid;
			Settings.Usage = static_cast<ETextureUsage>(Usage);
			Settings.CompressionQuality = static_cast<ETextureCompressionQuality>(Quality);
			Settings.AlphaMipMode = static_cast<ETextureAlphaMipMode>(AlphaMode);
			Settings.AlphaCoverageThreshold = std::bit_cast<float>(AlphaThreshold);
			Settings.bSRGB = bSRGB;
			Source.Pixels.assign(Bytes.begin() + Offset, Bytes.end());
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

		auto DecodeTextureCubeLocalInput(std::span<const uint8> Bytes,
			FTextureCubeSourceData& OutSourceData, std::string& OutError) -> bool
		{
			size_t Offset = 0;
			FTextureCubeSourceData Candidate;
			for (FTextureSourceData& Face : Candidate.Faces)
			{
				uint32 Channels = 0, Format = 0, Transparency = 0;
				uint64 ByteCount = 0;
				if (!ReadLittleEndianU32(Bytes, Offset, Face.Width)
					|| !ReadLittleEndianU32(Bytes, Offset, Face.Height)
					|| !ReadLittleEndianU32(Bytes, Offset, Channels)
					|| !ReadLittleEndianU32(Bytes, Offset, Format)
					|| !ReadLittleEndianU32(Bytes, Offset, Transparency)
					|| !ReadLittleEndianU64(Bytes, Offset, ByteCount)
					|| Channels > std::numeric_limits<uint8>::max()
					|| Offset > Bytes.size() || ByteCount > Bytes.size() - Offset)
				{
					OutError = "TextureCube local build input is malformed.";
					return false;
				}
				Face.SourceChannelCount = static_cast<uint8>(Channels);
				Face.Format = static_cast<ETextureSourceFormat>(Format);
				Face.bHasTransparency = Transparency != 0;
				Face.Pixels.assign(Bytes.begin() + Offset, Bytes.begin() + Offset + ByteCount);
				Offset += static_cast<size_t>(ByteCount);
			}
			if (Offset != Bytes.size() || !ValidateCubeSourceData(Candidate, OutError))
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
			std::vector<uint8> Bytes;
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
				return {.CacheRoot = "Textures/Objects",
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
				std::vector<uint8> Bytes;
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
				return {.CacheRoot = "TextureCube/Objects",
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
	}

	auto EncodeTexture2DLocalInput(const FTexture2DBuildRequest& Request, bool bSRGB)
		-> std::vector<uint8>
	{
		std::vector<uint8> Bytes;
		AppendLittleEndianU32(Bytes, Request.SourceData.Width);
		AppendLittleEndianU32(Bytes, Request.SourceData.Height);
		Bytes.push_back(Request.SourceData.SourceChannelCount);
		Bytes.push_back(static_cast<uint8>(Request.SourceData.Format));
		Bytes.push_back(Request.SourceData.bHasTransparency);
		Bytes.push_back(bSRGB);
		AppendLittleEndianU32(Bytes, static_cast<uint32>(Request.Settings.Usage));
		AppendLittleEndianU32(Bytes,
			static_cast<uint32>(Request.Settings.CompressionQuality));
		AppendLittleEndianU32(Bytes, static_cast<uint32>(Request.Settings.AlphaMipMode));
		AppendLittleEndianU32(Bytes,
			std::bit_cast<uint32>(Request.Settings.AlphaCoverageThreshold));
		AppendLittleEndianU32(Bytes, Request.Settings.MaxResolution);
		AppendLittleEndianU64(Bytes, Request.SourceData.Pixels.size());
		Bytes.insert(Bytes.end(),
			Request.SourceData.Pixels.begin(), Request.SourceData.Pixels.end());
		return Bytes;
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
		-> std::vector<uint8>
	{
		std::vector<uint8> Bytes;
		for (const FTextureSourceData& Face : SourceData.Faces)
		{
			AppendLittleEndianU32(Bytes, Face.Width);
			AppendLittleEndianU32(Bytes, Face.Height);
			AppendLittleEndianU32(Bytes, Face.SourceChannelCount);
			AppendLittleEndianU32(Bytes, static_cast<uint32>(Face.Format));
			AppendLittleEndianU32(Bytes, Face.bHasTransparency ? 1u : 0u);
			AppendLittleEndianU64(Bytes, Face.Pixels.size());
			Bytes.insert(Bytes.end(), Face.Pixels.begin(), Face.Pixels.end());
		}
		return Bytes;
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

	auto CreateTexture2DBuildFunction() -> std::shared_ptr<IBuildFunction>
	{
		return std::make_shared<FTexture2DBuildFunction>();
	}

	auto CreateTextureCubeBuildFunction() -> std::shared_ptr<IBuildFunction>
	{
		return std::make_shared<FTextureCubeBuildFunction>();
	}
}
