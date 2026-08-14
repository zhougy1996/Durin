#include "Terrain/TerrainHeightmapBuildOperations.h"

#include "AssetBuild/BuildCache.h"
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

namespace Durin::Asset::Build
{
	namespace
	{
		auto GetTerrainHeightmapStore() -> Asset::FDerivedDataObjectStore&
		{
			static Asset::FDerivedDataObjectStore Store(
				"TerrainHeightmap/Objects", MaximumTerrainHeightmapPayloadBytes);
			return Store;
		}
	}

	auto BuildTerrainHeightmap(
		FTerrainHeightmapBuildRequest Request,
		FTerrainHeightmapBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		OutProduct = {};
		if (Request.DecoderId.empty() || Request.DecoderVersion == 0
			|| Request.SourceFormat == ETerrainHeightmapSourceFormat::Unknown
			|| Request.SourceProfileVersion == 0)
		{
			OutError = "Terrain heightmap build requires an explicit source decoder profile.";
			return false;
		}
		std::shared_ptr<const FTerrainHeightmapPayload> Payload;
		if (!BuildTerrainHeightmapPayload(
			Request.Width, Request.Height, Request.Samples, Payload, OutError)) return false;
		std::string Key = BuildTerrainHeightmapDerivedDataKey({
			.SourceContentHash = {
				.HashLow = Request.SourceContentHashLow,
				.HashHigh = Request.SourceContentHashHigh},
			.DecoderId = Request.DecoderId,
			.DecoderVersion = Request.DecoderVersion,
			.SourceFormat = Request.SourceFormat,
			.SourceProfileVersion = Request.SourceProfileVersion,
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
			if (!FBuildCacheClient(GetTerrainHeightmapStore()).Store(Key,
				FBuildValue::FromOwned("TerrainHeightmapPayload", std::move(Bytes)),
				{.bRequireStoreSuccess = true}, &OutError)) return false;
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
		const auto QueryStart = std::chrono::steady_clock::now();
		std::filesystem::path ObjectPath;
		std::string PathError;
		const bool bValidPath = GetTerrainHeightmapStore().GetObjectPath(Key, ObjectPath, &PathError);
		std::error_code FileError;
		const bool bExists = bValidPath && std::filesystem::is_regular_file(ObjectPath, FileError);
		Result.QueryNanoseconds = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - QueryStart).count());
		if (!bExists)
		{
			OutError = bValidPath ? "Derived-data object is missing." : std::move(PathError);
			if (Diagnostics) *Diagnostics = Result;
			return false;
		}
		const auto ReadStart = std::chrono::steady_clock::now();
		std::vector<uint8> Bytes;
		const Asset::FDerivedDataObjectReadResult Read = GetTerrainHeightmapStore().Read(Key, Bytes);
		Result.ReadNanoseconds = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - ReadStart).count());
		if (!Read)
		{
			OutError = Read.Message;
			if (Diagnostics) *Diagnostics = Result;
			return false;
		}
		const auto DecodeStart = std::chrono::steady_clock::now();
		auto Candidate = std::make_shared<FTerrainHeightmapPayload>();
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload);
		Candidate->Serialize(Ar, Asset::ECookTargetPlatform::Win64,
			Asset::ECookTargetProfile::Game);
		if (Ar.HasError())
		{
			OutError = Ar.GetFailure()->Message;
			Result.DecodeNanoseconds = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - DecodeStart).count());
			if (Diagnostics) *Diagnostics = Result;
			return false;
		}
		Result.DecodeNanoseconds = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - DecodeStart).count());
		Result.bHit = true;
		OutPayload = std::move(Candidate);
		OutError.clear();
		if (Diagnostics) *Diagnostics = Result;
		return true;
	}
}
