#include <gtest/gtest.h>

#include "EngineTestSupport.h"
#include "ImageDecoder.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "Thumbnail/AssetThumbnail.h"
#include "Thumbnail/AssetThumbnailCache.h"
#include "Thumbnail/RenderedAssetThumbnailPipeline.h"
#include "Thumbnail/StaticMeshAssetThumbnail.h"

namespace Durin
{
	namespace
	{
		auto MakePath(std::string_view Value) -> FAssetPath
		{
			InitializeDObjectSystem();
			const std::filesystem::path Root = Testing::GetTestWorkDirectory() / "AssetThumbnailContracts";
			static std::unordered_set<std::filesystem::path> RegisteredRoots;
			if (RegisteredRoots.insert(Root).second)
			{
				std::filesystem::create_directories(Root);
				PathUtilities::RegisterMountPointForTests("/ThumbnailTests/", Root.generic_string() + "/");
			}
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

		class FTestThumbnailProvider final : public IAssetThumbnailProvider
		{
		public:
			explicit FTestThumbnailProvider(
				FAssetThumbnailProviderRegistration InRegistration,
				bool bInCaptureSucceeds = true)
				: Registration(std::move(InRegistration))
				, bCaptureSucceeds(bInCaptureSucceeds)
			{
			}

			auto GetRegistration() const -> FAssetThumbnailProviderRegistration override
			{
				return Registration;
			}

			auto CaptureGenerationRequest(
				const FAssetThumbnailRequest& Request,
				uint64 ProviderGeneration,
				FAssetThumbnailGenerationRequest& OutRequest,
				std::string& OutError
			) -> bool override
			{
				if (!bCaptureSucceeds)
				{
					OutError = "The test provider rejected the asset.";
					return false;
				}
				OutRequest.KeyInput.Asset = Request.Asset;
				OutRequest.KeyInput.ProviderName = Registration.ProviderName;
				OutRequest.KeyInput.GeneratorSchemaVersion = Registration.GeneratorSchemaVersion;
				OutRequest.ProviderGeneration = ProviderGeneration;
				OutRequest.RequestSerial = Request.RequestSerial;
				OutError.clear();
				return true;
			}

		private:
			FAssetThumbnailProviderRegistration Registration;
			bool bCaptureSucceeds = true;
		};

		auto MakeThumbnailRequest(
			std::string_view Path,
			std::string AssetClassName,
			uint64 RequestSerial,
			EAssetThumbnailPriority Priority = EAssetThumbnailPriority::Prefetch,
			uint64 FileSize = 100
		) -> FAssetThumbnailRequest
		{
			return {
				.Asset = MakePackage(Path, std::move(AssetClassName), 7, FileSize, 10),
				.Priority = Priority,
				.RequestSerial = RequestSerial};
		}

		auto MakeObjectStoreRoot(std::string_view Name) -> std::filesystem::path
		{
			const std::filesystem::path Root =
				Testing::GetTestWorkDirectory() / "AssetThumbnailObjectStore" / Name;
			Durin::Testing::RemoveTestWorkDirectory(Root);
			std::filesystem::create_directories(Root);
			return Root;
		}

		auto ExpectStaticMeshBoundsFit(
			const FBox& Bounds,
			double AspectRatio = 1.0) -> void
		{
			const FStaticMeshAssetThumbnailViewInput Input{
				.LocalBounds = Bounds,
				.OutputAspectRatio = AspectRatio};
			FStaticMeshAssetThumbnailView View;
			std::string Error;
			ASSERT_TRUE(CalculateStaticMeshAssetThumbnailView(Input, View, Error)) << Error;

			const FVector3 TransformedCenter = Bounds.GetCenter() + View.MeshTransform.Translation;
			EXPECT_NEAR(TransformedCenter.x, 0.0, 1.0e-12);
			EXPECT_NEAR(TransformedCenter.y, 0.0, 1.0e-12);
			EXPECT_NEAR(TransformedCenter.z, 0.0, 1.0e-12);
			EXPECT_GT(View.NearClipDistance, 0.0);
			EXPECT_GT(View.FarClipDistance, View.NearClipDistance);

			const FVector3 Extent = Bounds.GetExtent();
			const double VerticalTangent =
				std::tan(Math::DegreesToRadians(Input.VerticalFieldOfViewDegrees) * 0.5);
			const double MaximumProjectedCoordinate = 1.0 - Input.ImageMargin;
			double MaximumAbsoluteProjection = 0.0;
			for (uint32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
			{
				const FVector3 Corner(
					(CornerIndex & 1u) != 0 ? Extent.x : -Extent.x,
					(CornerIndex & 2u) != 0 ? Extent.y : -Extent.y,
					(CornerIndex & 4u) != 0 ? Extent.z : -Extent.z);
				const FVector3 FromCamera = Corner - View.CameraPosition;
				const double Depth = Math::Dot(View.CameraForward, FromCamera);
				ASSERT_GT(Depth, View.NearClipDistance);
				ASSERT_LT(Depth, View.FarClipDistance);
				const double ProjectedX =
					Math::Dot(View.CameraRight, FromCamera)
					/ (Depth * VerticalTangent * AspectRatio);
				const double ProjectedY =
					Math::Dot(View.CameraUp, FromCamera) / (Depth * VerticalTangent);
				EXPECT_LE(std::abs(ProjectedX), MaximumProjectedCoordinate + 1.0e-12);
				EXPECT_LE(std::abs(ProjectedY), MaximumProjectedCoordinate + 1.0e-12);
				MaximumAbsoluteProjection = std::max({
					MaximumAbsoluteProjection,
					std::abs(ProjectedX),
					std::abs(ProjectedY)});
			}
			EXPECT_NEAR(MaximumAbsoluteProjection, MaximumProjectedCoordinate, 1.0e-12);
		}
	} // namespace

	TEST(FAssetThumbnailContractTests, StaticMeshVisualContractIsFrozen)
	{
		EXPECT_EQ(FStaticMeshAssetThumbnailContract::AssetClassName, "DStaticMesh");
		EXPECT_EQ(FStaticMeshAssetThumbnailContract::ProviderName, "Durin.StaticMeshThumbnail");
		EXPECT_EQ(FStaticMeshAssetThumbnailContract::GeneratorSchemaVersion, 2u);
		EXPECT_EQ(
			FStaticMeshAssetThumbnailContract::PreviewFixtureIdentity,
			"/Engine/Editor/StaticMeshPreview/LOD0DefaultMaterials");
		EXPECT_EQ(FStaticMeshAssetThumbnailContract::PreviewFixtureVersion, 2u);
		EXPECT_EQ(FStaticMeshAssetThumbnailContract::ShaderContractVersion, 1u);
		EXPECT_EQ(FStaticMeshAssetThumbnailContract::ImageMargin, 0.04);
		EXPECT_EQ(FStaticMeshAssetThumbnailContract::LODIndex, 0u);
		EXPECT_FALSE(FStaticMeshAssetThumbnailContract::bOutputOpaque);
		const FStaticMeshAssetThumbnailViewInput ViewInput;
		EXPECT_EQ(ViewInput.CameraDirection, FVector3(2.4, -3.2, 2.4));
	}

	TEST(FAssetThumbnailContractTests, StaticMeshReadinessRequiresReadyNonZeroRevision)
	{
		EXPECT_FALSE(FStaticMeshRenderResourceStatus{}.IsReady());
		EXPECT_FALSE((FStaticMeshRenderResourceStatus{
			.Readiness = EStaticMeshRenderResourceReadiness::Ready}.IsReady()));
		EXPECT_TRUE((FStaticMeshRenderResourceStatus{
			.Readiness = EStaticMeshRenderResourceReadiness::Ready,
			.Revision = 1}.IsReady()));
		EXPECT_FALSE((FStaticMeshRenderResourceStatus{
			.Readiness = EStaticMeshRenderResourceReadiness::Queued,
			.Revision = 2}.IsReady()));
	}

	TEST(FAssetThumbnailContractTests, StaticMeshBoundsFramingContainsRepresentativeBounds)
	{
		ExpectStaticMeshBoundsFit(FBox(FVector3(-1.0), FVector3(1.0)));
		ExpectStaticMeshBoundsFit(FBox(FVector3(-0.5, -0.5, -4.0), FVector3(0.5, 0.5, 4.0)));
		ExpectStaticMeshBoundsFit(FBox(FVector3(-5.0, -0.25, -0.5), FVector3(5.0, 0.25, 0.5)));
		ExpectStaticMeshBoundsFit(FBox(FVector3(-0.5, -6.0, -0.5), FVector3(0.5, 6.0, 0.5)));
		ExpectStaticMeshBoundsFit(FBox(FVector3(9.0, -7.0, 2.0), FVector3(12.0, -1.0, 8.0)));
		ExpectStaticMeshBoundsFit(FBox(FVector3(-1.0e-4), FVector3(1.0e-4)));
		ExpectStaticMeshBoundsFit(FBox(FVector3(-2.0, -1.0, -0.5), FVector3(2.0, 1.0, 0.5)), 16.0 / 9.0);
	}

	TEST(FAssetThumbnailContractTests, StaticMeshBoundsFramingIsDeterministic)
	{
		const FStaticMeshAssetThumbnailViewInput Input{
			.LocalBounds = FBox(FVector3(3.0, -8.0, 2.0), FVector3(11.0, 4.0, 6.0)),
			.OutputAspectRatio = 1.25};
		FStaticMeshAssetThumbnailView First;
		FStaticMeshAssetThumbnailView Second;
		std::string Error;
		ASSERT_TRUE(CalculateStaticMeshAssetThumbnailView(Input, First, Error)) << Error;
		ASSERT_TRUE(CalculateStaticMeshAssetThumbnailView(Input, Second, Error)) << Error;
		EXPECT_EQ(First.MeshTransform.Translation, Second.MeshTransform.Translation);
		EXPECT_EQ(First.CameraPosition, Second.CameraPosition);
		EXPECT_EQ(First.CameraTarget, Second.CameraTarget);
		EXPECT_EQ(First.CameraForward, Second.CameraForward);
		EXPECT_EQ(First.CameraRight, Second.CameraRight);
		EXPECT_EQ(First.CameraUp, Second.CameraUp);
		EXPECT_EQ(First.CameraDistance, Second.CameraDistance);
		EXPECT_EQ(First.NearClipDistance, Second.NearClipDistance);
		EXPECT_EQ(First.FarClipDistance, Second.FarClipDistance);
	}

	TEST(FAssetThumbnailContractTests, StaticMeshBoundsFramingRejectsInvalidInputs)
	{
		auto ExpectRejected = [](FStaticMeshAssetThumbnailViewInput Input) {
			FStaticMeshAssetThumbnailView View;
			std::string Error;
			EXPECT_FALSE(CalculateStaticMeshAssetThumbnailView(Input, View, Error));
			EXPECT_FALSE(Error.empty());
		};

		ExpectRejected({});
		ExpectRejected({.LocalBounds = FBox(FVector3(-1.0), FVector3(1.0, 1.0, -1.0))});
		ExpectRejected({.LocalBounds = FBox(FVector3(0.0), FVector3(0.0, 1.0, 1.0))});
		ExpectRejected({
			.LocalBounds = FBox(
				FVector3(-1.0),
				FVector3(std::numeric_limits<double>::infinity(), 1.0, 1.0))});
		ExpectRejected({
			.LocalBounds = FBox(FVector3(-1.0), FVector3(1.0)),
			.OutputAspectRatio = 0.0});
		ExpectRejected({
			.LocalBounds = FBox(FVector3(-1.0), FVector3(1.0)),
			.VerticalFieldOfViewDegrees = 180.0});
		ExpectRejected({
			.LocalBounds = FBox(FVector3(-1.0), FVector3(1.0)),
			.CameraDirection = FVector3(0.0)});
		ExpectRejected({
			.LocalBounds = FBox(FVector3(-1.0), FVector3(1.0)),
			.CameraDirection = FVectorConstants::Up});
		ExpectRejected({
			.LocalBounds = FBox(FVector3(-1.0), FVector3(1.0)),
			.ImageMargin = 1.0});
	}

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
			"/Engine/Models/Sphere");
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

	TEST(FAssetThumbnailContractTests, ObjectStorePersistsAndRejectsUnsafeKeys)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("Persistence");
		const std::vector<uint8> Payload = {1, 2, 3, 4, 5};
		{
			FAssetThumbnailObjectStore Store({
				.CacheRoot = Root,
				.ObjectExtension = ".bin"});
			EXPECT_FALSE(Store.Store("../unsafe", Payload));
			ASSERT_TRUE(Store.Store("material-key-01", Payload));
		}
		FAssetThumbnailObjectStore WarmStore({
			.CacheRoot = Root,
			.ObjectExtension = ".bin"});
		std::vector<uint8> Loaded;
		EXPECT_EQ(WarmStore.Load("material-key-01", Loaded), EAssetThumbnailObjectLoadResult::Hit);
		EXPECT_EQ(Loaded, Payload);
		EXPECT_EQ(WarmStore.GetStats().CacheHits, 1u);
		EXPECT_EQ(WarmStore.Load("../unsafe", Loaded), EAssetThumbnailObjectLoadResult::Invalid);
	}

	TEST(FAssetThumbnailContractTests, ObjectStoreCleansMissingObjectsAndHonorsDiskBudget)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("Cleanup");
		const std::vector<uint8> Payload(80, 7);
		{
			FAssetThumbnailObjectStore Store({
				.CacheRoot = Root,
				.DiskBudgetBytes = 100,
				.ObjectExtension = ".bin"});
			ASSERT_TRUE(Store.Store("object-key-01", Payload));
			ASSERT_TRUE(Store.Store("object-key-02", Payload));
			EXPECT_EQ(Store.GetStats().Evictions, 1u);
			std::vector<uint8> Loaded;
			EXPECT_EQ(Store.Load("object-key-01", Loaded), EAssetThumbnailObjectLoadResult::Miss);
			EXPECT_EQ(Store.Load("object-key-02", Loaded), EAssetThumbnailObjectLoadResult::Hit);
		}
		for (const auto& Entry : std::filesystem::recursive_directory_iterator(Root / "Objects"))
			if (Entry.path().extension() == ".bin") std::filesystem::remove(Entry.path());
		FAssetThumbnailObjectStore MissingObjectStore({
			.CacheRoot = Root,
			.DiskBudgetBytes = 100,
			.ObjectExtension = ".bin"});
		EXPECT_GE(MissingObjectStore.GetStats().Regenerations, 1u);
	}

	TEST(FAssetThumbnailContractTests, BudgetSelectionEvictsOldestUnpinnedAllocations)
	{
		const std::vector<FAssetThumbnailBudgetEntry> Entries = {
			{.Key = "visible", .Bytes = 60, .LastUsed = 1, .bPinned = true},
			{.Key = "oldest", .Bytes = 40, .LastUsed = 2},
			{.Key = "newest", .Bytes = 30, .LastUsed = 3},
		};
		EXPECT_EQ(SelectAssetThumbnailBudgetEvictions(Entries, 100),
			std::vector<std::string>({"oldest"}));
		EXPECT_EQ(SelectAssetThumbnailBudgetEvictions(Entries, 50),
			(std::vector<std::string>{"oldest", "newest"}));
		EXPECT_TRUE(SelectAssetThumbnailBudgetEvictions(Entries, 200).empty());
	}

	TEST(FAssetThumbnailContractTests, ProviderRegistryRejectsInvalidAndDuplicateRegistrations)
	{
		FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		EXPECT_FALSE(Registry.Register(nullptr, Error));
		EXPECT_FALSE(Error.empty());

		auto Invalid = std::make_shared<FTestThumbnailProvider>(
			FAssetThumbnailProviderRegistration{.AssetClassName = "DMaterial"});
		EXPECT_FALSE(Registry.Register(Invalid, Error));
		EXPECT_FALSE(Error.empty());

		auto Material = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Material, Error)) << Error;
		EXPECT_EQ(Registry.Num(), 1u);

		auto Duplicate = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.OtherMaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		EXPECT_FALSE(Registry.Register(Duplicate, Error));
		EXPECT_NE(Error.find("DMaterial"), std::string::npos);
		EXPECT_EQ(Registry.Num(), 1u);
	}

	TEST(FAssetThumbnailContractTests, ProviderRegistryUsesExactClassesAndMonotonicGenerations)
	{
		FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Material = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		auto MaterialInstance = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterialInstance",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Material, Error)) << Error;
		ASSERT_TRUE(Registry.Register(MaterialInstance, Error)) << Error;

		const FAssetThumbnailProviderHandle MaterialHandle = Registry.Find("DMaterial");
		const FAssetThumbnailProviderHandle InstanceHandle = Registry.Find("DMaterialInstance");
		EXPECT_EQ(MaterialHandle.Provider, Material);
		EXPECT_EQ(InstanceHandle.Provider, MaterialInstance);
		EXPECT_NE(MaterialHandle.Generation, InstanceHandle.Generation);
		EXPECT_FALSE(Registry.Find("DMaterialInterface"));

		ASSERT_TRUE(Registry.Unregister("DMaterial", Error)) << Error;
		EXPECT_FALSE(Registry.Find("DMaterial"));
		EXPECT_FALSE(Registry.Unregister("DMaterial", Error));

		auto Replacement = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 2});
		ASSERT_TRUE(Registry.Register(Replacement, Error)) << Error;
		EXPECT_GT(Registry.Find("DMaterial").Generation, MaterialHandle.Generation);
	}

	TEST(FAssetThumbnailContractTests, ProviderRegistryShutdownDropsProvidersAndClosesRegistration)
	{
		FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
			.AssetClassName = "DTextureCube",
			.ProviderName = "Durin.TextureCubeThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;

		Registry.Shutdown();
		EXPECT_TRUE(Registry.IsShuttingDown());
		EXPECT_EQ(Registry.Num(), 0u);
		EXPECT_FALSE(Registry.Find("DTextureCube"));
		EXPECT_FALSE(Registry.Register(Provider, Error));
		EXPECT_NE(Error.find("shutdown"), std::string::npos);
	}

	TEST(FAssetThumbnailContractTests, SchedulerSkipsMissingProvidersAndRecordsProviderRejection)
	{
		FAssetThumbnailProviderRegistry Registry;
		FAssetThumbnailScheduler Scheduler(Registry);
		std::string Error;
		const FAssetThumbnailRequest Missing =
			MakeThumbnailRequest("/ThumbnailTests/Unsupported", "DUnsupported", 1);
		EXPECT_FALSE(Scheduler.Request(Missing, Error));
		EXPECT_EQ(Scheduler.NumQueued(), 0u);
		EXPECT_EQ(Scheduler.Find(Missing.Asset.VirtualPath).State, EAssetThumbnailState::NotRequested);

		auto Rejecting = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1}, false);
		ASSERT_TRUE(Registry.Register(Rejecting, Error)) << Error;
		const FAssetThumbnailRequest Invalid =
			MakeThumbnailRequest("/ThumbnailTests/Invalid", "DMaterial", 2);
		EXPECT_FALSE(Scheduler.Request(Invalid, Error));
		const FAssetThumbnailView InvalidView = Scheduler.Find(Invalid.Asset.VirtualPath);
		EXPECT_EQ(InvalidView.State, EAssetThumbnailState::Invalid);
		EXPECT_EQ(InvalidView.RequestSerial, 2u);
		EXPECT_NE(InvalidView.Diagnostic.find("rejected"), std::string::npos);
	}

	TEST(FAssetThumbnailContractTests, SchedulerCoalescesAndPromotesVisibleRequests)
	{
		FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		FAssetThumbnailScheduler Scheduler(Registry);
		const FAssetThumbnailRequest Prefetch =
			MakeThumbnailRequest("/ThumbnailTests/Coalesced", "DMaterial", 1);
		const FAssetThumbnailRequest Visible =
			MakeThumbnailRequest("/ThumbnailTests/Coalesced", "DMaterial", 2, EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(Scheduler.Request(Prefetch, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(Visible, Error)) << Error;
		EXPECT_EQ(Scheduler.NumQueued(), 1u);
		EXPECT_EQ(Scheduler.Find(Prefetch.Asset.VirtualPath).RequestSerial, 2u);

		std::optional<FAssetThumbnailScheduledJob> Job = Scheduler.TakeNext();
		ASSERT_TRUE(Job);
		EXPECT_EQ(Job->Priority, EAssetThumbnailPriority::Visible);
		EXPECT_EQ(Job->GenerationRequest.RequestSerial, 2u);
		EXPECT_EQ(Scheduler.Find(Prefetch.Asset.VirtualPath).State, EAssetThumbnailState::Loading);

		const FAssetThumbnailRequest NewSerial =
			MakeThumbnailRequest("/ThumbnailTests/Coalesced", "DMaterial", 3, EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(Scheduler.Request(NewSerial, Error)) << Error;
		EXPECT_TRUE(Job->GenerationRequest.Cancellation.IsCancelled());
		EXPECT_EQ(Scheduler.NumQueued(), 1u);
		const std::optional<FAssetThumbnailScheduledJob> Replacement = Scheduler.TakeNext();
		ASSERT_TRUE(Replacement);
		EXPECT_EQ(Replacement->GenerationRequest.RequestSerial, 3u);
	}

	TEST(FAssetThumbnailContractTests, SchedulerPrioritizesVisibleJobsAndEnforcesQueueBudget)
	{
		FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
			.AssetClassName = "DTextureCube",
			.ProviderName = "Durin.TextureCubeThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		FAssetThumbnailBudgets Budgets;
		Budgets.MaximumQueuedJobs = 2;
		FAssetThumbnailScheduler Scheduler(Registry, Budgets);
		const FAssetThumbnailRequest Prefetch =
			MakeThumbnailRequest("/ThumbnailTests/PrefetchCube", "DTextureCube", 1);
		const FAssetThumbnailRequest Visible =
			MakeThumbnailRequest("/ThumbnailTests/VisibleCube", "DTextureCube", 1, EAssetThumbnailPriority::Visible);
		const FAssetThumbnailRequest Overflow =
			MakeThumbnailRequest("/ThumbnailTests/OverflowCube", "DTextureCube", 1);
		ASSERT_TRUE(Scheduler.Request(Prefetch, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(Visible, Error)) << Error;
		EXPECT_FALSE(Scheduler.Request(Overflow, Error));
		EXPECT_NE(Error.find("budget"), std::string::npos);

		const std::optional<FAssetThumbnailScheduledJob> First = Scheduler.TakeNext();
		ASSERT_TRUE(First);
		EXPECT_EQ(First->GenerationRequest.KeyInput.Asset.VirtualPath, Visible.Asset.VirtualPath);
		const std::optional<FAssetThumbnailScheduledJob> Second = Scheduler.TakeNext();
		ASSERT_TRUE(Second);
		EXPECT_EQ(Second->GenerationRequest.KeyInput.Asset.VirtualPath, Prefetch.Asset.VirtualPath);
		EXPECT_FALSE(Scheduler.TakeNext());
	}

	TEST(FAssetThumbnailContractTests, SchedulerRejectsStaleSerialsAndCancelsReplacedWork)
	{
		FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		FAssetThumbnailScheduler Scheduler(Registry);
		const FAssetThumbnailRequest Current =
			MakeThumbnailRequest("/ThumbnailTests/Replaced", "DMaterial", 2);
		ASSERT_TRUE(Scheduler.Request(Current, Error)) << Error;
		std::optional<FAssetThumbnailScheduledJob> Active = Scheduler.TakeNext();
		ASSERT_TRUE(Active);

		const FAssetThumbnailRequest Stale =
			MakeThumbnailRequest("/ThumbnailTests/Replaced", "DMaterial", 1);
		EXPECT_FALSE(Scheduler.Request(Stale, Error));
		EXPECT_FALSE(Active->GenerationRequest.Cancellation.IsCancelled());

		const FAssetThumbnailRequest Changed =
			MakeThumbnailRequest("/ThumbnailTests/Replaced", "DMaterial", 3, EAssetThumbnailPriority::Visible, 200);
		ASSERT_TRUE(Scheduler.Request(Changed, Error)) << Error;
		EXPECT_TRUE(Active->GenerationRequest.Cancellation.IsCancelled());
		EXPECT_EQ(Scheduler.NumQueued(), 1u);
		EXPECT_EQ(Scheduler.Find(Changed.Asset.VirtualPath).RequestSerial, 3u);
	}

	TEST(FAssetThumbnailContractTests, MixedRenderedKindsSharePriorityCoalescingAndQueueBudgets)
	{
		FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		for (const FAssetThumbnailProviderRegistration Registration : {
			FAssetThumbnailProviderRegistration{
				.AssetClassName = "DMaterial",
				.ProviderName = "Durin.MaterialThumbnail",
				.GeneratorSchemaVersion = 1},
			FAssetThumbnailProviderRegistration{
				.AssetClassName = "DTextureCube",
				.ProviderName = "Durin.TextureCubeThumbnail",
				.GeneratorSchemaVersion = 1},
			FAssetThumbnailProviderRegistration{
				.AssetClassName = "DStaticMesh",
				.ProviderName = "Durin.StaticMeshThumbnail",
				.GeneratorSchemaVersion = 1}})
		{
			ASSERT_TRUE(Registry.Register(
				std::make_shared<FTestThumbnailProvider>(Registration), Error)) << Error;
		}
		FAssetThumbnailBudgets Budgets;
		Budgets.MaximumQueuedJobs = 3;
		FAssetThumbnailScheduler Scheduler(Registry, Budgets);
		const FAssetThumbnailRequest Material = MakeThumbnailRequest(
			"/ThumbnailTests/Mixed/Material", "DMaterial", 1);
		const FAssetThumbnailRequest Cube = MakeThumbnailRequest(
			"/ThumbnailTests/Mixed/Cube", "DTextureCube", 1);
		const FAssetThumbnailRequest MeshPrefetch = MakeThumbnailRequest(
			"/ThumbnailTests/Mixed/Mesh", "DStaticMesh", 1);
		const FAssetThumbnailRequest MeshVisible = MakeThumbnailRequest(
			"/ThumbnailTests/Mixed/Mesh", "DStaticMesh", 2,
			EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(Scheduler.Request(Material, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(Cube, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(MeshPrefetch, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(MeshVisible, Error)) << Error;
		EXPECT_EQ(Scheduler.NumQueued(), 3u);

		const std::optional<FAssetThumbnailScheduledJob> First = Scheduler.TakeNext();
		ASSERT_TRUE(First);
		EXPECT_EQ(
			First->GenerationRequest.KeyInput.Asset.VirtualPath,
			MeshVisible.Asset.VirtualPath);
		EXPECT_EQ(First->GenerationRequest.RequestSerial, 2u);
		const std::optional<FAssetThumbnailScheduledJob> Second = Scheduler.TakeNext();
		const std::optional<FAssetThumbnailScheduledJob> Third = Scheduler.TakeNext();
		ASSERT_TRUE(Second);
		ASSERT_TRUE(Third);
		EXPECT_NE(
			Second->GenerationRequest.KeyInput.Asset.AssetClassName,
			Third->GenerationRequest.KeyInput.Asset.AssetClassName);
		EXPECT_FALSE(Scheduler.TakeNext());
	}

	TEST(FAssetThumbnailContractTests, SchedulerShutdownCancelsWorkAndRejectsNewRequests)
	{
		FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		FAssetThumbnailScheduler Scheduler(Registry);
		const FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/Shutdown", "DMaterial", 1);
		ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
		std::optional<FAssetThumbnailScheduledJob> Active = Scheduler.TakeNext();
		ASSERT_TRUE(Active);

		Scheduler.Shutdown();
		EXPECT_TRUE(Scheduler.IsShuttingDown());
		EXPECT_TRUE(Active->GenerationRequest.Cancellation.IsCancelled());
		EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State, EAssetThumbnailState::NotRequested);
		EXPECT_FALSE(Scheduler.Request(Request, Error));
		EXPECT_NE(Error.find("shutdown"), std::string::npos);
	}

	TEST(FAssetThumbnailContractTests, RenderedPipelinePublishesColdOutputAndServesWarmHit)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("RenderedPipelineWarmHit");
		auto RunRequest = [&](uint64 Serial, bool bExpectWarmHit) {
			FAssetThumbnailProviderRegistry Registry;
			std::string Error;
			auto Provider = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
				.AssetClassName = "DMaterial",
				.ProviderName = "Durin.MaterialThumbnail",
				.GeneratorSchemaVersion = 1});
			EXPECT_TRUE(Registry.Register(Provider, Error)) << Error;
			FAssetThumbnailScheduler Scheduler(Registry);
			FRenderedAssetThumbnailPipeline Pipeline(
				Scheduler,
				{.CacheRoot = Root, .ObjectExtension = ".bin"});
			const FAssetThumbnailRequest Request =
				MakeThumbnailRequest("/ThumbnailTests/PersistentMaterial", "DMaterial", Serial);
			EXPECT_TRUE(Scheduler.Request(Request, Error)) << Error;
			Pipeline.BeginFrame();
			std::optional<FRenderedAssetThumbnailJob> Job = Pipeline.StartNext();
			if (bExpectWarmHit)
			{
				EXPECT_FALSE(Job);
				EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State, EAssetThumbnailState::Ready);
				const FRenderedAssetThumbnailPipelineStats Stats = Pipeline.GetStats();
				EXPECT_EQ(Stats.DiskHits, 1u);
				EXPECT_EQ(Stats.Loads, 0u);
				EXPECT_EQ(Stats.Renders, 0u);
				EXPECT_EQ(Stats.Readbacks, 0u);
				return;
			}

			ASSERT_TRUE(Job);
			ASSERT_TRUE(Pipeline.CompleteLoad(*Job, 10));
			ASSERT_TRUE(Pipeline.BeginRender(*Job, true, 10, 20));
			ASSERT_TRUE(Pipeline.CompleteRender(*Job, 10, 20));
			ASSERT_TRUE(Pipeline.CompleteReadback(*Job, 10, 20));
			const std::vector<uint8> Encoded = {1, 2, 3, 4};
			ASSERT_TRUE(Pipeline.CompleteEncoding(*Job, 10, 20, Encoded));
			EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State, EAssetThumbnailState::Ready);
			const FRenderedAssetThumbnailPipelineStats Stats = Pipeline.GetStats();
			EXPECT_EQ(Stats.Jobs, 1u);
			EXPECT_EQ(Stats.Loads, 1u);
			EXPECT_EQ(Stats.Renders, 1u);
			EXPECT_EQ(Stats.Readbacks, 1u);
		};

		RunRequest(1, false);
		RunRequest(2, true);
	}

	TEST(FAssetThumbnailContractTests, RenderedPipelineEncodesRgbaPixelsAsDecodablePng)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("RenderedPipelinePng");
		FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		FAssetThumbnailScheduler Scheduler(Registry);
		FRenderedAssetThumbnailPipeline Pipeline(
			Scheduler,
			{.CacheRoot = Root, .ObjectExtension = ".png"});
		const FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/EncodedMaterial", "DMaterial", 1);
		ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
		Pipeline.BeginFrame();
		std::optional<FRenderedAssetThumbnailJob> Job = Pipeline.StartNext();
		ASSERT_TRUE(Job);
		const std::string CacheKey = Job->ScheduledJob.CacheKey;
		ASSERT_TRUE(Pipeline.CompleteLoad(*Job, 10));
		ASSERT_TRUE(Pipeline.BeginRender(*Job, true, 10, 20));
		ASSERT_TRUE(Pipeline.CompleteRender(*Job, 10, 20));
		ASSERT_TRUE(Pipeline.CompleteReadback(*Job, 10, 20));
		const std::array<uint8, 8> Pixels = {
			255, 0, 0, 255,
			0, 255, 0, 128};
		ASSERT_TRUE(Pipeline.CompletePixels(*Job, 10, 20, Pixels, 2, 1));

		FAssetThumbnailObjectStore Store({
			.CacheRoot = Root,
			.ObjectExtension = ".png"});
		std::vector<uint8> Encoded;
		ASSERT_EQ(Store.Load(CacheKey, Encoded), EAssetThumbnailObjectLoadResult::Hit);
		Asset::FDecodedImage Decoded;
		ASSERT_TRUE(Asset::DecodeImageFromMemory(Encoded, Decoded, Error)) << Error;
		EXPECT_EQ(Decoded.Width, 2u);
		EXPECT_EQ(Decoded.Height, 1u);
		EXPECT_EQ(Decoded.Pixels, std::vector<uint8>(Pixels.begin(), Pixels.end()));
	}

	TEST(FAssetThumbnailContractTests, RenderedPipelineRevalidatesAfterEncodingBeforePublication)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("RenderedPipelinePublicationValidation");
		FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
			.AssetClassName = "DStaticMesh",
			.ProviderName = "Durin.StaticMeshThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		FAssetThumbnailScheduler Scheduler(Registry);
		FRenderedAssetThumbnailPipeline Pipeline(
			Scheduler,
			{.CacheRoot = Root, .ObjectExtension = ".png"});
		const FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/StaleStaticMesh", "DStaticMesh", 1);
		ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
		Pipeline.BeginFrame();
		std::optional<FRenderedAssetThumbnailJob> Job = Pipeline.StartNext();
		ASSERT_TRUE(Job);
		const std::string CacheKey = Job->ScheduledJob.CacheKey;
		ASSERT_TRUE(Pipeline.CompleteLoad(*Job, 10));
		ASSERT_TRUE(Pipeline.BeginRender(*Job, true, 10, 20));
		ASSERT_TRUE(Pipeline.CompleteRender(*Job, 10, 20));
		ASSERT_TRUE(Pipeline.CompleteReadback(*Job, 10, 20));
		const std::array<uint8, 4> Pixels = {255, 255, 255, 255};
		EXPECT_FALSE(Pipeline.CompletePixels(
			*Job,
			10,
			20,
			Pixels,
			1,
			1,
			{},
			[] { return std::string("StaticMesh changed before publication."); }));
		EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State, EAssetThumbnailState::Failed);

		FAssetThumbnailObjectStore Store({.CacheRoot = Root, .ObjectExtension = ".png"});
		std::vector<uint8> Encoded;
		EXPECT_EQ(Store.Load(CacheKey, Encoded), EAssetThumbnailObjectLoadResult::Miss);
	}

	TEST(FAssetThumbnailContractTests, RenderedPipelineBoundsRendersAndRejectsStaleCompletions)
	{
		FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		FAssetThumbnailScheduler Scheduler(Registry);
		FRenderedAssetThumbnailPipeline Pipeline(
			Scheduler,
			{.CacheRoot = MakeObjectStoreRoot("RenderedPipelineBounds"), .ObjectExtension = ".bin"});
		const FAssetThumbnailRequest FirstRequest =
			MakeThumbnailRequest("/ThumbnailTests/FirstRendered", "DMaterial", 1);
		const FAssetThumbnailRequest SecondRequest =
			MakeThumbnailRequest("/ThumbnailTests/SecondRendered", "DMaterial", 1);
		ASSERT_TRUE(Scheduler.Request(FirstRequest, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(SecondRequest, Error)) << Error;
		Pipeline.BeginFrame();
		std::optional<FRenderedAssetThumbnailJob> First = Pipeline.StartNext();
		std::optional<FRenderedAssetThumbnailJob> Second = Pipeline.StartNext();
		ASSERT_TRUE(First);
		ASSERT_TRUE(Second);
		ASSERT_TRUE(Pipeline.CompleteLoad(*First, 10));
		ASSERT_TRUE(Pipeline.CompleteLoad(*Second, 11));
		EXPECT_TRUE(Pipeline.BeginRender(*First, true, 10, 20));
		EXPECT_FALSE(Pipeline.BeginRender(*Second, true, 11, 21));
		EXPECT_EQ(Scheduler.Find(SecondRequest.Asset.VirtualPath).State,
			EAssetThumbnailState::WaitingForResources);

		Pipeline.BeginFrame();
		EXPECT_TRUE(Pipeline.BeginRender(*Second, true, 11, 21));
		EXPECT_FALSE(Pipeline.CompleteRender(*Second, 12, 21));
		EXPECT_EQ(Scheduler.Find(SecondRequest.Asset.VirtualPath).State, EAssetThumbnailState::Rendering);
		EXPECT_TRUE(Pipeline.CompleteRender(*Second, 11, 21));

		const FAssetThumbnailRequest Replacement =
			MakeThumbnailRequest("/ThumbnailTests/FirstRendered", "DMaterial", 2,
				EAssetThumbnailPriority::Visible, 200);
		ASSERT_TRUE(Scheduler.Request(Replacement, Error)) << Error;
		EXPECT_FALSE(Pipeline.CompleteRender(*First, 10, 20));
		EXPECT_TRUE(First->ScheduledJob.GenerationRequest.Cancellation.IsCancelled());
	}

	TEST(FAssetThumbnailContractTests, RenderedPipelineTracksWaitFailureCancellationAndRetry)
	{
		FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(FAssetThumbnailProviderRegistration{
			.AssetClassName = "DTextureCube",
			.ProviderName = "Durin.TextureCubeThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		FAssetThumbnailScheduler Scheduler(Registry);
		FRenderedAssetThumbnailPipeline Pipeline(
			Scheduler,
			{.CacheRoot = MakeObjectStoreRoot("RenderedPipelineCounters"), .ObjectExtension = ".bin"});
		const FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/CounterCube", "DTextureCube", 1);
		ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
		Pipeline.BeginFrame();
		std::optional<FRenderedAssetThumbnailJob> Job = Pipeline.StartNext();
		ASSERT_TRUE(Job);
		ASSERT_TRUE(Pipeline.CompleteLoad(*Job, 30));
		EXPECT_TRUE(Pipeline.BeginRender(*Job, false, 30, 0));
		EXPECT_FALSE(Pipeline.BeginRender(*Job, true, 30, 0, "Cube build failed."));
		EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State, EAssetThumbnailState::Failed);

		Pipeline.RecordRetry();
		const FAssetThumbnailRequest CancelRequest =
			MakeThumbnailRequest("/ThumbnailTests/CancelledCube", "DTextureCube", 1);
		ASSERT_TRUE(Scheduler.Request(CancelRequest, Error)) << Error;
		std::optional<FRenderedAssetThumbnailJob> Cancelled = Pipeline.StartNext();
		ASSERT_TRUE(Cancelled);
		Pipeline.Cancel(*Cancelled);

		const FRenderedAssetThumbnailPipelineStats Stats = Pipeline.GetStats();
		EXPECT_EQ(Stats.ResourceWaits, 1u);
		EXPECT_EQ(Stats.Failures, 1u);
		EXPECT_EQ(Stats.Retries, 1u);
		EXPECT_EQ(Stats.Cancellations, 1u);
	}
} // namespace Durin
