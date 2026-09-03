#include "Terrain/TerrainWorldBuild.h"
#include "Terrain/TerrainWorldBuildKey.h"

#include "Asset/AssetDerivedDataCache.h"

namespace Durin
{
	namespace
	{
		constexpr std::array<std::string_view, 5> ProductBuckets{
			"TerrainWorld/TerrainWorldMetadata/Objects",
			"TerrainWorld/TerrainWorldHeight/Objects",
			"TerrainWorld/TerrainWorldCoverage/Objects",
			"TerrainWorld/TerrainWorldCollision/Objects",
			"TerrainWorld/TerrainWorldQuery/Objects"};

		auto Fail(ETerrainWorldOutcome Outcome, std::string Message,
			ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
		{
			OutOutcome = Outcome;
			OutError = std::move(Message);
			return false;
		}
	}

	auto BuildTerrainWorldDerivedData(FTerrainWorldDerivedDataRequest Request,
		FTerrainWorldDerivedDataResult& OutResult,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		OutResult = {};
#if !DURIN_WITH_EDITOR
		return Fail(ETerrainWorldOutcome::Unavailable,
			"Terrain World authored build orchestration is unavailable outside editor builds.",
			OutOutcome, OutError);
#else
		if (!Request.GenerationId.IsValid())
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain World generation identity is invalid.", OutOutcome, OutError);
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<
			ITerrainWorldBuildProvider>([&](ITerrainWorldBuildProvider& Provider) {
			const FTerrainWorldBuildProviderDescriptor Descriptor =
				Provider.GetTerrainWorldDescriptor();
			if (!Descriptor.IsValid())
				return Fail(ETerrainWorldOutcome::InvalidDefinition,
					"The Terrain World provider descriptor is invalid.", OutOutcome, OutError);
			Request.Input.BuilderVersion = Descriptor.BuilderVersion;
			Request.Input.ProductSchemaVersion = Descriptor.ProductSchemaVersion;
			if (!ValidateTerrainNormalizedTileInput(Request.Input, OutOutcome, OutError)) return false;
			if (Request.Input.ShouldCancel && Request.Input.ShouldCancel())
				return Fail(ETerrainWorldOutcome::Cancelled,
					"Terrain tile generation was cancelled.", OutOutcome, OutError);

			// Schema-1 cache bodies have no checksum envelope. Preserve the legacy
			// exact-input validation until a separately versioned format replaces it.
			FTerrainWorldRecipeProduct Recipe;
			if (!Provider.Build({Request.Input}, Recipe, OutOutcome, OutError)) return false;
			std::array<FXxHash128, 5> Hashes;
			for (size_t Index = 0; Index < Recipe.Bodies.size(); ++Index)
			{
				const auto Class = static_cast<ETerrainTileProductClass>(Index + 1);
				if (!ValidateTerrainWorldProductBody(Class, Recipe.Bodies[Index], OutError))
					return Fail(ETerrainWorldOutcome::Corrupt, OutError, OutOutcome, OutError);
				Hashes[Index] = FXxHash128::HashBuffer(Recipe.Bodies[Index]);
			}
			FTerrainWorldDerivedDataResult Candidate;
			Candidate.Descriptor = Descriptor;
			Candidate.Generation.Tile = Request.Input.Tile;
			Candidate.Generation.GenerationId = Request.GenerationId;
			for (size_t Index = 0; Index < Recipe.Bodies.size(); ++Index)
			{
				if (Request.Input.ShouldCancel && Request.Input.ShouldCancel())
					return Fail(ETerrainWorldOutcome::Cancelled,
						"Terrain tile generation was cancelled.", OutOutcome, OutError);
				const auto Class = static_cast<ETerrainTileProductClass>(Index + 1);
				const std::string Key = MakeTerrainTileBuildKey(Request.Input, Class, OutError);
				if (Key.empty()) return false;
				const uint64 MaximumBytes = GetTerrainWorldProductBodyMaximumBytes(Class);
				AssetDerivedDataCache::FOperationDiagnostic ReadDiagnostic;
				FByteArray CachedBytes;
				bool bHit = Request.bQueryDerivedData
					&& AssetDerivedDataCache::Load(ProductBuckets[Index], Key,
						MaximumBytes, CachedBytes, ReadDiagnostic)
						== AssetDerivedDataCache::ELoadResult::Hit;
				if (bHit)
				{
					bHit = ValidateTerrainWorldProductBody(Class, CachedBytes, ReadDiagnostic.Message)
						&& CachedBytes == Recipe.Bodies[Index];
					if (!bHit && ReadDiagnostic.Message.empty())
						ReadDiagnostic.Message = "Cached Terrain World product does not match the canonical input.";
				}
				Candidate.CacheReadNanoseconds[Index] = ReadDiagnostic.DurationNanoseconds;
				OutError.clear();
				AssetDerivedDataCache::FOperationDiagnostic WriteDiagnostic;
				if (!bHit && Request.bPersistDerivedData)
				{
					AssetDerivedDataCache::Store(ProductBuckets[Index], Key,
						Recipe.Bodies[Index], MaximumBytes, WriteDiagnostic);
					Candidate.CacheWriteNanoseconds[Index] = WriteDiagnostic.DurationNanoseconds;
				}
				Candidate.Diagnostics[Index] = AssetDerivedDataCache::CombineDiagnostics(
					ReadDiagnostic, WriteDiagnostic);
				std::vector<FXxHash128> Dependencies;
				if (Class == ETerrainTileProductClass::Metadata
					|| Class == ETerrainTileProductClass::Collision) Dependencies.push_back(Hashes[1]);
				if (Class == ETerrainTileProductClass::Query)
					Dependencies = {Hashes[1], Hashes[2]};
				FByteArray Encoded;
				if (!EncodeTerrainTileProduct(Class, Request.Input.Tile, Request.GenerationId,
					Dependencies, Recipe.Bodies[Index], Encoded, OutOutcome, OutError)) return false;
				FTerrainTileProduct& Product = Candidate.Generation.Products[Index];
				if (!DecodeTerrainTileProduct(Encoded, Class, Product, OutOutcome, OutError)) return false;
				Candidate.Keys[Index] = Key;
				Candidate.PayloadBytes[Index] = Product.Bytes.size();
				Candidate.Origins[Index] = bHit ? ETerrainTileBuildOrigin::DerivedData
					: ETerrainTileBuildOrigin::LocalBuild;
			}
			if (Request.Input.ShouldCancel && Request.Input.ShouldCancel())
				return Fail(ETerrainWorldOutcome::Cancelled,
					"Terrain tile generation was cancelled.", OutOutcome, OutError);
			OutResult = std::move(Candidate);
			OutOutcome = ETerrainWorldOutcome::Ready;
			OutError.clear();
			return true;
		});
		if (Invocation.Status == EFeatureInvokeStatus::Invoked
			&& Invocation.Value.has_value() && *Invocation.Value) return true;
		OutResult = {};
		if (Invocation.Status == EFeatureInvokeStatus::Unavailable)
			return Fail(ETerrainWorldOutcome::Unavailable,
				"The Terrain World build provider is unavailable.", OutOutcome, OutError);
		if (Invocation.Status == EFeatureInvokeStatus::Ambiguous)
			return Fail(ETerrainWorldOutcome::Unavailable,
				"Multiple Terrain World build providers are registered.", OutOutcome, OutError);
		if (OutError.empty())
			return Fail(ETerrainWorldOutcome::PublicationFailed,
				"The Terrain World build provider failed without a diagnostic.", OutOutcome, OutError);
		return false;
#endif
	}
}
