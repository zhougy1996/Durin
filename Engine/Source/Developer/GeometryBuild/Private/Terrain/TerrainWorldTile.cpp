#include "Terrain/TerrainWorldTile.h"

#include "AssetBuild/BuildSession.h"
#include "Serialization/BinaryFormat.h"
#include "GeometryBuildFunctionRegistry.h"
#include "Terrain/TerrainWorldBuildFunctions.h"

namespace Durin::Asset::Build
{
	namespace
	{
		constexpr uint32 MagicMetadata = 0x444d5754; // TWMD
		constexpr uint32 MagicHeight = 0x54485754; // TWHT
		constexpr uint32 MagicCoverage = 0x56435754; // TWCV
		constexpr uint32 MagicCollision = 0x4c435754; // TWCL
		constexpr uint32 MagicQuery = 0x59515754; // TWQY
		constexpr uint32 RequiredFlags = 1;
		constexpr uint64 MaximumLatticeContributionMeters = 1ull << 40;
		constexpr uint64 MaximumCellsPerAxis = 1ull << 31;
		constexpr uint64 HeaderBytes = 108;

		auto Fail(ETerrainWorldOutcome Outcome, std::string Message,
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

		auto ProductMagic(ETerrainTileProductClass ProductClass) -> uint32
		{
			switch (ProductClass)
			{
			case ETerrainTileProductClass::Metadata: return MagicMetadata;
			case ETerrainTileProductClass::Height: return MagicHeight;
			case ETerrainTileProductClass::Coverage: return MagicCoverage;
			case ETerrainTileProductClass::Collision: return MagicCollision;
			case ETerrainTileProductClass::Query: return MagicQuery;
			}
			return 0;
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

		auto ReadGuid(FBinaryReader& Reader, FGuid& OutGuid) -> bool
		{
			std::array<uint32, 4> Words{};
			for (uint32& Word : Words)
				for (int Shift : {24, 16, 8, 0})
				{
					uint8 Byte = 0;
					if (!Reader.ReadU8(Byte)) return false;
					Word |= static_cast<uint32>(Byte) << Shift;
				}
			OutGuid = {Words[0], Words[1], Words[2], Words[3]};
			return true;
		}

		auto WriteHash(FBinaryWriter& Writer, const FXxHash128& Hash) -> void
		{
			Writer.WriteU64(Hash.HashLow);
			Writer.WriteU64(Hash.HashHigh);
		}

		auto ReadHash(FBinaryReader& Reader, FXxHash128& OutHash) -> bool
		{
			return Reader.ReadU64(OutHash.HashLow) && Reader.ReadU64(OutHash.HashHigh);
		}

		auto WriteTileKey(FBinaryWriter& Writer, const FTerrainTileKey& Tile) -> void
		{
			WriteGuid(Writer, Tile.WorldId.Value);
			Writer.WriteI64(Tile.TileX);
			Writer.WriteI64(Tile.TileY);
			Writer.WriteU16(Tile.SchemeVersion);
		}

		auto ReadTileKey(FBinaryReader& Reader, FTerrainTileKey& OutTile) -> bool
		{
			return ReadGuid(Reader, OutTile.WorldId.Value)
				&& Reader.ReadI64(OutTile.TileX) && Reader.ReadI64(OutTile.TileY)
				&& Reader.ReadU16(OutTile.SchemeVersion);
		}

		auto WriteDouble(FBinaryWriter& Writer, double Value) -> void
		{
			Writer.WriteU64(std::bit_cast<uint64>(Value));
		}

		auto IsFinite(double Value) -> bool
		{
			return std::isfinite(Value);
		}

		auto IsCanonicalPositive(double Value) -> bool
		{
			return IsFinite(Value) && Value >= 0.01 && Value <= 4096.0
				&& !(Value == 0.0 && std::signbit(Value));
		}

		auto CheckedAxisCells(int64 Min, int64 Max, uint64& OutCells) -> bool
		{
			if (Max <= Min) return false;
			const uint64 Difference = static_cast<uint64>(Max) - static_cast<uint64>(Min);
			if (Difference == 0 || Difference > MaximumCellsPerAxis) return false;
			OutCells = Difference;
			return true;
		}

		auto CheckedAdd(int64 Value, int64 Delta, int64& OutValue) -> bool
		{
			if ((Delta > 0 && Value > std::numeric_limits<int64>::max() - Delta)
				|| (Delta < 0 && Value < std::numeric_limits<int64>::min() - Delta)) return false;
			OutValue = Value + Delta;
			return true;
		}

		auto GuidLess(const FGuid& A, const FGuid& B) -> bool
		{
			// RFC 4122 order compares the four big-endian words lexicographically.
			return std::tuple(A.A, A.B, A.C, A.D) < std::tuple(B.A, B.B, B.C, B.D);
		}

		auto IsValidProductClass(ETerrainTileProductClass ProductClass) -> bool
		{
			return ProductMagic(ProductClass) != 0;
		}

		auto EncodeCommonKey(const FTerrainNormalizedTileInput& Input,
			ETerrainTileProductClass ProductClass, FBinaryWriter& Writer) -> void
		{
			Writer.WriteString("TerrainWorldTileBuildKey");
			Writer.WriteU16(TerrainWorldSchemaVersion);
			Writer.WriteU8(static_cast<uint8>(ProductClass));
			WriteTileKey(Writer, Input.Tile);
			Writer.WriteI64(Input.WorldExtent.Min.X);
			Writer.WriteI64(Input.WorldExtent.Min.Y);
			Writer.WriteI64(Input.WorldExtent.Max.X);
			Writer.WriteI64(Input.WorldExtent.Max.Y);
			WriteGuid(Writer, Input.CompositionPolicyId);
			Writer.WriteU32(Input.CompositionPolicyVersion);
			Writer.WriteU32(Input.BuilderVersion);
			Writer.WriteU16(Input.ProductSchemaVersion);
			Writer.WriteU32(static_cast<uint32>(Input.TargetPlatform));
			Writer.WriteU32(static_cast<uint32>(Input.TargetProfile));
			Writer.WriteU32(Input.OutputPolicy);
		}

		auto SourceAffects(ETerrainTileProductClass ProductClass,
			const FTerrainCompositionSource& Source) -> bool
		{
			if (ProductClass == ETerrainTileProductClass::Coverage)
				return (Source.AffectedProductMask & TerrainSourceAffectsCoverage) != 0;
			if (ProductClass == ETerrainTileProductClass::Query)
				return Source.AffectedProductMask != 0;
			return (Source.AffectedProductMask & TerrainSourceAffectsHeight) != 0;
		}

		auto WriteSources(FBinaryWriter& Writer,
			std::span<const FTerrainCompositionSource> Sources,
			ETerrainTileProductClass ProductClass) -> void
		{
			const uint32 Count = static_cast<uint32>(std::ranges::count_if(Sources,
				[&](const auto& Source) { return SourceAffects(ProductClass, Source); }));
			Writer.WriteU32(Count);
			for (const FTerrainCompositionSource& Source : Sources)
			{
				if (!SourceAffects(ProductClass, Source)) continue;
				WriteGuid(Writer, Source.SourceId);
				WriteHash(Writer, Source.ContentHash);
				Writer.WriteI64(Source.AffectedSamples.Min.X);
				Writer.WriteI64(Source.AffectedSamples.Min.Y);
				Writer.WriteI64(Source.AffectedSamples.Max.X);
				Writer.WriteI64(Source.AffectedSamples.Max.Y);
				Writer.WriteU8(Source.BlendOperation);
				Writer.WriteI32(Source.Strength);
				Writer.WriteU8(Source.bEnabled ? 1 : 0);
				Writer.WriteU8(Source.AffectedProductMask);
			}
		}

		auto WriteHeights(FBinaryWriter& Writer, std::span<const int16> Heights) -> void
		{
			Writer.WriteU32(static_cast<uint32>(Heights.size()));
			for (int16 Height : Heights) Writer.WriteU16(static_cast<uint16>(Height));
		}

		auto WriteCoverage(FBinaryWriter& Writer, std::span<const FGuid> LayerIds,
			std::span<const FTerrainCoverageSample> Coverage) -> void
		{
			Writer.WriteU32(static_cast<uint32>(LayerIds.size()));
			for (const FGuid& Layer : LayerIds) WriteGuid(Writer, Layer);
			Writer.WriteU32(static_cast<uint32>(Coverage.size()));
			for (const FTerrainCoverageSample& Sample : Coverage)
			{
				Writer.WriteU8(Sample.LayerCount);
				for (uint8 Index = 0; Index < Sample.LayerCount; ++Index)
				{
					const auto It = std::ranges::find(LayerIds, Sample.Layers[Index].LayerId);
					Writer.WriteU8(static_cast<uint8>(std::distance(LayerIds.begin(), It)));
					Writer.WriteU8(Sample.Layers[Index].Weight);
				}
			}
		}

		auto WriteNeighbors(FBinaryWriter& Writer,
			const std::array<FTerrainNeighborEvidence, TerrainWorldMaximumNeighbors>& Neighbors,
			bool bCoverage) -> void
		{
			for (const FTerrainNeighborEvidence& Neighbor : Neighbors)
			{
				Writer.WriteU8(Neighbor.bPresent ? 1 : 0);
				if (!Neighbor.bPresent) continue;
				WriteTileKey(Writer, Neighbor.Tile);
				WriteHash(Writer, bCoverage ? Neighbor.CoverageEdgeHash : Neighbor.HeightEdgeHash);
			}
		}

		auto BuildHeightBody(const FTerrainNormalizedTileInput& Input) -> std::vector<std::byte>
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

		auto BuildCoverageBody(const FTerrainNormalizedTileInput& Input) -> std::vector<std::byte>
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

		auto BuildCollisionBody(const FTerrainNormalizedTileInput& Input) -> std::vector<std::byte>
		{
			FBinaryWriter Writer;
			Writer.WriteU16(129);
			Writer.WriteU16(129);
			for (uint32 Y = 0; Y <= 256; Y += 2)
				for (uint32 X = 0; X <= 256; X += 2)
					Writer.WriteU16(static_cast<uint16>(Input.Heights[Y * 257 + X]));
			return Writer.TakeBytes();
		}

		auto BuildQueryBody(const FTerrainNormalizedTileInput& Input) -> std::vector<std::byte>
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

		auto BuildMetadataBody(const FTerrainNormalizedTileInput& Input) -> std::vector<std::byte>
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
			FBinaryWriter& CoverageWriter, const FTerrainNormalizedTileInput& Input,
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

		auto ValidateGeneration(const FTerrainTileGeneration& Candidate,
			ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
		{
			if (!Candidate.Tile.WorldId.IsValid() || !Candidate.GenerationId.IsValid())
				return Fail(ETerrainWorldOutcome::PublicationFailed,
					"Terrain tile generation identity is incomplete.", OutOutcome, OutError);
			std::array<FXxHash128, 5> Hashes{};
			for (uint8 Value = 1; Value <= 5; ++Value)
			{
				const size_t Index = Value - 1;
				const auto Class = static_cast<ETerrainTileProductClass>(Value);
				const FTerrainTileProduct& Product = Candidate.Products[Index];
				FTerrainTileProduct Decoded;
				if (Product.ProductClass != Class || Product.Tile != Candidate.Tile
					|| Product.GenerationId != Candidate.GenerationId
					|| !DecodeTerrainTileProduct(Product.Bytes, Class, Decoded, OutOutcome, OutError))
					return Fail(ETerrainWorldOutcome::PublicationFailed,
						"Terrain tile generation contains an invalid or mismatched product.", OutOutcome, OutError);
				Hashes[Index] = Decoded.BodyHash;
			}
			const auto& Metadata = Candidate.Products[ProductIndex(ETerrainTileProductClass::Metadata)];
			const auto& Collision = Candidate.Products[ProductIndex(ETerrainTileProductClass::Collision)];
			const auto& Query = Candidate.Products[ProductIndex(ETerrainTileProductClass::Query)];
			if (Metadata.Dependencies != std::vector{Hashes[ProductIndex(ETerrainTileProductClass::Height)]}
				|| Collision.Dependencies != std::vector{Hashes[ProductIndex(ETerrainTileProductClass::Height)]}
				|| Query.Dependencies != std::vector{Hashes[ProductIndex(ETerrainTileProductClass::Height)],
					Hashes[ProductIndex(ETerrainTileProductClass::Coverage)]})
				return Fail(ETerrainWorldOutcome::MissingDependency,
					"Terrain tile generation product dependencies are incomplete.", OutOutcome, OutError);
			return true;
		}
	}

	auto TerrainFloorDiv(int64 Value, int64 Divisor, int64& OutResult) -> bool
	{
		if (Divisor == 0 || (Value == std::numeric_limits<int64>::min() && Divisor == -1)) return false;
		const int64 Quotient = Value / Divisor;
		const int64 Remainder = Value % Divisor;
		OutResult = Quotient - ((Remainder != 0 && ((Remainder < 0) != (Divisor < 0))) ? 1 : 0);
		return true;
	}

	auto TerrainFloorMod(int64 Value, int64 Divisor, int64& OutResult) -> bool
	{
		int64 Quotient = 0;
		if (!TerrainFloorDiv(Value, Divisor, Quotient)) return false;
		if (Divisor == -1) { OutResult = 0; return true; }
		OutResult = Value % Divisor;
		if (OutResult < 0) OutResult += std::abs(Divisor);
		return true;
	}

	auto ValidateTerrainWorldDefinition(const FTerrainWorldDefinition& Definition,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		uint64 CellsX = 0, CellsY = 0;
		if (!Definition.WorldId.IsValid() || !CheckedAxisCells(
			Definition.SampleExtent.Min.X, Definition.SampleExtent.Max.X, CellsX)
			|| !CheckedAxisCells(Definition.SampleExtent.Min.Y, Definition.SampleExtent.Max.Y, CellsY)
			|| Definition.TileSchemeVersion != TerrainWorldTileSchemeVersion
			|| !IsFinite(Definition.Coordinates.OriginX)
			|| !IsFinite(Definition.Coordinates.OriginY)
			|| !IsFinite(Definition.Coordinates.OriginZ)
			|| !IsFinite(Definition.Coordinates.HeightDatumMeters)
			|| !IsCanonicalPositive(Definition.Coordinates.SampleSpacingMeters)
			|| !Definition.BuildPolicyId.IsValid() || Definition.BuildPolicyVersion == 0
			|| Definition.ProductProfile < 1 || Definition.ProductProfile > 4
			|| Definition.PeakBuildBudgetBytes == 0
			|| Definition.PeakBuildBudgetBytes > (Definition.ProductProfile == 1
				? 2ull * 1024ull * 1024ull * 1024ull : 4ull * 1024ull * 1024ull * 1024ull)
			|| Definition.TargetPlatform == ECookTargetPlatform::Invalid
			|| Definition.TargetProfile == ECookTargetProfile::Invalid
			|| Definition.RegionTileDimension != 8)
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain World definition has an invalid identity, extent, coordinate, policy, target, or region value.",
				OutOutcome, OutError);
		if (Definition.Layers.size() > TerrainWorldMaximumLayers
			|| Definition.Sources.size() > TerrainWorldMaximumSources)
			return Fail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain World definition exceeds a layer or source count ceiling.", OutOutcome, OutError);
		for (int64 Coordinate : {Definition.SampleExtent.Min.X, Definition.SampleExtent.Min.Y,
			Definition.SampleExtent.Max.X, Definition.SampleExtent.Max.Y})
			if (std::abs(static_cast<long double>(Coordinate)
				* Definition.Coordinates.SampleSpacingMeters) > MaximumLatticeContributionMeters)
				return Fail(ETerrainWorldOutcome::Overflow,
					"Terrain World definition lattice contribution exceeds 2^40 meters.", OutOutcome, OutError);
		std::unordered_set<FGuid> LayerIds;
		std::unordered_set<std::string> LayerNames;
		for (const FTerrainLayerDefinition& Layer : Definition.Layers)
			if (!Layer.LayerId.IsValid() || Layer.DisplayName.empty()
				|| !LayerIds.insert(Layer.LayerId).second || !LayerNames.insert(Layer.DisplayName).second)
				return Fail(ETerrainWorldOutcome::InvalidDefinition,
					"Terrain World layers require unique nonzero identities and display names.", OutOutcome, OutError);
		FGuid PreviousSource;
		for (const FTerrainCompositionSource& Source : Definition.Sources)
		{
			uint64 SourceCellsX = 0, SourceCellsY = 0;
			if (!Source.SourceId.IsValid() || Source.ContentHash.IsZero()
				|| !CheckedAxisCells(Source.AffectedSamples.Min.X, Source.AffectedSamples.Max.X, SourceCellsX)
				|| !CheckedAxisCells(Source.AffectedSamples.Min.Y, Source.AffectedSamples.Max.Y, SourceCellsY)
				|| Source.BlendOperation < static_cast<uint8>(ETerrainCompositionBlendOperation::Replace)
				|| Source.BlendOperation > static_cast<uint8>(ETerrainCompositionBlendOperation::Maximum)
				|| Source.Strength < 0 || Source.Strength > 255
				|| Source.AffectedProductMask == 0
				|| (Source.AffectedProductMask & ~(TerrainSourceAffectsHeight | TerrainSourceAffectsCoverage)) != 0
				|| (PreviousSource.IsValid() && !GuidLess(PreviousSource, Source.SourceId)))
				return Fail(ETerrainWorldOutcome::InvalidDefinition,
					"Terrain World sources are invalid, duplicated, or not canonically ordered.", OutOutcome, OutError);
			PreviousSource = Source.SourceId;
		}
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto GetTerrainTileSampleExtent(const FTerrainTileKey& Tile,
		FTerrainSampleExtent& OutExtent, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool
	{
		if (!Tile.WorldId.IsValid() || Tile.SchemeVersion != TerrainWorldTileSchemeVersion
			|| Tile.TileX > std::numeric_limits<int64>::max() / TerrainWorldTileCells
			|| Tile.TileX < std::numeric_limits<int64>::min() / TerrainWorldTileCells
			|| Tile.TileY > std::numeric_limits<int64>::max() / TerrainWorldTileCells
			|| Tile.TileY < std::numeric_limits<int64>::min() / TerrainWorldTileCells)
			return Fail(ETerrainWorldOutcome::Overflow,
				"Terrain tile rectangle cannot be represented.", OutOutcome, OutError);
		const int64 MinX = Tile.TileX * TerrainWorldTileCells;
		const int64 MinY = Tile.TileY * TerrainWorldTileCells;
		if (MinX > std::numeric_limits<int64>::max() - TerrainWorldTileCells
			|| MinY > std::numeric_limits<int64>::max() - TerrainWorldTileCells)
			return Fail(ETerrainWorldOutcome::Overflow,
				"Terrain tile rectangle cannot be represented.", OutOutcome, OutError);
		OutExtent = {{MinX, MinY}, {MinX + TerrainWorldTileCells, MinY + TerrainWorldTileCells}};
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto ResolveTerrainSampleAddress(const FTerrainWorldId& WorldId,
		const FTerrainSampleExtent& Extent, FTerrainGlobalSample Sample,
		FTerrainTileAddress& OutAddress, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool
	{
		uint64 CellsX = 0, CellsY = 0;
		if (!WorldId.IsValid() || !CheckedAxisCells(Extent.Min.X, Extent.Max.X, CellsX)
			|| !CheckedAxisCells(Extent.Min.Y, Extent.Max.Y, CellsY))
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain sample lookup extent is invalid.", OutOutcome, OutError);
		if (Sample.X < Extent.Min.X || Sample.X > Extent.Max.X
			|| Sample.Y < Extent.Min.Y || Sample.Y > Extent.Max.Y)
			return Fail(ETerrainWorldOutcome::Unavailable,
				"Terrain sample is outside the inclusive world extent.", OutOutcome, OutError);
		FTerrainGlobalSample Lookup = Sample;
		if (Lookup.X == Extent.Max.X) --Lookup.X;
		if (Lookup.Y == Extent.Max.Y) --Lookup.Y;
		int64 TileX = 0, TileY = 0, LocalX = 0, LocalY = 0;
		if (!TerrainFloorDiv(Lookup.X, 256, TileX) || !TerrainFloorDiv(Lookup.Y, 256, TileY)
			|| !TerrainFloorMod(Lookup.X, 256, LocalX) || !TerrainFloorMod(Lookup.Y, 256, LocalY))
			return Fail(ETerrainWorldOutcome::Overflow,
				"Terrain sample address arithmetic overflowed.", OutOutcome, OutError);
		if (Sample.X == Extent.Max.X) ++LocalX;
		if (Sample.Y == Extent.Max.Y) ++LocalY;
		OutAddress = {{WorldId, TileX, TileY, TerrainWorldTileSchemeVersion},
			{static_cast<uint16>(LocalX), static_cast<uint16>(LocalY)}};
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto TerrainSampleToWorldPosition(const FTerrainWorldCoordinates& Coordinates,
		FTerrainGlobalSample Sample, int16 Height, std::array<double, 3>& OutPosition,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		if (!IsFinite(Coordinates.OriginX) || !IsFinite(Coordinates.OriginY)
			|| !IsFinite(Coordinates.OriginZ) || !IsFinite(Coordinates.HeightDatumMeters)
			|| !IsCanonicalPositive(Coordinates.SampleSpacingMeters))
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain coordinate values are invalid.", OutOutcome, OutError);
		const long double ContributionX = static_cast<long double>(Sample.X) * Coordinates.SampleSpacingMeters;
		const long double ContributionY = static_cast<long double>(Sample.Y) * Coordinates.SampleSpacingMeters;
		if (std::abs(ContributionX) > MaximumLatticeContributionMeters
			|| std::abs(ContributionY) > MaximumLatticeContributionMeters)
			return Fail(ETerrainWorldOutcome::Overflow,
				"Terrain lattice contribution exceeds 2^40 meters.", OutOutcome, OutError);
		OutPosition = {Coordinates.OriginX + static_cast<double>(ContributionX),
			Coordinates.OriginY + static_cast<double>(ContributionY),
			Coordinates.OriginZ + Coordinates.HeightDatumMeters + static_cast<double>(Height) * 0.25};
		if (!std::ranges::all_of(OutPosition, IsFinite))
			return Fail(ETerrainWorldOutcome::Overflow,
				"Terrain world position is not finite.", OutOutcome, OutError);
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto NormalizeTerrainHeightQuantum(int32 Height, int16& OutHeight,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		if (Height < -32768 || Height > 32767)
			return Fail(ETerrainWorldOutcome::Overflow,
				"Terrain height is outside the schema-1 signed quantum envelope.",
				OutOutcome, OutError);
		OutHeight = static_cast<int16>(Height);
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto ValidateTerrainNormalizedTileInput(const FTerrainNormalizedTileInput& Input,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		uint64 CellsX = 0, CellsY = 0;
		FTerrainSampleExtent TileExtent;
		if (!Input.Tile.WorldId.IsValid() || Input.Tile.SchemeVersion != TerrainWorldTileSchemeVersion
			|| Input.ProductSchemaVersion != TerrainWorldSchemaVersion
			|| !Input.CompositionPolicyId.IsValid() || Input.CompositionPolicyVersion == 0
			|| Input.BuilderVersion == 0 || Input.TargetPlatform == ECookTargetPlatform::Invalid
			|| Input.TargetProfile == ECookTargetProfile::Invalid
			|| !IsFinite(Input.Coordinates.OriginX) || !IsFinite(Input.Coordinates.OriginY)
			|| !IsFinite(Input.Coordinates.OriginZ)
			|| !IsFinite(Input.Coordinates.HeightDatumMeters)
			|| !IsCanonicalPositive(Input.Coordinates.SampleSpacingMeters)
			|| !CheckedAxisCells(Input.WorldExtent.Min.X, Input.WorldExtent.Max.X, CellsX)
			|| !CheckedAxisCells(Input.WorldExtent.Min.Y, Input.WorldExtent.Max.Y, CellsY)
			|| !GetTerrainTileSampleExtent(Input.Tile, TileExtent, OutOutcome, OutError))
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
				"Normalized Terrain tile identity, schema, policy, target, or extent is invalid.", OutOutcome, OutError);
		for (int64 Coordinate : {Input.WorldExtent.Min.X, Input.WorldExtent.Min.Y,
			Input.WorldExtent.Max.X, Input.WorldExtent.Max.Y})
			if (std::abs(static_cast<long double>(Coordinate)
				* Input.Coordinates.SampleSpacingMeters) > MaximumLatticeContributionMeters)
				return Fail(ETerrainWorldOutcome::Overflow,
					"Normalized Terrain tile lattice contribution exceeds 2^40 meters.", OutOutcome, OutError);
		if (TileExtent.Max.X < Input.WorldExtent.Min.X || TileExtent.Min.X > Input.WorldExtent.Max.X
			|| TileExtent.Max.Y < Input.WorldExtent.Min.Y || TileExtent.Min.Y > Input.WorldExtent.Max.Y)
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
				"Normalized Terrain tile does not intersect its world.", OutOutcome, OutError);
		if ((Input.HeightHalo.empty() || Input.CoverageHalo.empty())
			&& (TileExtent.Min.X > Input.WorldExtent.Min.X || TileExtent.Max.X < Input.WorldExtent.Max.X
				|| TileExtent.Min.Y > Input.WorldExtent.Min.Y || TileExtent.Max.Y < Input.WorldExtent.Max.Y))
			return Fail(ETerrainWorldOutcome::MissingDependency,
				"Normalized Terrain tile requires a one-sample halo away from every world edge.",
				OutOutcome, OutError);
		if (Input.Sources.size() > TerrainWorldMaximumTileSources
			|| Input.LayerIds.size() > TerrainWorldMaximumTileLayers
			|| Input.Heights.size() != TerrainWorldSampleCount
			|| Input.Coverage.size() != TerrainWorldSampleCount
			|| (!Input.HeightHalo.empty() && Input.HeightHalo.size() != 259u * 259u)
			|| (!Input.CoverageHalo.empty() && Input.CoverageHalo.size() != 259u * 259u))
			return Fail(ETerrainWorldOutcome::BudgetRejected,
				"Normalized Terrain tile exceeds a count bound or has an invalid sample array.", OutOutcome, OutError);
		FGuid PreviousSource;
		for (const FTerrainCompositionSource& Source : Input.Sources)
		{
			if (!Source.SourceId.IsValid() || Source.ContentHash.IsZero()
				|| (PreviousSource.IsValid() && !GuidLess(PreviousSource, Source.SourceId))
				|| Source.AffectedProductMask == 0
				|| (Source.AffectedProductMask & ~(TerrainSourceAffectsHeight | TerrainSourceAffectsCoverage)) != 0)
				return Fail(ETerrainWorldOutcome::InvalidDefinition,
					"Normalized Terrain sources are invalid or not canonically ordered.", OutOutcome, OutError);
			PreviousSource = Source.SourceId;
		}
		if (!std::ranges::is_sorted(Input.LayerIds, GuidLess)
			|| std::adjacent_find(Input.LayerIds.begin(), Input.LayerIds.end()) != Input.LayerIds.end()
			|| std::ranges::any_of(Input.LayerIds, [](const FGuid& Id) { return !Id.IsValid(); }))
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
				"Normalized Terrain layers are invalid, duplicated, or not canonically ordered.", OutOutcome, OutError);
		auto ValidateCoverageSamples = [&](std::span<const FTerrainCoverageSample> Samples) -> bool
		{
			for (const FTerrainCoverageSample& Sample : Samples)
			{
				uint32 Sum = 0;
				if (Sample.LayerCount == 0 || Sample.LayerCount > TerrainWorldMaximumActiveLayers)
					return Fail(ETerrainWorldOutcome::InvalidDefinition,
						"Terrain coverage active layer count is invalid.", OutOutcome, OutError);
				FGuid Previous;
				for (uint8 Index = 0; Index < Sample.LayerCount; ++Index)
				{
					const auto& Weight = Sample.Layers[Index];
					if (Weight.Weight == 0
						|| std::ranges::find(Input.LayerIds, Weight.LayerId) == Input.LayerIds.end()
						|| (Previous.IsValid() && !GuidLess(Previous, Weight.LayerId)))
						return Fail(ETerrainWorldOutcome::InvalidDefinition,
							"Terrain coverage weights are invalid or not canonically ordered.", OutOutcome, OutError);
					Previous = Weight.LayerId;
					Sum += Weight.Weight;
				}
				if (Sum != 255)
					return Fail(ETerrainWorldOutcome::InvalidDefinition,
						"Terrain coverage weights must sum exactly to 255.", OutOutcome, OutError);
			}
			return true;
		};
		if (!ValidateCoverageSamples(Input.Coverage)
			|| (!Input.CoverageHalo.empty() && !ValidateCoverageSamples(Input.CoverageHalo))) return false;
		for (size_t Index = 0; Index < Input.Neighbors.size(); ++Index)
		{
			const FTerrainNeighborEvidence& Neighbor = Input.Neighbors[Index];
			if (!Neighbor.bPresent) continue;
			static constexpr std::array<std::pair<int64, int64>, 8> Offsets{{
				{-1,-1}, {0,-1}, {1,-1}, {-1,0}, {1,0}, {-1,1}, {0,1}, {1,1}}};
			int64 ExpectedX = 0, ExpectedY = 0;
			if (!CheckedAdd(Input.Tile.TileX, Offsets[Index].first, ExpectedX)
				|| !CheckedAdd(Input.Tile.TileY, Offsets[Index].second, ExpectedY))
				return Fail(ETerrainWorldOutcome::Overflow,
					"Terrain neighbor coordinate overflowed.", OutOutcome, OutError);
			if (Neighbor.Tile.WorldId != Input.Tile.WorldId
				|| Neighbor.Tile.SchemeVersion != Input.Tile.SchemeVersion
				|| Neighbor.Tile.TileX != ExpectedX || Neighbor.Tile.TileY != ExpectedY
				|| Neighbor.HeightEdgeHash.IsZero() || Neighbor.CoverageEdgeHash.IsZero())
				return Fail(ETerrainWorldOutcome::BorderMismatch,
					"Terrain neighbor evidence is missing, unordered, or identifies the wrong neighbor.", OutOutcome, OutError);
			FBinaryWriter HeightWriter;
			FBinaryWriter CoverageWriter;
			const int64 DeltaX = Offsets[Index].first;
			const int64 DeltaY = Offsets[Index].second;
			if (DeltaX != 0 && DeltaY != 0)
				AppendBorderSample(HeightWriter, CoverageWriter, Input,
					DeltaX < 0 ? 0 : 256, DeltaY < 0 ? 0 : 256);
			else if (DeltaX != 0)
				for (uint32 Y = 0; Y <= 256; ++Y)
					AppendBorderSample(HeightWriter, CoverageWriter, Input,
						DeltaX < 0 ? 0 : 256, Y);
			else
				for (uint32 X = 0; X <= 256; ++X)
					AppendBorderSample(HeightWriter, CoverageWriter, Input,
						X, DeltaY < 0 ? 0 : 256);
			if (FXxHash128::HashBuffer(HeightWriter.GetBytes()) != Neighbor.HeightEdgeHash
				|| FXxHash128::HashBuffer(CoverageWriter.GetBytes()) != Neighbor.CoverageEdgeHash)
				return Fail(ETerrainWorldOutcome::BorderMismatch,
					"Terrain neighbor evidence does not match the tile's canonical border.", OutOutcome, OutError);
		}
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto NormalizeTerrainTileInput(const FTerrainWorldDefinition& Definition,
		int64 TileX, int64 TileY, const FTerrainComposedTileValues& ComposedValues,
		FTerrainNormalizedTileInput& OutInput, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool
	{
		OutInput = {};
		if (!ValidateTerrainWorldDefinition(Definition, OutOutcome, OutError)) return false;
		if (ComposedValues.ShouldCancel && ComposedValues.ShouldCancel())
			return Fail(ETerrainWorldOutcome::Cancelled,
				"Terrain tile normalization was cancelled.", OutOutcome, OutError);
		FTerrainNormalizedTileInput Candidate;
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
				return Fail(ETerrainWorldOutcome::BudgetRejected,
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
				return Fail(ETerrainWorldOutcome::InvalidDefinition,
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
			return Fail(ETerrainWorldOutcome::BudgetRejected,
				"More than 16 logical layers overlap one Terrain tile.", OutOutcome, OutError);
		if (!ValidateTerrainNormalizedTileInput(Candidate, OutOutcome, OutError)) return false;
		const uint64 TaskCeiling = Definition.ProductProfile == 1
			? 512ull * 1024ull * 1024ull : 768ull * 1024ull * 1024ull;
		if (EstimateTerrainTileBuildBytes(Candidate) > TaskCeiling)
			return Fail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain tile normalization exceeds its profile task ceiling.", OutOutcome, OutError);
		OutInput = std::move(Candidate);
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto ComposeTerrainTileInput(const FTerrainWorldDefinition& Definition,
		int64 TileX, int64 TileY,
		std::span<const FTerrainTileSourceContribution> Contributions,
		FTerrainNormalizedTileInput& OutInput, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError, std::function<bool()> ShouldCancel) -> bool
	{
		OutInput = {};
		if (!ValidateTerrainWorldDefinition(Definition, OutOutcome, OutError)) return false;
		if (Contributions.size() > TerrainWorldMaximumTileSources)
			return Fail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain tile composition exceeds 64 source contributions.", OutOutcome, OutError);
		if (Definition.Layers.empty())
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
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
				return Fail(ETerrainWorldOutcome::MissingDependency,
					"Terrain tile source contribution is missing.", OutOutcome, OutError);
			const FTerrainTileSourceContribution& Contribution = Contributions[ContributionIndex++];
			if (Contribution.SourceId != Source.SourceId || Contribution.ContentHash != Source.ContentHash
				|| (!Contribution.Heights.empty() && Contribution.Heights.size() != TerrainWorldSampleCount)
				|| (!Contribution.Coverage.empty() && Contribution.Coverage.size() != TerrainWorldSampleCount)
				|| (((Source.AffectedProductMask & TerrainSourceAffectsHeight) != 0)
					!= !Contribution.Heights.empty())
				|| (((Source.AffectedProductMask & TerrainSourceAffectsCoverage) != 0)
					!= !Contribution.Coverage.empty()))
				return Fail(ETerrainWorldOutcome::MissingDependency,
					"Terrain tile source contribution identity or sample count is invalid.", OutOutcome, OutError);
			if (!Contribution.Coverage.empty()
				&& (Source.BlendOperation != static_cast<uint8>(ETerrainCompositionBlendOperation::Replace)
					|| Source.Strength != 255))
				return Fail(ETerrainWorldOutcome::InvalidDefinition,
					"Schema-1 coverage composition requires full-strength ordered Replace.", OutOutcome, OutError);
			for (uint32 Y = 0; Y <= 256; ++Y)
				for (uint32 X = 0; X <= 256; ++X)
				{
					if (ShouldCancel && ShouldCancel())
						return Fail(ETerrainWorldOutcome::Cancelled,
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
							return Fail(ETerrainWorldOutcome::Overflow,
								"Terrain height composition exceeded the schema-1 envelope.", OutOutcome, OutError);
						Values.Heights[Offset] = static_cast<int16>(Composed);
					}
					if (!Contribution.Coverage.empty()) Values.Coverage[Offset] = Contribution.Coverage[Offset];
				}
		}
		if (ContributionIndex != Contributions.size())
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain tile source contributions contain an unordered or non-overlapping extra value.",
				OutOutcome, OutError);
		return NormalizeTerrainTileInput(Definition, TileX, TileY, Values,
			OutInput, OutOutcome, OutError);
	}

	auto EstimateTerrainTileBuildBytes(const FTerrainNormalizedTileInput& Input) -> uint64
	{
		uint64 Bytes = sizeof(FTerrainNormalizedTileInput);
		Bytes += static_cast<uint64>(Input.Heights.size()) * sizeof(int16);
		Bytes += static_cast<uint64>(Input.HeightHalo.size()) * sizeof(int16);
		Bytes += static_cast<uint64>(Input.CoverageHalo.size()) * sizeof(FTerrainCoverageSample);
		Bytes += static_cast<uint64>(Input.Coverage.size()) * sizeof(FTerrainCoverageSample);
		Bytes += static_cast<uint64>(Input.Sources.size()) * sizeof(FTerrainCompositionSource);
		Bytes += static_cast<uint64>(Input.LayerIds.size()) * sizeof(FGuid);
		Bytes += 752ull * 1024ull;
		return Bytes;
	}

	auto BuildTerrainNeighborEvidence(const FTerrainNormalizedTileInput& Tile,
		const FTerrainNormalizedTileInput& Neighbor,
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
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
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
				return Fail(ETerrainWorldOutcome::BorderMismatch,
					"Terrain diagonal neighbor corner does not match bit-identically.", OutOutcome, OutError);
		}
		else if (DeltaX != 0)
		{
			for (uint32 Y = 0; Y <= 256; ++Y)
				if (!Compare(DeltaX < 0 ? 0 : 256, Y, DeltaX < 0 ? 256 : 0, Y))
					return Fail(ETerrainWorldOutcome::BorderMismatch,
						"Terrain east/west neighbor edge does not match bit-identically.", OutOutcome, OutError);
		}
		else
		{
			for (uint32 X = 0; X <= 256; ++X)
				if (!Compare(X, DeltaY < 0 ? 0 : 256, X, DeltaY < 0 ? 256 : 0))
					return Fail(ETerrainWorldOutcome::BorderMismatch,
						"Terrain north/south neighbor edge does not match bit-identically.", OutOutcome, OutError);
		}
		OutEvidence = {true, Neighbor.Tile, FXxHash128::HashBuffer(HeightWriter.GetBytes()),
			FXxHash128::HashBuffer(CoverageWriter.GetBytes())};
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	static auto MakeTerrainTileBuildKeyUnchecked(const FTerrainNormalizedTileInput& Input,
		ETerrainTileProductClass ProductClass, std::string& OutError) -> std::string
	{
		FBinaryWriter Writer;
		EncodeCommonKey(Input, ProductClass, Writer);
		WriteSources(Writer, Input.Sources, ProductClass);
		switch (ProductClass)
		{
		case ETerrainTileProductClass::Height:
			WriteHeights(Writer, Input.Heights);
			break;
		case ETerrainTileProductClass::Collision:
			WriteHeights(Writer, Input.Heights);
			break;
		case ETerrainTileProductClass::Metadata:
			WriteDouble(Writer, Input.Coordinates.OriginX);
			WriteDouble(Writer, Input.Coordinates.OriginY);
			WriteDouble(Writer, Input.Coordinates.OriginZ);
			WriteDouble(Writer, Input.Coordinates.SampleSpacingMeters);
			WriteDouble(Writer, Input.Coordinates.HeightDatumMeters);
			WriteHeights(Writer, Input.Heights);
			WriteHeights(Writer, Input.HeightHalo);
			WriteNeighbors(Writer, Input.Neighbors, false);
			break;
		case ETerrainTileProductClass::Coverage:
			WriteCoverage(Writer, Input.LayerIds, Input.Coverage);
			WriteCoverage(Writer, Input.LayerIds, Input.CoverageHalo);
			WriteNeighbors(Writer, Input.Neighbors, true);
			break;
		case ETerrainTileProductClass::Query:
			WriteHeights(Writer, Input.Heights);
			WriteHeights(Writer, Input.HeightHalo);
			WriteCoverage(Writer, Input.LayerIds, Input.Coverage);
			WriteCoverage(Writer, Input.LayerIds, Input.CoverageHalo);
			WriteNeighbors(Writer, Input.Neighbors, false);
			WriteNeighbors(Writer, Input.Neighbors, true);
			break;
		}
		OutError.clear();
		return FXxHash128::HashBuffer(Writer.GetBytes()).ToString();
	}

	auto MakeTerrainTileBuildKey(const FTerrainNormalizedTileInput& Input,
		ETerrainTileProductClass ProductClass, std::string& OutError) -> std::string
	{
		ETerrainWorldOutcome Outcome;
		if (!IsValidProductClass(ProductClass)
			|| !ValidateTerrainNormalizedTileInput(Input, Outcome, OutError)) return {};
		return MakeTerrainTileBuildKeyUnchecked(Input, ProductClass, OutError);
	}

	auto EncodeTerrainTileProduct(ETerrainTileProductClass ProductClass,
		const FTerrainTileKey& Tile, const FGuid& GenerationId,
		std::span<const FXxHash128> Dependencies, std::span<const std::byte> Body,
		std::vector<std::byte>& OutBytes, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		if (!IsValidProductClass(ProductClass) || !Tile.WorldId.IsValid()
			|| Tile.SchemeVersion != TerrainWorldTileSchemeVersion || !GenerationId.IsValid())
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain tile product identity is invalid.", OutOutcome, OutError);
		if (Dependencies.size() > TerrainWorldMaximumDependencies)
			return Fail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain tile product dependency count exceeds its ceiling.", OutOutcome, OutError);
		const uint64 TotalBytes = HeaderBytes + Dependencies.size() * 16ull + Body.size();
		if (TotalBytes > ProductCeiling(ProductClass))
			return Fail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain tile product exceeds its class byte ceiling.", OutOutcome, OutError);
		if (!Private::ValidateTerrainWorldProductBody(ProductClass, Body, OutError))
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
				"Terrain tile product body is structurally invalid: " + OutError,
				OutOutcome, OutError);
		const FXxHash128 BodyHash = FXxHash128::HashBuffer(Body);
		FBinaryWriter Writer;
		Writer.WriteU32(ProductMagic(ProductClass));
		Writer.WriteU16(TerrainWorldSchemaVersion);
		Writer.WriteU16(0);
		Writer.WriteU32(RequiredFlags);
		Writer.WriteU32(0);
		WriteTileKey(Writer, Tile);
		Writer.WriteU16(0);
		WriteGuid(Writer, GenerationId);
		Writer.WriteU64(Body.size());
		Writer.WriteU64(Body.size());
		WriteHash(Writer, BodyHash);
		Writer.WriteU32(static_cast<uint32>(Dependencies.size()));
		Writer.WriteU32(0);
		for (const FXxHash128& Dependency : Dependencies) WriteHash(Writer, Dependency);
		Writer.WriteBytes(Body);
		if (Writer.GetBytes().size() != TotalBytes)
			return Fail(ETerrainWorldOutcome::PublicationFailed,
				"Terrain tile product header size is inconsistent.", OutOutcome, OutError);
		OutBytes = Writer.TakeBytes();
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto DecodeTerrainTileProduct(std::span<const std::byte> Bytes,
		ETerrainTileProductClass ExpectedClass, FTerrainTileProduct& OutProduct,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		OutProduct = {};
		if (!IsValidProductClass(ExpectedClass))
			return Fail(ETerrainWorldOutcome::InvalidDefinition,
				"Expected Terrain tile product class is invalid.", OutOutcome, OutError);
		if (Bytes.size() < 6)
			return Fail(ETerrainWorldOutcome::Corrupt,
				"Terrain tile product header is truncated.", OutOutcome, OutError);
		uint32 Magic = 0;
		uint16 Version = 0;
		if (!ReadLittleEndianAt(Bytes, 0, Magic) || !ReadLittleEndianAt(Bytes, 4, Version))
			return Fail(ETerrainWorldOutcome::Corrupt,
				"Terrain tile product header is truncated.", OutOutcome, OutError);
		if (Magic != ProductMagic(ExpectedClass))
			return Fail(ETerrainWorldOutcome::UnsupportedLegacySchema,
				std::format("Terrain tile product magic {:#x} does not match expected {:#x}; legacy Terrain values are never decoded.",
					Magic, ProductMagic(ExpectedClass)), OutOutcome, OutError);
		if (Version != TerrainWorldSchemaVersion)
			return Fail(ETerrainWorldOutcome::Incompatible,
				"Terrain tile product schema version is incompatible.", OutOutcome, OutError);
		if (Bytes.size() > ProductCeiling(ExpectedClass) || Bytes.size() < HeaderBytes)
			return Fail(ETerrainWorldOutcome::BudgetRejected,
				"Terrain tile product byte count is outside its class bound.", OutOutcome, OutError);
		FBinaryReader Reader(Bytes);
		uint32 HeaderMagic = 0, Required = 0, Optional = 0, DependencyCount = 0, Reserved32 = 0;
		uint16 HeaderVersion = 0, Reserved16 = 0, TileReserved = 0;
		FTerrainTileProduct Candidate;
		uint64 LogicalBytes = 0, StoredBytes = 0;
		if (!Reader.ReadU32(HeaderMagic) || !Reader.ReadU16(HeaderVersion)
			|| !Reader.ReadU16(Reserved16) || !Reader.ReadU32(Required)
			|| !Reader.ReadU32(Optional) || !ReadTileKey(Reader, Candidate.Tile)
			|| !Reader.ReadU16(TileReserved) || !ReadGuid(Reader, Candidate.GenerationId)
			|| !Reader.ReadU64(LogicalBytes) || !Reader.ReadU64(StoredBytes)
			|| !ReadHash(Reader, Candidate.BodyHash) || !Reader.ReadU32(DependencyCount)
			|| !Reader.ReadU32(Reserved32))
			return Fail(ETerrainWorldOutcome::Corrupt,
				"Terrain tile product header is truncated.", OutOutcome, OutError);
		if (HeaderMagic != Magic || HeaderVersion != Version || Reserved16 != 0
			|| Required != RequiredFlags || Optional != 0 || TileReserved != 0 || Reserved32 != 0
			|| !Candidate.Tile.WorldId.IsValid() || Candidate.Tile.SchemeVersion != TerrainWorldTileSchemeVersion
			|| !Candidate.GenerationId.IsValid() || DependencyCount > TerrainWorldMaximumDependencies
			|| LogicalBytes != StoredBytes || StoredBytes != Reader.GetRemainingBytes() - DependencyCount * 16ull)
			return Fail(ETerrainWorldOutcome::Corrupt,
				"Terrain tile product header fields are invalid.", OutOutcome, OutError);
		Candidate.Dependencies.resize(DependencyCount);
		for (FXxHash128& Dependency : Candidate.Dependencies)
			if (!ReadHash(Reader, Dependency) || Dependency.IsZero())
				return Fail(ETerrainWorldOutcome::MissingDependency,
					"Terrain tile product dependency is missing or invalid.", OutOutcome, OutError);
		std::span<const std::byte> Body;
		if (!Reader.ReadRegion(Body, StoredBytes, ProductCeiling(ExpectedClass)) || !Reader.IsAtEnd())
			return Fail(ETerrainWorldOutcome::Corrupt,
				"Terrain tile product body is truncated or has trailing bytes.", OutOutcome, OutError);
		if (FXxHash128::HashBuffer(Body) != Candidate.BodyHash)
			return Fail(ETerrainWorldOutcome::Corrupt,
				"Terrain tile product body checksum is invalid.", OutOutcome, OutError);
		if (!Private::ValidateTerrainWorldProductBody(ExpectedClass, Body, OutError))
			return Fail(ETerrainWorldOutcome::Corrupt,
				"Terrain tile product body is structurally invalid: " + OutError,
				OutOutcome, OutError);
		Candidate.ProductClass = ExpectedClass;
		Candidate.Bytes.assign(Bytes.begin(), Bytes.end());
		OutProduct = std::move(Candidate);
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto BuildTerrainTileGeneration(const FTerrainNormalizedTileInput& Input,
		const FGuid& GenerationId, FTerrainTileGeneration& OutGeneration,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool
	{
		if (!GenerationId.IsValid() || !ValidateTerrainNormalizedTileInput(Input, OutOutcome, OutError))
			return false;
		if (!EnsureGeometryBuildFunctions(&OutError))
			return Fail(ETerrainWorldOutcome::PublicationFailed,
				"Terrain World build functions could not be registered: " + OutError,
				OutOutcome, OutError);
		if (Input.ShouldCancel && Input.ShouldCancel())
			return Fail(ETerrainWorldOutcome::Cancelled,
				"Terrain tile generation was cancelled.", OutOutcome, OutError);
		FTerrainTileGeneration Candidate{Input.Tile, GenerationId};
		std::array<std::vector<std::byte>, 5> Bodies;
		Bodies[ProductIndex(ETerrainTileProductClass::Height)] = BuildHeightBody(Input);
		Bodies[ProductIndex(ETerrainTileProductClass::Coverage)] = BuildCoverageBody(Input);
		Bodies[ProductIndex(ETerrainTileProductClass::Collision)] = BuildCollisionBody(Input);
		Bodies[ProductIndex(ETerrainTileProductClass::Query)] = BuildQueryBody(Input);
		std::array<FXxHash128, 5> Hashes{};
		for (size_t Index = 1; Index < Bodies.size(); ++Index)
			Hashes[Index] = FXxHash128::HashBuffer(Bodies[Index]);
		Bodies[0] = BuildMetadataBody(Input);
		Hashes[0] = FXxHash128::HashBuffer(Bodies[0]);
		for (uint8 Value = 1; Value <= 5; ++Value)
		{
			if (Input.ShouldCancel && Input.ShouldCancel())
				return Fail(ETerrainWorldOutcome::Cancelled,
					"Terrain tile generation was cancelled.", OutOutcome, OutError);
			const auto Class = static_cast<ETerrainTileProductClass>(Value);
			const std::string DerivedDataKey = MakeTerrainTileBuildKeyUnchecked(Input, Class, OutError);
			if (DerivedDataKey.empty())
				return Fail(ETerrainWorldOutcome::PublicationFailed,
					"Terrain tile product build key could not be created.", OutOutcome, OutError);
			FBuildDefinition Definition;
			FBuildDefinitionBuilder Builder(Private::GetTerrainWorldBuildFunctionIdentity(Class),
				std::string(Private::GetTerrainWorldBuildValueName(Class)));
			Builder.SetKey(FBuildKey::FromString(DerivedDataKey))
				.AddTargetFact("TerrainProductClass", std::to_string(Value))
				.AddTargetFact("Platform", std::to_string(static_cast<uint32>(Input.TargetPlatform)))
				.AddTargetFact("Profile", std::to_string(static_cast<uint32>(Input.TargetProfile)))
				.AddInput(FBuildValue::FromOwned(std::string(Private::TerrainWorldProductInputName),
					Bodies[ProductIndex(Class)]));
			if (!Builder.Build(Definition, &OutError))
				return Fail(ETerrainWorldOutcome::PublicationFailed,
					"Terrain tile product build definition is invalid: " + OutError,
					OutOutcome, OutError);
			const FBuildCancellationToken Cancellation(Input.ShouldCancel);
			const FBuildOutput Output = FBuildSession().Build(Definition, {
				.bQueryCache = Input.bQueryDerivedData,
				.bAllowLocalBuild = true,
				.bStoreBuildResult = Input.bPersistDerivedData,
				.bRequireStoreSuccess = Input.bPersistDerivedData,
				.bReturnData = true}, Input.ShouldCancel ? &Cancellation : nullptr);
			if (!Output.Succeeded())
			{
				const ETerrainWorldOutcome Outcome = Output.Status == EBuildStatus::Canceled
					? ETerrainWorldOutcome::Cancelled : ETerrainWorldOutcome::PublicationFailed;
				return Fail(Outcome, Output.Diagnostic, OutOutcome, OutError);
			}
			std::vector<FXxHash128> Dependencies;
			if (Class == ETerrainTileProductClass::Metadata
				|| Class == ETerrainTileProductClass::Collision)
				Dependencies.push_back(Hashes[ProductIndex(ETerrainTileProductClass::Height)]);
			if (Class == ETerrainTileProductClass::Query)
			{
				Dependencies.push_back(Hashes[ProductIndex(ETerrainTileProductClass::Height)]);
				Dependencies.push_back(Hashes[ProductIndex(ETerrainTileProductClass::Coverage)]);
			}
			FTerrainTileProduct Product;
			if (!EncodeTerrainTileProduct(Class, Input.Tile, GenerationId, Dependencies,
				Output.Value.GetBytes(), Product.Bytes, OutOutcome, OutError))
				return false;
			FTerrainTileProduct Decoded;
			if (!DecodeTerrainTileProduct(Product.Bytes, Class, Decoded, OutOutcome, OutError))
				return false;
			Product = std::move(Decoded);
			Product.DerivedDataKey = DerivedDataKey;
			Product.Origin = Output.Status == EBuildStatus::CacheHit
				? ETerrainTileBuildOrigin::DerivedData : ETerrainTileBuildOrigin::LocalBuild;
			Candidate.Products[ProductIndex(Class)] = std::move(Product);
		}
		OutGeneration = std::move(Candidate);
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto FTerrainTileGenerationPublisher::BeginRequest() -> uint64
	{
		std::lock_guard Lock(Mutex);
		return ++LatestRequestId;
	}

	auto FTerrainTileGenerationPublisher::Publish(uint64 RequestId,
		FTerrainTileGeneration Candidate, ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool
	{
		if (!ValidateGeneration(Candidate, OutOutcome, OutError)) return false;
		std::lock_guard Lock(Mutex);
		if (RequestId == 0 || RequestId != LatestRequestId)
			return Fail(ETerrainWorldOutcome::Superseded,
				"Terrain tile generation publication was superseded.", OutOutcome, OutError);
		Current = std::make_shared<const FTerrainTileGeneration>(std::move(Candidate));
		OutOutcome = ETerrainWorldOutcome::Ready;
		OutError.clear();
		return true;
	}

	auto FTerrainTileGenerationPublisher::GetCurrent() const
		-> std::shared_ptr<const FTerrainTileGeneration>
	{
		std::lock_guard Lock(Mutex);
		return Current;
	}

	auto FTerrainTileGenerationPublisher::Retire() -> void
	{
		std::lock_guard Lock(Mutex);
		++LatestRequestId;
		Current.reset();
	}
}
