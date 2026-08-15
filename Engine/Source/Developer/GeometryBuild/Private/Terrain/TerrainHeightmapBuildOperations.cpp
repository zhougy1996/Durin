#include "Terrain/TerrainHeightmapBuildOperations.h"

#include "AssetBuild/BuildSession.h"
#include "Serialization/Archive.h"
#include "Terrain/TerrainHeightmapBuildKey.h"
#include "Terrain/TerrainHeightmapDerivedData.h"

namespace Durin
{
	struct FTerrainHeightmapBuildOperations
	{
		static auto SetSourceFingerprint(
			DTerrainHeightmap& Heightmap,
			const std::filesystem::path& PhysicalPath) -> void;
	};
}

namespace Durin::Asset::Build
{
	namespace
	{
		const FBuildFunctionIdentity TerrainFunctionIdentity{
			"Durin.GeometryBuild.TerrainHeightmap", 1};
		constexpr std::string_view TerrainInputName = "TerrainHeightmapBuildInput";
		constexpr std::string_view TerrainValueName = "TerrainHeightmapPayload";

		auto AppendU32(std::vector<uint8>& Bytes, uint32 Value) -> void
		{
			for (uint32 Byte = 0; Byte < 4; ++Byte) Bytes.push_back(static_cast<uint8>(Value >> (Byte * 8)));
		}
		auto ReadU32(std::span<const uint8> Bytes, size_t& Offset, uint32& Value) -> bool
		{
			if (Offset + 4 > Bytes.size()) return false;
			Value = 0;
			for (uint32 Byte = 0; Byte < 4; ++Byte) Value |= uint32(Bytes[Offset++]) << (Byte * 8);
			return true;
		}

		auto EncodeInput(const FTerrainHeightmapBuildRequest& Request) -> std::vector<uint8>
		{
			std::vector<uint8> Bytes;
			AppendU32(Bytes, Request.Width); AppendU32(Bytes, Request.Height);
			AppendU32(Bytes, Request.DecoderVersion);
			AppendU32(Bytes, static_cast<uint32>(Request.SourceFormat));
			AppendU32(Bytes, Request.SourceProfileVersion);
			AppendU32(Bytes, static_cast<uint32>(Request.DecoderId.size()));
			Bytes.insert(Bytes.end(), Request.DecoderId.begin(), Request.DecoderId.end());
			for (uint16 Sample : Request.Samples)
			{
				Bytes.push_back(static_cast<uint8>(Sample));
				Bytes.push_back(static_cast<uint8>(Sample >> 8));
			}
			return Bytes;
		}

		auto DecodeInput(std::span<const uint8> Bytes, uint32& OutWidth, uint32& OutHeight,
			std::string& OutDecoderId, uint32& OutDecoderVersion,
			ETerrainHeightmapSourceFormat& OutFormat, uint32& OutProfile,
			std::vector<uint16>& OutSamples, std::string& OutError) -> bool
		{
			size_t Offset = 0;
			uint32 Format = 0, DecoderLength = 0;
			if (!ReadU32(Bytes, Offset, OutWidth) || !ReadU32(Bytes, Offset, OutHeight)
				|| !ReadU32(Bytes, Offset, OutDecoderVersion) || !ReadU32(Bytes, Offset, Format)
				|| !ReadU32(Bytes, Offset, OutProfile) || !ReadU32(Bytes, Offset, DecoderLength)
				|| OutDecoderVersion == 0 || Format == 0 || OutProfile == 0
				|| DecoderLength == 0 || DecoderLength > Bytes.size() - Offset)
			{
				OutError = "Terrain heightmap local build input is malformed.";
				return false;
			}
			OutDecoderId.assign(reinterpret_cast<const char*>(Bytes.data() + Offset), DecoderLength);
			OutFormat = static_cast<ETerrainHeightmapSourceFormat>(Format);
			Offset += DecoderLength;
			const uint64 SampleCount = static_cast<uint64>(OutWidth) * OutHeight;
			if (SampleCount > (Bytes.size() - Offset) / 2 || Offset + SampleCount * 2 != Bytes.size())
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

		auto EncodePayload(const FTerrainHeightmapPayload& Payload,
			FBuildValue& OutValue, std::string& OutError) -> bool
		{
			std::vector<uint8> Bytes;
			FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
			const_cast<FTerrainHeightmapPayload&>(Payload).Serialize(
				Ar, Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game);
			if (Ar.HasError()) { OutError = Ar.GetFailure()->Message; return false; }
			OutValue = FBuildValue::FromOwned(std::string(TerrainValueName), std::move(Bytes));
			return true;
		}

		auto DecodePayload(const FBuildValue& Value,
			std::shared_ptr<const FTerrainHeightmapPayload>& OutPayload,
			std::string& OutError) -> bool
		{
			if (Value.GetName() != TerrainValueName)
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

		auto ParseU32(std::string_view Text, uint32& OutValue) -> bool
		{
			if (Text.empty()) return false;
			uint64 Value = 0;
			for (const char Character : Text)
			{
				if (Character < '0' || Character > '9') return false;
				Value = Value * 10 + static_cast<uint32>(Character - '0');
				if (Value > std::numeric_limits<uint32>::max()) return false;
			}
			OutValue = static_cast<uint32>(Value);
			return true;
		}

		class FTerrainBuildFunction final : public IBuildFunction
		{
		public:
			auto GetConfig() const -> FBuildFunctionConfig override
			{
				return {.CacheRoot = "TerrainHeightmap/Objects",
					.ExpectedValueName = std::string(TerrainValueName),
					.MaximumValueBytes = MaximumTerrainHeightmapPayloadBytes};
			}
			auto Validate(const FBuildDefinition& Definition, const FBuildValue& Value,
				std::string& OutError) const -> bool override
			{
				if (Definition.GetTargetFact("Platform") != std::optional<std::string_view>("Win64")
					|| Definition.GetTargetFact("Profile") != std::optional<std::string_view>("Game"))
				{
					OutError = "Terrain heightmap target facts are missing or incompatible.";
					return false;
				}
				std::shared_ptr<const FTerrainHeightmapPayload> Payload;
				if (!DecodePayload(Value, Payload, OutError)) return false;
				const auto WidthFact = Definition.GetTargetFact("Width");
				const auto HeightFact = Definition.GetTargetFact("Height");
				uint32 Width = 0, Height = 0;
				if ((WidthFact && (!ParseU32(*WidthFact, Width) || Width != Payload->Width))
					|| (HeightFact && (!ParseU32(*HeightFact, Height) || Height != Payload->Height)))
				{
					OutError = "Terrain heightmap payload dimensions do not match the definition.";
					return false;
				}
				return true;
			}
			auto Build(const FBuildContext& Context, FBuildValue& OutValue,
				std::string& OutError) const -> bool override
			{
				const FBuildValue* Input = Context.GetInput(TerrainInputName);
				uint32 Width = 0, Height = 0;
				std::string DecoderId;
				uint32 DecoderVersion = 0, Profile = 0;
				ETerrainHeightmapSourceFormat Format = ETerrainHeightmapSourceFormat::Unknown;
				std::vector<uint16> Samples;
				if (!Input || !DecodeInput(Input->GetBytes(), Width, Height, DecoderId,
					DecoderVersion, Format, Profile, Samples, OutError)) return false;
				const FBuildDefinition& Definition = Context.GetDefinition();
				if (Definition.GetTargetFact("DecoderId") != std::optional<std::string_view>(DecoderId)
					|| Definition.GetTargetFact("DecoderVersion")
						!= std::optional<std::string_view>(std::to_string(DecoderVersion))
					|| Definition.GetTargetFact("SourceFormat")
						!= std::optional<std::string_view>(std::to_string(static_cast<uint32>(Format)))
					|| Definition.GetTargetFact("SourceProfile")
						!= std::optional<std::string_view>(std::to_string(Profile)))
				{
					OutError = "Terrain heightmap local input does not match the definition.";
					return false;
				}
				if (Context.IsCanceled()) { OutError = "Terrain heightmap build was canceled."; return false; }
				std::shared_ptr<const FTerrainHeightmapPayload> Payload;
				if (!BuildTerrainHeightmapPayload(Width, Height, Samples, Payload, OutError)) return false;
				if (Context.IsCanceled()) { OutError = "Terrain heightmap build was canceled."; return false; }
				return EncodePayload(*Payload, OutValue, OutError);
			}
		};

		std::mutex GTerrainFunctionMutex;
		FBuildFunctionRegistration GTerrainFunctionRegistration;
		auto EnsureTerrainBuildFunction(std::string* OutError,
			FModuleOwnedCallbackGate Gate = {}) -> bool
		{
			std::lock_guard Lock(GTerrainFunctionMutex);
			if (GTerrainFunctionRegistration.IsValid()) return true;
			GTerrainFunctionRegistration = RegisterBuildFunction(TerrainFunctionIdentity,
				std::make_shared<FTerrainBuildFunction>(), std::move(Gate), OutError);
			return GTerrainFunctionRegistration.IsValid();
		}
	}

	auto BuildTerrainHeightmap(
		FTerrainHeightmapBuildRequest Request,
		FTerrainHeightmapBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		if (!EnsureTerrainBuildFunction(&OutError)) return false;
		if (Request.DecoderId.empty() || Request.DecoderVersion == 0
			|| Request.SourceFormat == ETerrainHeightmapSourceFormat::Unknown
			|| Request.SourceProfileVersion == 0)
		{
			OutError = "Terrain heightmap build requires an explicit source decoder profile.";
			return false;
		}
		const FTerrainHeightmapBuildKeyInput KeyInput{
			.SourceContentHash = {
				.HashLow = Request.SourceContentHashLow,
				.HashHigh = Request.SourceContentHashHigh},
			.DecoderId = Request.DecoderId,
			.DecoderVersion = Request.DecoderVersion,
			.SourceFormat = Request.SourceFormat,
			.SourceProfileVersion = Request.SourceProfileVersion,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game};
		const std::vector<uint8> KeyBytes = BuildTerrainHeightmapDerivedDataKeyBytes(KeyInput, OutError);
		std::string Key = KeyBytes.empty() ? std::string{} : FXxHash128::HashBuffer(KeyBytes).ToString();
		if (Key.empty()) return false;
		FBuildDefinition Definition;
		FBuildDefinitionBuilder Builder(TerrainFunctionIdentity, std::string(TerrainValueName));
		Builder.SetKey(FBuildKey::FromString(Key), KeyBytes)
			.AddTargetFact("Platform", "Win64").AddTargetFact("Profile", "Game")
			.AddTargetFact("Width", std::to_string(Request.Width))
			.AddTargetFact("Height", std::to_string(Request.Height))
			.AddTargetFact("DecoderId", Request.DecoderId)
			.AddTargetFact("DecoderVersion", std::to_string(Request.DecoderVersion))
			.AddTargetFact("SourceFormat", std::to_string(static_cast<uint32>(Request.SourceFormat)))
			.AddTargetFact("SourceProfile", std::to_string(Request.SourceProfileVersion))
			.AddInput(FBuildValue::FromOwned(std::string(TerrainInputName), EncodeInput(Request)));
		if (!Builder.Build(Definition, &OutError)) return false;
		const FBuildCancellationToken Cancellation(Request.ShouldCancel);
		const FBuildOutput Output = FBuildSession().Build(Definition, {
			.bQueryCache = Request.bPersistDerivedData && Request.bQueryDerivedData,
			.bAllowLocalBuild = true,
			.bStoreBuildResult = Request.bPersistDerivedData,
			.bRequireStoreSuccess = Request.bPersistDerivedData,
			.bReturnData = true}, Request.ShouldCancel ? &Cancellation : nullptr);
		if (!Output.Succeeded()) { OutError = Output.Diagnostic; return false; }
		std::shared_ptr<const FTerrainHeightmapPayload> Payload;
		if (!DecodePayload(Output.Value, Payload, OutError)) return false;
		OutProduct = {
			.Payload = std::move(Payload),
			.DerivedDataKey = std::move(Key),
			.SourceContentHashLow = Request.SourceContentHashLow,
			.SourceContentHashHigh = Request.SourceContentHashHigh};
		OutError.clear();
		return true;
	}

	auto PublishTerrainHeightmapProduct(
		DTerrainHeightmap& Heightmap,
		FTerrainHeightmapBuildProduct Product,
		const FTerrainHeightmapPublicationContext& Context,
		std::string& OutError) -> bool
	{
		if (!Product.Payload || !Product.Payload->IsValid()
			|| Product.DerivedDataKey.empty() || Context.SourcePath.IsEmpty()
			|| Context.DecoderId.empty() || Context.DecoderVersion == 0
			|| Context.SourceFormat == ETerrainHeightmapSourceFormat::Unknown
			|| Context.SourceProfileVersion == 0)
		{
			OutError = "Terrain heightmap publication requires a complete product and provenance.";
			return false;
		}
		Heightmap.PublishAuthoringCandidate({
			.SourcePath = Context.SourcePath,
			.SourceContentHashLow = Product.SourceContentHashLow,
			.SourceContentHashHigh = Product.SourceContentHashHigh,
			.DecoderId = Context.DecoderId,
			.DecoderVersion = Context.DecoderVersion,
			.SourceFormat = Context.SourceFormat,
			.SourceProfileVersion = Context.SourceProfileVersion},
			Context.SourceFileSize, Context.SourceLastWriteTime,
			std::move(Product.Payload), std::move(Product.DerivedDataKey),
			"Built canonical terrain heightmap payload from normalized height samples.",
			Context.bAdvanceRevision, Context.bMarkPackageDirty);
		OutError.clear();
		return true;
	}

	auto MakeTerrainHeightmapDerivedDataKey(
		const DTerrainHeightmap& Heightmap,
		std::string& OutError) -> std::string
	{
		const FTerrainHeightmapSourceImportData& Source = Heightmap.GetSourceImportData();
		if (!Source.HasContentHash())
		{
			OutError = "Terrain heightmap source content identity is missing.";
			return {};
		}
		return BuildTerrainHeightmapDerivedDataKey({
			.SourceContentHash = {
				.HashLow = Source.SourceContentHashLow,
				.HashHigh = Source.SourceContentHashHigh},
			.DecoderId = Source.DecoderId,
			.DecoderVersion = Source.DecoderVersion,
			.SourceFormat = Source.SourceFormat,
			.SourceProfileVersion = Source.SourceProfileVersion,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game}, OutError);
	}

	auto LoadTerrainHeightmapDerivedData(
		std::string_view Key,
		std::shared_ptr<const FTerrainHeightmapPayload>& OutPayload,
		std::string& OutError,
		FTerrainHeightmapDerivedDataLoadDiagnostics* Diagnostics) -> bool
	{
		FTerrainHeightmapDerivedDataLoadDiagnostics Result;
		if (!EnsureTerrainBuildFunction(&OutError))
		{
			if (Diagnostics) *Diagnostics = Result;
			return false;
		}
		FBuildDefinition Definition;
		FBuildDefinitionBuilder Builder(TerrainFunctionIdentity, std::string(TerrainValueName));
		Builder.SetKey(FBuildKey::FromString(Key)).AddTargetFact("Platform", "Win64")
			.AddTargetFact("Profile", "Game");
		if (!Builder.Build(Definition, &OutError))
		{
			if (Diagnostics) *Diagnostics = Result;
			return false;
		}
		const FBuildOutput Output = FBuildSession().Build(Definition, {
			.bQueryCache = true, .bAllowLocalBuild = false,
			.bStoreBuildResult = false, .bReturnData = true});
		Result.QueryNanoseconds = Output.PhaseDurations.CacheQueryNanoseconds;
		Result.DecodeNanoseconds = Output.PhaseDurations.CachedValueValidationNanoseconds;
		if (!Output.Succeeded())
		{
			OutError = Output.Diagnostic;
			if (Diagnostics) *Diagnostics = Result;
			return false;
		}
		std::shared_ptr<const FTerrainHeightmapPayload> Candidate;
		if (!DecodePayload(Output.Value, Candidate, OutError))
		{
			if (Diagnostics) *Diagnostics = Result;
			return false;
		}
		Result.bHit = true;
		OutPayload = std::move(Candidate);
		OutError.clear();
		if (Diagnostics) *Diagnostics = Result;
		return true;
	}

	auto InitializeTerrainBuildFunction(FModuleOwnedCallbackGate Gate,
		std::string* OutError) -> bool
	{
		return EnsureTerrainBuildFunction(OutError, std::move(Gate));
	}

	auto ShutdownTerrainBuildFunction() -> void
	{
		std::lock_guard Lock(GTerrainFunctionMutex);
		GTerrainFunctionRegistration.Reset();
	}
}
