#include <gtest/gtest.h>

#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "Asset/AssetOperations.h"
#include "Asset/PackageSerialization.h"
#include "DObject/Object.h"
#include "Misc/FileHelper.h"
#include "Terrain/TerrainWorldCook.h"
#include "Terrain/TerrainWorldTile.h"
#include "Editor/AssetForgeBuiltins/Private/TerrainWorldBuildAdapter.h"

namespace
{
	using namespace Durin;
	using namespace Durin::Asset;

	auto Id(uint32 Value) -> FGuid
	{
		return {0x01020304u, 0x11121314u, 0x21222324u, Value};
	}

	auto MakeInput() -> FTerrainNormalizedTileInput
	{
		FTerrainNormalizedTileInput Input;
		Input.Tile = {{Id(1)}, -2, 3, TerrainWorldTileSchemeVersion};
		Input.WorldExtent = {{-512, 768}, {-256, 1024}};
		Input.Coordinates = {10.0, 20.0, 30.0, 1.0, -5.0};
		Input.LayerIds = {Id(2)};
		Input.Heights.resize(TerrainWorldSampleCount);
		for (uint32 Y = 0; Y < 257; ++Y)
			for (uint32 X = 0; X < 257; ++X)
				Input.Heights[Y * 257 + X] = static_cast<int16>(-30000 + (X * 97 + Y * 113) % 60000);
		Input.Heights[0] = -32768;
		Input.Heights[256] = -17;
		Input.Heights[256 * 257] = 23;
		Input.Heights.back() = 32767;
		Input.Coverage.resize(TerrainWorldSampleCount);
		for (FTerrainCoverageSample& Sample : Input.Coverage)
		{
			Sample.LayerCount = 1;
			Sample.Layers[0] = {Input.LayerIds[0], 255};
		}
		Input.HeightHalo.resize(259u * 259u, 7);
		Input.CoverageHalo.resize(259u * 259u);
		for (FTerrainCoverageSample& Sample : Input.CoverageHalo)
		{
			Sample.LayerCount = 1;
			Sample.Layers[0] = {Input.LayerIds[0], 255};
		}
		Input.CompositionPolicyId = Id(3);
		Input.CompositionPolicyVersion = 1;
		Input.bQueryDerivedData = false;
		Input.bPersistDerivedData = false;
		return Input;
	}

	class FScopedTerrainWorldCache
	{
	public:
		FScopedTerrainWorldCache()
			: Previous(FPaths::DerivedDataCacheDir())
			, Root(Testing::CreateTestFixtureDirectory("TerrainWorldCache"))
		{
			FPaths::SetDerivedDataCacheDirForTests(Root.generic_string());
		}

		~FScopedTerrainWorldCache()
		{
			FPaths::SetDerivedDataCacheDirForTests(Previous);
			Testing::RemoveTestWorkDirectory(Root);
		}

		auto GetRoot() const -> const std::filesystem::path& { return Root; }

	private:
		std::string Previous;
		std::filesystem::path Root;
	};

	auto MakePackageTemplate() -> std::vector<std::byte>
	{
		Testing::InitializeDObjectSystemForTests();
		const std::filesystem::path Content = Testing::GetTestWorkDirectory() / "TerrainWorldTemplateContent";
		std::filesystem::create_directories(Content);
		Testing::RegisterMountPointForTests(
			"/TerrainWorld/", Content.generic_string() + "/"
		);
		FAssetPath Path;
		requiref(FAssetPath::TryCreate("/TerrainWorld/PackageTemplate", Path), "Terrain World test package path must be valid.");
		DObject* Object = nullptr;
		const Asset::FAssetResult Created = Asset::CreateAsset(Path, Object);
		requiref(Created && Object, "{}", Created.Message);
		std::vector<std::byte> Bytes;
		const Asset::FAssetResult Serialized = Asset::SerializeAssetPackageBytes(
			Object->GetPackage(), Bytes
		);
		requiref(Serialized, "{}", Serialized.Message);
		return Bytes;
	}
} // namespace

TEST(FTerrainWorldBuildTests, FloorDivisionAndInclusiveMaximumMatchGoldenCoordinates)
{
	for (const auto [Global, Tile, Local] : std::array{
			 std::tuple{-257ll, -2ll, 255ll}, std::tuple{-256ll, -1ll, 0ll},
			 std::tuple{-1ll, -1ll, 255ll}, std::tuple{0ll, 0ll, 0ll},
			 std::tuple{255ll, 0ll, 255ll}, std::tuple{256ll, 1ll, 0ll},
			 std::tuple{257ll, 1ll, 1ll}
		 })
	{
		int64 ActualTile = 0, ActualLocal = 0;
		ASSERT_TRUE(TerrainFloorDiv(Global, 256, ActualTile));
		ASSERT_TRUE(TerrainFloorMod(Global, 256, ActualLocal));
		EXPECT_EQ(ActualTile, Tile);
		EXPECT_EQ(ActualLocal, Local);
	}

	ETerrainWorldOutcome Outcome{};
	std::string Error;
	FTerrainTileAddress Address;
	ASSERT_TRUE(ResolveTerrainSampleAddress({Id(1)}, {{-513, 769}, {259, 1282}}, {259, 1282}, Address, Outcome, Error)) << Error;
	EXPECT_EQ(Address.Tile.TileX, 1);
	EXPECT_EQ(Address.Tile.TileY, 5);
	EXPECT_EQ(Address.Local.X, 3);
	EXPECT_EQ(Address.Local.Y, 2);
	EXPECT_FALSE(TerrainFloorDiv(std::numeric_limits<int64>::min(), -1, Address.Tile.TileX));
}

TEST(FTerrainWorldBuildTests, DefinitionRejectsContractBoundaryAndNumericViolations)
{
	FTerrainWorldDefinition Definition;
	Definition.WorldId = {Id(1)};
	Definition.SampleExtent = {{-513, 769}, {259, 1282}};
	Definition.BuildPolicyId = Id(2);
	Definition.BuildPolicyVersion = 1;
	Definition.ProductProfile = 1;
	Definition.PeakBuildBudgetBytes = 2ull * 1024ull * 1024ull * 1024ull;
	ETerrainWorldOutcome Outcome{};
	std::string Error;
	ASSERT_TRUE(ValidateTerrainWorldDefinition(Definition, Outcome, Error)) << Error;
	Definition.SampleExtent.Max.X = Definition.SampleExtent.Min.X;
	EXPECT_FALSE(ValidateTerrainWorldDefinition(Definition, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::InvalidDefinition);
	Definition.SampleExtent.Max.X = Definition.SampleExtent.Min.X + (1ll << 31) + 1;
	EXPECT_FALSE(ValidateTerrainWorldDefinition(Definition, Outcome, Error));
	Definition.SampleExtent.Max.X = 259;
	Definition.Coordinates.SampleSpacingMeters = std::numeric_limits<double>::quiet_NaN();
	EXPECT_FALSE(ValidateTerrainWorldDefinition(Definition, Outcome, Error));
	Definition.Coordinates.SampleSpacingMeters = 1.0;
	Definition.Sources.resize(TerrainWorldMaximumSources + 1);
	EXPECT_FALSE(ValidateTerrainWorldDefinition(Definition, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::BudgetRejected);
	int16 Height = 0;
	EXPECT_TRUE(NormalizeTerrainHeightQuantum(-32768, Height, Outcome, Error));
	EXPECT_EQ(Height, -32768);
	EXPECT_FALSE(NormalizeTerrainHeightQuantum(-32769, Height, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::Overflow);
	EXPECT_FALSE(NormalizeTerrainHeightQuantum(32768, Height, Outcome, Error));
	std::array<double, 3> Position{};
	EXPECT_FALSE(TerrainSampleToWorldPosition({0, 0, 0, 2.0, 0}, {1ll << 40, 0}, 0, Position, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::Overflow);
	EXPECT_FALSE(TerrainSampleToWorldPosition(
		{std::numeric_limits<double>::infinity(), 0, 0, 1.0, 0},
		{0, 0}, 0, Position, Outcome, Error
	));
}

TEST(FTerrainWorldBuildTests, AuthoredValuesNormalizeToImmutableBoundedWorkerInput)
{
	FTerrainWorldDefinition Definition;
	Definition.WorldId = {Id(1)};
	Definition.SampleExtent = {{-512, 768}, {-256, 1024}};
	Definition.BuildPolicyId = Id(3);
	Definition.BuildPolicyVersion = 1;
	Definition.ProductProfile = 1;
	Definition.PeakBuildBudgetBytes = 2ull * 1024ull * 1024ull * 1024ull;
	Definition.Layers = {{Id(2), "Base", Id(20)}};
	Definition.Sources = {
		{Id(4), {4, 5}, {{-1024, 0}, {-768, 256}}, 1, 255, true},
		{Id(5), {6, 7}, {{-512, 768}, {-256, 1024}}, 1, 255, true}
	};
	FTerrainNormalizedTileInput Fixture = MakeInput();
	FTerrainComposedTileValues Values{Fixture.Heights, Fixture.Coverage, Fixture.HeightHalo, Fixture.CoverageHalo, Fixture.Neighbors};
	FTerrainNormalizedTileInput Normalized;
	ETerrainWorldOutcome Outcome{};
	std::string Error;
	ASSERT_TRUE(NormalizeTerrainTileInput(Definition, -2, 3, Values, Normalized, Outcome, Error)) << Error;
	ASSERT_EQ(Normalized.Sources.size(), 1u);
	EXPECT_EQ(Normalized.Sources[0].SourceId, Id(5));
	EXPECT_EQ(Normalized.LayerIds, std::vector{Id(2)});
	EXPECT_LT(EstimateTerrainTileBuildBytes(Normalized), 768ull * 1024ull * 1024ull);
	Values.Heights[0] = 0;
	EXPECT_EQ(Normalized.Heights[0], -32768);
	EXPECT_FALSE(Normalized.ShouldCancel);
}

TEST(FTerrainWorldBuildTests, OrderedHeightAndCoverageSourcesComposeDeterministically)
{
	FTerrainWorldDefinition Definition;
	Definition.WorldId = {Id(1)};
	Definition.SampleExtent = {{-512, 768}, {-256, 1024}};
	Definition.BuildPolicyId = Id(3);
	Definition.BuildPolicyVersion = 1;
	Definition.ProductProfile = 1;
	Definition.PeakBuildBudgetBytes = 2ull * 1024ull * 1024ull * 1024ull;
	Definition.Layers = {{Id(2), "Base", Id(20)}};
	Definition.Sources = {
		{Id(4), {4, 5}, Definition.SampleExtent, static_cast<uint8>(ETerrainCompositionBlendOperation::Replace), 255, true, TerrainSourceAffectsHeight | TerrainSourceAffectsCoverage},
		{Id(5), {6, 7}, Definition.SampleExtent, static_cast<uint8>(ETerrainCompositionBlendOperation::Add), 128, true, TerrainSourceAffectsHeight}
	};
	FTerrainTileSourceContribution Base{Id(4), {4, 5}, std::vector<int16>(TerrainWorldSampleCount, 100), std::vector<FTerrainCoverageSample>(TerrainWorldSampleCount)};
	for (FTerrainCoverageSample& Sample : Base.Coverage)
	{
		Sample.LayerCount = 1;
		Sample.Layers[0] = {Id(2), 255};
	}
	FTerrainTileSourceContribution Add{Id(5), {6, 7}, std::vector<int16>(TerrainWorldSampleCount, 20), {}};
	std::array Contributions{std::move(Base), std::move(Add)};
	FTerrainNormalizedTileInput Composed;
	ETerrainWorldOutcome Outcome{};
	std::string Error;
	ASSERT_TRUE(ComposeTerrainTileInput(Definition, -2, 3, Contributions, Composed, Outcome, Error)) << Error;
	EXPECT_EQ(Composed.Heights.front(), 110);
	EXPECT_EQ(Composed.Heights.back(), 110);
	EXPECT_EQ(Composed.Coverage.front().Layers[0].LayerId, Id(2));
	std::ranges::swap(Contributions[0], Contributions[1]);
	EXPECT_FALSE(ComposeTerrainTileInput(Definition, -2, 3, Contributions, Composed, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::MissingDependency);
}

TEST(FTerrainWorldBuildTests, AsymmetricTileBuildIsDeterministicAndEveryProductBodyRoundTrips)
{
	const FTerrainNormalizedTileInput Input = MakeInput();
	ETerrainWorldOutcome Outcome{};
	std::string Error;
	auto Build = [&Input] {
		FTerrainTileGeneration Generation;
		ETerrainWorldOutcome LocalOutcome{};
		std::string LocalError;
		const bool bSucceeded = BuildTerrainTileGeneration(
			Input, Id(9), Generation, LocalOutcome, LocalError
		);
		return std::tuple{bSucceeded, std::move(Generation), LocalOutcome, std::move(LocalError)};
	};
	auto FirstTask = std::async(std::launch::async, Build);
	auto SecondTask = std::async(std::launch::async, Build);
	auto [FirstSucceeded, First, FirstOutcome, FirstError] = FirstTask.get();
	auto [SecondSucceeded, Second, SecondOutcome, SecondError] = SecondTask.get();
	ASSERT_TRUE(FirstSucceeded) << FirstError;
	ASSERT_TRUE(SecondSucceeded) << SecondError;
	for (size_t Index = 0; Index < First.Products.size(); ++Index)
	{
		const FTerrainTileProduct& Product = First.Products[Index];
		EXPECT_EQ(Product.Bytes, Second.Products[Index].Bytes);
		EXPECT_EQ(Product.DerivedDataKey, Second.Products[Index].DerivedDataKey);
		const std::array ProductMagic{std::byte{'T'}, std::byte{'W'}, std::byte{'P'}, std::byte{'D'}};
		EXPECT_TRUE(std::equal(Product.Bytes.begin(), Product.Bytes.begin() + 4, ProductMagic.begin()));
		EXPECT_TRUE(std::equal(Product.Bytes.begin(), Product.Bytes.begin() + 4, First.Products[0].Bytes.begin()));
		EXPECT_EQ(std::to_integer<uint8>(Product.Bytes[6]), static_cast<uint8>(Product.ProductClass));
		EXPECT_EQ(Product.Bytes[7], std::byte{0});
		FTerrainTileProduct Decoded;
		ASSERT_TRUE(DecodeTerrainTileProduct(Product.Bytes, Product.ProductClass, Decoded, Outcome, Error)) << Error;
		EXPECT_EQ(Decoded.Tile, Input.Tile);
		EXPECT_EQ(Decoded.GenerationId, Id(9));
		EXPECT_EQ(Decoded.BodyHash, Product.BodyHash);
	}
	EXPECT_LE(First.Products[0].Bytes.size(), 16u * 1024u);
	EXPECT_LE(First.Products[1].Bytes.size(), 160u * 1024u);
	EXPECT_LE(First.Products[2].Bytes.size(), 320u * 1024u);
	EXPECT_LE(First.Products[3].Bytes.size(), 96u * 1024u);
	EXPECT_LE(First.Products[4].Bytes.size(), 160u * 1024u);
}

TEST(FTerrainWorldBuildTests, EnvelopeRejectsLegacyClassMismatchCorruptionTrailingAndOversizedInput)
{
	const FTerrainNormalizedTileInput Input = MakeInput();
	ETerrainWorldOutcome Outcome{};
	std::string Error;
	FTerrainTileGeneration Generation;
	ASSERT_TRUE(BuildTerrainTileGeneration(Input, Id(9), Generation, Outcome, Error)) << Error;
	std::vector<std::byte> Bytes = Generation.Products[1].Bytes;
	FTerrainTileProduct Product;
	Bytes[0] = std::byte{'T'};
	Bytes[1] = std::byte{'W'};
	Bytes[2] = std::byte{'H'};
	Bytes[3] = std::byte{'T'};
	EXPECT_FALSE(DecodeTerrainTileProduct(Bytes, ETerrainTileProductClass::Height, Product, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::UnsupportedLegacySchema);
	Bytes = Generation.Products[1].Bytes;
	Bytes[6] = static_cast<std::byte>(
		static_cast<uint8>(ETerrainTileProductClass::Coverage)
	);
	EXPECT_FALSE(DecodeTerrainTileProduct(Bytes, ETerrainTileProductClass::Height, Product, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::Corrupt);
	Bytes = Generation.Products[1].Bytes;
	Bytes.back() ^= std::byte{1};
	EXPECT_FALSE(DecodeTerrainTileProduct(Bytes, ETerrainTileProductClass::Height, Product, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::Corrupt);
	Bytes = Generation.Products[1].Bytes;
	Bytes.push_back(std::byte{0});
	EXPECT_FALSE(DecodeTerrainTileProduct(Bytes, ETerrainTileProductClass::Height, Product, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::Corrupt);
	Bytes.resize(160u * 1024u + 1u);
	EXPECT_FALSE(DecodeTerrainTileProduct(Bytes, ETerrainTileProductClass::Height, Product, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::BudgetRejected);
	std::vector<std::byte> Encoded;
	EXPECT_FALSE(EncodeTerrainTileProduct(ETerrainTileProductClass::Height, Input.Tile, Id(9), {}, std::array{std::byte{0}}, Encoded, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::InvalidDefinition);
}

TEST(FTerrainWorldBuildTests, ProductKeysInvalidateOnlyDeclaredHeightCoverageAndNeighborInputs)
{
	FTerrainNormalizedTileInput Input = MakeInput();
	std::string Error;
	const std::string Height = MakeTerrainTileBuildKey(Input, ETerrainTileProductClass::Height, Error);
	const std::string Coverage = MakeTerrainTileBuildKey(Input, ETerrainTileProductClass::Coverage, Error);
	Input.LayerIds.push_back(Id(4));
	for (FTerrainCoverageSample& Sample : Input.Coverage)
	{
		Sample.LayerCount = 2;
		Sample.Layers[0].Weight = 128;
		Sample.Layers[1] = {Id(4), 127};
	}
	Input.Coverage[1].Layers[0].Weight = 127;
	Input.Coverage[1].Layers[1].Weight = 128;
	EXPECT_EQ(MakeTerrainTileBuildKey(Input, ETerrainTileProductClass::Height, Error), Height);
	EXPECT_NE(MakeTerrainTileBuildKey(Input, ETerrainTileProductClass::Coverage, Error), Coverage);
	Input.Heights[1] += 1;
	EXPECT_NE(MakeTerrainTileBuildKey(Input, ETerrainTileProductClass::Height, Error), Height);
	const std::string CoverageAfterEdit = MakeTerrainTileBuildKey(Input, ETerrainTileProductClass::Coverage, Error);
	Input.Heights[2] += 1;
	EXPECT_EQ(MakeTerrainTileBuildKey(Input, ETerrainTileProductClass::Coverage, Error), CoverageAfterEdit);
	FTerrainNormalizedTileInput SourceInput = MakeInput();
	SourceInput.Sources.push_back({Id(7), {8, 9}, SourceInput.WorldExtent, static_cast<uint8>(ETerrainCompositionBlendOperation::Replace), 255, true, TerrainSourceAffectsHeight});
	const std::string SourceHeight = MakeTerrainTileBuildKey(
		SourceInput, ETerrainTileProductClass::Height, Error
	);
	const std::string SourceCoverage = MakeTerrainTileBuildKey(
		SourceInput, ETerrainTileProductClass::Coverage, Error
	);
	SourceInput.Sources[0].ContentHash.HashLow += 1;
	EXPECT_NE(MakeTerrainTileBuildKey(SourceInput, ETerrainTileProductClass::Height, Error), SourceHeight);
	EXPECT_EQ(MakeTerrainTileBuildKey(SourceInput, ETerrainTileProductClass::Coverage, Error), SourceCoverage);
}

TEST(FTerrainWorldBuildTests, FiveIndependentProductsUseValidatedColdAndWarmDerivedData)
{
	FScopedTerrainWorldCache Cache;
	FTerrainNormalizedTileInput Input = MakeInput();
	Input.bQueryDerivedData = true;
	Input.bPersistDerivedData = true;
	ETerrainWorldOutcome Outcome{};
	std::string Error;
	FTerrainTileGeneration Cold;
	FTerrainTileGeneration Warm;
	ASSERT_TRUE(BuildTerrainTileGeneration(Input, Id(9), Cold, Outcome, Error)) << Error;
	ASSERT_TRUE(BuildTerrainTileGeneration(Input, Id(9), Warm, Outcome, Error)) << Error;
	for (size_t Index = 0; Index < Cold.Products.size(); ++Index)
	{
		EXPECT_EQ(Cold.Products[Index].Origin, ETerrainTileBuildOrigin::LocalBuild);
		EXPECT_EQ(Warm.Products[Index].Origin, ETerrainTileBuildOrigin::DerivedData);
		EXPECT_EQ(Cold.Products[Index].DerivedDataKey, Warm.Products[Index].DerivedDataKey);
		EXPECT_EQ(Cold.Products[Index].Bytes, Warm.Products[Index].Bytes);
	}
	const std::string& HeightKey = Cold.Products[1].DerivedDataKey;
	const std::filesystem::path HeightObject = Cache.GetRoot()
											   / "TerrainWorld/TerrainWorldHeight/Objects" / HeightKey.substr(0, 2)
											   / (HeightKey + ".bin");
	std::vector<std::byte> Corrupt;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Corrupt, HeightObject));
	Corrupt.back() ^= std::byte{1};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Corrupt, HeightObject));
	FTerrainTileGeneration Recovered;
	ASSERT_TRUE(BuildTerrainTileGeneration(Input, Id(9), Recovered, Outcome, Error)) << Error;
	EXPECT_EQ(Recovered.Products[1].Origin, ETerrainTileBuildOrigin::LocalBuild);
	for (size_t Index : {0u, 2u, 3u, 4u})
		EXPECT_EQ(Recovered.Products[Index].Origin, ETerrainTileBuildOrigin::DerivedData);
}

TEST(FTerrainWorldBuildTests, NeighborEvidenceRequiresBitIdenticalStableHeightAndCoverageBorders)
{
	FTerrainNormalizedTileInput West = MakeInput();
	West.WorldExtent.Max.X += 256;
	FTerrainNormalizedTileInput East = West;
	East.Tile.TileX += 1;
	for (uint32 Y = 0; Y <= 256; ++Y)
	{
		East.Heights[Y * 257] = West.Heights[Y * 257 + 256];
		East.Coverage[Y * 257] = West.Coverage[Y * 257 + 256];
	}
	FTerrainNeighborEvidence Evidence;
	ETerrainWorldOutcome Outcome{};
	std::string Error;
	ASSERT_TRUE(BuildTerrainNeighborEvidence(West, East, Evidence, Outcome, Error)) << Error;
	EXPECT_TRUE(Evidence.bPresent);
	EXPECT_EQ(Evidence.Tile, East.Tile);
	EXPECT_FALSE(Evidence.HeightEdgeHash.IsZero());
	EXPECT_FALSE(Evidence.CoverageEdgeHash.IsZero());
	East.Heights[7 * 257] += 1;
	EXPECT_FALSE(BuildTerrainNeighborEvidence(West, East, Evidence, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::BorderMismatch);
}

TEST(FTerrainWorldBuildTests, AtomicPublisherRejectsStaleOrPartialCandidatesAndRetainsPriorGeneration)
{
	const FTerrainNormalizedTileInput Input = MakeInput();
	ETerrainWorldOutcome Outcome{};
	std::string Error;
	FTerrainTileGeneration First;
	FTerrainTileGeneration Second;
	ASSERT_TRUE(BuildTerrainTileGeneration(Input, Id(9), First, Outcome, Error)) << Error;
	ASSERT_TRUE(BuildTerrainTileGeneration(Input, Id(10), Second, Outcome, Error)) << Error;
	FTerrainTileGenerationPublisher Publisher;
	const uint64 FirstRequest = Publisher.BeginRequest();
	ASSERT_TRUE(Publisher.Publish(FirstRequest, First, Outcome, Error)) << Error;
	ASSERT_EQ(Publisher.GetCurrent()->GenerationId, Id(9));
	const uint64 InvalidRequest = Publisher.BeginRequest();
	FTerrainTileGeneration ValidSecond = Second;
	Second.Products[4].Bytes.pop_back();
	EXPECT_FALSE(Publisher.Publish(InvalidRequest, Second, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::PublicationFailed);
	ASSERT_EQ(Publisher.GetCurrent()->GenerationId, Id(9));
	const uint64 StaleRequest = Publisher.BeginRequest();
	const uint64 LatestRequest = Publisher.BeginRequest();
	EXPECT_FALSE(Publisher.Publish(StaleRequest, ValidSecond, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::Superseded);
	EXPECT_TRUE(Publisher.Publish(LatestRequest, std::move(ValidSecond), Outcome, Error)) << Error;
	ASSERT_EQ(Publisher.GetCurrent()->GenerationId, Id(10));
	Publisher.Retire();
	EXPECT_EQ(Publisher.GetCurrent(), nullptr);
}

TEST(FTerrainWorldBuildTests, AssetForgeBridgeNormalizesBuildsAndPublishesWithConservedDiagnostics)
{
	FTerrainWorldDefinition Definition;
	Definition.WorldId = {Id(1)};
	Definition.SampleExtent = {{-512, 768}, {-256, 1024}};
	Definition.BuildPolicyId = Id(3);
	Definition.BuildPolicyVersion = 1;
	Definition.ProductProfile = 1;
	Definition.PeakBuildBudgetBytes = 2ull * 1024ull * 1024ull * 1024ull;
	Definition.Layers = {{Id(2), "Base", Id(20)}};
	FTerrainNormalizedTileInput Fixture = MakeInput();
	FTerrainComposedTileValues Values{Fixture.Heights, Fixture.Coverage, Fixture.HeightHalo, Fixture.CoverageHalo, Fixture.Neighbors};
	FTerrainTileGenerationPublisher Publisher;
	Durin::AssetForge::Builtins::FTerrainWorldBuildDiagnostics Diagnostics;
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::BuildAndPublishTerrainWorldTile(Definition, -2, 3, Values, Id(9), Publisher, Diagnostics, Error)) << Error;
	EXPECT_EQ(Diagnostics.Outcome, ETerrainWorldOutcome::Ready);
	EXPECT_EQ(Diagnostics.LocalProductCount + Diagnostics.CachedProductCount, 5u);
	EXPECT_GT(Diagnostics.ProductBytes, 0u);
	EXPECT_LT(Diagnostics.PeakTaskBytes, 768ull * 1024ull * 1024ull);
	ASSERT_NE(Publisher.GetCurrent(), nullptr);
	EXPECT_EQ(Publisher.GetCurrent()->GenerationId, Id(9));
}

TEST(FTerrainWorldBuildTests, CancellationHasOneTerminalAndDoesNotPublishCandidate)
{
	FTerrainNormalizedTileInput Input = MakeInput();
	Input.ShouldCancel = [] { return true; };
	FTerrainTileGeneration Candidate;
	Candidate.GenerationId = Id(77);
	ETerrainWorldOutcome Outcome{};
	std::string Error;
	EXPECT_FALSE(BuildTerrainTileGeneration(Input, Id(9), Candidate, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::Cancelled);
	EXPECT_EQ(Candidate.GenerationId, Id(77));
}

TEST(FTerrainWorldBuildTests, ManifestRoundTripsSignedRegionsAndRejectsLegacyOrCorruptRecords)
{
	FTerrainWorldManifest Manifest;
	Manifest.WorldId = {Id(1)};
	Manifest.Regions = {
		{{Manifest.WorldId, -2, 3, 1}, false, "/Game/Terrain/Regions/N2_P3", {}},
		{{Manifest.WorldId, 1, 4, 1}, false, "/Game/Terrain/Regions/P1_P4", {}}
	};
	ETerrainWorldOutcome Outcome{};
	std::string Error;
	std::vector<std::byte> Bytes;
	ASSERT_TRUE(EncodeTerrainWorldManifest(Manifest, Bytes, Outcome, Error)) << Error;
	FTerrainWorldManifest Decoded;
	ASSERT_TRUE(DecodeTerrainWorldManifest(Bytes, Manifest.WorldId, Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game, Decoded, Outcome, Error)) << Error;
	ASSERT_EQ(Decoded.Regions.size(), 2u);
	EXPECT_EQ(Decoded.Regions[0].Region.RegionX, -2);
	EXPECT_EQ(Decoded.Regions[0].Region.RegionY, 3);
	std::vector<std::byte> Invalid = Bytes;
	Invalid[0] = static_cast<std::byte>('D');
	EXPECT_FALSE(DecodeTerrainWorldManifest(Invalid, Manifest.WorldId, Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game, Decoded, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::UnsupportedLegacySchema);
	Invalid = Bytes;
	Invalid.back() ^= std::byte{1};
	EXPECT_FALSE(DecodeTerrainWorldManifest(Invalid, Manifest.WorldId, Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game, Decoded, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::Corrupt);
}

TEST(FTerrainWorldBuildTests, F0TileGridReconcilesToFourRegionsPlusOneManifest)
{
	std::set<std::pair<int64, int64>> Regions;
	ETerrainWorldOutcome Outcome{};
	std::string Error;
	for (int64 TileY = 0; TileY < 16; ++TileY)
		for (int64 TileX = 0; TileX < 16; ++TileX)
		{
			FTerrainRegionKey Region;
			ASSERT_TRUE(GetTerrainRegionKey({{Id(1)}, TileX, TileY, 1}, Region, Outcome, Error)) << Error;
			Regions.emplace(Region.RegionX, Region.RegionY);
		}
	EXPECT_EQ(Regions.size(), 4u);
	EXPECT_EQ(Regions.size() + 1u, 5u);
	FTerrainRegionKey Negative;
	ASSERT_TRUE(GetTerrainRegionKey({{Id(1)}, -1, -8, 1}, Negative, Outcome, Error)) << Error;
	EXPECT_EQ(Negative.RegionX, -1);
	EXPECT_EQ(Negative.RegionY, -1);
}

TEST(FTerrainWorldBuildTests, CookSupportsPartialInstallAndSourceAndDdcFreeProductLoading)
{
	FTerrainNormalizedTileInput FirstInput = MakeInput();
	FirstInput.WorldExtent = {{-512, 768}, {2304, 1024}};
	ETerrainWorldOutcome Outcome{};
	std::string Error;
	FTerrainTileGeneration First;
	ASSERT_TRUE(BuildTerrainTileGeneration(FirstInput, Id(9), First, Outcome, Error)) << Error;
	FTerrainTileGeneration Second = First;
	Second.Tile.TileX = 8;
	Second.GenerationId = Id(10);
	FTerrainRegionKey Installed;
	ASSERT_TRUE(GetTerrainRegionKey(First.Tile, Installed, Outcome, Error)) << Error;
	const std::filesystem::path Root = Testing::CreateTestFixtureDirectory("TerrainWorldCook");
	const std::filesystem::path CookRoot = std::filesystem::absolute(Root / "Cooked");
	FTerrainWorldManifest CookedManifest;
	const FTerrainWorldCookRequest CookRequest{CookRoot, "/Game/TerrainWorld", {Id(1)}, {First, Second}, {Installed}, MakePackageTemplate()};
	Asset::FCookContext Cook(CookRoot, Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game);
	ASSERT_TRUE(ContributeTerrainWorldToCook(
		CookRequest, Cook, CookedManifest, Outcome, Error
	)) << Error;
	FAssetPath PackageTemplatePath;
	ASSERT_TRUE(FAssetPath::TryCreate(
		"/TerrainWorld/PackageTemplate", PackageTemplatePath));
	ASSERT_TRUE(Cook.AddPackage(
		"/Game/Metadata", PackageTemplatePath,
		CookRequest.PackageTemplateBytes, &Error)) << Error;
	ASSERT_TRUE(Cook.Publish(&Error)) << Error;
	ASSERT_EQ(CookedManifest.Regions.size(), 2u);
	EXPECT_TRUE(CookedManifest.Regions[0].bInstalled);
	EXPECT_FALSE(CookedManifest.Regions[1].bInstalled);

	Asset::FAssetRuntimeConfiguration Runtime = Asset::FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(Asset::FAssetRuntimeConfiguration::Cooked(CookRoot, Runtime));
	std::shared_ptr<const FTerrainWorldManifest> LoadedManifest;
	ASSERT_TRUE(LoadCookedTerrainWorldManifest(Runtime, "/Game/TerrainWorld", {Id(1)}, Asset::ECookTargetPlatform::Win64, Asset::ECookTargetProfile::Game, LoadedManifest, Outcome, Error)) << Error;
	FTerrainCookedProductHandle Height;
	ASSERT_TRUE(LoadCookedTerrainProduct(Runtime, LoadedManifest, First.Tile, Id(9), ETerrainTileProductClass::Height, Height, Outcome, Error)) << Error;
	EXPECT_EQ(std::vector<std::byte>(Height.GetBytes().begin(), Height.GetBytes().end()), First.Products[1].Bytes);
	FTerrainCookedProductHandle Missing;
	EXPECT_FALSE(LoadCookedTerrainProduct(Runtime, LoadedManifest, Second.Tile, Id(10), ETerrainTileProductClass::Height, Missing, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::Unavailable);

	const FTerrainManifestRegion& InstalledRecord = LoadedManifest->Regions[0];
	std::filesystem::path RegionPackage;
	ASSERT_TRUE(Asset::ResolveCookedPackagePath(CookRoot, InstalledRecord.VirtualPackagePath, RegionPackage, &Error)) << Error;
	std::filesystem::path RegionBulk;
	ASSERT_TRUE(Asset::ResolveCookedCompanionPath(CookRoot, RegionPackage, RegionBulk, &Error)) << Error;
	std::vector<std::byte> Corrupt;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Corrupt, RegionBulk));
	Corrupt.back() ^= std::byte{1};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Corrupt, RegionBulk));
	Height = {};
	EXPECT_FALSE(LoadCookedTerrainProduct(Runtime, LoadedManifest, First.Tile, Id(9), ETerrainTileProductClass::Height, Height, Outcome, Error));
	EXPECT_EQ(Outcome, ETerrainWorldOutcome::Corrupt);
	Testing::RemoveTestWorkDirectory(Root);
}
