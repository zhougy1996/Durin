#include "Terrain/TerrainHeightmapBuildOperations.h"

#include "DerivedDataObjectStore.h"
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

namespace Durin::AssetBuild
{
	auto BuildTerrainHeightmap(
		FTerrainHeightmapBuildRequest Request,
		FTerrainHeightmapBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		std::shared_ptr<const FTerrainHeightmapPayload> Payload;
		if (!BuildTerrainHeightmapPayload(
			Request.Width, Request.Height, Request.Samples, Payload, OutError)) return false;
		std::string Key = BuildTerrainHeightmapDerivedDataKey({
			.SourceContentHash = {
				.HashLow = Request.SourceContentHashLow,
				.HashHigh = Request.SourceContentHashHigh},
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game}, OutError);
		if (Key.empty()) return false;
		if (Request.bPersistDerivedData)
		{
			std::vector<uint8> Bytes;
			FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload);
			const_cast<FTerrainHeightmapPayload&>(*Payload).Serialize(
				Ar, Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game);
			if (Ar.HasError())
			{
				OutError = Ar.GetFailure()->Message;
				return false;
			}
			if (!Asset::FDerivedDataObjectStore(
				"TerrainHeightmap/Objects", MaximumTerrainHeightmapPayloadBytes)
				.Write(Key, Bytes, &OutError)) return false;
		}
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
			|| Product.DerivedDataKey.empty() || Context.SourcePath.IsEmpty())
		{
			OutError = "Terrain heightmap publication requires a complete product and provenance.";
			return false;
		}
		Heightmap.PublishAuthoringCandidate({
			.SourcePath = Context.SourcePath,
			.SourceContentHashLow = Product.SourceContentHashLow,
			.SourceContentHashHigh = Product.SourceContentHashHigh,
			.DecoderId = Context.DecoderId,
			.DecoderVersion = Context.DecoderVersion},
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
			.TargetPlatform = Asset::ECookTargetPlatform::Win64,
			.TargetProfile = Asset::ECookTargetProfile::Game}, OutError);
	}

	auto LoadTerrainHeightmapDerivedData(
		std::string_view Key,
		std::shared_ptr<const FTerrainHeightmapPayload>& OutPayload,
		std::string& OutError) -> bool
	{
		std::vector<uint8> Bytes;
		const Asset::FDerivedDataObjectReadResult Read = Asset::FDerivedDataObjectStore(
			"TerrainHeightmap/Objects", MaximumTerrainHeightmapPayloadBytes).Read(Key, Bytes);
		if (!Read)
		{
			OutError = Read.Message;
			return false;
		}
		auto Candidate = std::make_shared<FTerrainHeightmapPayload>();
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload);
		Candidate->Serialize(Ar, Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game);
		if (Ar.HasError())
		{
			OutError = Ar.GetFailure()->Message;
			return false;
		}
		OutPayload = std::move(Candidate);
		OutError.clear();
		return true;
	}
}
