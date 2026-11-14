#include "ThumbnailManagerTestSupport.h"
#include <gtest/gtest.h>

#include "EngineTestSupport.h"
#include "Image/ImageDecoder.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/AssetThumbnailKey.h"
#include "AssetRegistry/Publication.h"
#include "Thumbnail/ThumbnailStorage.h"
#include "Thumbnail/AssetThumbnailRequestQueue.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "Thumbnail/AssetThumbnailCache.h"
#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Thumbnail/StaticMeshThumbnailRenderer.h"
#include "Thumbnail/AssetThumbnail.h"

namespace Durin
{
	namespace
	{
		auto PumpThumbnailPoolUntil(FAssetThumbnailPool& Pool,
			const std::function<bool()>& IsDone) -> bool
		{
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!IsDone() && std::chrono::steady_clock::now() < Deadline)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));

				Pool.Tick();
			}
			return IsDone();
		}

		constexpr uint8 WrongSizedThumbnailPng[] = {
			137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82,
			0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
			0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240,
			159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
			0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};

		auto MakePath(std::string_view Value) -> FPackagePath
		{
			InitializeDObjectSystem();
			const std::filesystem::path Root = Testing::GetTestWorkDirectory() / "AssetThumbnailContracts";
			static std::unordered_set<std::filesystem::path> RegisteredRoots;
			if (RegisteredRoots.insert(Root).second)
			{
				std::filesystem::create_directories(Root);
				Testing::RegisterMountPointForTests("/ThumbnailTests/", Root.generic_string() + "/");
			}
			FPackagePath Path;
			EXPECT_TRUE(FPackagePath::TryCreate(Value, Path));
			return Path;
		}

		auto MakeAssetPath(std::string_view Value) -> FTopLevelAssetPath
		{
			const FPackagePath PackagePath = MakePath(Value);
			FTopLevelAssetPath AssetPath;
			EXPECT_TRUE(FTopLevelAssetPath::TryCreate(
				PackagePath, PackagePath.GetPackageName(), AssetPath));
			return AssetPath;
		}

		auto PublishTestPackage(FPackageData Data) -> void
		{
			const auto Existing = FindPackageExact(Data.PackagePath);
			if (Existing && *Existing == Data) return;
			FAssetRegistryDelta Delta{.ExpectedRevision = GetAssetCatalogRevision()};
			(Existing ? Delta.Replaces : Delta.Adds).push_back(std::move(Data));
			const auto Result = PublishAssetRegistryDelta(std::move(Delta));
			EXPECT_TRUE(Result) << FormatAssetRegistryError(Result);
		}

		auto MakeAsset(std::string_view Path, std::string AssetClassName,
			uint32 FormatVersion, uint64 FileSize, int64 LastWriteTimeTicks) -> FAssetData
		{
			const FPackagePath PackagePath = MakePath(Path);
			FAssetData Asset{.AssetPath = MakeAssetPath(Path), .AssetClassName = std::move(AssetClassName)};
			PublishTestPackage({.PackagePath = PackagePath, .TopLevelAssets = {Asset},
				.FormatVersion = FormatVersion, .ObjectCount = 1,
				.FileSize = FileSize, .LastWriteTimeTicks = LastWriteTimeTicks});
			return Asset;
		}

		auto MakeMaterialKeyInput() -> FAssetThumbnailKeyInput
		{
			const auto Asset = MakeAsset("/ThumbnailTests/Materials/Preview", "DMaterial", 7, 4096, 100);
			const auto Texture = MakeAsset("/ThumbnailTests/Textures/BaseColor", "DTexture2D", 5, 1024, 200);
			const auto Parent = MakeAsset("/ThumbnailTests/Materials/Parent", "DMaterial", 7, 2048, 150);
			return {.Asset = Asset, .RendererClassName = "TestThumbnailRenderer",
				.Output = {},
				.Packages = {*FindPackageExact(Asset.AssetPath.GetPackagePath()),
					*FindPackageExact(Texture.AssetPath.GetPackagePath()), *FindPackageExact(Parent.AssetPath.GetPackagePath())}};
		}

		class FTestThumbnailRenderer final : public DThumbnailRenderer
		{
		DURIN_TEST_THUMBNAIL_RENDERER(FTestThumbnailRenderer)
		public:
			auto CanVisualizeAsset(DObject*) const -> bool override { return true; }
			auto Draw(DObject*, FThumbnailDrawContext& Context) -> void override
			{ Context.Fail({}); }

		};

		struct FFakeThumbnailRendererState
		{

			uint32 PreviewPreparations = 0;

			uint32 ExtensionDestructions = 0;
			uint32 ResourcePollsBeforeReady = 0;
			uint32 ResourcePolls = 0;
		};

		class FFakeThumbnailRenderer final
			: public DThumbnailRenderer
		{
		DURIN_TEST_THUMBNAIL_RENDERER(FFakeThumbnailRenderer)
		public:
			auto Configure(
				std::shared_ptr<FFakeThumbnailRendererState> InState) -> void
			{
				State = std::move(InState);
			}

			~FFakeThumbnailRenderer()
			{
				if (State) ++State->ExtensionDestructions;
			}



			auto CanVisualizeAsset(DObject*) const -> bool override { return true; }
			auto Draw(DObject*, FThumbnailDrawContext& Context) -> void override
			{
				if (State->ResourcePolls++ < State->ResourcePollsBeforeReady) return;
				++State->PreviewPreparations;
				Context.SetValidation([State = State] { return State->PreviewPreparations != 0; });
				Context.MarkReady();
			}

		private:
			std::shared_ptr<FFakeThumbnailRendererState> State;
		};

		auto MakeThumbnailRequest(
			std::string_view Path,
			std::string AssetClassName,
			EAssetThumbnailPriority Priority = EAssetThumbnailPriority::Prefetch,
			uint64 FileSize = 100
		) -> FAssetThumbnailRequest
		{
			return {
				.Asset = MakeAsset(Path, std::move(AssetClassName), 7, FileSize, 10),
				.Priority = Priority};
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
			const StaticMeshEditor::FStaticMeshThumbnailRendererViewInput Input{
				.LocalBounds = Bounds,
				.OutputAspectRatio = AspectRatio};
			StaticMeshEditor::FStaticMeshThumbnailRendererView View;

			auto ThumbnailResult1 = CalculateStaticMeshThumbnailRendererView(Input);
			ASSERT_TRUE(ThumbnailResult1);
			View = std::move(*ThumbnailResult1);

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
		EXPECT_EQ(StaticMeshEditor::FStaticMeshThumbnailRendererContract::ImageMargin, 0.04);
		EXPECT_EQ(StaticMeshEditor::FStaticMeshThumbnailRendererContract::LODIndex, 0u);
		EXPECT_FALSE(StaticMeshEditor::FStaticMeshThumbnailRendererContract::bOutputOpaque);
		const StaticMeshEditor::FStaticMeshThumbnailRendererViewInput ViewInput;
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
		ExpectStaticMeshBoundsFit(FBox(FVector3(-2.0, -1.0, 0.0), FVector3(2.0, 1.0, 0.0)));
		ExpectStaticMeshBoundsFit(FBox(FVector3(-2.0, -1.0, -0.5), FVector3(2.0, 1.0, 0.5)), 16.0 / 9.0);
	}

	TEST(FAssetThumbnailContractTests, StaticMeshBoundsFramingIsDeterministic)
	{
		const StaticMeshEditor::FStaticMeshThumbnailRendererViewInput Input{
			.LocalBounds = FBox(FVector3(3.0, -8.0, 2.0), FVector3(11.0, 4.0, 6.0)),
			.OutputAspectRatio = 1.25};
		StaticMeshEditor::FStaticMeshThumbnailRendererView First;
		StaticMeshEditor::FStaticMeshThumbnailRendererView Second;

		auto ThumbnailResult2 = CalculateStaticMeshThumbnailRendererView(Input);
		ASSERT_TRUE(ThumbnailResult2);
		First = std::move(*ThumbnailResult2);
		auto ThumbnailResult3 = CalculateStaticMeshThumbnailRendererView(Input);
		ASSERT_TRUE(ThumbnailResult3);
		Second = std::move(*ThumbnailResult3);
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
		auto ExpectRejected = [](StaticMeshEditor::FStaticMeshThumbnailRendererViewInput Input) {
			StaticMeshEditor::FStaticMeshThumbnailRendererView View;

			auto ThumbnailResult4 = CalculateStaticMeshThumbnailRendererView(Input);
			ASSERT_FALSE(ThumbnailResult4);
			EXPECT_FALSE(ThumbnailResult4.error().empty());
		};

		ExpectRejected({});
		ExpectRejected({.LocalBounds = FBox(FVector3(-1.0), FVector3(1.0, 1.0, -2.0))});
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

	TEST(FAssetThumbnailContractTests, PackageOrderDoesNotAffectKeys)
	{
		FAssetThumbnailKeyInput Forward = MakeMaterialKeyInput();
		FAssetThumbnailKeyInput Reverse = Forward;
		std::ranges::reverse(Reverse.Packages);
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

		ExpectChanged([](auto& Value) { Value.Asset.AssetPath = MakeAssetPath("/ThumbnailTests/Materials/Renamed"); });
		ExpectChanged([](auto& Value) { ++Value.Packages.front().FormatVersion; });
		ExpectChanged([](auto& Value) { ++Value.Packages.front().FileSize; });
		ExpectChanged([](auto& Value) { ++Value.Packages.front().LastWriteTimeTicks; });
		ExpectChanged([](auto& Value) { Value.Asset.AssetClassName = "DMaterialInstance"; });
		ExpectChanged([](auto& Value) { Value.RendererClassName += ".Changed"; });
		ExpectChanged([](auto& Value) { ++Value.Output.Width; });
		ExpectChanged([](auto& Value) { ++Value.Output.Height; });
		ExpectChanged([](auto& Value) { ++Value.Output.ColorSpaceVersion; });
		ExpectChanged([](auto& Value) { ++Value.Output.EncodingVersion; });
		ExpectChanged([](auto& Value) { ++Value.Packages.back().LastWriteTimeTicks; });
	}

	TEST(FAssetThumbnailContractTests, CacheKeyReadsCurrentPackageDataWithoutCallerReassignment)
	{
		const auto Asset = MakeAsset("/ThumbnailTests/RegistryKey/Asset", "DMaterial", 1, 10, 1);
		const auto Renderer = MakeThumbnailRenderer<FTestThumbnailRenderer>();
		const auto Before = CaptureThumbnailKeyInput(Asset, Renderer->GetClass());
		ASSERT_TRUE(Before);
		auto Package = *FindPackageExact(Asset.AssetPath.GetPackagePath());
		++Package.LastWriteTimeTicks;
		PublishTestPackage(std::move(Package));
		const auto After = CaptureThumbnailKeyInput(Asset, Renderer->GetClass());
		ASSERT_TRUE(After);
		EXPECT_EQ(Before->Asset, After->Asset);
		EXPECT_NE(BuildAssetThumbnailCacheKey(*Before), BuildAssetThumbnailCacheKey(*After));
	}

	TEST(FAssetThumbnailContractTests, CacheAdmissionRejectsMissingPackagesDependenciesAndWrongClasses)
	{
		const auto Renderer = MakeThumbnailRenderer<FTestThumbnailRenderer>();
		const FAssetData Missing{.AssetPath = MakeAssetPath("/ThumbnailTests/RegistryKey/Missing"), .AssetClassName = "DMaterial"};
		EXPECT_FALSE(CaptureThumbnailKeyInput(Missing, Renderer->GetClass()));
		const auto Asset = MakeAsset("/ThumbnailTests/RegistryKey/Owner", "DMaterial", 1, 10, 1);
		auto Wrong = Asset;
		Wrong.AssetClassName = "DWrongClass";
		EXPECT_FALSE(CaptureThumbnailKeyInput(Wrong, Renderer->GetClass()));
		auto Package = *FindPackageExact(Asset.AssetPath.GetPackagePath());
		Package.Dependencies = {Missing.AssetPath.GetPackagePath()};
		PublishTestPackage(std::move(Package));
		EXPECT_FALSE(CaptureThumbnailKeyInput(Asset, Renderer->GetClass()));
	}

	TEST(FAssetThumbnailContractTests, InitialFixtureAndBudgetContractsAreBounded)
	{
		const FThumbnailVisualContract Visual;
		EXPECT_EQ(Visual.Output.Width, Visual.Output.Height);
		EXPECT_EQ(Visual.Output.Width, 256u);
		EXPECT_EQ(FThumbnailVisualContract::SphereAssetPath,
			"/Engine/Models/Sphere.Sphere");
		EXPECT_EQ(FThumbnailVisualContract::OutputEncoding, "PNG");
		EXPECT_EQ(FThumbnailVisualContract::OutputColorSpace, "sRGB");
		EXPECT_TRUE(Visual.bOutputOpaque);
		EXPECT_EQ(Visual.CubeDirectionConvention,
			EAssetThumbnailCubeDirectionConvention::WorldSpaceReflectionVector);

		const FAssetThumbnailBudgets Budgets;
		EXPECT_EQ(Budgets.MaximumRendersPerFrame, 1u);
	}

	TEST(FAssetThumbnailContractTests, ObjectStorePersistsAndRejectsUnsafeKeys)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("Persistence");
		const Durin::FByteBuffer Payload = {
			std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
		{
			FThumbnailObjectStore Store({
				.CacheRoot = Root,
				.ObjectExtension = ".bin"});
			EXPECT_FALSE(Store.Store("../unsafe", Payload));
			ASSERT_TRUE(Store.Store("material-key-01", Payload));
		}
		FThumbnailObjectStore WarmStore({
			.CacheRoot = Root,
			.ObjectExtension = ".bin"});
		Durin::FByteBuffer Loaded;
		EXPECT_EQ(WarmStore.Load("material-key-01", Loaded), EThumbnailObjectLoadResult::Hit);
		EXPECT_EQ(Loaded, Payload);
		EXPECT_EQ(WarmStore.GetStats().CacheHits, 1u);
		EXPECT_EQ(WarmStore.Load("../unsafe", Loaded), EThumbnailObjectLoadResult::Invalid);
	}

	TEST(FAssetThumbnailContractTests, ObjectStoreCleansMissingObjectsAndHonorsDiskBudget)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("Cleanup");
		const Durin::FByteBuffer Payload(80, std::byte{7});
		{
			FThumbnailObjectStore Store({
				.CacheRoot = Root,
				.DiskBudgetBytes = 100,
				.ObjectExtension = ".bin"});
			ASSERT_TRUE(Store.Store("object-key-01", Payload));
			ASSERT_TRUE(Store.Store("object-key-02", Payload));
			EXPECT_EQ(Store.GetStats().Evictions, 1u);
			Durin::FByteBuffer Loaded;
			EXPECT_EQ(Store.Load("object-key-01", Loaded), EThumbnailObjectLoadResult::Miss);
			EXPECT_EQ(Store.Load("object-key-02", Loaded), EThumbnailObjectLoadResult::Hit);
		}
		for (const auto& Entry : std::filesystem::recursive_directory_iterator(Root / "Objects"))
			if (Entry.path().extension() == ".bin") std::filesystem::remove(Entry.path());
		FThumbnailObjectStore MissingObjectStore({
			.CacheRoot = Root,
			.DiskBudgetBytes = 100,
			.ObjectExtension = ".bin"});
		Durin::FByteBuffer MissingBytes;
		EXPECT_EQ(MissingObjectStore.Load("object-key-02", MissingBytes),
			EThumbnailObjectLoadResult::Invalid);
		EXPECT_GE(MissingObjectStore.GetStats().Regenerations, 1u);
	}

	TEST(FAssetThumbnailContractTests, BudgetSelectionEvictsOldestUnpinnedAllocations)
	{
		const std::vector<FThumbnailBudgetEntry> Entries = {
			{.Key = "visible", .Bytes = 60, .LastUsed = 1, .bPinned = true},
			{.Key = "oldest", .Bytes = 40, .LastUsed = 2},
			{.Key = "newest", .Bytes = 30, .LastUsed = 3},
		};
		EXPECT_EQ(SelectThumbnailBudgetEvictions(Entries, 100),
			std::vector<std::string>({"oldest"}));
		EXPECT_EQ(SelectThumbnailBudgetEvictions(Entries, 50),
			(std::vector<std::string>{"oldest", "newest"}));
		EXPECT_TRUE(SelectThumbnailBudgetEvictions(Entries, 200).empty());
	}

	TEST(FAssetThumbnailContractTests, RendererRegistryRejectsInvalidAndDuplicateRegistrations)
	{
		DThumbnailManager Registry;

		const auto NullRegistration = Durin::FThumbnailManagerTestAccess::Register(Registry, nullptr, "DMaterial");
		ASSERT_FALSE(NullRegistration);

		auto Invalid = Durin::MakeThumbnailRenderer<FTestThumbnailRenderer>();
		const auto InvalidRegistration = Durin::FThumbnailManagerTestAccess::Register(Registry, Invalid, "");
		ASSERT_FALSE(InvalidRegistration);

		auto Material = Durin::MakeThumbnailRenderer<FTestThumbnailRenderer>();
		ASSERT_TRUE(Durin::FThumbnailManagerTestAccess::Register(Registry, Material, "DMaterial"));
		EXPECT_EQ(Registry.Num(), 1u);

		auto Duplicate = Durin::MakeThumbnailRenderer<FTestThumbnailRenderer>();
		const auto DuplicateRegistration = Durin::FThumbnailManagerTestAccess::Register(Registry, Duplicate, "DMaterial");
		ASSERT_FALSE(DuplicateRegistration);
		EXPECT_EQ(Registry.Num(), 1u);
	}

	TEST(FAssetThumbnailContractTests, RendererRegistryUsesExactClassesAndReturnsRegisteredObjects)
	{
		DThumbnailManager Registry;

		auto Material = Durin::MakeThumbnailRenderer<FTestThumbnailRenderer>();
		auto MaterialInstance = Durin::MakeThumbnailRenderer<FTestThumbnailRenderer>();
		ASSERT_TRUE(Durin::FThumbnailManagerTestAccess::Register(Registry, Material, "DMaterial"));
		ASSERT_TRUE(Durin::FThumbnailManagerTestAccess::Register(Registry, MaterialInstance, "DMaterialInstance"));

		const auto* MaterialHandle = Registry.Find("DMaterial");
		const auto* InstanceHandle = Registry.Find("DMaterialInstance");
		EXPECT_TRUE(MaterialHandle);
		EXPECT_TRUE(InstanceHandle);
		EXPECT_NE(MaterialHandle, InstanceHandle);
		EXPECT_FALSE(Registry.Find("DMaterialInterface"));

		ASSERT_TRUE(Durin::FThumbnailManagerTestAccess::Unregister(Registry, "DMaterial"));
		EXPECT_FALSE(Registry.Find("DMaterial"));
		EXPECT_FALSE(Durin::FThumbnailManagerTestAccess::Unregister(Registry, "DMaterial"));

		auto Replacement = Durin::MakeThumbnailRenderer<FTestThumbnailRenderer>();
		ASSERT_TRUE(Durin::FThumbnailManagerTestAccess::Register(Registry, Replacement, "DMaterial"));
		EXPECT_EQ(Registry.Find("DMaterial"), Replacement.Get());
	}

	TEST(FAssetThumbnailContractTests, RendererRegistryShutdownDropsRenderersAndClosesRegistration)
	{
		DThumbnailManager Registry;

		auto Renderer = Durin::MakeThumbnailRenderer<FTestThumbnailRenderer>();
		ASSERT_TRUE(Durin::FThumbnailManagerTestAccess::Register(Registry, Renderer, "DTextureCube"));

		Registry.Shutdown();
		EXPECT_TRUE(Registry.IsShuttingDown());
		EXPECT_EQ(Registry.Num(), 0u);
		EXPECT_FALSE(Registry.Find("DTextureCube"));
		EXPECT_FALSE(Durin::FThumbnailManagerTestAccess::Register(Registry, Renderer, "DTextureCube"));
	}

	TEST(FAssetThumbnailContractTests, ScopedRendererRegistrationRejectsDuplicatesAndAllowsLaterReplacement)
	{
		DThumbnailManager Registry;

		auto FirstState = std::make_shared<FFakeThumbnailRendererState>();
		auto First = Durin::FThumbnailManagerTestAccess::RegisterScoped(Registry,
			Durin::MakeThumbnailRenderer<FFakeThumbnailRenderer>(FirstState), "DFakeRenderedAsset");
		ASSERT_TRUE(First);
		const TWeakObjectPtr<DThumbnailRenderer> FirstRenderer(Registry.Find("DFakeRenderedAsset"));

		auto DuplicateState = std::make_shared<FFakeThumbnailRendererState>();
		auto Duplicate = Durin::FThumbnailManagerTestAccess::RegisterScoped(Registry,
			Durin::MakeThumbnailRenderer<FFakeThumbnailRenderer>(DuplicateState), "DFakeRenderedAsset");
		ASSERT_FALSE(Duplicate);
		CollectGarbage(Durin::EObjectFlags::Standalone);
		EXPECT_EQ(DuplicateState->ExtensionDestructions, 1u);
		EXPECT_EQ(Registry.Find("DFakeRenderedAsset"), FirstRenderer.Get());

		First.Reset();
		EXPECT_FALSE(Registry.Find("DFakeRenderedAsset"));
		CollectGarbage(Durin::EObjectFlags::Standalone);
		EXPECT_EQ(FirstState->ExtensionDestructions, 1u);
		auto ReplacementState = std::make_shared<FFakeThumbnailRendererState>();
		auto Replacement = Durin::FThumbnailManagerTestAccess::RegisterScoped(Registry,
			Durin::MakeThumbnailRenderer<FFakeThumbnailRenderer>(ReplacementState), "DFakeRenderedAsset");
		ASSERT_TRUE(Replacement);
		EXPECT_TRUE(Registry.Find("DFakeRenderedAsset"));
		EXPECT_FALSE(FirstRenderer.IsValid());
	}

	TEST(FAssetThumbnailContractTests, RetiredRegistrationHandleCannotRemoveItsReplacement)
	{
		DThumbnailManager Manager;
		const FThumbnailRenderingInfo Info{.AssetClassName = "DReplacedRenderer"};
		auto FirstRenderer = MakeThumbnailRenderer<FTestThumbnailRenderer>();
		auto First = FThumbnailManagerTestAccess::RegisterScoped(Manager, FirstRenderer, "DReplacedRenderer");
		ASSERT_TRUE(First);
		ASSERT_TRUE(FThumbnailManagerTestAccess::Unregister(Manager, Info.AssetClassName));
		auto ReplacementRenderer = MakeThumbnailRenderer<FTestThumbnailRenderer>();
		auto Replacement = FThumbnailManagerTestAccess::RegisterScoped(Manager, ReplacementRenderer, "DReplacedRenderer");
		ASSERT_TRUE(Replacement);
		First.Reset();
		EXPECT_EQ(Manager.Find(Info.AssetClassName), ReplacementRenderer.Get());
	}

	TEST(FAssetThumbnailContractTests, RequiredSceneCanDeferBoundedThumbnailAdmissions)
	{
		DThumbnailManager Manager;
		auto Renderer = MakeThumbnailRenderer<FTestThumbnailRenderer>();
		auto Registration = FThumbnailManagerTestAccess::RegisterScoped(Manager, Renderer, "DFakeRenderedAsset");
		ASSERT_TRUE(Registration);
		FAssetThumbnailBudgets Budgets;
		Budgets.MaximumAdmissionsPerFrame = 1;
		Budgets.FrameTimeAllowanceSeconds = 1.0;
		FAssetThumbnailPool Pool(Manager, Budgets, {.CacheRoot = MakeObjectStoreRoot("DeferredSceneAdmission")});
		const auto First = MakeThumbnailRequest("/ThumbnailTests/ScenePriority/First", "DFakeRenderedAsset");
		const auto Second = MakeThumbnailRequest("/ThumbnailTests/ScenePriority/Second", "DFakeRenderedAsset");
		Pool.AccessTexture(First.Asset);
		Pool.AccessTexture(Second.Asset);
		Pool.Tick(false);
		EXPECT_EQ(Pool.GetStats().QueuedJobs, 2u);
		EXPECT_EQ(Pool.GetStats().Generation.Jobs, 0u);
		Pool.Tick();
		EXPECT_EQ(Pool.GetStats().QueuedJobs, 1u);
		EXPECT_EQ(Pool.GetStats().Generation.Jobs, 1u);
	}

	TEST(FAssetThumbnailContractTests, AssetThumbnailReferencesCoalesceAndReleasePoolPins)
	{
		DThumbnailManager Manager;

		auto State = std::make_shared<FFakeThumbnailRendererState>();
		auto Registration = Durin::FThumbnailManagerTestAccess::RegisterScoped(Manager,
			Durin::MakeThumbnailRenderer<FFakeThumbnailRenderer>(State), "DFakeRenderedAsset");
		ASSERT_TRUE(Registration);
		FAssetThumbnailPool Pool(
			Manager, {}, {.CacheRoot = MakeObjectStoreRoot("AssetThumbnailReferences")});
		const FAssetThumbnailRequest Request = MakeThumbnailRequest(
			"/ThumbnailTests/References/Shared", "DFakeRenderedAsset");
		{
			FAssetThumbnail First(Request.Asset, &Pool);
			FAssetThumbnail Second(Request.Asset, &Pool);
			First.AccessTexture(EAssetThumbnailPriority::Prefetch);
			Second.AccessTexture(EAssetThumbnailPriority::Visible);
			const FAssetThumbnailPoolStats Stats = Pool.GetStats();
			EXPECT_EQ(Stats.QueuedJobs, 1u);
			EXPECT_EQ(Stats.PinnedEntries, 1u);
			EXPECT_EQ(Stats.Referencers, 2u);
		}
		EXPECT_EQ(Pool.GetStats().PinnedEntries, 0u);
		EXPECT_EQ(Pool.GetStats().Referencers, 0u);
	}

	TEST(FAssetThumbnailContractTests, PoolEvictsUnreferencedMetadataEntriesToBudget)
	{
		DThumbnailManager Manager;
		FAssetThumbnailBudgets Budgets;
		Budgets.MaximumRetainedEntries = 2;
		FAssetThumbnailPool Pool(
			Manager,
			Budgets,
			{.CacheRoot = MakeObjectStoreRoot("RetainedEntryBudget")});

		for (uint32 Index = 0; Index < 3; ++Index)
		{
			Pool.AccessTexture(
				MakeAsset(
					std::format("/ThumbnailTests/Retained/Asset{}", Index),
					"DUnsupportedAsset",
					1,
					100 + Index,
					200 + Index),
				EAssetThumbnailPriority::Visible);
		}
		Pool.Tick();

		EXPECT_EQ(Pool.GetStats().RetainedEntries, 3u);
		Pool.Tick();
		EXPECT_EQ(Pool.GetStats().RetainedEntries, 2u);
	}

	TEST(FAssetThumbnailContractTests, PoolRejectsWrongSizedPersistentThumbnailBeforeUpload)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("WrongSizedWarmObject");
		DThumbnailManager Manager;

		auto Renderer = Durin::MakeThumbnailRenderer<FTestThumbnailRenderer>();
		ASSERT_TRUE(Durin::FThumbnailManagerTestAccess::Register(Manager, Renderer, "DMaterial"));
		const FAssetThumbnailRequest Request = MakeThumbnailRequest(
			"/ThumbnailTests/WrongSizedWarm/Material", "DMaterial");

		std::string CacheKey;
		{
			CacheKey = BuildAssetThumbnailCacheKey(*CaptureThumbnailKeyInput(Request.Asset, Renderer->GetClass()));
		}
		{
			FThumbnailObjectStore Store({.CacheRoot = Root});
			ASSERT_TRUE(Store.Store(
				CacheKey, std::as_bytes(std::span{WrongSizedThumbnailPng})));
		}

		FAssetThumbnailPool Pool(Manager, {}, {.CacheRoot = Root});

		Pool.AccessTexture(Request.Asset, Request.Priority);
		Pool.Tick();
		ASSERT_TRUE(PumpThumbnailPoolUntil(Pool, [&] {
			return Pool.GetStats().Generation.Retries == 1;
		}));

		EXPECT_EQ(Pool.GetStats().Generation.Retries, 1u);
		EXPECT_EQ(Pool.GetStats().Generation.Loads, 1u);
		EXPECT_EQ(Pool.GetStats().UploadsQueued, 0u);
		FThumbnailObjectStore Store({.CacheRoot = Root});
		Durin::FByteBuffer Bytes;
		EXPECT_EQ(Store.Load(CacheKey, Bytes), EThumbnailObjectLoadResult::Miss);
	}

	TEST(FAssetThumbnailContractTests, RenderedCachesResolveOneServiceBeforeAndAfterRegistration)
	{
		DThumbnailManager ThumbnailManager;
		FAssetThumbnailPool BeforeRegistration(
			ThumbnailManager,
			{},
			{.CacheRoot = MakeObjectStoreRoot("CacheBeforeRegistration"),
				.ObjectExtension = ".bin"});

		auto State = std::make_shared<FFakeThumbnailRendererState>();
		auto Registration = Durin::FThumbnailManagerTestAccess::RegisterScoped(ThumbnailManager,
			Durin::MakeThumbnailRenderer<FFakeThumbnailRenderer>(State), "DFakeRenderedAsset");
		ASSERT_TRUE(Registration);
		FAssetThumbnailPool AfterRegistration(
			ThumbnailManager,
			{},
			{.CacheRoot = MakeObjectStoreRoot("CacheAfterRegistration"),
				.ObjectExtension = ".bin"});
		const FAssetThumbnailRequest BeforeRequest = MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/CacheBeforeRegistration",
			"DFakeRenderedAsset");
		const FAssetThumbnailRequest AfterRequest = MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/CacheAfterRegistration",
			"DFakeRenderedAsset");
		BeforeRegistration.AccessTexture(
			BeforeRequest.Asset, EAssetThumbnailPriority::Visible);
		AfterRegistration.AccessTexture(
			AfterRequest.Asset, EAssetThumbnailPriority::Visible);
		EXPECT_EQ(
			BeforeRegistration.Find(BeforeRequest.Asset.AssetPath).State,
			EAssetThumbnailState::Pending);
		EXPECT_EQ(
			AfterRegistration.Find(AfterRequest.Asset.AssetPath).State,
			EAssetThumbnailState::Pending);

		Registration.Reset();
		BeforeRegistration.Tick();
		AfterRegistration.Tick();
		EXPECT_EQ(BeforeRegistration.GetStats().QueuedJobs, 0u);
		EXPECT_EQ(AfterRegistration.GetStats().QueuedJobs, 0u);
		CollectGarbage(Durin::EObjectFlags::Standalone);
		EXPECT_EQ(State->ExtensionDestructions, 1u);
	}

	TEST(FAssetThumbnailContractTests, ParkedResourceWaitDoesNotBlockLaterRenderedWork)
	{
		DThumbnailManager ThumbnailManager;

		auto WaitingState = std::make_shared<FFakeThumbnailRendererState>();
		WaitingState->ResourcePollsBeforeReady = 100;
		auto ReadyState = std::make_shared<FFakeThumbnailRendererState>();
		auto WaitingRegistration = Durin::FThumbnailManagerTestAccess::RegisterScoped(ThumbnailManager,
			Durin::MakeThumbnailRenderer<FFakeThumbnailRenderer>(WaitingState), "DWaitingRenderedAsset");
		ASSERT_TRUE(WaitingRegistration);
		auto ReadyRegistration = Durin::FThumbnailManagerTestAccess::RegisterScoped(ThumbnailManager,
			Durin::MakeThumbnailRenderer<FFakeThumbnailRenderer>(ReadyState), "DReadyRenderedAsset");
		ASSERT_TRUE(ReadyRegistration);
		FAssetThumbnailBudgets Budgets;
		Budgets.ResourcePollIntervalFrames = 4;
		FAssetThumbnailPool Cache(
			ThumbnailManager,
			Budgets,
			{.CacheRoot = MakeObjectStoreRoot("ParkedResourceWait"),
				.ObjectExtension = ".bin"});
		const FAssetThumbnailRequest Waiting = MakeThumbnailRequest(
			"/ThumbnailTests/Parked/Waiting", "DWaitingRenderedAsset",
			EAssetThumbnailPriority::Visible);
		const FAssetThumbnailRequest Ready = MakeThumbnailRequest(
			"/ThumbnailTests/Parked/Ready", "DReadyRenderedAsset",
			EAssetThumbnailPriority::Visible);

		Cache.AccessTexture(Waiting.Asset, Waiting.Priority);
		Cache.Tick();
		ASSERT_TRUE(PumpThumbnailPoolUntil(Cache, [&] {
			return Cache.GetStats().ParkedResourceWaits == 1;
		}));
		EXPECT_EQ(Cache.Find(Waiting.Asset.AssetPath).State,
			EAssetThumbnailState::Pending);
		EXPECT_EQ(Cache.GetStats().ParkedResourceWaits, 1u);
		EXPECT_FALSE(Cache.GetStats().bHasActiveJob);

		Cache.AccessTexture(Ready.Asset, Ready.Priority);
		Cache.Tick();
		ASSERT_TRUE(PumpThumbnailPoolUntil(Cache, [&] {
			return ReadyState->PreviewPreparations == 1;
		}));
		EXPECT_EQ(Cache.Find(Waiting.Asset.AssetPath).State,
			EAssetThumbnailState::Pending);
		EXPECT_EQ(Cache.GetStats().ParkedResourceWaits, 1u);
		EXPECT_EQ(Cache.GetStats().PeakParkedResourceWaits, 1u);
	}

	TEST(FAssetThumbnailContractTests, SharedPoolInvalidatesCapturedDependenciesWithoutBrowserSnapshots)
	{
		DThumbnailManager Registry;
		auto OwnerState = std::make_shared<FFakeThumbnailRendererState>();
		auto OtherState = std::make_shared<FFakeThumbnailRendererState>();
		OwnerState->ResourcePollsBeforeReady = OtherState->ResourcePollsBeforeReady = 1000;
		const auto Base = MakeAsset("/ThumbnailTests/Dependencies/Base", "DMaterial", 1, 100, 1);
		auto OwnerRegistration = Durin::FThumbnailManagerTestAccess::RegisterScoped(Registry, Durin::MakeThumbnailRenderer<FFakeThumbnailRenderer>(OwnerState), "DFakeRenderedAsset");
		auto OtherRegistration = Durin::FThumbnailManagerTestAccess::RegisterScoped(Registry, Durin::MakeThumbnailRenderer<FFakeThumbnailRenderer>(OtherState), "DOtherAsset");
		ASSERT_TRUE(OwnerRegistration);
		ASSERT_TRUE(OtherRegistration);
		FAssetThumbnailPool Pool(Registry, {.ResourcePollIntervalFrames = 1, .MaximumResourceWaitFrames = 0},
			{.CacheRoot = MakeObjectStoreRoot("SharedDependencies")});
		const auto Owner = MakeThumbnailRequest("/ThumbnailTests/Materials/Owner", "DFakeRenderedAsset");
		const auto Other = MakeThumbnailRequest("/ThumbnailTests/Materials/Other", "DOtherAsset");
		auto OwnerPackage = *FindPackageExact(Owner.Asset.AssetPath.GetPackagePath());
		OwnerPackage.Dependencies = {Base.AssetPath.GetPackagePath()};
		PublishTestPackage(std::move(OwnerPackage));
		FAssetThumbnail OwnerReference(Owner.Asset, &Pool), OtherReference(Other.Asset, &Pool);
		OwnerReference.AccessTexture(EAssetThumbnailPriority::Visible);
		OtherReference.AccessTexture(EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(PumpThumbnailPoolUntil(Pool, [&] { return Pool.GetStats().ParkedResourceWaits == 2; }));
		const auto Resolved = FMountPaths::ResolveAssetPath(Base.AssetPath.GetPackagePath().GetView());
		ASSERT_TRUE(Resolved);
		const auto Physical = Resolved->PhysicalPath.generic_string() + ".dasset";
		Pool.ApplyContentChanges({.Changes = {{EContentChangeKind::Removed, Physical}}});
		EXPECT_EQ(Pool.GetStats().ParkedResourceWaits, 1u);
		EXPECT_EQ(OwnerReference.GetView().State, EAssetThumbnailState::None);
		EXPECT_EQ(OtherReference.GetView().State, EAssetThumbnailState::Pending);
		Pool.ApplyContentChanges({.bFullRefresh = true});
		EXPECT_EQ(Pool.GetStats().ParkedResourceWaits, 0u);
		EXPECT_EQ(OtherReference.GetView().State, EAssetThumbnailState::None);
	}

	TEST(FAssetThumbnailContractTests, LastReferencerRetiresParkedDrawWithoutCancellingOtherConsumers)
	{
		DThumbnailManager Manager;

		auto State = std::make_shared<FFakeThumbnailRendererState>();
		State->ResourcePollsBeforeReady = 1000;
		auto Registration = Durin::FThumbnailManagerTestAccess::RegisterScoped(Manager,
			Durin::MakeThumbnailRenderer<FFakeThumbnailRenderer>(State), "DNavigationAsset");
		ASSERT_TRUE(Registration);
		FAssetThumbnailPool Pool(Manager, {},
			{.CacheRoot = MakeObjectStoreRoot("NavigationCancel"), .ObjectExtension = ".bin"});
		const auto Request = MakeThumbnailRequest("/ThumbnailTests/Navigation/Asset",
			"DNavigationAsset", EAssetThumbnailPriority::Visible);
		Pool.AddReferencer(Request.Asset.AssetPath);
		Pool.AddReferencer(Request.Asset.AssetPath);

		Pool.AccessTexture(Request.Asset, Request.Priority);
		Pool.Tick();
		ASSERT_TRUE(PumpThumbnailPoolUntil(Pool, [&] { return Pool.GetStats().ParkedResourceWaits == 1; }));
		Pool.RemoveReferencer(Request.Asset.AssetPath);
		EXPECT_EQ(Pool.GetStats().ParkedResourceWaits, 1u);
		Pool.RemoveReferencer(Request.Asset.AssetPath);
		EXPECT_EQ(Pool.GetStats().ParkedResourceWaits, 0u);
		Pool.AddReferencer(Request.Asset.AssetPath);

		Pool.AccessTexture(Request.Asset, Request.Priority);
		Pool.Tick();
		ASSERT_TRUE(PumpThumbnailPoolUntil(Pool, [&] { return Pool.GetStats().ParkedResourceWaits == 1; }));
		Pool.RemoveReferencer(Request.Asset.AssetPath);
	}

	TEST(FAssetThumbnailContractTests, ParkedResourceWaitTimesOutAndRetiresPendingWork)
	{
		DThumbnailManager ThumbnailManager;

		auto State = std::make_shared<FFakeThumbnailRendererState>();
		State->ResourcePollsBeforeReady = 100;
		auto Registration = Durin::FThumbnailManagerTestAccess::RegisterScoped(ThumbnailManager,
			Durin::MakeThumbnailRenderer<FFakeThumbnailRenderer>(State), "DTimeoutRenderedAsset");
		ASSERT_TRUE(Registration);
		FAssetThumbnailBudgets Budgets;
		Budgets.ResourcePollIntervalFrames = 1;
		Budgets.MaximumResourceWaitFrames = 2;
		FAssetThumbnailPool Cache(
			ThumbnailManager,
			Budgets,
			{.CacheRoot = MakeObjectStoreRoot("ParkedResourceTimeout"),
				.ObjectExtension = ".bin"});
		const FAssetThumbnailRequest Request = MakeThumbnailRequest(
			"/ThumbnailTests/Parked/Timeout", "DTimeoutRenderedAsset",
			EAssetThumbnailPriority::Visible);

		Cache.AccessTexture(Request.Asset, Request.Priority);
		Cache.Tick();
		ASSERT_TRUE(PumpThumbnailPoolUntil(Cache, [&] {
			return Cache.Find(Request.Asset.AssetPath).State == EAssetThumbnailState::Failed;
		}));

		const FAssetThumbnailView View =
			Cache.Find(Request.Asset.AssetPath);
		EXPECT_EQ(View.State, EAssetThumbnailState::Failed);
		EXPECT_NE(View.Diagnostic.find("Timed out"), std::string::npos);
		EXPECT_EQ(Cache.GetStats().ParkedResourceWaits, 0u);
		EXPECT_EQ(Cache.GetStats().ResourceWaitTimeouts, 1u);
	}

	TEST(FAssetThumbnailContractTests, FailedDrawWithoutDiagnosticBecomesTerminal)
	{
		DThumbnailManager Manager;
		auto Renderer = MakeThumbnailRenderer<FTestThumbnailRenderer>();
		auto Registration = FThumbnailManagerTestAccess::RegisterScoped(Manager, Renderer, "DFailedDraw");
		ASSERT_TRUE(Registration);
		FAssetThumbnailPool Pool(Manager, {}, {.CacheRoot = MakeObjectStoreRoot("FailedDraw")});
		const auto Request = MakeThumbnailRequest("/ThumbnailTests/FailedDraw", "DFailedDraw");
		Pool.AccessTexture(Request.Asset, EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(PumpThumbnailPoolUntil(Pool, [&] {
			return Pool.Find(Request.Asset.AssetPath).State == EAssetThumbnailState::Failed;
		}));
		EXPECT_FALSE(Pool.Find(Request.Asset.AssetPath).Diagnostic.empty());
		EXPECT_FALSE(Pool.GetStats().bHasActiveJob);
	}

	TEST(FAssetThumbnailContractTests, QueueCoalescesPromotesAndBoundsPendingDemand)
	{
		FAssetThumbnailRequestQueue Queue(2);
		auto First = MakeThumbnailRequest("/ThumbnailTests/Queue/First", "DMaterial");
		auto Second = MakeThumbnailRequest("/ThumbnailTests/Queue/Second", "DMaterial");
		ASSERT_TRUE(Queue.Request(First));
		ASSERT_TRUE(Queue.Request(Second));
		Second.Asset.AssetClassName += "Updated";
		Second.Priority = EAssetThumbnailPriority::Visible;
		ASSERT_TRUE(Queue.Request(Second));
		EXPECT_EQ(Queue.Num(), 2u);
		EXPECT_FALSE(Queue.Request(MakeThumbnailRequest("/ThumbnailTests/Queue/Overflow", "DMaterial")));
		const auto Next = Queue.TakeNext();
		ASSERT_TRUE(Next);
		EXPECT_EQ(Next->Asset, Second.Asset);
		EXPECT_EQ(Queue.Num(), 1u);
		Queue.Remove(First.Asset.AssetPath);
		EXPECT_FALSE(Queue.TakeNext());
	}

	TEST(FAssetThumbnailContractTests, PoolRejectsUnsupportedAndInvalidAssetsWithoutLoading)
	{
		DThumbnailManager Manager;
		FAssetThumbnailPool Pool(Manager, {}, {.CacheRoot = MakeObjectStoreRoot("AdmissionFailure")});
		const auto Missing = MakeThumbnailRequest("/ThumbnailTests/Unsupported", "DUnsupported");
		EXPECT_EQ(Pool.AccessTexture(Missing.Asset).State, EAssetThumbnailState::None);
		EXPECT_EQ(Pool.GetStats().QueuedJobs, 0u);
		auto Renderer = MakeThumbnailRenderer<FTestThumbnailRenderer>();
		ASSERT_TRUE(FThumbnailManagerTestAccess::Register(Manager, Renderer, "DMaterial"));
		const FAssetData Invalid{.AssetPath = MakeAssetPath("/ThumbnailTests/Unregistered/Invalid"), .AssetClassName = "DMaterial"};
		Pool.AccessTexture(Invalid);
		Pool.Tick();
		const auto View = Pool.Find(Invalid.AssetPath);
		EXPECT_EQ(View.State, EAssetThumbnailState::Failed);
		EXPECT_FALSE(View.Diagnostic.empty());
		EXPECT_EQ(Pool.GetStats().Generation.Loads, 0u);
		Pool.AccessTexture(Invalid);
		Pool.Tick();
		EXPECT_EQ(Pool.GetStats().Generation.Failures, 1u);
	}

	TEST(FAssetThumbnailContractTests, CancelledDemandCanReuseTheSamePathWithoutAdoptingOldWork)
	{
		DThumbnailManager Manager;
		auto State = std::make_shared<FFakeThumbnailRendererState>();
		State->ResourcePollsBeforeReady = 10000;
		auto Registration = FThumbnailManagerTestAccess::RegisterScoped(Manager, MakeThumbnailRenderer<FFakeThumbnailRenderer>(State), "DFakeRenderedAsset");
		FAssetThumbnailBudgets Budgets;
		Budgets.MaximumAdmissionsPerFrame = 1;
		Budgets.FrameTimeAllowanceSeconds = 1.0;
		FAssetThumbnailPool Pool(Manager, Budgets, {.CacheRoot = MakeObjectStoreRoot("ReusedDemand")});
		const auto Request = MakeThumbnailRequest("/ThumbnailTests/ReusedDemand/Asset", "DFakeRenderedAsset");
		Pool.AccessTexture(Request.Asset);
		Pool.Tick();
		Pool.CancelPendingRequests();
		Pool.Tick(false);
		EXPECT_EQ(State->ResourcePolls, 0u);
		EXPECT_EQ(Pool.Find(Request.Asset.AssetPath).State, EAssetThumbnailState::None);
		Pool.AccessTexture(Request.Asset);
		ASSERT_TRUE(PumpThumbnailPoolUntil(Pool, [&] { return Pool.GetStats().ParkedResourceWaits == 1; }));
		Pool.Refresh(Request.Asset.AssetPath);
		EXPECT_EQ(Pool.GetStats().ParkedResourceWaits, 0u);
		Pool.AccessTexture(Request.Asset);
		ASSERT_TRUE(PumpThumbnailPoolUntil(Pool, [&] { return Pool.GetStats().ParkedResourceWaits == 1; }));
		Manager.Shutdown();
		EXPECT_EQ(Pool.AccessTexture(Request.Asset).State, EAssetThumbnailState::None);
		EXPECT_EQ(Pool.GetStats().QueuedJobs, 0u);
	}

	auto ReadCachedPixels(FAssetThumbnailCache& Cache, const std::string& Key, uint32 Size = 1) -> FThumbnailCacheReadResult
	{
		auto Read = Cache.Load(Key, {.Width = Size, .Height = Size});
		Cache.WaitForTasksForTesting();
		return Read.Consume();
	}
	TEST(FAssetThumbnailContractTests, CachePersistsPixelsAcrossInstances)
	{
		const auto Root = MakeObjectStoreRoot("PixelCacheRoundTrip");
		const std::string Key(64, 'a');
		const FByteBuffer Pixels(4, std::byte{127});
		{
			FAssetThumbnailCache Cache({.CacheRoot = Root});
			Cache.Save(Key, Pixels, 1, 1);
		}
		FAssetThumbnailCache Cache({.CacheRoot = Root});
		EXPECT_EQ(ReadCachedPixels(Cache, Key).Pixels, Pixels);
	}
	TEST(FAssetThumbnailContractTests, CacheRejectsAndInvalidatesWrongSizedPixels)
	{
		const auto Root = MakeObjectStoreRoot("PixelCacheWrongSize");
		const std::string Key(64, 'b');
		FAssetThumbnailCache Cache({.CacheRoot = Root});
		Cache.Save(Key, FByteBuffer(4, std::byte{255}), 1, 1);
		Cache.WaitForTasksForTesting();
		const auto Wrong = ReadCachedPixels(Cache, Key, 2);
		EXPECT_TRUE(Wrong.bInvalid);
		EXPECT_TRUE(Wrong.Pixels.empty());
		EXPECT_TRUE(ReadCachedPixels(Cache, Key).Pixels.empty());
	}
	TEST(FAssetThumbnailContractTests, AcceptedCacheWriteOwnsPixelsAfterCallerReleasesBuffer)
	{
		FAssetThumbnailCache Cache({.CacheRoot = MakeObjectStoreRoot("PixelCacheDetachedWrite")});
		const std::string Key(64, 'c');
		{
			const FByteBuffer Pixels(4, std::byte{255});
			Cache.Save(Key, Pixels, 1, 1);
		}
		Cache.WaitForTasksForTesting();
		EXPECT_EQ(ReadCachedPixels(Cache, Key).Pixels, FByteBuffer(4, std::byte{255}));
	}

	TEST(FAssetThumbnailContractTests, CacheReadAndWriteBudgetsRejectOversizedPixels)
	{
		FAssetThumbnailCache Cache({.CacheRoot = MakeObjectStoreRoot("PixelCacheBudget")}, 4);
		const std::string Key(64, 'd');
		Cache.Save(Key, FByteBuffer(16), 2, 2);
		Cache.WaitForTasksForTesting();
		EXPECT_TRUE(ReadCachedPixels(Cache, Key, 2).Pixels.empty());
		EXPECT_TRUE(ReadCachedPixels(Cache, Key, std::numeric_limits<uint32>::max()).Pixels.empty());
		EXPECT_TRUE(ReadCachedPixels(Cache, Key).Pixels.empty());
	}

	TEST(FAssetThumbnailContractTests, PreparationPressureDefersInsteadOfFailingAssets)
	{
		DThumbnailManager Manager;
		auto State = std::make_shared<FFakeThumbnailRendererState>();
		State->ResourcePollsBeforeReady = 10000;
		auto Registration = Durin::FThumbnailManagerTestAccess::RegisterScoped(Manager, Durin::MakeThumbnailRenderer<FFakeThumbnailRenderer>(State), "DFakeRenderedAsset");
		ASSERT_TRUE(Registration);
		FAssetThumbnailBudgets Budgets;
		Budgets.MaximumParkedRenderedJobs = 2;
		Budgets.FrameTimeAllowanceSeconds = 1.0;
		FAssetThumbnailPool Pool(Manager, Budgets,
			{.CacheRoot = MakeObjectStoreRoot("PreparationPressure")});
		std::vector<FAssetData> Assets;

		for (uint32 Index = 0; Index < 5; ++Index)
		{
			Assets.push_back(MakeAsset(std::format("/ThumbnailTests/Pressure/Asset{}", Index),
				"DFakeRenderedAsset", 1, 32, 1));
			Pool.AccessTexture(Assets.back(), EAssetThumbnailPriority::Visible);
		}
		Pool.Tick();
		ASSERT_TRUE(PumpThumbnailPoolUntil(Pool, [&] { return Pool.GetStats().ParkedResourceWaits == 2; }));
		for (uint32 Frame = 0; Frame < 16; ++Frame)
		{

			Pool.Tick();
		}
		EXPECT_EQ(Pool.GetStats().ParkedResourceWaits, 2u);
		EXPECT_EQ(Pool.GetStats().Generation.Failures, 0u);
		for (const auto& Asset : Assets)
			EXPECT_NE(Pool.Find(Asset.AssetPath).State, EAssetThumbnailState::Failed);
		Pool.CancelPendingRequests();
	}

	TEST(FAssetThumbnailContractTests, BackgroundPrefetchSurvivesLastUIReferencer)
	{
		DThumbnailManager Manager;
		auto State = std::make_shared<FFakeThumbnailRendererState>();
		State->ResourcePollsBeforeReady = 10000;
		auto Registration = Durin::FThumbnailManagerTestAccess::RegisterScoped(Manager, Durin::MakeThumbnailRenderer<FFakeThumbnailRenderer>(State), "DFakeRenderedAsset");
		ASSERT_TRUE(Registration);
		FAssetThumbnailPool Pool(Manager, {},
			{.CacheRoot = MakeObjectStoreRoot("BackgroundInterest")});
		const auto Asset = MakeAsset("/ThumbnailTests/Background/Interest", "DFakeRenderedAsset", 1, 32, 1);
		Pool.AddReferencer(Asset.AssetPath);
		Pool.Prefetch(Asset);
		ASSERT_TRUE(PumpThumbnailPoolUntil(Pool, [&] { return Pool.GetStats().ParkedResourceWaits == 1; }));
		Pool.RemoveReferencer(Asset.AssetPath);
		EXPECT_EQ(Pool.GetStats().ParkedResourceWaits, 1u);
		Pool.CancelPendingRequests();
	}

	TEST(FAssetThumbnailContractTests, CurrentFrameVisibilityReordersPreviouslyVisibleRequests)
	{
		FAssetThumbnailRequestQueue Queue;
		const auto First = MakeThumbnailRequest("/ThumbnailTests/Priority/First", "DTest",
			EAssetThumbnailPriority::Visible);
		const auto Second = MakeThumbnailRequest("/ThumbnailTests/Priority/Second", "DTest",
			EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(Queue.Request(First));
		ASSERT_TRUE(Queue.Request(Second));
		Queue.BeginFrame();
		Queue.Prioritize(Second.Asset.AssetPath);
		const auto Next = Queue.TakeNext();
		ASSERT_TRUE(Next);
		EXPECT_EQ(Next->Asset.AssetPath, Second.Asset.AssetPath);
	}

	TEST(FAssetThumbnailContractTests, BatchedIndexFlushPreservesReplacementAndEvictionAccounting)
	{
		const auto Root = MakeObjectStoreRoot("BatchedIndex");
		FThumbnailObjectStore Store({.CacheRoot = Root, .DiskBudgetBytes = 10, .bBatchIndexWrites = true});
		ASSERT_TRUE(Store.Store("aa", FByteBuffer(8, std::byte{1})));
		ASSERT_TRUE(Store.Store("bb", FByteBuffer(8, std::byte{2})));
		EXPECT_EQ(Store.GetStats().Evictions, 1u);
		Store.Flush();
		{
			FThumbnailObjectStore Reopened({.CacheRoot = Root, .DiskBudgetBytes = 10});
			FByteBuffer Bytes;
			EXPECT_EQ(Reopened.Load("aa", Bytes), EThumbnailObjectLoadResult::Miss);
			EXPECT_EQ(Reopened.Load("bb", Bytes), EThumbnailObjectLoadResult::Hit);
			EXPECT_EQ(Bytes, FByteBuffer(8, std::byte{2}));
		}
		ASSERT_TRUE(Store.Store("bb", FByteBuffer(3, std::byte{3})));
		ASSERT_TRUE(Store.Store("cc", FByteBuffer(7, std::byte{4})));
		EXPECT_EQ(Store.GetStats().Evictions, 1u);
		Store.Flush();
		FThumbnailObjectStore Reopened({.CacheRoot = Root, .DiskBudgetBytes = 10});
		FByteBuffer Bytes;
		EXPECT_EQ(Reopened.Load("bb", Bytes), EThumbnailObjectLoadResult::Hit);
		EXPECT_EQ(Bytes, FByteBuffer(3, std::byte{3}));
		EXPECT_EQ(Reopened.Load("cc", Bytes), EThumbnailObjectLoadResult::Hit);
	}
} // namespace Durin
