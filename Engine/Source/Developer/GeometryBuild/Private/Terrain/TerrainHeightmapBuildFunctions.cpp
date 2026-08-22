#include "Terrain/TerrainHeightmapBuildFunctions.h"

#include "Serialization/BinaryFormat.h"
#include "Serialization/Archive.h"
#include "Terrain/TerrainHeightmapDerivedData.h"

namespace Durin::Asset::Build::Private
{
	const FBuildFunctionIdentity TerrainHeightmapFunctionIdentity{
		"Durin.GeometryBuild.TerrainHeightmap", 1};

	namespace
	{
		auto DecodeTerrainHeightmapLocalInput(std::span<const std::byte> Bytes,
			uint32& OutWidth, uint32& OutHeight,
			std::string& OutDecoderId, uint32& OutDecoderVersion,
			ETerrainHeightmapSourceFormat& OutFormat, uint32& OutProfile,
			std::vector<uint16>& OutSamples, std::string& OutError) -> bool
		{
			FBinaryReader Reader(Bytes);
			uint32 Format = 0, DecoderLength = 0;
			if (!Reader.ReadU32(OutWidth)
				|| !Reader.ReadU32(OutHeight)
				|| !Reader.ReadU32(OutDecoderVersion)
				|| !Reader.ReadU32(Format)
				|| !Reader.ReadU32(OutProfile)
				|| !Reader.ReadU32(DecoderLength)
				|| OutDecoderVersion == 0 || Format == 0 || OutProfile == 0
				|| DecoderLength == 0 || DecoderLength > Reader.GetRemainingBytes())
			{
				OutError = "Terrain heightmap local build input is malformed.";
				return false;
			}
			std::vector<std::byte> DecoderBytes;
			if (!Reader.ReadBytes(DecoderBytes, DecoderLength, Bytes.size()))
			{
				OutError = "Terrain heightmap local build input is malformed.";
				return false;
			}
			OutDecoderId.assign(
				reinterpret_cast<const char*>(DecoderBytes.data()), DecoderBytes.size());
			OutFormat = static_cast<ETerrainHeightmapSourceFormat>(Format);
			const uint64 SampleCount = static_cast<uint64>(OutWidth) * OutHeight;
			if (SampleCount > Reader.GetRemainingBytes() / 2
				|| SampleCount * 2 != Reader.GetRemainingBytes())
			{
				OutError = "Terrain heightmap local sample count is inconsistent.";
				return false;
			}
			OutSamples.resize(static_cast<size_t>(SampleCount));
			for (uint16& Sample : OutSamples)
			{
				uint8 Low = 0, High = 0;
				if (!Reader.ReadU8(Low) || !Reader.ReadU8(High)) return false;
				Sample = uint16(Low) | uint16(High) << 8;
			}
			return Reader.IsAtEnd();
		}

		auto EncodeTerrainHeightmapPayload(const FTerrainHeightmapPayload& Payload,
			FBuildValue& OutValue, std::string& OutError) -> bool
		{
			std::vector<std::byte> Bytes;
			FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
			const_cast<FTerrainHeightmapPayload&>(Payload).Serialize(
				Ar, Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game);
			if (Ar.HasError())
			{
				OutError = Ar.GetFailure()->Message;
				return false;
			}
			OutValue = FBuildValue::FromOwned(
				std::string(TerrainHeightmapValueName), std::move(Bytes));
			return true;
		}

		class FTerrainHeightmapBuildFunction final : public IBuildFunction
		{
		public:
			auto GetConfig() const -> FBuildFunctionConfig override
			{
				return {.CacheRoot = "TerrainHeightmap/Objects",
					.ExpectedValueName = std::string(TerrainHeightmapValueName),
					.MaximumValueBytes = MaximumTerrainHeightmapPayloadBytes};
			}

			auto Validate(const FBuildDefinition& Definition, const FBuildValue& Value,
				std::string& OutError) const -> bool override
			{
				if (Definition.GetTargetFact("Platform")
						!= std::optional<std::string_view>("Win64")
					|| Definition.GetTargetFact("Profile")
						!= std::optional<std::string_view>("Game"))
				{
					OutError = "Terrain heightmap target facts are missing or incompatible.";
					return false;
				}
				std::shared_ptr<const FTerrainHeightmapPayload> Payload;
				if (!DecodeTerrainHeightmapPayload(Value, Payload, OutError)) return false;
				const auto WidthFact = Definition.GetTargetFact("Width");
				const auto HeightFact = Definition.GetTargetFact("Height");
				uint32 Width = 0, Height = 0;
				if ((WidthFact && (!ParseBuildTargetFactUInt32(*WidthFact, Width)
						|| Width != Payload->Width))
					|| (HeightFact && (!ParseBuildTargetFactUInt32(*HeightFact, Height)
						|| Height != Payload->Height)))
				{
					OutError = "Terrain heightmap payload dimensions do not match the definition.";
					return false;
				}
				return true;
			}

			auto Build(const FBuildContext& Context, FBuildValue& OutValue,
				std::string& OutError) const -> bool override
			{
				const FBuildValue* Input = Context.GetInput(TerrainHeightmapInputName);
				uint32 Width = 0, Height = 0;
				std::string DecoderId;
				uint32 DecoderVersion = 0, Profile = 0;
				ETerrainHeightmapSourceFormat Format =
					ETerrainHeightmapSourceFormat::Unknown;
				std::vector<uint16> Samples;
				if (!Input || !DecodeTerrainHeightmapLocalInput(Input->GetBytes(),
					Width, Height, DecoderId, DecoderVersion, Format, Profile,
					Samples, OutError)) return false;
				const FBuildDefinition& Definition = Context.GetDefinition();
				if (Definition.GetTargetFact("DecoderId")
						!= std::optional<std::string_view>(DecoderId)
					|| Definition.GetTargetFact("DecoderVersion")
						!= std::optional<std::string_view>(std::to_string(DecoderVersion))
					|| Definition.GetTargetFact("SourceFormat")
						!= std::optional<std::string_view>(
							std::to_string(static_cast<uint32>(Format)))
					|| Definition.GetTargetFact("SourceProfile")
						!= std::optional<std::string_view>(std::to_string(Profile)))
				{
					OutError = "Terrain heightmap local input does not match the definition.";
					return false;
				}
				if (Context.IsCanceled())
				{
					OutError = "Terrain heightmap build was canceled.";
					return false;
				}
				std::shared_ptr<const FTerrainHeightmapPayload> Payload;
				if (!BuildTerrainHeightmapPayload(
					Width, Height, Samples, Payload, OutError)) return false;
				if (Context.IsCanceled())
				{
					OutError = "Terrain heightmap build was canceled.";
					return false;
				}
				return EncodeTerrainHeightmapPayload(*Payload, OutValue, OutError);
			}
		};
	}

	auto EncodeTerrainHeightmapLocalInput(const FTerrainHeightmapBuildRequest& Request)
		-> std::vector<std::byte>
	{
		FBinaryWriter Writer;
		Writer.WriteU32(Request.Width);
		Writer.WriteU32(Request.Height);
		Writer.WriteU32(Request.DecoderVersion);
		Writer.WriteU32(static_cast<uint32>(Request.SourceFormat));
		Writer.WriteU32(Request.SourceProfileVersion);
		Writer.WriteU32(static_cast<uint32>(Request.DecoderId.size()));
		Writer.WriteBytes(std::as_bytes(std::span(Request.DecoderId)));
		for (uint16 Sample : Request.Samples)
		{
			Writer.WriteU8(static_cast<uint8>(Sample));
			Writer.WriteU8(static_cast<uint8>(Sample >> 8));
		}
		return Writer.TakeBytes();
	}

	auto DecodeTerrainHeightmapPayload(const FBuildValue& Value,
		std::shared_ptr<const FTerrainHeightmapPayload>& OutPayload,
		std::string& OutError) -> bool
	{
		if (Value.GetName() != TerrainHeightmapValueName)
		{
			OutError = "Terrain heightmap value name is incompatible.";
			return false;
		}
		auto Candidate = std::make_shared<FTerrainHeightmapPayload>();
		FCanonicalMemoryReader Ar(Value.GetBytes(), EArchivePurpose::DerivedDataPayload);
		Candidate->Serialize(Ar, Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game);
		if (Ar.HasError() || !RequireArchiveEnd(Ar) || !Candidate->IsValid())
		{
			OutError = Ar.GetFailure() ? Ar.GetFailure()->Message
				: "Terrain heightmap payload is invalid or has trailing bytes.";
			return false;
		}
		OutPayload = std::move(Candidate);
		return true;
	}

	auto CreateTerrainHeightmapBuildFunction() -> std::shared_ptr<IBuildFunction>
	{
		return std::make_shared<FTerrainHeightmapBuildFunction>();
	}
}
