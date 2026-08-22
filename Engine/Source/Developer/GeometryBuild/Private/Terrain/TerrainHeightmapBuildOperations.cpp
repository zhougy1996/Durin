#include "Terrain/TerrainHeightmapBuildOperations.h"

#include "AssetBuild/BuildSession.h"
#include "GeometryBuildFunctionRegistry.h"
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

namespace Durin::Asset::Build
{
	auto BuildTerrainHeightmap(
		FTerrainHeightmapBuildRequest Request,
		FTerrainHeightmapBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		if (!EnsureGeometryBuildFunctions(&OutError)) return false;
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
		const std::vector<std::byte> KeyBytes = BuildTerrainHeightmapDerivedDataKeyBytes(KeyInput, OutError);
		std::string Key = KeyBytes.empty() ? std::string{} : FXxHash128::HashBuffer(KeyBytes).ToString();
		if (Key.empty()) return false;
		FBuildDefinition Definition;
		FBuildDefinitionBuilder Builder(Private::TerrainHeightmapFunctionIdentity, std::string(Private::TerrainHeightmapValueName));
		Builder.SetKey(FBuildKey::FromString(Key), KeyBytes)
			.AddTargetFact("Platform", "Win64").AddTargetFact("Profile", "Game")
			.AddTargetFact("Width", std::to_string(Request.Width))
			.AddTargetFact("Height", std::to_string(Request.Height))
			.AddTargetFact("DecoderId", Request.DecoderId)
			.AddTargetFact("DecoderVersion", std::to_string(Request.DecoderVersion))
			.AddTargetFact("SourceFormat", std::to_string(static_cast<uint32>(Request.SourceFormat)))
			.AddTargetFact("SourceProfile", std::to_string(Request.SourceProfileVersion))
			.AddInput(FBuildValue::FromOwned(std::string(Private::TerrainHeightmapInputName), Private::EncodeTerrainHeightmapLocalInput(Request)));
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
		if (!Private::DecodeTerrainHeightmapPayload(Output.Value, Payload, OutError)) return false;
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
		if (!EnsureGeometryBuildFunctions(&OutError))
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
