#include "Terrain/TerrainHeightmapBuildOperations.h"

#include "DerivedDataCache/DerivedDataBuildSession.h"
#include "TerrainBuildFunctionRegistry.h"
#include "Terrain/TerrainHeightmapBuildFunctions.h"
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

namespace Durin::Asset
{
	using namespace ::Durin::DerivedData;

	auto BuildTerrainHeightmap(
		FTerrainHeightmapBuildRequest Request,
		FTerrainHeightmapBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		if (!EnsureTerrainBuildFunctions(&OutError)) return false;
		FTerrainHeightmapImportedData Imported;
		if (!Imported.SetSamples(Request.Width, Request.Height, Request.Samples))
		{
			OutError = "Terrain heightmap build requires valid canonical uint16 samples.";
			return false;
		}
		const FXxHash128 CanonicalIdentity = Imported.GetIdentity();
		const FTerrainHeightmapBuildKeyInput KeyInput{
			.SourceContentHash = CanonicalIdentity,
			.DecoderId = "canonical-u16",
			.DecoderVersion = TerrainHeightmapImportedDataSchemaVersion,
			.SourceFormat = ETerrainHeightmapSourceFormat::Raw16,
			.SourceProfileVersion = TerrainHeightmapImportedDataSchemaVersion,
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game};
		const std::vector<std::byte> KeyBytes = BuildTerrainHeightmapDerivedDataKeyBytes(KeyInput, OutError);
		std::string Key = KeyBytes.empty() ? std::string{} : FXxHash128::HashBuffer(KeyBytes).ToString();
		if (Key.empty()) return false;
		FBuildDefinition Definition;
		FBuildDefinitionBuilder Builder(Private::TerrainHeightmapFunctionIdentity, std::string(Private::TerrainHeightmapValueName));
		FTerrainHeightmapBuildRequest CanonicalRequest = Request;
		CanonicalRequest.DecoderId = "canonical-u16";
		CanonicalRequest.DecoderVersion = TerrainHeightmapImportedDataSchemaVersion;
		CanonicalRequest.SourceFormat = ETerrainHeightmapSourceFormat::Raw16;
		CanonicalRequest.SourceProfileVersion = TerrainHeightmapImportedDataSchemaVersion;
		Builder.SetKey(FBuildKey::FromString(Key), KeyBytes)
			.AddTargetFact("Platform", "Win64").AddTargetFact("Profile", "Game")
			.AddTargetFact("Width", std::to_string(Request.Width))
			.AddTargetFact("Height", std::to_string(Request.Height))
			.AddTargetFact("DecoderId", CanonicalRequest.DecoderId)
			.AddTargetFact("DecoderVersion", std::to_string(CanonicalRequest.DecoderVersion))
			.AddTargetFact("SourceFormat", std::to_string(
				static_cast<uint32>(CanonicalRequest.SourceFormat)))
			.AddTargetFact("SourceProfile", std::to_string(
				CanonicalRequest.SourceProfileVersion))
			.AddTargetFact("ImportedSchema", std::to_string(TerrainHeightmapImportedDataSchemaVersion))
			.AddInput(FBuildValue::FromOwned(std::string(Private::TerrainHeightmapInputName),
				Private::EncodeTerrainHeightmapLocalInput(CanonicalRequest)));
		if (!Builder.Build(Definition, &OutError)) return false;
		const FBuildCancellationToken Cancellation(Request.ShouldCancel);
		const FBuildOutput Output = FBuildSession().Build(Definition, {
			.bQueryCache = Request.bPersistDerivedData && Request.bQueryDerivedData,
			.bAllowLocalBuild = true,
			.bStoreBuildResult = Request.bPersistDerivedData},
			Request.ShouldCancel ? &Cancellation : nullptr);
		if (!Output.Succeeded()) { OutError = Output.Diagnostic; return false; }
		std::shared_ptr<const FTerrainHeightmapPayload> Payload;
		if (!Private::DecodeTerrainHeightmapPayload(Output.Value, Payload, OutError)) return false;
		OutProduct = {
			.Payload = std::move(Payload),
			.DerivedDataKey = std::move(Key),
			.SourceContentHashLow = CanonicalIdentity.HashLow,
			.SourceContentHashHigh = CanonicalIdentity.HashHigh,
			.PersistenceDiagnostic = Output.StoreDiagnostic};
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
			|| Product.DerivedDataKey.empty() || Context.SourceFilename.empty()
			|| Context.DecoderId.empty() || Context.DecoderVersion == 0
			|| Context.SourceFormat == ETerrainHeightmapSourceFormat::Unknown
			|| Context.SourceProfileVersion == 0)
		{
			OutError = "Terrain heightmap publication requires a complete product and provenance.";
			return false;
		}
		Heightmap.PublishDerivedDataLoadResult(
			std::move(Product.Payload), std::move(Product.DerivedDataKey),
			Product.PersistenceDiagnostic.empty()
				? "Built terrain heightmap payload from canonical height samples."
				: std::format("Built terrain heightmap payload from canonical height samples; DDC persistence was best effort: {}",
					Product.PersistenceDiagnostic),
			Context.bAdvanceRevision, Context.bMarkPackageDirty);
		OutError.clear();
		return true;
	}

	auto BuildTerrainHeightmapInto(
		DTerrainHeightmap& Heightmap,
		FTerrainHeightmapBuildRequest Request,
		const FTerrainHeightmapPublicationContext& Context,
		std::string& OutError) -> bool
	{
		FTerrainHeightmapBuildProduct Product;
		return BuildTerrainHeightmap(std::move(Request), Product, OutError)
			&& PublishTerrainHeightmapProduct(
				Heightmap, std::move(Product), Context, OutError);
	}

	auto MakeTerrainHeightmapDerivedDataKey(
		const FTerrainHeightmapSourceImportData& Source,
		std::string& OutError) -> std::string
	{
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

	auto MakeTerrainHeightmapDerivedDataKey(
		const DTerrainHeightmap& Heightmap, std::string& OutError) -> std::string
	{
		const FXxHash128 Identity = Heightmap.GetImportedDataIdentity();
		if (Identity.IsZero())
		{
			OutError = "Terrain heightmap canonical imported data is missing or invalid.";
			return {};
		}
		return BuildTerrainHeightmapDerivedDataKey({
			.SourceContentHash = Identity,
			.DecoderId = "canonical-u16",
			.DecoderVersion = TerrainHeightmapImportedDataSchemaVersion,
			.SourceFormat = ETerrainHeightmapSourceFormat::Raw16,
			.SourceProfileVersion = TerrainHeightmapImportedDataSchemaVersion,
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
		if (!EnsureTerrainBuildFunctions(&OutError))
		{
			if (Diagnostics) *Diagnostics = Result;
			return false;
		}
		FBuildDefinition Definition;
		FBuildDefinitionBuilder Builder(Private::TerrainHeightmapFunctionIdentity, std::string(Private::TerrainHeightmapValueName));
		Builder.SetKey(FBuildKey::FromString(Key)).AddTargetFact("Platform", "Win64")
			.AddTargetFact("Profile", "Game");
		if (!Builder.Build(Definition, &OutError))
		{
			if (Diagnostics) *Diagnostics = Result;
			return false;
		}
		const FBuildOutput Output = FBuildSession().Build(Definition, {
			.bQueryCache = true, .bAllowLocalBuild = false,
			.bStoreBuildResult = false});
		Result.QueryNanoseconds = Output.PhaseDurations.CacheQueryNanoseconds;
		Result.DecodeNanoseconds = Output.PhaseDurations.CachedValueValidationNanoseconds;
		if (!Output.Succeeded())
		{
			OutError = Output.Diagnostic;
			if (Diagnostics) *Diagnostics = Result;
			return false;
		}
		std::shared_ptr<const FTerrainHeightmapPayload> Candidate;
		if (!Private::DecodeTerrainHeightmapPayload(Output.Value, Candidate, OutError))
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

}
