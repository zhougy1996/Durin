#include "Terrain/TerrainWorldTile.h"

#include "Serialization/BinaryFormat.h"

namespace Durin
{

	namespace
	{

		auto TerrainWorldFail(ETerrainWorldOutcome Outcome, std::string Message,
			ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
		{
			OutOutcome = Outcome;
			OutError = std::move(Message);
			return false;
		}

		auto ProductIndex(ETerrainTileProductClass ProductClass) -> size_t
		{
			return static_cast<size_t>(ProductClass) - 1;
		}

		auto ProductCeiling(ETerrainTileProductClass ProductClass) -> uint64
		{
			switch (ProductClass)
			{
			case ETerrainTileProductClass::Metadata: return 16ull * 1024ull;
			case ETerrainTileProductClass::Height: return 160ull * 1024ull;
			case ETerrainTileProductClass::Coverage: return 320ull * 1024ull;
			case ETerrainTileProductClass::Collision: return 96ull * 1024ull;
			case ETerrainTileProductClass::Query: return 160ull * 1024ull;
			}
			return 0;
		}

		auto WriteGuid(FBinaryWriter& Writer, const FGuid& Guid) -> void
		{
			for (uint32 Word : {Guid.A, Guid.B, Guid.C, Guid.D})
				for (int Shift : {24, 16, 8, 0})
					Writer.WriteU8(static_cast<uint8>(Word >> Shift));
		}

		auto WriteDouble(FBinaryWriter& Writer, double Value) -> void
		{
			Writer.WriteU64(std::bit_cast<uint64>(Value));
		}

		auto GuidLess(const FGuid& A, const FGuid& B) -> bool
		{
			// RFC 4122 order compares the four big-endian words lexicographically.
			return std::tuple(A.A, A.B, A.C, A.D) < std::tuple(B.A, B.B, B.C, B.D);
		}

		auto BuildHeightBody(const FTerrainTileRecipeInput& Input) -> FByteArray
		{
			FBinaryWriter Writer;
			Writer.WriteU16(257);
			Writer.WriteU16(257);
			for (int16 Height : Input.Heights) Writer.WriteU16(static_cast<uint16>(Height));
			return Writer.TakeBytes();
		}

		auto FindLayerIndex(std::span<const FGuid> Layers, const FGuid& Layer) -> uint8
		{
			const auto It = std::ranges::find(Layers, Layer);
			return static_cast<uint8>(std::distance(Layers.begin(), It));
		}

		auto BuildCoverageBody(const FTerrainTileRecipeInput& Input) -> FByteArray
		{
			FBinaryWriter Writer;
			Writer.WriteU16(257);
			Writer.WriteU16(257);
			Writer.WriteU8(static_cast<uint8>(Input.LayerIds.size()));
			for (const FGuid& Layer : Input.LayerIds) WriteGuid(Writer, Layer);
			for (const FTerrainCoverageSample& Sample : Input.Coverage)
			{
				Writer.WriteU8(Sample.LayerCount);
				for (uint8 Index = 0; Index < Sample.LayerCount; ++Index)
				{
					Writer.WriteU8(FindLayerIndex(Input.LayerIds, Sample.Layers[Index].LayerId));
					Writer.WriteU8(Sample.Layers[Index].Weight);
				}
			}
			return Writer.TakeBytes();
		}

		auto BuildCollisionBody(const FTerrainTileRecipeInput& Input) -> FByteArray
		{
			FBinaryWriter Writer;
			Writer.WriteU16(129);
			Writer.WriteU16(129);
			for (uint32 Y = 0; Y <= 256; Y += 2)
				for (uint32 X = 0; X <= 256; X += 2)
					Writer.WriteU16(static_cast<uint16>(Input.Heights[Y * 257 + X]));
			return Writer.TakeBytes();
		}

		auto BuildQueryBody(const FTerrainTileRecipeInput& Input) -> FByteArray
		{
			FBinaryWriter Writer;
			Writer.WriteU16(129);
			Writer.WriteU16(129);
			for (uint32 Y = 0; Y <= 256; Y += 2)
				for (uint32 X = 0; X <= 256; X += 2)
				{
					const uint32 Offset = Y * 257 + X;
					auto HeightAt = [&](int32 SampleX, int32 SampleY) -> int16 {
						if (!Input.HeightHalo.empty())
							return Input.HeightHalo[(SampleY + 1) * 259 + SampleX + 1];
						const uint32 ClampedX = static_cast<uint32>(std::clamp(SampleX, 0, 256));
						const uint32 ClampedY = static_cast<uint32>(std::clamp(SampleY, 0, 256));
						return Input.Heights[ClampedY * 257 + ClampedX];
					};
					const int32 SlopeX = static_cast<int32>(HeightAt(X + 1, Y)) - HeightAt(X - 1, Y);
					const int32 SlopeY = static_cast<int32>(HeightAt(X, Y + 1)) - HeightAt(X, Y - 1);
					Writer.WriteU16(static_cast<uint16>(Input.Heights[Offset]));
					Writer.WriteU16(static_cast<uint16>(std::clamp(SlopeX, -32768, 32767)));
					Writer.WriteU16(static_cast<uint16>(std::clamp(SlopeY, -32768, 32767)));
					const FTerrainCoverageSample& Coverage = Input.Coverage[Offset];
					Writer.WriteU8(Coverage.LayerCount == 0 ? 0xff
						: FindLayerIndex(Input.LayerIds, Coverage.Layers[0].LayerId));
				}
			return Writer.TakeBytes();
		}

		auto BuildMetadataBody(const FTerrainTileRecipeInput& Input) -> FByteArray
		{
			FBinaryWriter Writer;
			const auto [Min, Max] = std::ranges::minmax(Input.Heights);
			Writer.WriteI32(Min);
			Writer.WriteI32(Max);
			int32 MaximumDelta = 0;
			auto HeightAt = [&](int32 SampleX, int32 SampleY) -> int16 {
				if (!Input.HeightHalo.empty())
					return Input.HeightHalo[(SampleY + 1) * 259 + SampleX + 1];
				const uint32 ClampedX = static_cast<uint32>(std::clamp(SampleX, 0, 256));
				const uint32 ClampedY = static_cast<uint32>(std::clamp(SampleY, 0, 256));
				return Input.Heights[ClampedY * 257 + ClampedX];
			};
			for (int32 Y = 0; Y <= 256; ++Y)
				for (int32 X = 0; X <= 256; ++X)
				{
					MaximumDelta = std::max(MaximumDelta,
						std::abs(static_cast<int32>(HeightAt(X + 1, Y)) - HeightAt(X - 1, Y)));
					MaximumDelta = std::max(MaximumDelta,
						std::abs(static_cast<int32>(HeightAt(X, Y + 1)) - HeightAt(X, Y - 1)));
				}
			WriteDouble(Writer, static_cast<double>(MaximumDelta) * 0.25);
			FTerrainSampleExtent TileExtent;
			ETerrainWorldOutcome IgnoredOutcome{};
			std::string IgnoredError;
			const bool bHasExtent = GetTerrainTileSampleExtent(
				Input.Tile, TileExtent, IgnoredOutcome, IgnoredError);
			check(bHasExtent);
			const int64 MinX = std::max(TileExtent.Min.X, Input.WorldExtent.Min.X);
			const int64 MinY = std::max(TileExtent.Min.Y, Input.WorldExtent.Min.Y);
			const int64 MaxX = std::min(TileExtent.Max.X, Input.WorldExtent.Max.X);
			const int64 MaxY = std::min(TileExtent.Max.Y, Input.WorldExtent.Max.Y);
			WriteDouble(Writer, Input.Coordinates.OriginX + MinX * Input.Coordinates.SampleSpacingMeters);
			WriteDouble(Writer, Input.Coordinates.OriginY + MinY * Input.Coordinates.SampleSpacingMeters);
			WriteDouble(Writer, Input.Coordinates.OriginZ + Input.Coordinates.HeightDatumMeters + Min * 0.25);
			WriteDouble(Writer, Input.Coordinates.OriginX + MaxX * Input.Coordinates.SampleSpacingMeters);
			WriteDouble(Writer, Input.Coordinates.OriginY + MaxY * Input.Coordinates.SampleSpacingMeters);
			WriteDouble(Writer, Input.Coordinates.OriginZ + Input.Coordinates.HeightDatumMeters + Max * 0.25);
			Writer.WriteU8(5);
			for (uint8 Product = 1; Product <= 5; ++Product)
			{
				Writer.WriteU8(Product);
				Writer.WriteU32(static_cast<uint32>(
					ProductCeiling(static_cast<ETerrainTileProductClass>(Product))));
			}
			return Writer.TakeBytes();
		}

		auto CoverageEqual(const FTerrainCoverageSample& A,
			const FTerrainCoverageSample& B) -> bool
		{
			if (A.LayerCount != B.LayerCount) return false;
			for (uint8 Index = 0; Index < A.LayerCount; ++Index)
				if (A.Layers[Index] != B.Layers[Index]) return false;
			return true;
		}

		auto AppendBorderSample(FBinaryWriter& HeightWriter,
			FBinaryWriter& CoverageWriter, const FTerrainTileRecipeInput& Input,
			uint32 X, uint32 Y) -> void
		{
			const uint32 Offset = Y * 257 + X;
			HeightWriter.WriteU16(static_cast<uint16>(Input.Heights[Offset]));
			const FTerrainCoverageSample& Coverage = Input.Coverage[Offset];
			CoverageWriter.WriteU8(Coverage.LayerCount);
			for (uint8 Index = 0; Index < Coverage.LayerCount; ++Index)
			{
				WriteGuid(CoverageWriter, Coverage.Layers[Index].LayerId);
				CoverageWriter.WriteU8(Coverage.Layers[Index].Weight);
			}
		}

	}

	auto NormalizeTerrainTileInput(const FTerrainWorldDefinition& Definition,
		int64 TileX, int64 TileY, const FTerrainComposedTileValues& ComposedValues,
		FTerrainTileRecipeInput& OutInput, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool
	{
		OutInput = {};
		if (!ValidateTerrainWorldDefinition(Definition, OutOutcome, OutError)) return false;
		if (ComposedValues.ShouldCancel && ComposedValues.ShouldCancel())
			return TerrainWorldFail(ETerrainWorldOutcome::Cancelled,
				"Terrain tile normalization was cancelled.", OutOutcome, OutError);
		FTerrainTileRecipeInput Candidate;
		Candidate.Tile = {Definition.WorldId, TileX, TileY, Definition.TileSchemeVersion};
		Candidate.WorldExtent = Definition.SampleExtent;
		Candidate.Coordinates = Definition.Coordinates;
		Candidate.CompositionPolicyId = Definition.BuildPolicyId;
		Candidate.CompositionPolicyVersion = Definition.BuildPolicyVersion;
		Candidate.TargetPlatform = Definition.TargetPlatform;
		Candidate.TargetProfile = Definition.TargetProfile;
		Candidate.Heights = ComposedValues.Heights;
		Candidate.Coverage = ComposedValues.Coverage;
		Candidate.HeightHalo = ComposedValues.HeightHalo;
		Candidate.CoverageHalo = ComposedValues.CoverageHalo;
		Candidate.Neighbors = ComposedValues.Neighbors;
		Candidate.ShouldCancel = ComposedValues.ShouldCancel;
		FTerrainSampleExtent TileExtent;
		if (!GetTerrainTileSampleExtent(Candidate.Tile, TileExtent, OutOutcome, OutError)) return false;
		for (const FTerrainCompositionSource& Source : Definition.Sources)
		{
			if (!Source.bEnabled || Source.AffectedSamples.Max.X < TileExtent.Min.X
				|| Source.AffectedSamples.Min.X > TileExtent.Max.X
				|| Source.AffectedSamples.Max.Y < TileExtent.Min.Y
				|| Source.AffectedSamples.Min.Y > TileExtent.Max.Y) continue;
			Candidate.Sources.push_back(Source);
			if (Candidate.Sources.size() > TerrainWorldMaximumTileSources)
				return TerrainWorldFail(ETerrainWorldOutcome::BudgetRejected,
					"More than 64 authored sources overlap one Terrain tile.", OutOutcome, OutError);
		}
		for (const FTerrainCoverageSample& Sample : Candidate.Coverage)
			for (uint8 Index = 0; Index < Sample.LayerCount; ++Index)
				Candidate.LayerIds.push_back(Sample.Layers[Index].LayerId);
		for (const FTerrainCoverageSample& Sample : Candidate.CoverageHalo)
			for (uint8 Index = 0; Index < Sample.LayerCount; ++Index)
				Candidate.LayerIds.push_back(Sample.Layers[Index].LayerId);
		std::ranges::sort(Candidate.LayerIds, GuidLess);
		Candidate.LayerIds.erase(std::unique(Candidate.LayerIds.begin(), Candidate.LayerIds.end()),
			Candidate.LayerIds.end());
		for (const FGuid& LayerId : Candidate.LayerIds)
			if (std::ranges::find(Definition.Layers, LayerId,
				&FTerrainLayerDefinition::LayerId) == Definition.Layers.end())
				return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
					"Composed Terrain coverage references a layer outside the authored definition.",
					OutOutcome, OutError);
		for (FTerrainCoverageSample& Sample : Candidate.Coverage)
			std::ranges::sort(std::span(Sample.Layers).first(Sample.LayerCount),
				[](const FTerrainCoverageWeight& A, const FTerrainCoverageWeight& B) {
					return GuidLess(A.LayerId, B.LayerId);
				});
		for (FTerrainCoverageSample& Sample : Candidate.CoverageHalo)
			std::ranges::sort(std::span(Sample.Layers).first(Sample.LayerCount),
				[](const FTerrainCoverageWeight& A, const FTerrainCoverageWeight& B) {
					return GuidLess(A.LayerId, B.LayerId);
				});
		if (Candidate.LayerIds.size() > TerrainWorldMaximumTileLayers)
			return TerrainWorldFail(ETerrainWorldOutcome::BudgetRejected,
				"More than 16 logical layers overlap one Terrain tile.", OutOutcome, OutError);
		if (!ValidateTerrainNormalizedTileInput(Candidate, OutOutcome, OutError)) return false;
		const uint64 TaskCeiling = Definition.ProductProfile == 1
			? 512ull * 1024ull * 1024ull : 768ull * 1024ull * 1024ull;
		if (EstimateTerrainTileBuildBytes(Candidate) > TaskCeiling)
			return TerrainWorldFail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain tile normalization exceeds its profile task ceiling.", OutOutcome, OutError);
		OutInput = std::move(Candidate);
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto ComposeTerrainTileInput(const FTerrainWorldDefinition& Definition,
		int64 TileX, int64 TileY,
		std::span<const FTerrainTileSourceContribution> Contributions,
		FTerrainTileRecipeInput& OutInput, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError, std::function<bool()> ShouldCancel) -> bool
	{
		OutInput = {};
		if (!ValidateTerrainWorldDefinition(Definition, OutOutcome, OutError)) return false;
		if (Contributions.size() > TerrainWorldMaximumTileSources)
			return TerrainWorldFail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain tile composition exceeds 64 source contributions.", OutOutcome, OutError);
		if (Definition.Layers.empty())
			return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain tile composition requires at least one logical layer.", OutOutcome, OutError);
		FTerrainTileKey Tile{Definition.WorldId, TileX, TileY, Definition.TileSchemeVersion};
		FTerrainSampleExtent TileExtent;
		if (!GetTerrainTileSampleExtent(Tile, TileExtent, OutOutcome, OutError)) return false;
		FTerrainComposedTileValues Values;
		Values.Heights.resize(TerrainWorldSampleCount, 0);
		Values.Coverage.resize(TerrainWorldSampleCount);
		Values.HeightHalo.resize(259u * 259u, 0);
		Values.CoverageHalo.resize(259u * 259u);
		Values.ShouldCancel = ShouldCancel;
		if (!Definition.Layers.empty())
		{
			for (FTerrainCoverageSample& Sample : Values.Coverage)
			{
				Sample.LayerCount = 1;
				Sample.Layers[0] = {Definition.Layers.front().LayerId, 255};
			}
			for (FTerrainCoverageSample& Sample : Values.CoverageHalo)
			{
				Sample.LayerCount = 1;
				Sample.Layers[0] = {Definition.Layers.front().LayerId, 255};
			}
		}
		auto DivideQ8 = [](int64 Value) -> int64 {
			return Value >= 0 ? (Value + 127) / 255 : (Value - 127) / 255;
		};
		size_t ContributionIndex = 0;
		for (const FTerrainCompositionSource& Source : Definition.Sources)
		{
			if (!Source.bEnabled || Source.AffectedSamples.Max.X < TileExtent.Min.X
				|| Source.AffectedSamples.Min.X > TileExtent.Max.X
				|| Source.AffectedSamples.Max.Y < TileExtent.Min.Y
				|| Source.AffectedSamples.Min.Y > TileExtent.Max.Y) continue;
			if (ContributionIndex >= Contributions.size())
				return TerrainWorldFail(ETerrainWorldOutcome::MissingDependency,
					"Terrain tile source contribution is missing.", OutOutcome, OutError);
			const FTerrainTileSourceContribution& Contribution = Contributions[ContributionIndex++];
			if (Contribution.SourceId != Source.SourceId || Contribution.ContentHash != Source.ContentHash
				|| (!Contribution.Heights.empty() && Contribution.Heights.size() != TerrainWorldSampleCount)
				|| (!Contribution.Coverage.empty() && Contribution.Coverage.size() != TerrainWorldSampleCount)
				|| (((Source.AffectedProductMask & TerrainSourceAffectsHeight) != 0)
					!= !Contribution.Heights.empty())
				|| (((Source.AffectedProductMask & TerrainSourceAffectsCoverage) != 0)
					!= !Contribution.Coverage.empty()))
				return TerrainWorldFail(ETerrainWorldOutcome::MissingDependency,
					"Terrain tile source contribution identity or sample count is invalid.", OutOutcome, OutError);
			if (!Contribution.Coverage.empty()
				&& (Source.BlendOperation != static_cast<uint8>(ETerrainCompositionBlendOperation::Replace)
					|| Source.Strength != 255))
				return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
					"Schema-1 coverage composition requires full-strength ordered Replace.", OutOutcome, OutError);
			for (uint32 Y = 0; Y <= 256; ++Y)
				for (uint32 X = 0; X <= 256; ++X)
				{
					if (ShouldCancel && ShouldCancel())
						return TerrainWorldFail(ETerrainWorldOutcome::Cancelled,
							"Terrain tile composition was cancelled.", OutOutcome, OutError);
					const int64 GlobalX = TileExtent.Min.X + X;
					const int64 GlobalY = TileExtent.Min.Y + Y;
					if (GlobalX < Source.AffectedSamples.Min.X || GlobalX > Source.AffectedSamples.Max.X
						|| GlobalY < Source.AffectedSamples.Min.Y || GlobalY > Source.AffectedSamples.Max.Y) continue;
					const uint32 Offset = Y * 257 + X;
					if (!Contribution.Heights.empty())
					{
						const int64 Current = Values.Heights[Offset];
						const int64 SourceHeight = Contribution.Heights[Offset];
						const int64 Strength = Source.Strength;
						int64 Composed = Current;
						switch (static_cast<ETerrainCompositionBlendOperation>(Source.BlendOperation))
						{
						case ETerrainCompositionBlendOperation::Replace:
							Composed = DivideQ8(Current * (255 - Strength) + SourceHeight * Strength);
							break;
						case ETerrainCompositionBlendOperation::Add:
							Composed = Current + DivideQ8(SourceHeight * Strength);
							break;
						case ETerrainCompositionBlendOperation::Minimum:
							Composed = std::min(Current, DivideQ8(SourceHeight * Strength));
							break;
						case ETerrainCompositionBlendOperation::Maximum:
							Composed = std::max(Current, DivideQ8(SourceHeight * Strength));
							break;
						}
						if (Composed < -32768 || Composed > 32767)
							return TerrainWorldFail(ETerrainWorldOutcome::Overflow,
								"Terrain height composition exceeded the schema-1 envelope.", OutOutcome, OutError);
						Values.Heights[Offset] = static_cast<int16>(Composed);
					}
					if (!Contribution.Coverage.empty()) Values.Coverage[Offset] = Contribution.Coverage[Offset];
				}
		}
		if (ContributionIndex != Contributions.size())
			return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain tile source contributions contain an unordered or non-overlapping extra value.",
				OutOutcome, OutError);
		return NormalizeTerrainTileInput(Definition, TileX, TileY, Values,
			OutInput, OutOutcome, OutError);
	}

	auto EstimateTerrainTileBuildBytes(const FTerrainTileRecipeInput& Input) -> uint64
	{
		uint64 Bytes = sizeof(FTerrainTileRecipeInput);
		Bytes += static_cast<uint64>(Input.Heights.size()) * sizeof(int16);
		Bytes += static_cast<uint64>(Input.HeightHalo.size()) * sizeof(int16);
		Bytes += static_cast<uint64>(Input.CoverageHalo.size()) * sizeof(FTerrainCoverageSample);
		Bytes += static_cast<uint64>(Input.Coverage.size()) * sizeof(FTerrainCoverageSample);
		Bytes += static_cast<uint64>(Input.Sources.size()) * sizeof(FTerrainCompositionSource);
		Bytes += static_cast<uint64>(Input.LayerIds.size()) * sizeof(FGuid);
		Bytes += 752ull * 1024ull;
		return Bytes;
	}

	auto BuildTerrainNeighborEvidence(const FTerrainTileRecipeInput& Tile,
		const FTerrainTileRecipeInput& Neighbor,
		FTerrainNeighborEvidence& OutEvidence, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool
	{
		OutEvidence = {};
		if (!ValidateTerrainNormalizedTileInput(Tile, OutOutcome, OutError)
			|| !ValidateTerrainNormalizedTileInput(Neighbor, OutOutcome, OutError)) return false;
		int64 DeltaX = 0, DeltaY = 0;
		auto ResolveDelta = [](int64 Center, int64 Other, int64& OutDelta) {
			if (Other == Center) { OutDelta = 0; return true; }
			if (Center != std::numeric_limits<int64>::max() && Other == Center + 1)
				{ OutDelta = 1; return true; }
			if (Center != std::numeric_limits<int64>::min() && Other == Center - 1)
				{ OutDelta = -1; return true; }
			return false;
		};
		if (Neighbor.Tile.WorldId != Tile.Tile.WorldId
			|| Neighbor.Tile.SchemeVersion != Tile.Tile.SchemeVersion
			|| !ResolveDelta(Tile.Tile.TileX, Neighbor.Tile.TileX, DeltaX)
			|| !ResolveDelta(Tile.Tile.TileY, Neighbor.Tile.TileY, DeltaY)
			|| (DeltaX == 0 && DeltaY == 0))
			return TerrainWorldFail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain neighbor is not one of the ordered eight adjacent tiles.", OutOutcome, OutError);
		FBinaryWriter HeightWriter;
		FBinaryWriter CoverageWriter;
		auto Compare = [&](uint32 TileX, uint32 TileY, uint32 NeighborX, uint32 NeighborY) {
			const uint32 TileOffset = TileY * 257 + TileX;
			const uint32 NeighborOffset = NeighborY * 257 + NeighborX;
			if (Tile.Heights[TileOffset] != Neighbor.Heights[NeighborOffset]
				|| !CoverageEqual(Tile.Coverage[TileOffset], Neighbor.Coverage[NeighborOffset]))
				return false;
			AppendBorderSample(HeightWriter, CoverageWriter, Tile, TileX, TileY);
			return true;
		};
		if (DeltaX != 0 && DeltaY != 0)
		{
			if (!Compare(DeltaX < 0 ? 0 : 256, DeltaY < 0 ? 0 : 256,
				DeltaX < 0 ? 256 : 0, DeltaY < 0 ? 256 : 0))
				return TerrainWorldFail(ETerrainWorldOutcome::BorderMismatch,
					"Terrain diagonal neighbor corner does not match bit-identically.", OutOutcome, OutError);
		}
		else if (DeltaX != 0)
		{
			for (uint32 Y = 0; Y <= 256; ++Y)
				if (!Compare(DeltaX < 0 ? 0 : 256, Y, DeltaX < 0 ? 256 : 0, Y))
					return TerrainWorldFail(ETerrainWorldOutcome::BorderMismatch,
						"Terrain east/west neighbor edge does not match bit-identically.", OutOutcome, OutError);
		}
		else
		{
			for (uint32 X = 0; X <= 256; ++X)
				if (!Compare(X, DeltaY < 0 ? 0 : 256, X, DeltaY < 0 ? 256 : 0))
					return TerrainWorldFail(ETerrainWorldOutcome::BorderMismatch,
						"Terrain north/south neighbor edge does not match bit-identically.", OutOutcome, OutError);
		}
		OutEvidence = {true, Neighbor.Tile, FXxHash128::HashBuffer(HeightWriter.GetBytes()),
			FXxHash128::HashBuffer(CoverageWriter.GetBytes())};
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto BuildTerrainWorldRecipe(FTerrainWorldRecipeRequest Request,
		FTerrainWorldRecipeProduct& OutProduct,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		OutProduct = {};
		FTerrainTileRecipeInput Input = std::move(Request.Input);
		if (!ValidateTerrainNormalizedTileInput(Input, OutOutcome, OutError)) return false;
		if (Input.ShouldCancel && Input.ShouldCancel())
			return TerrainWorldFail(ETerrainWorldOutcome::Cancelled,
				"Terrain tile generation was cancelled.", OutOutcome, OutError);
		OutProduct.Bodies[ProductIndex(ETerrainTileProductClass::Height)] =
			BuildHeightBody(Input);
		OutProduct.Bodies[ProductIndex(ETerrainTileProductClass::Coverage)] =
			BuildCoverageBody(Input);
		OutProduct.Bodies[ProductIndex(ETerrainTileProductClass::Collision)] =
			BuildCollisionBody(Input);
		OutProduct.Bodies[ProductIndex(ETerrainTileProductClass::Query)] =
			BuildQueryBody(Input);
		OutProduct.Bodies[ProductIndex(ETerrainTileProductClass::Metadata)] =
			BuildMetadataBody(Input);
		if (Input.ShouldCancel && Input.ShouldCancel())
		{
			OutProduct = {};
			return TerrainWorldFail(ETerrainWorldOutcome::Cancelled,
				"Terrain tile generation was cancelled.", OutOutcome, OutError);
		}
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

}
