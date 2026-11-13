#include <gtest/gtest.h>

#include "EngineTestSupport.h"
#include "Image/ImageDecoder.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/ThumbnailStorage.h"
#include "Editor/DurinEd/Private/Thumbnail/AssetThumbnailRequestQueue.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "Editor/DurinEd/Private/Thumbnail/AssetThumbnailGeneration.h"
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
				Pool.BeginFrame();
				Pool.EndFrame();
			}
			return IsDone();
		}

		auto StartThumbnailWork(FAssetThumbnailGeneration& Pipeline) -> FAssetThumbnailStartResult
		{
			auto Result = Pipeline.StartNextDetailed();
			if (!Result.ColdJob && !Result.WarmJob && Result.bProgressed)
			{
				Pipeline.WaitForCacheTasksForTesting();
				Result = Pipeline.StartNextDetailed();
			}
			return Result;
		}

		auto CompleteThumbnailPixels(FAssetThumbnailGeneration& Pipeline, FAssetThumbnailJob& Job,
			FByteView Pixels, uint32 Width, uint32 Height) -> bool
		{
			Pipeline.BeginFrame();
			return Pipeline.CompleteLoad(Job) && Pipeline.BeginRender(Job, true)
				&& Pipeline.CompleteRender(Job) && Pipeline.CompleteReadback(Job)
				&& Pipeline.CompletePixels(Job, Pixels, Width, Height);
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

		auto MakePackage(
			std::string_view Path,
			std::string AssetClassName,
			uint32 FormatVersion,
			uint64 FileSize,
			int64 LastWriteTimeTicks) -> FAssetThumbnailPackageFingerprint
		{
			const FPackagePath PackagePath = MakePath(Path);
			FTopLevelAssetPath AssetPath;
			EXPECT_TRUE(FTopLevelAssetPath::TryCreate(
				PackagePath, PackagePath.GetPackageName(), AssetPath));
			return {
				.AssetPath = std::move(AssetPath),
				.PackagePath = PackagePath,
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
				.RendererName = "Durin.MaterialThumbnail",
				.GeneratorSchemaVersion = FThumbnailVisualContract::SchemaVersion,
				.Output = {},
				.PreviewFixtureIdentity = std::string(FThumbnailVisualContract::SphereAssetPath),
				.PreviewFixtureVersion = FThumbnailVisualContract::SphereFixtureVersion,
				.ShaderContractVersion = 1,
				.Dependencies = {
					MakePackage("/ThumbnailTests/Textures/BaseColor", "DTexture2D", 5, 1024, 200),
					MakePackage("/ThumbnailTests/Materials/Parent", "DMaterial", 7, 2048, 150),
				},
			};
		}

		class FTestThumbnailRenderer final : public DThumbnailRenderer
		{
		public:
			explicit FTestThumbnailRenderer(
				FThumbnailRenderingInfo InRegistration,
				bool bInCaptureSucceeds = true,
				bool bInSmallOutput = false)
				: Registration(std::move(InRegistration))
				, bCaptureSucceeds(bInCaptureSucceeds)
				, bSmallOutput(bInSmallOutput)
			{
			}

			auto GetRegistration() const -> FThumbnailRenderingInfo override
			{
				return Registration;
			}

			auto CaptureGenerationRequest(
				const FAssetThumbnailRequest& Request,
				uint64 RendererGeneration)
				-> std::expected<FAssetThumbnailGenerationRequest, std::string> override
			{
				FAssetThumbnailGenerationRequest OutRequest;
				if (!bCaptureSucceeds)
				{
					return std::unexpected("The test renderer rejected the asset.");
				}
				OutRequest.KeyInput.Asset = Request.Asset;
				OutRequest.KeyInput.RendererName = Registration.RendererName;
				OutRequest.KeyInput.GeneratorSchemaVersion = Registration.GeneratorSchemaVersion;
				OutRequest.RendererGeneration = RendererGeneration;
				OutRequest.RequestSerial = Request.RequestSerial;
				if (bSmallOutput) OutRequest.KeyInput.Output = {.Width = 1, .Height = 1};
				return OutRequest;
			}

		private:
			FThumbnailRenderingInfo Registration;
			bool bCaptureSucceeds = true;
			bool bSmallOutput = false;
		};

		struct FFakeThumbnailRendererState
		{
			uint32 Captures = 0;
			std::vector<FAssetThumbnailPackageFingerprint> Dependencies;
			uint32 Sessions = 0;
			std::optional<std::string> SessionFailure;
			uint32 PreviewPreparations = 0;
			uint32 PreviewResets = 0;
			uint32 InputDestructions = 0;
			uint32 SessionDestructions = 0;
			uint32 ExtensionDestructions = 0;
			uint32 ResourcePollsBeforeReady = 0;
			uint32 ResourcePolls = 0;
		};

		class FFakeThumbnailInput final : public IAssetThumbnailGenerationInput
		{
		public:
			explicit FFakeThumbnailInput(
				std::shared_ptr<FFakeThumbnailRendererState> InState)
				: State(std::move(InState))
			{
			}

			~FFakeThumbnailInput() override
			{
				++State->InputDestructions;
			}

		private:
			std::shared_ptr<FFakeThumbnailRendererState> State;
		};

		class FFakeThumbnailPreviewScene final
			: public IThumbnailPreviewScene
		{
		public:
			auto GetWorld() -> DWorld* override { return nullptr; }

			auto SetView(
				const FThumbnailPreviewView& View) -> std::expected<void, std::string> override
			{
				LastView = View;
				return {};
			}

			auto SetViewEnvironment(
				const FViewEnvironmentOverride& Environment) -> std::expected<void, std::string> override
			{
				LastEnvironment = Environment;
				return {};
			}

			FThumbnailPreviewView LastView;
			std::optional<FViewEnvironmentOverride> LastEnvironment;
		};

		class FFakeThumbnailSession final
			: public IThumbnailRendererSession
		{
		public:
			explicit FFakeThumbnailSession(
				std::shared_ptr<FFakeThumbnailRendererState> InState)
				: State(std::move(InState))
			{
			}

			~FFakeThumbnailSession() override
			{
				++State->SessionDestructions;
			}

			auto Load() -> FThumbnailRendererSessionUpdate override
			{
				return {
					.State = State->ResourcePollsBeforeReady == 0
						? EThumbnailRendererSessionState::ReadyToRender
						: EThumbnailRendererSessionState::WaitingForResources};
			}

			auto PollResources() -> FThumbnailRendererSessionUpdate override
			{
				++State->ResourcePolls;
				const bool bReady = State->ResourcePolls
					> State->ResourcePollsBeforeReady;
				return {
					.State = bReady
						? EThumbnailRendererSessionState::ReadyToRender
						: EThumbnailRendererSessionState::WaitingForResources};
			}

			auto PreparePreview(
				IThumbnailPreviewScene& PreviewScene) -> std::expected<void, std::string> override
			{
				FThumbnailPreviewView View;
				View.CameraPosition = {2.0, -2.0, 1.0};
				if (auto ViewResult = PreviewScene.SetView(View); !ViewResult) return ViewResult;
				++State->PreviewPreparations;
				return {};
			}

			auto ValidatePreparedInput() const -> std::expected<void, std::string> override
			{
				if (!bReset && State->PreviewPreparations != 0)
				{
					return {};
				}
				return std::unexpected("The fake rendered-thumbnail input is not prepared.");
			}

			auto ResetPreview() -> void override
			{
				if (bReset) return;
				bReset = true;
				++State->PreviewResets;
			}

		private:
			std::shared_ptr<FFakeThumbnailRendererState> State;
			bool bReset = false;
		};

		class FFakeThumbnailRenderer final
			: public DThumbnailRenderer
		{
		public:
			explicit FFakeThumbnailRenderer(
				std::shared_ptr<FFakeThumbnailRendererState> InState,
				std::string InAssetClassName = "DFakeRenderedAsset")
				: State(std::move(InState))
				, AssetClassName(std::move(InAssetClassName))
			{
			}

			~FFakeThumbnailRenderer() override
			{
				++State->ExtensionDestructions;
			}

			auto GetRegistration() const -> FThumbnailRenderingInfo override
			{
				return {
					.AssetClassName = AssetClassName,
					.RendererName = "Durin.Tests.FakeThumbnail",
					.GeneratorSchemaVersion = 1};
			}

			auto CaptureGenerationRequest(
				const FAssetThumbnailRequest& Request,
				uint64 RendererGeneration)
				-> std::expected<FAssetThumbnailGenerationRequest, std::string> override
			{
				FAssetThumbnailGenerationRequest OutRequest;
				++State->Captures;
				OutRequest.KeyInput = {
					.Asset = Request.Asset,
					.RendererName = "Durin.Tests.FakeThumbnail",
					.GeneratorSchemaVersion = 1,
					.Output = {.Width = 1, .Height = 1},
					.PreviewFixtureIdentity = "/Tests/FakeThumbnail",
					.PreviewFixtureVersion = 1,
					.ShaderContractVersion = 1};
				OutRequest.KeyInput.Dependencies = State->Dependencies;
				OutRequest.Input =
					std::make_shared<FFakeThumbnailInput>(State);
				OutRequest.RendererGeneration = RendererGeneration;
				OutRequest.RequestSerial = Request.RequestSerial;
				return OutRequest;
			}

			auto CreateGenerationSession(
				const FAssetThumbnailGenerationRequest& Request,
				const IAssetThumbnailGenerationInput& Input)
				-> std::expected<std::unique_ptr<IThumbnailRendererSession>, std::string> override
			{
				(void)Request;
				if (dynamic_cast<const FFakeThumbnailInput*>(&Input) == nullptr)
				{
					return std::unexpected("The fake rendered-thumbnail input type is invalid.");
				}
				if (State->SessionFailure) return std::unexpected(*State->SessionFailure);
				++State->Sessions;
				return std::make_unique<FFakeThumbnailSession>(State);
			}

		private:
			std::shared_ptr<FFakeThumbnailRendererState> State;
			std::string AssetClassName;
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
		EXPECT_EQ(StaticMeshEditor::FStaticMeshThumbnailRendererContract::AssetClassName, "DStaticMesh");
		EXPECT_EQ(StaticMeshEditor::FStaticMeshThumbnailRendererContract::RendererName, "Durin.StaticMeshThumbnail");
		EXPECT_EQ(StaticMeshEditor::FStaticMeshThumbnailRendererContract::GeneratorSchemaVersion, 2u);
		EXPECT_EQ(
			StaticMeshEditor::FStaticMeshThumbnailRendererContract::PreviewFixtureIdentity,
			"/Engine/Editor/StaticMeshPreview/LOD0DefaultMaterials");
		EXPECT_EQ(StaticMeshEditor::FStaticMeshThumbnailRendererContract::PreviewFixtureVersion, 3u);
		EXPECT_EQ(StaticMeshEditor::FStaticMeshThumbnailRendererContract::ShaderContractVersion, 1u);
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

		ExpectChanged([](auto& Value) { Value.Asset.AssetPath = MakeAssetPath("/ThumbnailTests/Materials/Renamed"); });
		ExpectChanged([](auto& Value) { ++Value.Asset.PackageFormatVersion; });
		ExpectChanged([](auto& Value) { ++Value.Asset.FileSize; });
		ExpectChanged([](auto& Value) { ++Value.Asset.LastWriteTimeTicks; });
		ExpectChanged([](auto& Value) { Value.Asset.AssetClassName = "DMaterialInstance"; });
		ExpectChanged([](auto& Value) { Value.RendererName += ".Changed"; });
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
		const FPackagePath Root = MakePath("/ThumbnailTests/Materials/Instance");
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

		auto ForwardClosureResult = BuildAssetThumbnailDependencyClosure(Root, Forward);
		ASSERT_TRUE(ForwardClosureResult) << ForwardClosureResult.error();
		ForwardClosure = std::move(*ForwardClosureResult);
		ASSERT_EQ(ForwardClosure.size(), 3u);
		EXPECT_EQ(ForwardClosure[0].PackagePath.GetView(), "/ThumbnailTests/Materials/Parent");
		EXPECT_EQ(ForwardClosure[1].PackagePath.GetView(), "/ThumbnailTests/Textures/BaseColor");
		EXPECT_EQ(ForwardClosure[2].PackagePath.GetView(), "/ThumbnailTests/Textures/Normal");

		std::ranges::reverse(Forward);
		for (FAssetThumbnailDependencyNode& Node : Forward)
			std::ranges::reverse(Node.Dependencies);
		std::vector<FAssetThumbnailPackageFingerprint> ReverseClosure;
		auto ReverseClosureResult = BuildAssetThumbnailDependencyClosure(Root, Forward);
		ASSERT_TRUE(ReverseClosureResult) << ReverseClosureResult.error();
		ReverseClosure = std::move(*ReverseClosureResult);
		EXPECT_EQ(ReverseClosure, ForwardClosure);
	}

	TEST(FAssetThumbnailContractTests, MissingDependenciesCannotProduceTrustedClosure)
	{
		const FPackagePath Root = MakePath("/ThumbnailTests/Materials/Invalid");
		const std::vector<FAssetThumbnailDependencyNode> Nodes = {
			{MakePackage("/ThumbnailTests/Materials/Invalid", "DMaterial", 7, 100, 10),
				{MakePath("/ThumbnailTests/Textures/Missing")}},
		};

		const auto ClosureResult = BuildAssetThumbnailDependencyClosure(Root, Nodes);
		ASSERT_FALSE(ClosureResult);
		EXPECT_NE(ClosureResult.error().find("/ThumbnailTests/Textures/Missing"), std::string::npos);
	}

	TEST(FAssetThumbnailContractTests, DuplicateRegistryEntriesCannotProduceAmbiguousClosure)
	{
		const FPackagePath Root = MakePath("/ThumbnailTests/Materials/Duplicate");
		const std::vector<FAssetThumbnailDependencyNode> Nodes = {
			{MakePackage("/ThumbnailTests/Materials/Duplicate", "DMaterial", 7, 100, 10), {}},
			{MakePackage("/ThumbnailTests/Materials/Duplicate", "DMaterial", 7, 100, 10), {}},
		};

		const auto ClosureResult = BuildAssetThumbnailDependencyClosure(Root, Nodes);
		ASSERT_FALSE(ClosureResult);
		EXPECT_NE(ClosureResult.error().find("duplicate"), std::string::npos);
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
		const FThumbnailVisualContract Visual;
		EXPECT_EQ(Visual.Output.Width, Visual.Output.Height);
		EXPECT_EQ(Visual.Output.Width, 256u);
		EXPECT_EQ(FThumbnailVisualContract::SphereAssetPath,
			"/Engine/Models/Sphere.Sphere");
		EXPECT_EQ(FThumbnailVisualContract::SphereFixtureVersion, 1u);
		EXPECT_EQ(FThumbnailVisualContract::OutputEncoding, "PNG");
		EXPECT_EQ(FThumbnailVisualContract::OutputColorSpace, "sRGB");
		EXPECT_TRUE(Visual.bOutputOpaque);
		EXPECT_EQ(Visual.CubeDirectionConvention,
			EAssetThumbnailCubeDirectionConvention::WorldSpaceReflectionVector);

		const FAssetThumbnailBudgets Budgets;
		EXPECT_EQ(Budgets.MaximumRendersPerFrame, 1u);
		EXPECT_EQ(Budgets.MaximumLivePreviewScenes, 1u);
		EXPECT_GT(Budgets.MaximumParkedRenderedJobs, 0u);
		EXPECT_GT(Budgets.MaximumRetainedEntries, 0u);
		EXPECT_GT(Budgets.ResourcePollIntervalFrames, 0u);
		EXPECT_GT(Budgets.MaximumResourceWaitFrames, 0u);
		EXPECT_GT(Budgets.MaximumQueuedJobs, 0u);
		EXPECT_GT(Budgets.CpuPixelBudgetBytes, 0u);
		EXPECT_GT(Budgets.GpuTextureBudgetBytes, 0u);
		const FAssetThumbnailPoolStorageSettings Storage;
		EXPECT_GT(Storage.MaximumObjectBytes, 0u);
		EXPECT_GT(Storage.DiskBudgetBytes, 0u);
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

		const auto NullRegistration = Registry.Register(nullptr);
		ASSERT_FALSE(NullRegistration);
		EXPECT_FALSE(NullRegistration.error().empty());

		auto Invalid = std::make_shared<FTestThumbnailRenderer>(
			FThumbnailRenderingInfo{.AssetClassName = "DMaterial"});
		const auto InvalidRegistration = Registry.Register(Invalid);
		ASSERT_FALSE(InvalidRegistration);
		EXPECT_FALSE(InvalidRegistration.error().empty());

		auto Material = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Material));
		EXPECT_EQ(Registry.Num(), 1u);

		auto Duplicate = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.OtherMaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		const auto DuplicateRegistration = Registry.Register(Duplicate);
		ASSERT_FALSE(DuplicateRegistration);
		EXPECT_NE(DuplicateRegistration.error().find("DMaterial"), std::string::npos);
		EXPECT_EQ(Registry.Num(), 1u);
	}

	TEST(FAssetThumbnailContractTests, RendererRegistryUsesExactClassesAndMonotonicGenerations)
	{
		DThumbnailManager Registry;

		auto Material = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		auto MaterialInstance = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
			.AssetClassName = "DMaterialInstance",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Material));
		ASSERT_TRUE(Registry.Register(MaterialInstance));

		const FThumbnailRendererHandle MaterialHandle = Registry.Find("DMaterial");
		const FThumbnailRendererHandle InstanceHandle = Registry.Find("DMaterialInstance");
		EXPECT_TRUE(MaterialHandle);
		EXPECT_TRUE(InstanceHandle);
		EXPECT_NE(MaterialHandle.Generation, InstanceHandle.Generation);
		EXPECT_FALSE(Registry.Find("DMaterialInterface"));

		ASSERT_TRUE(Registry.Unregister("DMaterial"));
		EXPECT_FALSE(Registry.Find("DMaterial"));
		EXPECT_FALSE(Registry.Unregister("DMaterial"));

		auto Replacement = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 2});
		ASSERT_TRUE(Registry.Register(Replacement));
		EXPECT_GT(Registry.Find("DMaterial").Generation, MaterialHandle.Generation);
	}

	TEST(FAssetThumbnailContractTests, RendererRegistryShutdownDropsRenderersAndClosesRegistration)
	{
		DThumbnailManager Registry;

		auto Renderer = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
			.AssetClassName = "DTextureCube",
			.RendererName = "Durin.TextureCubeThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer));

		Registry.Shutdown();
		EXPECT_TRUE(Registry.IsShuttingDown());
		EXPECT_EQ(Registry.Num(), 0u);
		EXPECT_FALSE(Registry.Find("DTextureCube"));
		EXPECT_FALSE(Registry.Register(Renderer));
	}

	TEST(FAssetThumbnailContractTests, ScopedRendererRegistrationRejectsDuplicatesAndAllowsLaterReplacement)
	{
		DThumbnailManager Registry;

		auto FirstState = std::make_shared<FFakeThumbnailRendererState>();
		auto First = Registry.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(FirstState));
		ASSERT_TRUE(First) << First.error();
		const uint64 FirstGeneration = Registry.Find("DFakeRenderedAsset").Generation;

		auto DuplicateState = std::make_shared<FFakeThumbnailRendererState>();
		auto Duplicate = Registry.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(DuplicateState));
		ASSERT_FALSE(Duplicate);
		EXPECT_NE(Duplicate.error().find("DFakeRenderedAsset"), std::string::npos);
		EXPECT_EQ(DuplicateState->ExtensionDestructions, 1u);
		EXPECT_EQ(Registry.Find("DFakeRenderedAsset").Generation, FirstGeneration);

		First->Reset();
		EXPECT_FALSE(Registry.Find("DFakeRenderedAsset"));
		EXPECT_EQ(FirstState->ExtensionDestructions, 1u);
		auto ReplacementState = std::make_shared<FFakeThumbnailRendererState>();
		auto Replacement = Registry.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(ReplacementState));
		ASSERT_TRUE(Replacement) << Replacement.error();
		EXPECT_GT(Registry.Find("DFakeRenderedAsset").Generation, FirstGeneration);
	}

	TEST(FAssetThumbnailContractTests, RequiredSceneCanDeferOptionalAdmissionsWithoutLosingRequests)
	{
		DThumbnailManager Manager;
		auto State = std::make_shared<FFakeThumbnailRendererState>();
		auto Registration = Manager.RegisterScoped(std::make_unique<FFakeThumbnailRenderer>(State));
		ASSERT_TRUE(Registration) << Registration.error();
		FAssetThumbnailPool Pool(Manager, {}, {.CacheRoot = MakeObjectStoreRoot("DeferredSceneAdmission")});
		const auto Request = MakeThumbnailRequest("/ThumbnailTests/ScenePriority/Deferred", "DFakeRenderedAsset", 1);
		Pool.BeginFrame();
		Pool.Request(Request.Asset, Request.Priority);
		Pool.EndFrame(false);
		EXPECT_EQ(Pool.GetStats().QueuedJobs, 1u);
		EXPECT_EQ(State->Captures, 0u);
		EXPECT_FALSE(Pool.GetStats().bHasActiveJob);
		ASSERT_TRUE(PumpThumbnailPoolUntil(Pool, [&] { return State->Captures != 0; }));
		EXPECT_EQ(State->Captures, 1u);
	}

	TEST(FAssetThumbnailContractTests, AssetThumbnailReferencesCoalesceAndReleasePoolPins)
	{
		DThumbnailManager Manager;

		auto State = std::make_shared<FFakeThumbnailRendererState>();
		auto Registration = Manager.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(State));
		ASSERT_TRUE(Registration) << Registration.error();
		FAssetThumbnailPool Pool(
			Manager, {}, {.CacheRoot = MakeObjectStoreRoot("AssetThumbnailReferences")});
		const FAssetThumbnailRequest Request = MakeThumbnailRequest(
			"/ThumbnailTests/References/Shared", "DFakeRenderedAsset", 1);
		{
			FAssetThumbnail First(Request.Asset, &Pool);
			FAssetThumbnail Second(Request.Asset, &Pool);
			First.Request(EAssetThumbnailPriority::Prefetch);
			Second.Request(EAssetThumbnailPriority::Visible);
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

		Pool.BeginFrame();
		for (uint32 Index = 0; Index < 3; ++Index)
		{
			Pool.Request(
				MakePackage(
					std::format("/ThumbnailTests/Retained/Asset{}", Index),
					"DUnsupportedAsset",
					1,
					100 + Index,
					200 + Index),
				EAssetThumbnailPriority::Prefetch);
		}
		Pool.EndFrame();

		EXPECT_EQ(Pool.GetStats().RetainedEntries, 2u);
	}

	TEST(FAssetThumbnailContractTests, PoolRejectsWrongSizedPersistentThumbnailBeforeUpload)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("WrongSizedWarmObject");
		DThumbnailManager Manager;

		auto Renderer = std::make_shared<FTestThumbnailRenderer>(
			FThumbnailRenderingInfo{
				.AssetClassName = "DMaterial",
				.RendererName = "Durin.MaterialThumbnail",
				.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Manager.Register(Renderer));
		const FAssetThumbnailRequest Request = MakeThumbnailRequest(
			"/ThumbnailTests/WrongSizedWarm/Material", "DMaterial", 1);

		std::string CacheKey;
		{
			FAssetThumbnailRequestQueue Scheduler(Manager);
			ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Request)));
			auto Scheduled = Scheduler.TakeNext();
			ASSERT_TRUE(Scheduled);
			CacheKey = Scheduled->CacheKey;
		}
		{
			FThumbnailObjectStore Store({.CacheRoot = Root});
			ASSERT_TRUE(Store.Store(
				CacheKey, std::as_bytes(std::span{WrongSizedThumbnailPng})));
		}

		FAssetThumbnailPool Pool(Manager, {}, {.CacheRoot = Root});
		Pool.BeginFrame();
		Pool.Request(Request.Asset, Request.Priority);
		Pool.EndFrame();
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

	TEST(FAssetThumbnailContractTests, SchedulersCreatedBeforeAndAfterScopedRegistrationObserveItsGeneration)
	{
		DThumbnailManager Registry;
		FAssetThumbnailRequestQueue BeforeRegistration(Registry);

		auto State = std::make_shared<FFakeThumbnailRendererState>();
		auto Registration = Registry.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(State));
		ASSERT_TRUE(Registration) << Registration.error();
		const uint64 Generation = Registry.Find("DFakeRenderedAsset").Generation;
		ASSERT_NE(Generation, 0u);
		FAssetThumbnailRequestQueue AfterRegistration(Registry);
		ASSERT_TRUE(IsThumbnailRequestAccepted(BeforeRegistration.Request(MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/BeforeRegistry",
			"DFakeRenderedAsset",
			1
		))));
		ASSERT_TRUE(IsThumbnailRequestAccepted(AfterRegistration.Request(MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/AfterRegistry",
			"DFakeRenderedAsset",
			1
		))));
		const std::optional<FAssetThumbnailScheduledRequest> BeforeJob =
			BeforeRegistration.TakeNext();
		const std::optional<FAssetThumbnailScheduledRequest> AfterJob =
			AfterRegistration.TakeNext();
		ASSERT_TRUE(BeforeJob);
		ASSERT_TRUE(AfterJob);
		EXPECT_EQ(BeforeJob->GenerationRequest.RendererGeneration, Generation);
		EXPECT_EQ(AfterJob->GenerationRequest.RendererGeneration, Generation);

		Registration->Reset();
		EXPECT_FALSE(BeforeRegistration.Transition(
			*BeforeJob,
			EAssetThumbnailState::Loading,
			EAssetThumbnailState::WaitingForResources));
		EXPECT_FALSE(AfterRegistration.Transition(
			*AfterJob,
			EAssetThumbnailState::Loading,
			EAssetThumbnailState::WaitingForResources));
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
		auto Registration = ThumbnailManager.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(State));
		ASSERT_TRUE(Registration) << Registration.error();
		FAssetThumbnailPool AfterRegistration(
			ThumbnailManager,
			{},
			{.CacheRoot = MakeObjectStoreRoot("CacheAfterRegistration"),
				.ObjectExtension = ".bin"});
		const FAssetThumbnailRequest BeforeRequest = MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/CacheBeforeRegistration",
			"DFakeRenderedAsset",
			1);
		const FAssetThumbnailRequest AfterRequest = MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/CacheAfterRegistration",
			"DFakeRenderedAsset",
			1);
		BeforeRegistration.Request(
			BeforeRequest.Asset, EAssetThumbnailPriority::Visible);
		AfterRegistration.Request(
			AfterRequest.Asset, EAssetThumbnailPriority::Visible);
		EXPECT_EQ(
			BeforeRegistration.Find(BeforeRequest.Asset.AssetPath).State,
			EAssetThumbnailState::Queued);
		EXPECT_EQ(
			AfterRegistration.Find(AfterRequest.Asset.AssetPath).State,
			EAssetThumbnailState::Queued);

		Registration->Reset();
		BeforeRegistration.EndFrame();
		AfterRegistration.EndFrame();
		EXPECT_NE(
			BeforeRegistration.Find(BeforeRequest.Asset.AssetPath).State,
			EAssetThumbnailState::Loading);
		EXPECT_NE(
			AfterRegistration.Find(AfterRequest.Asset.AssetPath).State,
			EAssetThumbnailState::Loading);
		EXPECT_EQ(State->ExtensionDestructions, 1u);
	}

	TEST(FAssetThumbnailContractTests, ParkedResourceWaitDoesNotBlockLaterRenderedWork)
	{
		DThumbnailManager ThumbnailManager;

		auto WaitingState = std::make_shared<FFakeThumbnailRendererState>();
		WaitingState->ResourcePollsBeforeReady = 100;
		auto ReadyState = std::make_shared<FFakeThumbnailRendererState>();
		auto WaitingRegistration = ThumbnailManager.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(
				WaitingState, "DWaitingRenderedAsset"));
		ASSERT_TRUE(WaitingRegistration) << WaitingRegistration.error();
		auto ReadyRegistration = ThumbnailManager.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(
				ReadyState, "DReadyRenderedAsset"));
		ASSERT_TRUE(ReadyRegistration) << ReadyRegistration.error();
		FAssetThumbnailBudgets Budgets;
		Budgets.ResourcePollIntervalFrames = 4;
		FAssetThumbnailPool Cache(
			ThumbnailManager,
			Budgets,
			{.CacheRoot = MakeObjectStoreRoot("ParkedResourceWait"),
				.ObjectExtension = ".bin"});
		const FAssetThumbnailRequest Waiting = MakeThumbnailRequest(
			"/ThumbnailTests/Parked/Waiting", "DWaitingRenderedAsset", 1,
			EAssetThumbnailPriority::Visible);
		const FAssetThumbnailRequest Ready = MakeThumbnailRequest(
			"/ThumbnailTests/Parked/Ready", "DReadyRenderedAsset", 1,
			EAssetThumbnailPriority::Visible);

		Cache.BeginFrame();
		Cache.Request(Waiting.Asset, Waiting.Priority);
		Cache.EndFrame();
		ASSERT_TRUE(PumpThumbnailPoolUntil(Cache, [&] {
			return Cache.GetStats().ParkedResourceWaits == 1;
		}));
		EXPECT_EQ(Cache.Find(Waiting.Asset.AssetPath).State,
			EAssetThumbnailState::WaitingForResources);
		EXPECT_EQ(Cache.GetStats().ParkedResourceWaits, 1u);
		EXPECT_FALSE(Cache.GetStats().bHasActiveJob);

		Cache.BeginFrame();
		Cache.Request(Ready.Asset, Ready.Priority);
		Cache.EndFrame();
		ASSERT_TRUE(PumpThumbnailPoolUntil(Cache, [&] {
			return ReadyState->Sessions == 1;
		}));
		EXPECT_EQ(Cache.Find(Waiting.Asset.AssetPath).State,
			EAssetThumbnailState::WaitingForResources);
		EXPECT_NE(Cache.Find(Ready.Asset.AssetPath).State,
			EAssetThumbnailState::Queued);
		EXPECT_EQ(Cache.GetStats().ParkedResourceWaits, 1u);
		EXPECT_EQ(Cache.GetStats().PeakParkedResourceWaits, 1u);
	}

	TEST(FAssetThumbnailContractTests, SharedPoolInvalidatesCapturedDependenciesWithoutBrowserSnapshots)
	{
		DThumbnailManager Registry;
		auto OwnerState = std::make_shared<FFakeThumbnailRendererState>();
		auto OtherState = std::make_shared<FFakeThumbnailRendererState>();
		OwnerState->ResourcePollsBeforeReady = OtherState->ResourcePollsBeforeReady = 1000;
		const auto Base = MakePackage("/ThumbnailTests/Dependencies/Base", "DMaterial", 1, 100, 1);
		OwnerState->Dependencies = {Base};
		auto OwnerRegistration = Registry.RegisterScoped(std::make_unique<FFakeThumbnailRenderer>(OwnerState));
		auto OtherRegistration = Registry.RegisterScoped(std::make_unique<FFakeThumbnailRenderer>(OtherState, "DOtherAsset"));
		ASSERT_TRUE(OwnerRegistration);
		ASSERT_TRUE(OtherRegistration);
		FAssetThumbnailPool Pool(Registry, {.ResourcePollIntervalFrames = 1, .MaximumResourceWaitFrames = 0},
			{.CacheRoot = MakeObjectStoreRoot("SharedDependencies")});
		const auto Owner = MakeThumbnailRequest("/ThumbnailTests/Materials/Owner", "DFakeRenderedAsset", 1);
		const auto Other = MakeThumbnailRequest("/ThumbnailTests/Materials/Other", "DOtherAsset", 1);
		FAssetThumbnail OwnerReference(Owner.Asset, &Pool), OtherReference(Other.Asset, &Pool);
		OwnerReference.Request(EAssetThumbnailPriority::Visible);
		OtherReference.Request(EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(PumpThumbnailPoolUntil(Pool, [&] { return Pool.GetStats().ParkedResourceWaits == 2; }));
		const auto Resolved = FMountPaths::ResolveAssetPath(Base.PackagePath.GetView());
		ASSERT_TRUE(Resolved);
		const auto Physical = Resolved->PhysicalPath.generic_string() + ".dasset";
		Pool.ApplyContentChanges({.Changes = {{EContentChangeKind::Removed, Physical}}});
		EXPECT_EQ(OwnerReference.GetView().State, EAssetThumbnailState::NotRequested);
		EXPECT_EQ(OtherReference.GetView().State, EAssetThumbnailState::WaitingForResources);
		Pool.ApplyContentChanges({.bFullRefresh = true});
		EXPECT_EQ(OtherReference.GetView().State, EAssetThumbnailState::NotRequested);
	}

	TEST(FAssetThumbnailContractTests, LastReferencerCancelsParkedSessionWithoutCancellingOtherConsumers)
	{
		DThumbnailManager Manager;

		auto State = std::make_shared<FFakeThumbnailRendererState>();
		State->ResourcePollsBeforeReady = 1000;
		auto Registration = Manager.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(State, "DNavigationAsset"));
		ASSERT_TRUE(Registration) << Registration.error();
		FAssetThumbnailPool Pool(Manager, {},
			{.CacheRoot = MakeObjectStoreRoot("NavigationCancel"), .ObjectExtension = ".bin"});
		const auto Request = MakeThumbnailRequest("/ThumbnailTests/Navigation/Asset",
			"DNavigationAsset", 1, EAssetThumbnailPriority::Visible);
		Pool.AddReferencer(Request.Asset.AssetPath);
		Pool.AddReferencer(Request.Asset.AssetPath);
		Pool.BeginFrame();
		Pool.Request(Request.Asset, Request.Priority);
		Pool.EndFrame();
		ASSERT_TRUE(PumpThumbnailPoolUntil(Pool, [&] { return Pool.GetStats().ParkedResourceWaits == 1; }));
		Pool.RemoveReferencer(Request.Asset.AssetPath);
		EXPECT_EQ(State->SessionDestructions, 0u);
		EXPECT_EQ(Pool.GetStats().ParkedResourceWaits, 1u);
		Pool.RemoveReferencer(Request.Asset.AssetPath);
		EXPECT_EQ(State->SessionDestructions, 1u);
		EXPECT_EQ(Pool.GetStats().ParkedResourceWaits, 0u);
		Pool.AddReferencer(Request.Asset.AssetPath);
		Pool.BeginFrame();
		Pool.Request(Request.Asset, Request.Priority);
		Pool.EndFrame();
		ASSERT_TRUE(PumpThumbnailPoolUntil(Pool, [&] { return Pool.GetStats().ParkedResourceWaits == 1; }));
		EXPECT_EQ(State->Sessions, 2u);
		Pool.RemoveReferencer(Request.Asset.AssetPath);
	}

	TEST(FAssetThumbnailContractTests, ParkedResourceWaitTimesOutAndReleasesSession)
	{
		DThumbnailManager ThumbnailManager;

		auto State = std::make_shared<FFakeThumbnailRendererState>();
		State->ResourcePollsBeforeReady = 100;
		auto Registration = ThumbnailManager.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(
				State, "DTimeoutRenderedAsset"));
		ASSERT_TRUE(Registration) << Registration.error();
		FAssetThumbnailBudgets Budgets;
		Budgets.ResourcePollIntervalFrames = 1;
		Budgets.MaximumResourceWaitFrames = 2;
		FAssetThumbnailPool Cache(
			ThumbnailManager,
			Budgets,
			{.CacheRoot = MakeObjectStoreRoot("ParkedResourceTimeout"),
				.ObjectExtension = ".bin"});
		const FAssetThumbnailRequest Request = MakeThumbnailRequest(
			"/ThumbnailTests/Parked/Timeout", "DTimeoutRenderedAsset", 1,
			EAssetThumbnailPriority::Visible);

		Cache.BeginFrame();
		Cache.Request(Request.Asset, Request.Priority);
		Cache.EndFrame();
		ASSERT_TRUE(PumpThumbnailPoolUntil(Cache, [&] {
			return Cache.Find(Request.Asset.AssetPath).State == EAssetThumbnailState::Failed;
		}));

		const FAssetThumbnailView View =
			Cache.Find(Request.Asset.AssetPath);
		EXPECT_EQ(View.State, EAssetThumbnailState::Failed);
		EXPECT_NE(View.Diagnostic.find("Timed out"), std::string::npos);
		EXPECT_EQ(Cache.GetStats().ParkedResourceWaits, 0u);
		EXPECT_EQ(Cache.GetStats().ResourceWaitTimeouts, 1u);
		EXPECT_EQ(State->PreviewResets, 1u);
		EXPECT_EQ(State->SessionDestructions, 1u);
	}

	TEST(FAssetThumbnailContractTests, FakeThumbnailRendererRunsColdAndWarmBeforeScopedRemoval)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("FakeThumbnailRendererWarmHit");
		DThumbnailManager Registry;

		auto State = std::make_shared<FFakeThumbnailRendererState>();
		auto Registration = Registry.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(State));
		ASSERT_TRUE(Registration) << Registration.error();
		FAssetThumbnailRequestQueue Scheduler(Registry);
		FAssetThumbnailGeneration Pipeline(
			Scheduler, {.CacheRoot = Root, .ObjectExtension = ".bin"});
		const FAssetThumbnailRequest ColdRequest = MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/ColdWarm", "DFakeRenderedAsset", 1);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(ColdRequest)));
		Pipeline.BeginFrame();
		std::optional<FAssetThumbnailJob> ColdJob = StartThumbnailWork(Pipeline).ColdJob;
		ASSERT_TRUE(ColdJob);
		State->SessionFailure = "";
		const auto EmptyFailure = ColdJob->ScheduledJob.GenerationRequest.BeginRenderedSession();
		ASSERT_FALSE(EmptyFailure);
		EXPECT_FALSE(EmptyFailure.error().empty());
		EXPECT_EQ(ColdJob->ScheduledJob.GenerationRequest.GetRenderedSession(), nullptr);
		State->SessionFailure = "Renderer-specific session failure.";
		const auto DetailedFailure = ColdJob->ScheduledJob.GenerationRequest.BeginRenderedSession();
		ASSERT_FALSE(DetailedFailure);
		EXPECT_EQ(DetailedFailure.error(), *State->SessionFailure);
		State->SessionFailure.reset();
		auto Session = ColdJob->ScheduledJob.GenerationRequest.BeginRenderedSession();
		ASSERT_TRUE(Session) << Session.error();
		ASSERT_NE(*Session, nullptr);
		EXPECT_FALSE((*Session)->ValidatePreparedInput());
		const FThumbnailRendererSessionUpdate Update = (*Session)->Load();
		ASSERT_EQ(Update.State, EThumbnailRendererSessionState::ReadyToRender);
		FFakeThumbnailPreviewScene PreviewScene;
		ASSERT_TRUE((*Session)->PreparePreview(PreviewScene));
		ASSERT_TRUE((*Session)->ValidatePreparedInput());
		ASSERT_TRUE(Pipeline.CompleteLoad(*ColdJob));
		ASSERT_TRUE(Pipeline.BeginRender(
			*ColdJob, true));
		ASSERT_TRUE(Pipeline.CompleteRender(
			*ColdJob));
		ASSERT_TRUE(Pipeline.CompleteReadback(
			*ColdJob));
		const FByteBuffer Pixels(4, std::byte{127});
		ASSERT_TRUE(Pipeline.CompletePixels(*ColdJob, Pixels, 1, 1));
		Pipeline.WaitForCacheTasksForTesting();
		ColdJob->ScheduledJob.GenerationRequest.ReleaseRenderedSession();
		EXPECT_EQ(State->PreviewResets, 1u);

		const FAssetThumbnailRequest WarmRequest = MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/ColdWarm", "DFakeRenderedAsset", 2);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(WarmRequest)));
		FAssetThumbnailStartResult Warm = StartThumbnailWork(Pipeline);
		EXPECT_FALSE(Warm.ColdJob);
		ASSERT_TRUE(Warm.WarmJob);
		EXPECT_EQ(Warm.Pixels, Pixels);
		EXPECT_EQ(State->Captures, 2u);
		EXPECT_EQ(State->Sessions, 1u);
		EXPECT_EQ(Pipeline.GetStats().DiskHits, 1u);

		Registration->Reset();
		const auto RetiredSession = ColdJob->ScheduledJob.GenerationRequest.BeginRenderedSession();
		ASSERT_FALSE(RetiredSession);
		EXPECT_NE(RetiredSession.error().find("no longer active"), std::string::npos);
		EXPECT_EQ(State->InputDestructions, 2u);
		EXPECT_EQ(State->SessionDestructions, 1u);
		EXPECT_EQ(State->ExtensionDestructions, 1u);
		EXPECT_FALSE(IsThumbnailRequestAccepted(Scheduler.Request(MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/AfterRemoval",
			"DFakeRenderedAsset",
			3
		))));
	}


	TEST(FAssetThumbnailContractTests, ScopedRemovalRejectsEveryInFlightStateAndDrainsSession)
	{
		const std::array States = {
			EAssetThumbnailState::Loading,
			EAssetThumbnailState::WaitingForResources,
			EAssetThumbnailState::Rendering,
			EAssetThumbnailState::Readback,
			EAssetThumbnailState::Encoding,
			EAssetThumbnailState::Uploading};
		for (size_t StateIndex = 0; StateIndex < States.size(); ++StateIndex)
		{
			SCOPED_TRACE(static_cast<uint32>(States[StateIndex]));
			DThumbnailManager Registry;

			auto State = std::make_shared<FFakeThumbnailRendererState>();
			const std::string AssetClassName = std::format("DFakeRenderedAsset{}", StateIndex);
			auto Registration = Registry.RegisterScoped(
				std::make_unique<FFakeThumbnailRenderer>(State, AssetClassName));
			ASSERT_TRUE(Registration) << Registration.error();
			FAssetThumbnailRequestQueue Scheduler(Registry);
			const FAssetThumbnailRequest Request = MakeThumbnailRequest(
				std::format("/ThumbnailTests/FakeRendered/InFlight{}", StateIndex),
				AssetClassName,
				1);
			ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Request)));
			std::optional<FAssetThumbnailScheduledRequest> Job = Scheduler.TakeNext();
			ASSERT_TRUE(Job);
			ASSERT_TRUE(Job->GenerationRequest.BeginRenderedSession());

			EAssetThumbnailState Current = EAssetThumbnailState::Loading;
			for (const EAssetThumbnailState Next : {
				EAssetThumbnailState::WaitingForResources,
				EAssetThumbnailState::Rendering,
				EAssetThumbnailState::Readback,
				EAssetThumbnailState::Encoding,
				EAssetThumbnailState::Uploading})
			{
				if (Current == States[StateIndex]) break;
				ASSERT_TRUE(Scheduler.Transition(*Job, Current, Next));
				Current = Next;
			}
			ASSERT_EQ(Current, States[StateIndex]);
			ASSERT_NE(Job->GenerationRequest.GetInput(), nullptr);
			ASSERT_NE(Job->GenerationRequest.GetRenderedSession(), nullptr);

			Registration->Reset();
			EXPECT_TRUE(Job->GenerationRequest.Cancellation.IsCancelled());
			EXPECT_EQ(Job->GenerationRequest.GetInput(), nullptr);
			EXPECT_EQ(Job->GenerationRequest.GetRenderedSession(), nullptr);
			EXPECT_EQ(State->PreviewResets, 1u);
			EXPECT_EQ(State->SessionDestructions, 1u);
			EXPECT_EQ(State->InputDestructions, 1u);
			EXPECT_EQ(State->ExtensionDestructions, 1u);
			EXPECT_FALSE(Scheduler.Transition(
				*Job, Current, EAssetThumbnailState::Ready));
		}
	}

	TEST(FAssetThumbnailContractTests, SchedulerSkipsMissingRenderersAndRecordsRendererRejection)
	{
		DThumbnailManager Registry;
		FAssetThumbnailRequestQueue Scheduler(Registry);

		const FAssetThumbnailRequest Missing =
			MakeThumbnailRequest("/ThumbnailTests/Unsupported", "DUnsupported", 1);
		EXPECT_FALSE(IsThumbnailRequestAccepted(Scheduler.Request(Missing)));
		EXPECT_EQ(Scheduler.NumQueued(), 0u);
		EXPECT_EQ(Scheduler.Find(Missing.Asset.AssetPath).State, EAssetThumbnailState::NotRequested);

		auto Rejecting = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1}, false);
		ASSERT_TRUE(Registry.Register(Rejecting));
		const FAssetThumbnailRequest Invalid =
			MakeThumbnailRequest("/ThumbnailTests/Invalid", "DMaterial", 2);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Invalid)));
		EXPECT_FALSE(Scheduler.TakeNext());
		const FAssetThumbnailView InvalidView = Scheduler.Find(Invalid.Asset.AssetPath);
		EXPECT_EQ(InvalidView.State, EAssetThumbnailState::Invalid);
		EXPECT_EQ(InvalidView.RequestSerial, 2u);
		EXPECT_NE(InvalidView.Diagnostic.find("rejected"), std::string::npos);
	}

	TEST(FAssetThumbnailContractTests, SchedulerDefersAndBoundsRendererCaptureUntilWorkAdmission)
	{
		DThumbnailManager Registry;

		auto State = std::make_shared<FFakeThumbnailRendererState>();
		auto Registration = Registry.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(State));
		ASSERT_TRUE(Registration) << Registration.error();
		FAssetThumbnailRequestQueue Scheduler(Registry);
		const FAssetThumbnailRequest First = MakeThumbnailRequest(
			"/ThumbnailTests/DeferredCapture/First", "DFakeRenderedAsset", 1);
		const FAssetThumbnailRequest Second = MakeThumbnailRequest(
			"/ThumbnailTests/DeferredCapture/Second", "DFakeRenderedAsset", 1,
			EAssetThumbnailPriority::Visible);

		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(First)));
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Second)));
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Second)));
		EXPECT_EQ(State->Captures, 0u);
		EXPECT_EQ(Scheduler.NumQueued(), 2u);

		const auto Admitted = Scheduler.TakeNext();
		ASSERT_TRUE(Admitted);
		EXPECT_EQ(State->Captures, 1u);
		EXPECT_EQ(Admitted->GenerationRequest.KeyInput.Asset.AssetPath,
			Second.Asset.AssetPath);
		EXPECT_EQ(Scheduler.NumQueued(), 1u);
	}

	TEST(FAssetThumbnailContractTests, SchedulerCoalescesAndPromotesVisibleRequests)
	{
		DThumbnailManager Registry;

		auto Renderer = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer));
		FAssetThumbnailRequestQueue Scheduler(Registry);
		const FAssetThumbnailRequest Prefetch =
			MakeThumbnailRequest("/ThumbnailTests/Coalesced", "DMaterial", 1);
		const FAssetThumbnailRequest Visible =
			MakeThumbnailRequest("/ThumbnailTests/Coalesced", "DMaterial", 2, EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Prefetch)));
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Visible)));
		EXPECT_EQ(Scheduler.NumQueued(), 1u);
		EXPECT_EQ(Scheduler.Find(Prefetch.Asset.AssetPath).RequestSerial, 2u);

		std::optional<FAssetThumbnailScheduledRequest> Job = Scheduler.TakeNext();
		ASSERT_TRUE(Job);
		EXPECT_EQ(Job->Priority, EAssetThumbnailPriority::Visible);
		EXPECT_EQ(Job->GenerationRequest.RequestSerial, 2u);
		EXPECT_EQ(Scheduler.Find(Prefetch.Asset.AssetPath).State, EAssetThumbnailState::Loading);

		const FAssetThumbnailRequest NewSerial =
			MakeThumbnailRequest("/ThumbnailTests/Coalesced", "DMaterial", 3, EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(NewSerial)));
		EXPECT_TRUE(Job->GenerationRequest.Cancellation.IsCancelled());
		EXPECT_EQ(Scheduler.NumQueued(), 1u);
		const std::optional<FAssetThumbnailScheduledRequest> Replacement = Scheduler.TakeNext();
		ASSERT_TRUE(Replacement);
		EXPECT_EQ(Replacement->GenerationRequest.RequestSerial, 3u);
	}

	TEST(FAssetThumbnailContractTests, SchedulerPrioritizesVisibleJobsAndEnforcesQueueBudget)
	{
		DThumbnailManager Registry;

		auto Renderer = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
			.AssetClassName = "DTextureCube",
			.RendererName = "Durin.TextureCubeThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer));
		FAssetThumbnailBudgets Budgets;
		Budgets.MaximumQueuedJobs = 2;
		FAssetThumbnailRequestQueue Scheduler(Registry, Budgets);
		const FAssetThumbnailRequest Prefetch =
			MakeThumbnailRequest("/ThumbnailTests/PrefetchCube", "DTextureCube", 1);
		const FAssetThumbnailRequest Visible =
			MakeThumbnailRequest("/ThumbnailTests/VisibleCube", "DTextureCube", 1, EAssetThumbnailPriority::Visible);
		const FAssetThumbnailRequest Overflow =
			MakeThumbnailRequest("/ThumbnailTests/OverflowCube", "DTextureCube", 1);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Prefetch)));
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Visible)));
		EXPECT_EQ(Scheduler.Request(Overflow), EThumbnailRequestStatus::QueueFull);

		const std::optional<FAssetThumbnailScheduledRequest> First = Scheduler.TakeNext();
		ASSERT_TRUE(First);
		EXPECT_EQ(First->GenerationRequest.KeyInput.Asset.AssetPath, Visible.Asset.AssetPath);
		const std::optional<FAssetThumbnailScheduledRequest> Second = Scheduler.TakeNext();
		ASSERT_TRUE(Second);
		EXPECT_EQ(Second->GenerationRequest.KeyInput.Asset.AssetPath, Prefetch.Asset.AssetPath);
		EXPECT_FALSE(Scheduler.TakeNext());
	}

	TEST(FAssetThumbnailContractTests, SchedulerKeepsNewerWorkForStaleSerialsAndCancelsReplacedWork)
	{
		DThumbnailManager Registry;

		auto Renderer = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer));
		FAssetThumbnailRequestQueue Scheduler(Registry);
		const FAssetThumbnailRequest Current =
			MakeThumbnailRequest("/ThumbnailTests/Replaced", "DMaterial", 2);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Current)));
		std::optional<FAssetThumbnailScheduledRequest> Active = Scheduler.TakeNext();
		ASSERT_TRUE(Active);

		const FAssetThumbnailRequest Stale =
			MakeThumbnailRequest("/ThumbnailTests/Replaced", "DMaterial", 1);
		EXPECT_EQ(Scheduler.Request(Stale), EThumbnailRequestStatus::Coalesced);
		EXPECT_FALSE(Active->GenerationRequest.Cancellation.IsCancelled());
		EXPECT_EQ(Scheduler.Find(Current.Asset.AssetPath).RequestSerial, 2u);
		EXPECT_EQ(Scheduler.NumQueued(), 0u);
		EXPECT_FALSE(Scheduler.TakeNext());

		const FAssetThumbnailRequest Changed =
			MakeThumbnailRequest("/ThumbnailTests/Replaced", "DMaterial", 3, EAssetThumbnailPriority::Visible, 200);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Changed)));
		EXPECT_TRUE(Active->GenerationRequest.Cancellation.IsCancelled());
		EXPECT_EQ(Scheduler.NumQueued(), 1u);
		EXPECT_EQ(Scheduler.Find(Changed.Asset.AssetPath).RequestSerial, 3u);
	}

	TEST(FAssetThumbnailContractTests, MixedRenderedKindsSharePriorityCoalescingAndQueueBudgets)
	{
		DThumbnailManager Registry;

		for (const FThumbnailRenderingInfo Registration : {
			FThumbnailRenderingInfo{
				.AssetClassName = "DMaterial",
				.RendererName = "Durin.MaterialThumbnail",
				.GeneratorSchemaVersion = 1},
			FThumbnailRenderingInfo{
				.AssetClassName = "DTextureCube",
				.RendererName = "Durin.TextureCubeThumbnail",
				.GeneratorSchemaVersion = 1},
			FThumbnailRenderingInfo{
				.AssetClassName = "DStaticMesh",
				.RendererName = "Durin.StaticMeshThumbnail",
				.GeneratorSchemaVersion = 1}})
		{
			ASSERT_TRUE(Registry.Register(
				std::make_shared<FTestThumbnailRenderer>(Registration)));
		}
		FAssetThumbnailBudgets Budgets;
		Budgets.MaximumQueuedJobs = 3;
		FAssetThumbnailRequestQueue Scheduler(Registry, Budgets);
		const FAssetThumbnailRequest Material = MakeThumbnailRequest(
			"/ThumbnailTests/Mixed/Material", "DMaterial", 1);
		const FAssetThumbnailRequest Cube = MakeThumbnailRequest(
			"/ThumbnailTests/Mixed/Cube", "DTextureCube", 1);
		const FAssetThumbnailRequest MeshPrefetch = MakeThumbnailRequest(
			"/ThumbnailTests/Mixed/Mesh", "DStaticMesh", 1);
		const FAssetThumbnailRequest MeshVisible = MakeThumbnailRequest(
			"/ThumbnailTests/Mixed/Mesh", "DStaticMesh", 2,
			EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Material)));
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Cube)));
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(MeshPrefetch)));
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(MeshVisible)));
		EXPECT_EQ(Scheduler.NumQueued(), 3u);

		const std::optional<FAssetThumbnailScheduledRequest> First = Scheduler.TakeNext();
		ASSERT_TRUE(First);
		EXPECT_EQ(
			First->GenerationRequest.KeyInput.Asset.AssetPath,
			MeshVisible.Asset.AssetPath);
		EXPECT_EQ(First->GenerationRequest.RequestSerial, 2u);
		const std::optional<FAssetThumbnailScheduledRequest> Second = Scheduler.TakeNext();
		const std::optional<FAssetThumbnailScheduledRequest> Third = Scheduler.TakeNext();
		ASSERT_TRUE(Second);
		ASSERT_TRUE(Third);
		EXPECT_NE(
			Second->GenerationRequest.KeyInput.Asset.AssetClassName,
			Third->GenerationRequest.KeyInput.Asset.AssetClassName);
		EXPECT_FALSE(Scheduler.TakeNext());
	}

	TEST(FAssetThumbnailContractTests, SchedulerShutdownCancelsWorkAndRejectsNewRequests)
	{
		DThumbnailManager Registry;

		auto Renderer = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer));
		FAssetThumbnailRequestQueue Scheduler(Registry);
		const FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/Shutdown", "DMaterial", 1);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Request)));
		std::optional<FAssetThumbnailScheduledRequest> Active = Scheduler.TakeNext();
		ASSERT_TRUE(Active);

		Scheduler.Shutdown();
		EXPECT_TRUE(Scheduler.IsShuttingDown());
		EXPECT_TRUE(Active->GenerationRequest.Cancellation.IsCancelled());
		EXPECT_EQ(Scheduler.Find(Request.Asset.AssetPath).State, EAssetThumbnailState::NotRequested);
		EXPECT_EQ(Scheduler.Request(Request), EThumbnailRequestStatus::ShuttingDown);
	}

	TEST(FAssetThumbnailContractTests, RenderedPipelinePublishesColdOutputAndServesWarmHit)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("RenderedPipelineWarmHit");
		auto RunRequest = [&](uint64 Serial, bool bExpectWarmHit) {
			DThumbnailManager Registry;

			auto Renderer = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
				.AssetClassName = "DMaterial",
				.RendererName = "Durin.MaterialThumbnail",
				.GeneratorSchemaVersion = 1});
			EXPECT_TRUE(Registry.Register(Renderer));
			FAssetThumbnailRequestQueue Scheduler(Registry);
			FAssetThumbnailGeneration Pipeline(
				Scheduler,
				{.CacheRoot = Root, .ObjectExtension = ".bin"});
			const FAssetThumbnailRequest Request =
				MakeThumbnailRequest("/ThumbnailTests/PersistentMaterial", "DMaterial", Serial);
			EXPECT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Request)));
			Pipeline.BeginFrame();
			std::optional<FAssetThumbnailJob> Job = StartThumbnailWork(Pipeline).ColdJob;
			if (bExpectWarmHit)
			{
				EXPECT_FALSE(Job);
				EXPECT_EQ(Scheduler.Find(Request.Asset.AssetPath).State, EAssetThumbnailState::Ready);
				const FAssetThumbnailGenerationStats Stats = Pipeline.GetStats();
				EXPECT_EQ(Stats.DiskHits, 1u);
				EXPECT_EQ(Stats.Loads, 0u);
				EXPECT_EQ(Stats.Renders, 0u);
				EXPECT_EQ(Stats.Readbacks, 0u);
				return;
			}

			ASSERT_TRUE(Job);
			ASSERT_TRUE(Pipeline.CompleteLoad(*Job));
			ASSERT_TRUE(Pipeline.BeginRender(*Job, true));
			ASSERT_TRUE(Pipeline.CompleteRender(*Job));
			ASSERT_TRUE(Pipeline.CompleteReadback(*Job));
			const FByteBuffer Pixels(256 * 256 * 4, std::byte{127});
			ASSERT_TRUE(Pipeline.CompletePixels(*Job, Pixels, 256, 256));
			Pipeline.WaitForCacheTasksForTesting();
			EXPECT_EQ(Scheduler.Find(Request.Asset.AssetPath).State, EAssetThumbnailState::Ready);
			const FAssetThumbnailGenerationStats Stats = Pipeline.GetStats();
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
		DThumbnailManager Registry;

		auto Renderer = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer));
		FAssetThumbnailRequestQueue Scheduler(Registry);
		FAssetThumbnailGeneration Pipeline(
			Scheduler,
			{.CacheRoot = Root, .ObjectExtension = ".png"});
		const FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/EncodedMaterial", "DMaterial", 1);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Request)));
		Pipeline.BeginFrame();
		std::optional<FAssetThumbnailJob> Job = StartThumbnailWork(Pipeline).ColdJob;
		ASSERT_TRUE(Job);
		const std::string CacheKey = Job->ScheduledJob.CacheKey;
		ASSERT_TRUE(Pipeline.CompleteLoad(*Job));
		ASSERT_TRUE(Pipeline.BeginRender(*Job, true));
		ASSERT_TRUE(Pipeline.CompleteRender(*Job));
		ASSERT_TRUE(Pipeline.CompleteReadback(*Job));
		const std::array<uint8, 8> Pixels = {
			255, 0, 0, 255,
			0, 255, 0, 128};
		ASSERT_TRUE(Pipeline.CompletePixels(
			*Job, std::as_bytes(std::span{Pixels}), 2, 1));

		Pipeline.WaitForCacheTasksForTesting();
		FThumbnailObjectStore Store({
			.CacheRoot = Root,
			.ObjectExtension = ".png"});
		Durin::FByteBuffer Encoded;
		ASSERT_EQ(Store.Load(CacheKey, Encoded), EThumbnailObjectLoadResult::Hit);
		auto DecodeResult = Image::DecodeImageFromMemory(Encoded);
		ASSERT_TRUE(DecodeResult) << Durin::Image::ToString(DecodeResult.error());
		auto Decoded = std::move(*DecodeResult);
		EXPECT_EQ(Decoded.Width, 2u);
		EXPECT_EQ(Decoded.Height, 1u);
		const auto ExpectedPixels = std::as_bytes(std::span{Pixels});
		EXPECT_EQ(Decoded.Pixels, Durin::FByteBuffer(
			ExpectedPixels.begin(), ExpectedPixels.end()));
	}

	TEST(FAssetThumbnailContractTests, RenderedPipelineRevalidatesBeforePublishingAndSavingPixels)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("RenderedPipelinePublicationValidation");
		DThumbnailManager Registry;

		auto Renderer = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
			.AssetClassName = "DStaticMesh",
			.RendererName = "Durin.StaticMeshThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer));
		FAssetThumbnailRequestQueue Scheduler(Registry);
		FAssetThumbnailGeneration Pipeline(
			Scheduler,
			{.CacheRoot = Root, .ObjectExtension = ".png"});
		const FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/StaleStaticMesh", "DStaticMesh", 1);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Request)));
		Pipeline.BeginFrame();
		std::optional<FAssetThumbnailJob> Job = StartThumbnailWork(Pipeline).ColdJob;
		ASSERT_TRUE(Job);
		const std::string CacheKey = Job->ScheduledJob.CacheKey;
		ASSERT_TRUE(Pipeline.CompleteLoad(*Job));
		ASSERT_TRUE(Pipeline.BeginRender(*Job, true));
		ASSERT_TRUE(Pipeline.CompleteRender(*Job));
		ASSERT_TRUE(Pipeline.CompleteReadback(*Job));
		const std::array<uint8, 4> Pixels = {255, 255, 255, 255};
		EXPECT_FALSE(Pipeline.CompletePixels(
			*Job,
			std::as_bytes(std::span{Pixels}),
			1,
			1,
			{},
			[] { return std::string("StaticMesh changed before publication."); }));
		EXPECT_EQ(Scheduler.Find(Request.Asset.AssetPath).State, EAssetThumbnailState::Failed);

		FThumbnailObjectStore Store({.CacheRoot = Root, .ObjectExtension = ".png"});
		Durin::FByteBuffer Encoded;
		EXPECT_EQ(Store.Load(CacheKey, Encoded), EThumbnailObjectLoadResult::Miss);
	}




	TEST(FAssetThumbnailContractTests, RenderedPipelineBoundsRendersAndRejectsStaleCompletions)
	{
		DThumbnailManager Registry;

		auto Renderer = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer));
		FAssetThumbnailRequestQueue Scheduler(Registry);
		FAssetThumbnailGeneration Pipeline(
			Scheduler,
			{.CacheRoot = MakeObjectStoreRoot("RenderedPipelineBounds"), .ObjectExtension = ".bin"});
		const FAssetThumbnailRequest FirstRequest =
			MakeThumbnailRequest("/ThumbnailTests/FirstRendered", "DMaterial", 1);
		const FAssetThumbnailRequest SecondRequest =
			MakeThumbnailRequest("/ThumbnailTests/SecondRendered", "DMaterial", 1);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(FirstRequest)));
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(SecondRequest)));
		Pipeline.BeginFrame();
		std::optional<FAssetThumbnailJob> First = StartThumbnailWork(Pipeline).ColdJob;
		std::optional<FAssetThumbnailJob> Second = StartThumbnailWork(Pipeline).ColdJob;
		ASSERT_TRUE(First);
		ASSERT_TRUE(Second);
		ASSERT_TRUE(Pipeline.CompleteLoad(*First));
		ASSERT_TRUE(Pipeline.CompleteLoad(*Second));
		EXPECT_TRUE(Pipeline.BeginRender(*First, true));
		EXPECT_FALSE(Pipeline.BeginRender(*Second, true));
		EXPECT_EQ(Scheduler.Find(SecondRequest.Asset.AssetPath).State,
			EAssetThumbnailState::WaitingForResources);

		Pipeline.BeginFrame();
		EXPECT_TRUE(Pipeline.BeginRender(*Second, true));
		EXPECT_FALSE(Pipeline.CompleteReadback(*Second));
		EXPECT_EQ(Scheduler.Find(SecondRequest.Asset.AssetPath).State, EAssetThumbnailState::Rendering);
		EXPECT_TRUE(Pipeline.CompleteRender(*Second));

		const FAssetThumbnailRequest Replacement =
			MakeThumbnailRequest("/ThumbnailTests/FirstRendered", "DMaterial", 2,
				EAssetThumbnailPriority::Visible, 200);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Replacement)));
		EXPECT_FALSE(Pipeline.CompleteRender(*First));
		EXPECT_TRUE(First->ScheduledJob.GenerationRequest.Cancellation.IsCancelled());
	}

	TEST(FAssetThumbnailContractTests, BackgroundCachePublishesPixelsBeforeSavingAndDecodesWarmHits)
	{
		const auto Root = MakeObjectStoreRoot("BackgroundCacheRoundTrip");
		DThumbnailManager Registry;

		ASSERT_TRUE(Registry.Register(std::make_shared<FTestThumbnailRenderer>(
			FThumbnailRenderingInfo{.AssetClassName = "DGeneratedPixelAsset",
				.RendererName = "BackgroundPixels", .GeneratorSchemaVersion = 1}, true, true)));
		FAssetThumbnailRequestQueue Scheduler(Registry);
		FAssetThumbnailGeneration Pipeline(Scheduler, {.CacheRoot = Root}, {});
		const auto Request = MakeThumbnailRequest(
			"/ThumbnailTests/Background/RoundTrip", "DGeneratedPixelAsset", 1);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Request)));
		const auto Pending = Pipeline.StartNextDetailed();
		EXPECT_FALSE(Pending.ColdJob);
		EXPECT_FALSE(Pending.WarmJob);
		EXPECT_EQ(Scheduler.Find(Request.Asset.AssetPath).State, EAssetThumbnailState::Loading);
		Pipeline.WaitForCacheTasksForTesting();
		auto Cold = StartThumbnailWork(Pipeline);
		ASSERT_TRUE(Cold.ColdJob);
		const FByteBuffer Pixels(4, std::byte{32});
		ASSERT_TRUE(CompleteThumbnailPixels(Pipeline, *Cold.ColdJob,
			Pixels, 1, 1));
		// No cache task is pumped between accepting pixels and observing display readiness.
		EXPECT_EQ(Scheduler.Find(Request.Asset.AssetPath).State, EAssetThumbnailState::Ready);
		EXPECT_EQ(Pipeline.GetStats().CacheWrites, 0u);
		{
			FThumbnailObjectStore Store({.CacheRoot = Root});
			FByteBuffer Encoded;
			EXPECT_EQ(Store.Load(Cold.ColdJob->ScheduledJob.CacheKey, Encoded),
				EThumbnailObjectLoadResult::Miss);
		}
		Pipeline.WaitForCacheTasksForTesting();
		EXPECT_EQ(Pipeline.GetStats().CacheWrites, 1u);
		Scheduler.Cancel(Request.Asset.AssetPath);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Request)));
		EXPECT_FALSE(Pipeline.StartNextDetailed().WarmJob);
		Pipeline.WaitForCacheTasksForTesting();
		auto Warm = StartThumbnailWork(Pipeline);
		ASSERT_TRUE(Warm.WarmJob);
		EXPECT_EQ(Warm.Pixels, Pixels);
		EXPECT_EQ(Pipeline.GetStats().DiskHits, 1u);
		EXPECT_EQ(Pipeline.GetStats().Renders, 1u);

		// A completed worker result still cannot publish after a refresh/cancellation.
		Scheduler.Cancel(Request.Asset.AssetPath);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Request)));
		Pipeline.StartNextDetailed();
		Pipeline.WaitForCacheTasksForTesting();
		Scheduler.Cancel(Request.Asset.AssetPath);
		const auto Canceled = StartThumbnailWork(Pipeline);
		EXPECT_FALSE(Canceled.WarmJob);
		EXPECT_FALSE(Canceled.ColdJob);
		EXPECT_EQ(Pipeline.GetStats().DiskHits, 1u);
	}

	TEST(FAssetThumbnailContractTests, BackgroundCacheBoundsQueuedPixelsAndSkipsCanceledWrites)
	{
		DThumbnailManager Registry;

		ASSERT_TRUE(Registry.Register(std::make_shared<FTestThumbnailRenderer>(
			FThumbnailRenderingInfo{.AssetClassName = "DGeneratedPixelAsset",
				.RendererName = "BackgroundBudget", .GeneratorSchemaVersion = 1})));
		FAssetThumbnailRequestQueue Scheduler(Registry);
		FAssetThumbnailGeneration Pipeline(Scheduler,
			{.CacheRoot = MakeObjectStoreRoot("BackgroundBudget")}, {.CpuPixelBudgetBytes = 8});
		const FByteBuffer Pixels(4, std::byte{127});
		for (uint32 Index = 0; Index < 3; ++Index)
		{
			const auto Request = MakeThumbnailRequest(std::format(
				"/ThumbnailTests/Background/Budget{}", Index), "DGeneratedPixelAsset", 1);
			ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Request)));
			auto Scheduled = Scheduler.TakeNext();
			ASSERT_TRUE(Scheduled);
			FAssetThumbnailJob Job{.ScheduledJob = std::move(*Scheduled)};
			ASSERT_TRUE(CompleteThumbnailPixels(Pipeline, Job, Pixels, 1, 1));
			EXPECT_EQ(Scheduler.Find(Request.Asset.AssetPath).State, EAssetThumbnailState::Ready);
		}
		EXPECT_EQ(Pipeline.GetStats().CacheWritesSkipped, 1u);
		Scheduler.CancelAll();
		Pipeline.WaitForCacheTasksForTesting();
		EXPECT_EQ(Pipeline.GetStats().CacheWrites, 0u);
	}

	TEST(FAssetThumbnailContractTests, BackgroundCacheDrainsPendingSaveBeforeDestruction)
	{
		const auto Root = MakeObjectStoreRoot("BackgroundShutdown");
		DThumbnailManager Registry;

		ASSERT_TRUE(Registry.Register(std::make_shared<FTestThumbnailRenderer>(
			FThumbnailRenderingInfo{.AssetClassName = "DGeneratedPixelAsset",
				.RendererName = "BackgroundShutdown", .GeneratorSchemaVersion = 1})));
		FAssetThumbnailRequestQueue Scheduler(Registry);
		const auto Request = MakeThumbnailRequest(
			"/ThumbnailTests/Background/Shutdown", "DGeneratedPixelAsset", 1);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Request)));
		auto Scheduled = Scheduler.TakeNext();
		ASSERT_TRUE(Scheduled);
		FAssetThumbnailJob Job{.ScheduledJob = std::move(*Scheduled)};
		const FByteBuffer Pixels(4, std::byte{127});
		{
			FAssetThumbnailGeneration Pipeline(Scheduler, {.CacheRoot = Root}, {});
			ASSERT_TRUE(CompleteThumbnailPixels(Pipeline, Job, Pixels, 1, 1));
			EXPECT_EQ(Pipeline.GetStats().CacheWrites, 0u);
		}
		FThumbnailObjectStore Store({.CacheRoot = Root});
		FByteBuffer Encoded;
		ASSERT_EQ(Store.Load(Job.ScheduledJob.CacheKey, Encoded), EThumbnailObjectLoadResult::Hit);
		auto DecodeResult = Image::DecodeImageFromMemory(Encoded);
		ASSERT_TRUE(DecodeResult) << Durin::Image::ToString(DecodeResult.error());
		auto Decoded = std::move(*DecodeResult);
		EXPECT_EQ(Decoded.Pixels, Pixels);
	}

	TEST(FAssetThumbnailContractTests, BackgroundCacheWriteFailureKeepsPixelsReady)
	{
		DThumbnailManager Registry;

		ASSERT_TRUE(Registry.Register(std::make_shared<FTestThumbnailRenderer>(
			FThumbnailRenderingInfo{.AssetClassName = "DGeneratedPixelAsset",
				.RendererName = "BackgroundWriteFailure", .GeneratorSchemaVersion = 1})));
		FAssetThumbnailRequestQueue Scheduler(Registry);
		FAssetThumbnailGeneration Pipeline(Scheduler,
			{.CacheRoot = MakeObjectStoreRoot("BackgroundWriteFailure"), .MaximumObjectBytes = 1}, {});
		const auto Request = MakeThumbnailRequest(
			"/ThumbnailTests/Background/WriteFailure", "DGeneratedPixelAsset", 1);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Request)));
		auto Scheduled = Scheduler.TakeNext();
		ASSERT_TRUE(Scheduled);
		FAssetThumbnailJob Job{.ScheduledJob = std::move(*Scheduled)};
		const FByteBuffer Pixels(4, std::byte{127});
		ASSERT_TRUE(CompleteThumbnailPixels(Pipeline, Job, Pixels, 1, 1));
		Pipeline.WaitForCacheTasksForTesting();
		EXPECT_EQ(Pipeline.GetStats().CacheWriteFailures, 1u);
		EXPECT_EQ(Pipeline.GetStats().Failures, 0u);
		EXPECT_EQ(Scheduler.Find(Request.Asset.AssetPath).State, EAssetThumbnailState::Ready);
	}

	TEST(FAssetThumbnailContractTests, RenderedPipelineTracksWaitFailureCancellationAndRetry)
	{
		DThumbnailManager Registry;

		auto Renderer = std::make_shared<FTestThumbnailRenderer>(FThumbnailRenderingInfo{
			.AssetClassName = "DTextureCube",
			.RendererName = "Durin.TextureCubeThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer));
		FAssetThumbnailRequestQueue Scheduler(Registry);
		FAssetThumbnailGeneration Pipeline(
			Scheduler,
			{.CacheRoot = MakeObjectStoreRoot("RenderedPipelineCounters"), .ObjectExtension = ".bin"});
		const FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/CounterCube", "DTextureCube", 1);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(Request)));
		Pipeline.BeginFrame();
		std::optional<FAssetThumbnailJob> Job = StartThumbnailWork(Pipeline).ColdJob;
		ASSERT_TRUE(Job);
		ASSERT_TRUE(Pipeline.CompleteLoad(*Job));
		EXPECT_TRUE(Pipeline.BeginRender(*Job, false));
		EXPECT_FALSE(Pipeline.BeginRender(*Job, true, "Cube build failed."));
		EXPECT_EQ(Scheduler.Find(Request.Asset.AssetPath).State, EAssetThumbnailState::Failed);

		Pipeline.RecordRetry();
		const FAssetThumbnailRequest CancelRequest =
			MakeThumbnailRequest("/ThumbnailTests/CancelledCube", "DTextureCube", 1);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Scheduler.Request(CancelRequest)));
		std::optional<FAssetThumbnailJob> Cancelled = StartThumbnailWork(Pipeline).ColdJob;
		ASSERT_TRUE(Cancelled);
		Pipeline.Cancel(*Cancelled);

		const FAssetThumbnailGenerationStats Stats = Pipeline.GetStats();
		EXPECT_EQ(Stats.ResourceWaits, 1u);
		EXPECT_EQ(Stats.Failures, 1u);
		EXPECT_EQ(Stats.Retries, 1u);
		EXPECT_EQ(Stats.Cancellations, 1u);
	}

	TEST(FAssetThumbnailContractTests, PreparationPressureDefersInsteadOfFailingAssets)
	{
		DThumbnailManager Manager;
		auto State = std::make_shared<FFakeThumbnailRendererState>();
		State->ResourcePollsBeforeReady = 10000;
		auto Registration = Manager.RegisterScoped(std::make_unique<FFakeThumbnailRenderer>(State));
		ASSERT_TRUE(Registration);
		FAssetThumbnailBudgets Budgets;
		Budgets.MaximumParkedRenderedJobs = 2;
		Budgets.FrameTimeAllowanceSeconds = 1.0;
		FAssetThumbnailPool Pool(Manager, Budgets,
			{.CacheRoot = MakeObjectStoreRoot("PreparationPressure")});
		std::vector<FAssetThumbnailPackageFingerprint> Assets;
		Pool.BeginFrame();
		for (uint32 Index = 0; Index < 5; ++Index)
		{
			Assets.push_back(MakePackage(std::format("/ThumbnailTests/Pressure/Asset{}", Index),
				"DFakeRenderedAsset", 1, 32, 1));
			Pool.Request(Assets.back(), EAssetThumbnailPriority::Visible);
		}
		Pool.EndFrame();
		ASSERT_TRUE(PumpThumbnailPoolUntil(Pool, [&] { return State->Sessions == 2; }));
		for (uint32 Frame = 0; Frame < 16; ++Frame)
		{
			Pool.BeginFrame();
			Pool.EndFrame();
		}
		EXPECT_EQ(State->Sessions, 2u);
		EXPECT_EQ(Pool.GetStats().ParkedResourceWaits, 2u);
		EXPECT_EQ(Pool.GetStats().Generation.Failures, 0u);
		for (const auto& Asset : Assets)
			EXPECT_NE(Pool.Find(Asset.AssetPath).State, EAssetThumbnailState::Failed);
		Pool.CancelPendingRequests();
		EXPECT_EQ(State->SessionDestructions, 2u);
	}

	TEST(FAssetThumbnailContractTests, BackgroundPrefetchSurvivesLastUIReferencer)
	{
		DThumbnailManager Manager;
		auto State = std::make_shared<FFakeThumbnailRendererState>();
		State->ResourcePollsBeforeReady = 10000;
		auto Registration = Manager.RegisterScoped(std::make_unique<FFakeThumbnailRenderer>(State));
		ASSERT_TRUE(Registration);
		FAssetThumbnailPool Pool(Manager, {},
			{.CacheRoot = MakeObjectStoreRoot("BackgroundInterest")});
		const auto Asset = MakePackage("/ThumbnailTests/Background/Interest", "DFakeRenderedAsset", 1, 32, 1);
		Pool.AddReferencer(Asset.AssetPath);
		Pool.Prefetch(Asset);
		ASSERT_TRUE(PumpThumbnailPoolUntil(Pool, [&] { return State->Sessions == 1; }));
		Pool.RemoveReferencer(Asset.AssetPath);
		EXPECT_EQ(State->SessionDestructions, 0u);
		EXPECT_EQ(Pool.GetStats().ParkedResourceWaits, 1u);
		Pool.CancelPendingRequests();
		EXPECT_EQ(State->SessionDestructions, 1u);
	}

	TEST(FAssetThumbnailContractTests, CurrentFrameVisibilityReordersPreviouslyVisibleRequests)
	{
		DThumbnailManager Manager;
		ASSERT_TRUE(Manager.Register(std::make_shared<FTestThumbnailRenderer>(
			FThumbnailRenderingInfo{.AssetClassName = "DTest", .RendererName = "Test", .GeneratorSchemaVersion = 1})));
		FAssetThumbnailRequestQueue Queue(Manager);
		const auto First = MakeThumbnailRequest("/ThumbnailTests/Priority/First", "DTest", 1,
			EAssetThumbnailPriority::Visible);
		const auto Second = MakeThumbnailRequest("/ThumbnailTests/Priority/Second", "DTest", 1,
			EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(IsThumbnailRequestAccepted(Queue.Request(First)));
		ASSERT_TRUE(IsThumbnailRequestAccepted(Queue.Request(Second)));
		Queue.BeginFrame();
		ASSERT_TRUE(IsThumbnailRequestAccepted(Queue.Request(Second)));
		const auto Next = Queue.TakeNext();
		ASSERT_TRUE(Next);
		EXPECT_EQ(Next->GenerationRequest.KeyInput.Asset.AssetPath, Second.Asset.AssetPath);
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
