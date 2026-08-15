#include "Terrain/TerrainHeightmapBuildFunctions.h"

#include "GeometryBuildCodec.h"
#include "Serialization/Archive.h"
#include "Terrain/TerrainHeightmapDerivedData.h"

namespace Durin::Asset::Build::Private
{
	const FBuildFunctionIdentity TerrainHeightmapFunctionIdentity{
		"Durin.GeometryBuild.TerrainHeightmap", 1};

	namespace
	{
		auto DecodeTerrainHeightmapLocalInput(std::span<const uint8> Bytes,
			uint32& OutWidth, uint32& OutHeight,
			std::string& OutDecoderId, uint32& OutDecoderVersion,
			ETerrainHeightmapSourceFormat& OutFormat, uint32& OutProfile,
			std::vector<uint16>& OutSamples, std::string& OutError) -> bool
		{
			size_t Offset = 0;
			uint32 Format = 0, DecoderLength = 0;
			if (!ReadLittleEndianU32(Bytes, Offset, OutWidth)
				|| !ReadLittleEndianU32(Bytes, Offset, OutHeight)
				|| !ReadLittleEndianU32(Bytes, Offset, OutDecoderVersion)
				|| !ReadLittleEndianU32(Bytes, Offset, Format)
				|| !ReadLittleEndianU32(Bytes, Offset, OutProfile)
				|| !ReadLittleEndianU32(Bytes, Offset, DecoderLength)
				|| OutDecoderVersion == 0 || Format == 0 || OutProfile == 0
				|| DecoderLength == 0 || Offset > Bytes.size()
				|| DecoderLength > Bytes.size() - Offset)
			{
				OutError = "Terrain heightmap local build input is malformed.";
				return false;
			}
			OutDecoderId.assign(
				reinterpret_cast<const char*>(Bytes.data() + Offset), DecoderLength);
			OutFormat = static_cast<ETerrainHeightmapSourceFormat>(Format);
			Offset += DecoderLength;
			const uint64 SampleCount = static_cast<uint64>(OutWidth) * OutHeight;
			if (Offset > Bytes.size() || SampleCount > (Bytes.size() - Offset) / 2
				|| SampleCount * 2 != Bytes.size() - Offset)
			{
				OutError = "Terrain heightmap local sample count is inconsistent.";
				return false;
			}
			OutSamples.resize(static_cast<size_t>(SampleCount));
			for (uint16& Sample : OutSamples)
			{
				Sample = uint16(Bytes[Offset]) | uint16(Bytes[Offset + 1]) << 8;
				Offset += 2;
			}
			return true;
		}

		auto EncodeTerrainHeightmapPayload(const FTerrainHeightmapPayload& Payload,
			FBuildValue& OutValue, std::string& OutError) -> bool
		{
			std::vector<uint8> Bytes;
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
		-> std::vector<uint8>
	{
		std::vector<uint8> Bytes;
		AppendLittleEndianU32(Bytes, Request.Width);
		AppendLittleEndianU32(Bytes, Request.Height);
		AppendLittleEndianU32(Bytes, Request.DecoderVersion);
		AppendLittleEndianU32(Bytes, static_cast<uint32>(Request.SourceFormat));
		AppendLittleEndianU32(Bytes, Request.SourceProfileVersion);
		AppendLittleEndianU32(Bytes, static_cast<uint32>(Request.DecoderId.size()));
		Bytes.insert(Bytes.end(), Request.DecoderId.begin(), Request.DecoderId.end());
		for (uint16 Sample : Request.Samples)
		{
			Bytes.push_back(static_cast<uint8>(Sample));
			Bytes.push_back(static_cast<uint8>(Sample >> 8));
		}
		return Bytes;
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
