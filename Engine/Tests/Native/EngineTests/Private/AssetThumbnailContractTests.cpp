#include <gtest/gtest.h>

#include "EngineTestSupport.h"
#include "Misc/Paths.h"
#include "Thumbnail/AssetThumbnail.h"

namespace Durin
{
	namespace
	{
		auto MakePath(std::string_view Value) -> FAssetPath
		{
			InitializeDObjectSystem();
			static const bool bMountRegistered = [] {
				const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "AssetThumbnailContracts";
				std::filesystem::create_directories(Root);
				PathUtilities::RegisterMountPoint("/ThumbnailTests/", Root.generic_string() + "/");
				return true;
			}();
			(void)bMountRegistered;
			FAssetPath Path;
			EXPECT_TRUE(FAssetPath::TryCreate(Value, Path));
			return Path;
		}

		auto MakePackage(
			std::string_view Path,
			std::string AssetClassName,
			uint32 FormatVersion,
			uint64 FileSize,
			int64 LastWriteTimeTicks) -> FAssetThumbnailPackageFingerprint
		{
			return {
				.VirtualPath = MakePath(Path),
				.AssetClassName = std::move(AssetClassName),
				.PackageFormatVersion = FormatVersion,
				.FileSize = FileSize,
				.LastWriteTimeTicks = LastWriteTimeTicks,
			};
		}

		auto MakeMaterialKeyInput() -> FAssetThumbnailKeyInput
		{
			return {
				.Asset = MakePackage("/ThumbnailTests/Materials/Preview", "DMaterial", 7, 4096, 100),
				.ProviderName = "Durin.MaterialThumbnail",
				.GeneratorSchemaVersion = FRenderedAssetThumbnailVisualContract::SchemaVersion,
				.Output = {},
				.PreviewFixtureIdentity = std::string(FRenderedAssetThumbnailVisualContract::SphereVirtualPath),
				.PreviewFixtureVersion = FRenderedAssetThumbnailVisualContract::SphereFixtureVersion,
				.ShaderContractVersion = 1,
				.Dependencies = {
					MakePackage("/ThumbnailTests/Textures/BaseColor", "DTexture2D", 5, 1024, 200),
					MakePackage("/ThumbnailTests/Materials/Parent", "DMaterial", 7, 2048, 150),
				},
			};
		}
	} // namespace

	TEST(FAssetThumbnailContractTests, IdenticalInputsProduceIdenticalKeys)
	{
		const FAssetThumbnailKeyInput Input = MakeMaterialKeyInput();
		EXPECT_EQ(BuildAssetThumbnailCacheKey(Input), BuildAssetThumbnailCacheKey(Input));
	}

	TEST(FAssetThumbnailContractTests, DependencyOrderDoesNotAffectKeys)
	{
		FAssetThumbnailKeyInput Forward = MakeMaterialKeyInput();
		FAssetThumbnailKeyInput Reverse = Forward;
		std::ranges::reverse(Reverse.Dependencies);
		EXPECT_EQ(BuildAssetThumbnailCacheKey(Forward), BuildAssetThumbnailCacheKey(Reverse));
	}

	TEST(FAssetThumbnailContractTests, EveryKeyContractFieldInvalidatesTheKey)
	{
		const FAssetThumbnailKeyInput Base = MakeMaterialKeyInput();
		const std::string BaseKey = BuildAssetThumbnailCacheKey(Base);
		auto ExpectChanged = [&](auto Mutate) {
			FAssetThumbnailKeyInput Changed = Base;
			Mutate(Changed);
			EXPECT_NE(BuildAssetThumbnailCacheKey(Changed), BaseKey);
		};

		ExpectChanged([](auto& Value) { Value.Asset.VirtualPath = MakePath("/ThumbnailTests/Materials/Renamed"); });
		ExpectChanged([](auto& Value) { ++Value.Asset.PackageFormatVersion; });
		ExpectChanged([](auto& Value) { ++Value.Asset.FileSize; });
		ExpectChanged([](auto& Value) { ++Value.Asset.LastWriteTimeTicks; });
		ExpectChanged([](auto& Value) { Value.Asset.AssetClassName = "DMaterialInstance"; });
		ExpectChanged([](auto& Value) { Value.ProviderName += ".Changed"; });
		ExpectChanged([](auto& Value) { ++Value.GeneratorSchemaVersion; });
		ExpectChanged([](auto& Value) { ++Value.Output.Width; });
		ExpectChanged([](auto& Value) { ++Value.Output.Height; });
		ExpectChanged([](auto& Value) { ++Value.Output.ColorSpaceVersion; });
		ExpectChanged([](auto& Value) { ++Value.Output.EncodingVersion; });
		ExpectChanged([](auto& Value) { Value.PreviewFixtureIdentity += ".Changed"; });
		ExpectChanged([](auto& Value) { ++Value.PreviewFixtureVersion; });
		ExpectChanged([](auto& Value) { ++Value.ShaderContractVersion; });
		ExpectChanged([](auto& Value) { ++Value.Dependencies.front().LastWriteTimeTicks; });
	}

	TEST(FAssetThumbnailContractTests, DependencyClosureIsSortedAndCycleGuarded)
	{
		const FAssetPath Root = MakePath("/ThumbnailTests/Materials/Instance");
		std::vector<FAssetThumbnailDependencyNode> Forward = {
			{MakePackage("/ThumbnailTests/Materials/Instance", "DMaterialInstance", 7, 100, 10),
				{MakePath("/ThumbnailTests/Textures/BaseColor"), MakePath("/ThumbnailTests/Materials/Parent")}},
			{MakePackage("/ThumbnailTests/Materials/Parent", "DMaterial", 7, 200, 20),
				{MakePath("/ThumbnailTests/Materials/Instance"), MakePath("/ThumbnailTests/Textures/Normal")}},
			{MakePackage("/ThumbnailTests/Textures/BaseColor", "DTexture2D", 5, 300, 30), {}},
			{MakePackage("/ThumbnailTests/Textures/Normal", "DTexture2D", 5, 400, 40),
				{MakePath("/ThumbnailTests/Materials/Parent")}},
		};
		std::vector<FAssetThumbnailPackageFingerprint> ForwardClosure;
		std::string Error;
		ASSERT_TRUE(BuildAssetThumbnailDependencyClosure(Root, Forward, ForwardClosure, Error)) << Error;
		ASSERT_EQ(ForwardClosure.size(), 3u);
		EXPECT_EQ(ForwardClosure[0].VirtualPath.GetView(), "/ThumbnailTests/Materials/Parent");
		EXPECT_EQ(ForwardClosure[1].VirtualPath.GetView(), "/ThumbnailTests/Textures/BaseColor");
		EXPECT_EQ(ForwardClosure[2].VirtualPath.GetView(), "/ThumbnailTests/Textures/Normal");

		std::ranges::reverse(Forward);
		for (FAssetThumbnailDependencyNode& Node : Forward)
			std::ranges::reverse(Node.Dependencies);
		std::vector<FAssetThumbnailPackageFingerprint> ReverseClosure;
		ASSERT_TRUE(BuildAssetThumbnailDependencyClosure(Root, Forward, ReverseClosure, Error)) << Error;
		EXPECT_EQ(ReverseClosure, ForwardClosure);
	}

	TEST(FAssetThumbnailContractTests, MissingDependenciesCannotProduceTrustedClosure)
	{
		const FAssetPath Root = MakePath("/ThumbnailTests/Materials/Invalid");
		const std::vector<FAssetThumbnailDependencyNode> Nodes = {
			{MakePackage("/ThumbnailTests/Materials/Invalid", "DMaterial", 7, 100, 10),
				{MakePath("/ThumbnailTests/Textures/Missing")}},
		};
		std::vector<FAssetThumbnailPackageFingerprint> Closure;
		std::string Error;
		EXPECT_FALSE(BuildAssetThumbnailDependencyClosure(Root, Nodes, Closure, Error));
		EXPECT_TRUE(Closure.empty());
		EXPECT_NE(Error.find("/ThumbnailTests/Textures/Missing"), std::string::npos);
	}

	TEST(FAssetThumbnailContractTests, DuplicateRegistryEntriesCannotProduceAmbiguousClosure)
	{
		const FAssetPath Root = MakePath("/ThumbnailTests/Materials/Duplicate");
		const std::vector<FAssetThumbnailDependencyNode> Nodes = {
			{MakePackage("/ThumbnailTests/Materials/Duplicate", "DMaterial", 7, 100, 10), {}},
			{MakePackage("/ThumbnailTests/Materials/Duplicate", "DMaterial", 7, 100, 10), {}},
		};
		std::vector<FAssetThumbnailPackageFingerprint> Closure;
		std::string Error;
		EXPECT_FALSE(BuildAssetThumbnailDependencyClosure(Root, Nodes, Closure, Error));
		EXPECT_TRUE(Closure.empty());
		EXPECT_NE(Error.find("duplicate"), std::string::npos);
	}

	TEST(FAssetThumbnailContractTests, CancellationIsSharedAcrossCapturedRequests)
	{
		const FAssetThumbnailCancellation Cancellation;
		const FAssetThumbnailCancellation Captured = Cancellation;
		EXPECT_FALSE(Captured.IsCancelled());
		Cancellation.Cancel();
		EXPECT_TRUE(Captured.IsCancelled());
	}

	TEST(FAssetThumbnailContractTests, InitialFixtureAndBudgetContractsAreBounded)
	{
		const FRenderedAssetThumbnailVisualContract Visual;
		EXPECT_EQ(Visual.Output.Width, Visual.Output.Height);
		EXPECT_EQ(Visual.Output.Width, 256u);
		EXPECT_EQ(FRenderedAssetThumbnailVisualContract::SphereVirtualPath,
			"/Engine/Editor/MaterialPreview/Sphere");
		EXPECT_EQ(FRenderedAssetThumbnailVisualContract::SphereFixtureVersion, 1u);
		EXPECT_EQ(FRenderedAssetThumbnailVisualContract::OutputEncoding, "PNG");
		EXPECT_EQ(FRenderedAssetThumbnailVisualContract::OutputColorSpace, "sRGB");
		EXPECT_TRUE(Visual.bOutputOpaque);
		EXPECT_EQ(Visual.CubeDirectionConvention,
			EAssetThumbnailCubeDirectionConvention::WorldSpaceReflectionVector);

		const FAssetThumbnailBudgets Budgets;
		EXPECT_EQ(Budgets.MaximumRendersPerFrame, 1u);
		EXPECT_EQ(Budgets.MaximumLivePreviewScenes, 1u);
		EXPECT_GT(Budgets.MaximumQueuedJobs, 0u);
		EXPECT_GT(Budgets.CpuPixelBudgetBytes, 0u);
		EXPECT_GT(Budgets.GpuTextureBudgetBytes, 0u);
		EXPECT_GT(Budgets.DiskBudgetBytes, 0u);
	}
} // namespace Durin
