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
#include "AssetThumbnail.h"

namespace Durin
{
	namespace
	{
		constexpr uint8 WrongSizedThumbnailPng[] = {
			137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82,
			0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
			0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240,
			159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
			0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};

		auto MakePath(std::string_view Value) -> FAssetPath
		{
			InitializeDObjectSystem();
			const std::filesystem::path Root = Testing::GetTestWorkDirectory() / "AssetThumbnailContracts";
			static std::unordered_set<std::filesystem::path> RegisteredRoots;
			if (RegisteredRoots.insert(Root).second)
			{
				std::filesystem::create_directories(Root);
				Testing::RegisterMountPointForTests("/ThumbnailTests/", Root.generic_string() + "/");
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
			int64 LastWriteTimeTicks) -> Editor::FAssetThumbnailPackageFingerprint
		{
			return {
				.VirtualPath = MakePath(Path),
				.AssetClassName = std::move(AssetClassName),
				.PackageFormatVersion = FormatVersion,
				.FileSize = FileSize,
				.LastWriteTimeTicks = LastWriteTimeTicks,
			};
		}

		auto MakeMaterialKeyInput() -> Editor::FAssetThumbnailKeyInput
		{
			return {
				.Asset = MakePackage("/ThumbnailTests/Materials/Preview", "DMaterial", 7, 4096, 100),
				.RendererName = "Durin.MaterialThumbnail",
				.GeneratorSchemaVersion = Editor::FThumbnailVisualContract::SchemaVersion,
				.Output = {},
				.PreviewFixtureIdentity = std::string(Editor::FThumbnailVisualContract::SphereVirtualPath),
				.PreviewFixtureVersion = Editor::FThumbnailVisualContract::SphereFixtureVersion,
				.ShaderContractVersion = 1,
				.Dependencies = {
					MakePackage("/ThumbnailTests/Textures/BaseColor", "DTexture2D", 5, 1024, 200),
					MakePackage("/ThumbnailTests/Materials/Parent", "DMaterial", 7, 2048, 150),
				},
			};
		}

		class FTestThumbnailRenderer final : public Editor::DThumbnailRenderer
		{
		public:
			explicit FTestThumbnailRenderer(
				Editor::FThumbnailRenderingInfo InRegistration,
				bool bInCaptureSucceeds = true,
				bool bInGeneratesPixels = false)
				: Registration(std::move(InRegistration))
				, bCaptureSucceeds(bInCaptureSucceeds)
				, bGeneratesPixels(bInGeneratesPixels)
			{
			}

			auto GetRegistration() const -> Editor::FThumbnailRenderingInfo override
			{
				return Registration;
			}

			auto CaptureGenerationRequest(
				const Editor::FAssetThumbnailRequest& Request,
				uint64 RendererGeneration,
				Editor::FAssetThumbnailGenerationRequest& OutRequest,
				std::string& OutError
			) -> bool override
			{
				if (!bCaptureSucceeds)
				{
					OutError = "The test renderer rejected the asset.";
					return false;
				}
				OutRequest.KeyInput.Asset = Request.Asset;
				OutRequest.KeyInput.RendererName = Registration.RendererName;
				OutRequest.KeyInput.GeneratorSchemaVersion = Registration.GeneratorSchemaVersion;
				OutRequest.RendererGeneration = RendererGeneration;
				OutRequest.RequestSerial = Request.RequestSerial;
				if (bGeneratesPixels)
				{
					auto Pixels = std::make_shared<Editor::FAssetThumbnailGeneratedPixels>();
					Pixels->Pixels = {
						std::byte{32}, std::byte{32}, std::byte{32}, std::byte{255}};
					Pixels->Width = 1;
					Pixels->Height = 1;
					Pixels->AssetRevision = 1;
					OutRequest.KeyInput.Output = {.Width = 1, .Height = 1};
					OutRequest.AssetRevision = Pixels->AssetRevision;
					OutRequest.GeneratedPixels = std::move(Pixels);
				}
				OutError.clear();
				return true;
			}

		private:
			Editor::FThumbnailRenderingInfo Registration;
			bool bCaptureSucceeds = true;
			bool bGeneratesPixels = false;
		};

		struct FFakeThumbnailRendererState
		{
			uint32 Captures = 0;
			uint32 Sessions = 0;
			uint32 PreviewPreparations = 0;
			uint32 PreviewResets = 0;
			uint32 InputDestructions = 0;
			uint32 SessionDestructions = 0;
			uint32 ExtensionDestructions = 0;
			uint32 ResourcePollsBeforeReady = 0;
			uint32 ResourcePolls = 0;
		};

		class FFakeThumbnailInput final : public Editor::IAssetThumbnailGenerationInput
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
			: public Editor::IThumbnailPreviewScene
		{
		public:
			auto GetWorld() -> DWorld* override { return nullptr; }

			auto SetView(
				const Editor::FThumbnailPreviewView& View,
				std::string& OutError) -> bool override
			{
				LastView = View;
				OutError.clear();
				return true;
			}

			auto SetViewEnvironment(
				const FViewEnvironmentOverride& Environment,
				std::string& OutError) -> bool override
			{
				LastEnvironment = Environment;
				OutError.clear();
				return true;
			}

			Editor::FThumbnailPreviewView LastView;
			std::optional<FViewEnvironmentOverride> LastEnvironment;
		};

		class FFakeThumbnailSession final
			: public Editor::IThumbnailRendererSession
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

			auto Load() -> Editor::FThumbnailRendererSessionUpdate override
			{
				return {
					.State = State->ResourcePollsBeforeReady == 0
						? Editor::EThumbnailRendererSessionState::ReadyToRender
						: Editor::EThumbnailRendererSessionState::WaitingForResources,
					.AssetRevision = 17,
					.ResourceRevision = State->ResourcePollsBeforeReady == 0 ? 29u : 0u};
			}

			auto PollResources() -> Editor::FThumbnailRendererSessionUpdate override
			{
				++State->ResourcePolls;
				const bool bReady = State->ResourcePolls
					> State->ResourcePollsBeforeReady;
				return {
					.State = bReady
						? Editor::EThumbnailRendererSessionState::ReadyToRender
						: Editor::EThumbnailRendererSessionState::WaitingForResources,
					.AssetRevision = 17,
					.ResourceRevision = bReady ? 29u : 0u};
			}

			auto PreparePreview(
				Editor::IThumbnailPreviewScene& PreviewScene,
				std::string& OutError) -> bool override
			{
				Editor::FThumbnailPreviewView View;
				View.CameraPosition = {2.0, -2.0, 1.0};
				if (!PreviewScene.SetView(View, OutError)) return false;
				++State->PreviewPreparations;
				return true;
			}

			auto ValidateRevisions(
				uint64 ExpectedAssetRevision,
				uint64 ExpectedResourceRevision,
				std::string& OutError) const -> bool override
			{
				if (ExpectedAssetRevision == 17 && ExpectedResourceRevision == 29)
				{
					OutError.clear();
					return true;
				}
				OutError = "The fake rendered-thumbnail revisions changed.";
				return false;
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
			: public Editor::DThumbnailRenderer
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

			auto GetRegistration() const -> Editor::FThumbnailRenderingInfo override
			{
				return {
					.AssetClassName = AssetClassName,
					.RendererName = "Durin.Tests.FakeThumbnail",
					.GeneratorSchemaVersion = 1};
			}

			auto CaptureGenerationRequest(
				const Editor::FAssetThumbnailRequest& Request,
				uint64 RendererGeneration,
				Editor::FAssetThumbnailGenerationRequest& OutRequest,
				std::string& OutError) -> bool override
			{
				++State->Captures;
				OutRequest.KeyInput = {
					.Asset = Request.Asset,
					.RendererName = "Durin.Tests.FakeThumbnail",
					.GeneratorSchemaVersion = 1,
					.Output = {.Width = 1, .Height = 1},
					.PreviewFixtureIdentity = "/Tests/FakeThumbnail",
					.PreviewFixtureVersion = 1,
					.ShaderContractVersion = 1};
				OutRequest.Input =
					std::make_shared<FFakeThumbnailInput>(State);
				OutRequest.RendererGeneration = RendererGeneration;
				OutRequest.RequestSerial = Request.RequestSerial;
				OutError.clear();
				return true;
			}

			auto CreateGenerationSession(
				const Editor::FAssetThumbnailGenerationRequest& Request,
				const Editor::IAssetThumbnailGenerationInput& Input,
				std::string& OutError)
				-> std::unique_ptr<Editor::IThumbnailRendererSession> override
			{
				(void)Request;
				if (dynamic_cast<const FFakeThumbnailInput*>(&Input) == nullptr)
				{
					OutError = "The fake rendered-thumbnail input type is invalid.";
					return nullptr;
				}
				++State->Sessions;
				OutError.clear();
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
			Editor::EAssetThumbnailPriority Priority = Editor::EAssetThumbnailPriority::Prefetch,
			uint64 FileSize = 100
		) -> Editor::FAssetThumbnailRequest
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
			const Editor::StaticMesh::FStaticMeshThumbnailRendererViewInput Input{
				.LocalBounds = Bounds,
				.OutputAspectRatio = AspectRatio};
			Editor::StaticMesh::FStaticMeshThumbnailRendererView View;
			std::string Error;
			ASSERT_TRUE(CalculateStaticMeshThumbnailRendererView(Input, View, Error)) << Error;

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
		EXPECT_EQ(Editor::StaticMesh::FStaticMeshThumbnailRendererContract::AssetClassName, "DStaticMesh");
		EXPECT_EQ(Editor::StaticMesh::FStaticMeshThumbnailRendererContract::RendererName, "Durin.StaticMeshThumbnail");
		EXPECT_EQ(Editor::StaticMesh::FStaticMeshThumbnailRendererContract::GeneratorSchemaVersion, 2u);
		EXPECT_EQ(
			Editor::StaticMesh::FStaticMeshThumbnailRendererContract::PreviewFixtureIdentity,
			"/Engine/Editor/StaticMeshPreview/LOD0DefaultMaterials");
		EXPECT_EQ(Editor::StaticMesh::FStaticMeshThumbnailRendererContract::PreviewFixtureVersion, 2u);
		EXPECT_EQ(Editor::StaticMesh::FStaticMeshThumbnailRendererContract::ShaderContractVersion, 1u);
		EXPECT_EQ(Editor::StaticMesh::FStaticMeshThumbnailRendererContract::ImageMargin, 0.04);
		EXPECT_EQ(Editor::StaticMesh::FStaticMeshThumbnailRendererContract::LODIndex, 0u);
		EXPECT_FALSE(Editor::StaticMesh::FStaticMeshThumbnailRendererContract::bOutputOpaque);
		const Editor::StaticMesh::FStaticMeshThumbnailRendererViewInput ViewInput;
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
		const Editor::StaticMesh::FStaticMeshThumbnailRendererViewInput Input{
			.LocalBounds = FBox(FVector3(3.0, -8.0, 2.0), FVector3(11.0, 4.0, 6.0)),
			.OutputAspectRatio = 1.25};
		Editor::StaticMesh::FStaticMeshThumbnailRendererView First;
		Editor::StaticMesh::FStaticMeshThumbnailRendererView Second;
		std::string Error;
		ASSERT_TRUE(CalculateStaticMeshThumbnailRendererView(Input, First, Error)) << Error;
		ASSERT_TRUE(CalculateStaticMeshThumbnailRendererView(Input, Second, Error)) << Error;
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
		auto ExpectRejected = [](Editor::StaticMesh::FStaticMeshThumbnailRendererViewInput Input) {
			Editor::StaticMesh::FStaticMeshThumbnailRendererView View;
			std::string Error;
			EXPECT_FALSE(CalculateStaticMeshThumbnailRendererView(Input, View, Error));
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
		const Editor::FAssetThumbnailKeyInput Input = MakeMaterialKeyInput();
		EXPECT_EQ(Editor::BuildAssetThumbnailCacheKey(Input), Editor::BuildAssetThumbnailCacheKey(Input));
	}

	TEST(FAssetThumbnailContractTests, DependencyOrderDoesNotAffectKeys)
	{
		Editor::FAssetThumbnailKeyInput Forward = MakeMaterialKeyInput();
		Editor::FAssetThumbnailKeyInput Reverse = Forward;
		std::ranges::reverse(Reverse.Dependencies);
		EXPECT_EQ(Editor::BuildAssetThumbnailCacheKey(Forward), Editor::BuildAssetThumbnailCacheKey(Reverse));
	}

	TEST(FAssetThumbnailContractTests, EveryKeyContractFieldInvalidatesTheKey)
	{
		const Editor::FAssetThumbnailKeyInput Base = MakeMaterialKeyInput();
		const std::string BaseKey = Editor::BuildAssetThumbnailCacheKey(Base);
		auto ExpectChanged = [&](auto Mutate) {
			Editor::FAssetThumbnailKeyInput Changed = Base;
			Mutate(Changed);
			EXPECT_NE(Editor::BuildAssetThumbnailCacheKey(Changed), BaseKey);
		};

		ExpectChanged([](auto& Value) { Value.Asset.VirtualPath = MakePath("/ThumbnailTests/Materials/Renamed"); });
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
		const FAssetPath Root = MakePath("/ThumbnailTests/Materials/Instance");
		std::vector<Editor::FAssetThumbnailDependencyNode> Forward = {
			{MakePackage("/ThumbnailTests/Materials/Instance", "DMaterialInstance", 7, 100, 10),
				{MakePath("/ThumbnailTests/Textures/BaseColor"), MakePath("/ThumbnailTests/Materials/Parent")}},
			{MakePackage("/ThumbnailTests/Materials/Parent", "DMaterial", 7, 200, 20),
				{MakePath("/ThumbnailTests/Materials/Instance"), MakePath("/ThumbnailTests/Textures/Normal")}},
			{MakePackage("/ThumbnailTests/Textures/BaseColor", "DTexture2D", 5, 300, 30), {}},
			{MakePackage("/ThumbnailTests/Textures/Normal", "DTexture2D", 5, 400, 40),
				{MakePath("/ThumbnailTests/Materials/Parent")}},
		};
		std::vector<Editor::FAssetThumbnailPackageFingerprint> ForwardClosure;
		std::string Error;
		ASSERT_TRUE(Editor::BuildAssetThumbnailDependencyClosure(Root, Forward, ForwardClosure, Error)) << Error;
		ASSERT_EQ(ForwardClosure.size(), 3u);
		EXPECT_EQ(ForwardClosure[0].VirtualPath.GetView(), "/ThumbnailTests/Materials/Parent");
		EXPECT_EQ(ForwardClosure[1].VirtualPath.GetView(), "/ThumbnailTests/Textures/BaseColor");
		EXPECT_EQ(ForwardClosure[2].VirtualPath.GetView(), "/ThumbnailTests/Textures/Normal");

		std::ranges::reverse(Forward);
		for (Editor::FAssetThumbnailDependencyNode& Node : Forward)
			std::ranges::reverse(Node.Dependencies);
		std::vector<Editor::FAssetThumbnailPackageFingerprint> ReverseClosure;
		ASSERT_TRUE(Editor::BuildAssetThumbnailDependencyClosure(Root, Forward, ReverseClosure, Error)) << Error;
		EXPECT_EQ(ReverseClosure, ForwardClosure);
	}

	TEST(FAssetThumbnailContractTests, MissingDependenciesCannotProduceTrustedClosure)
	{
		const FAssetPath Root = MakePath("/ThumbnailTests/Materials/Invalid");
		const std::vector<Editor::FAssetThumbnailDependencyNode> Nodes = {
			{MakePackage("/ThumbnailTests/Materials/Invalid", "DMaterial", 7, 100, 10),
				{MakePath("/ThumbnailTests/Textures/Missing")}},
		};
		std::vector<Editor::FAssetThumbnailPackageFingerprint> Closure;
		std::string Error;
		EXPECT_FALSE(Editor::BuildAssetThumbnailDependencyClosure(Root, Nodes, Closure, Error));
		EXPECT_TRUE(Closure.empty());
		EXPECT_NE(Error.find("/ThumbnailTests/Textures/Missing"), std::string::npos);
	}

	TEST(FAssetThumbnailContractTests, DuplicateRegistryEntriesCannotProduceAmbiguousClosure)
	{
		const FAssetPath Root = MakePath("/ThumbnailTests/Materials/Duplicate");
		const std::vector<Editor::FAssetThumbnailDependencyNode> Nodes = {
			{MakePackage("/ThumbnailTests/Materials/Duplicate", "DMaterial", 7, 100, 10), {}},
			{MakePackage("/ThumbnailTests/Materials/Duplicate", "DMaterial", 7, 100, 10), {}},
		};
		std::vector<Editor::FAssetThumbnailPackageFingerprint> Closure;
		std::string Error;
		EXPECT_FALSE(Editor::BuildAssetThumbnailDependencyClosure(Root, Nodes, Closure, Error));
		EXPECT_TRUE(Closure.empty());
		EXPECT_NE(Error.find("duplicate"), std::string::npos);
	}

	TEST(FAssetThumbnailContractTests, CancellationIsSharedAcrossCapturedRequests)
	{
		const Editor::FAssetThumbnailCancellation Cancellation;
		const Editor::FAssetThumbnailCancellation Captured = Cancellation;
		EXPECT_FALSE(Captured.IsCancelled());
		Cancellation.Cancel();
		EXPECT_TRUE(Captured.IsCancelled());
	}

	TEST(FAssetThumbnailContractTests, InitialFixtureAndBudgetContractsAreBounded)
	{
		const Editor::FThumbnailVisualContract Visual;
		EXPECT_EQ(Visual.Output.Width, Visual.Output.Height);
		EXPECT_EQ(Visual.Output.Width, 256u);
		EXPECT_EQ(Editor::FThumbnailVisualContract::SphereVirtualPath,
			"/Engine/Models/Sphere");
		EXPECT_EQ(Editor::FThumbnailVisualContract::SphereFixtureVersion, 1u);
		EXPECT_EQ(Editor::FThumbnailVisualContract::OutputEncoding, "PNG");
		EXPECT_EQ(Editor::FThumbnailVisualContract::OutputColorSpace, "sRGB");
		EXPECT_TRUE(Visual.bOutputOpaque);
		EXPECT_EQ(Visual.CubeDirectionConvention,
			Editor::EAssetThumbnailCubeDirectionConvention::WorldSpaceReflectionVector);

		const Editor::FAssetThumbnailBudgets Budgets;
		EXPECT_EQ(Budgets.MaximumRendersPerFrame, 1u);
		EXPECT_EQ(Budgets.MaximumLivePreviewScenes, 1u);
		EXPECT_GT(Budgets.MaximumParkedRenderedJobs, 0u);
		EXPECT_GT(Budgets.MaximumRetainedEntries, 0u);
		EXPECT_GT(Budgets.ResourcePollIntervalFrames, 0u);
		EXPECT_GT(Budgets.MaximumResourceWaitFrames, 0u);
		EXPECT_GT(Budgets.MaximumQueuedJobs, 0u);
		EXPECT_GT(Budgets.CpuPixelBudgetBytes, 0u);
		EXPECT_GT(Budgets.GpuTextureBudgetBytes, 0u);
		const Editor::FAssetThumbnailPoolStorageSettings Storage;
		EXPECT_GT(Storage.MaximumObjectBytes, 0u);
		EXPECT_GT(Storage.DiskBudgetBytes, 0u);
	}

	TEST(FAssetThumbnailContractTests, ObjectStorePersistsAndRejectsUnsafeKeys)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("Persistence");
		const std::vector<std::byte> Payload = {
			std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
		{
			Editor::FThumbnailObjectStore Store({
				.CacheRoot = Root,
				.ObjectExtension = ".bin"});
			EXPECT_FALSE(Store.Store("../unsafe", Payload));
			ASSERT_TRUE(Store.Store("material-key-01", Payload));
		}
		Editor::FThumbnailObjectStore WarmStore({
			.CacheRoot = Root,
			.ObjectExtension = ".bin"});
		std::vector<std::byte> Loaded;
		EXPECT_EQ(WarmStore.Load("material-key-01", Loaded), Editor::EThumbnailObjectLoadResult::Hit);
		EXPECT_EQ(Loaded, Payload);
		EXPECT_EQ(WarmStore.GetStats().CacheHits, 1u);
		EXPECT_EQ(WarmStore.Load("../unsafe", Loaded), Editor::EThumbnailObjectLoadResult::Invalid);
	}

	TEST(FAssetThumbnailContractTests, ObjectStoreCleansMissingObjectsAndHonorsDiskBudget)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("Cleanup");
		const std::vector<std::byte> Payload(80, std::byte{7});
		{
			Editor::FThumbnailObjectStore Store({
				.CacheRoot = Root,
				.DiskBudgetBytes = 100,
				.ObjectExtension = ".bin"});
			ASSERT_TRUE(Store.Store("object-key-01", Payload));
			ASSERT_TRUE(Store.Store("object-key-02", Payload));
			EXPECT_EQ(Store.GetStats().Evictions, 1u);
			std::vector<std::byte> Loaded;
			EXPECT_EQ(Store.Load("object-key-01", Loaded), Editor::EThumbnailObjectLoadResult::Miss);
			EXPECT_EQ(Store.Load("object-key-02", Loaded), Editor::EThumbnailObjectLoadResult::Hit);
		}
		for (const auto& Entry : std::filesystem::recursive_directory_iterator(Root / "Objects"))
			if (Entry.path().extension() == ".bin") std::filesystem::remove(Entry.path());
		Editor::FThumbnailObjectStore MissingObjectStore({
			.CacheRoot = Root,
			.DiskBudgetBytes = 100,
			.ObjectExtension = ".bin"});
		std::vector<std::byte> MissingBytes;
		EXPECT_EQ(MissingObjectStore.Load("object-key-02", MissingBytes),
			Editor::EThumbnailObjectLoadResult::Invalid);
		EXPECT_GE(MissingObjectStore.GetStats().Regenerations, 1u);
	}

	TEST(FAssetThumbnailContractTests, BudgetSelectionEvictsOldestUnpinnedAllocations)
	{
		const std::vector<Editor::FThumbnailBudgetEntry> Entries = {
			{.Key = "visible", .Bytes = 60, .LastUsed = 1, .bPinned = true},
			{.Key = "oldest", .Bytes = 40, .LastUsed = 2},
			{.Key = "newest", .Bytes = 30, .LastUsed = 3},
		};
		EXPECT_EQ(Editor::SelectThumbnailBudgetEvictions(Entries, 100),
			std::vector<std::string>({"oldest"}));
		EXPECT_EQ(Editor::SelectThumbnailBudgetEvictions(Entries, 50),
			(std::vector<std::string>{"oldest", "newest"}));
		EXPECT_TRUE(Editor::SelectThumbnailBudgetEvictions(Entries, 200).empty());
	}

	TEST(FAssetThumbnailContractTests, RendererRegistryRejectsInvalidAndDuplicateRegistrations)
	{
		Editor::DThumbnailManager Registry;
		std::string Error;
		EXPECT_FALSE(Registry.Register(nullptr, Error));
		EXPECT_FALSE(Error.empty());

		auto Invalid = std::make_shared<FTestThumbnailRenderer>(
			Editor::FThumbnailRenderingInfo{.AssetClassName = "DMaterial"});
		EXPECT_FALSE(Registry.Register(Invalid, Error));
		EXPECT_FALSE(Error.empty());

		auto Material = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Material, Error)) << Error;
		EXPECT_EQ(Registry.Num(), 1u);

		auto Duplicate = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.OtherMaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		EXPECT_FALSE(Registry.Register(Duplicate, Error));
		EXPECT_NE(Error.find("DMaterial"), std::string::npos);
		EXPECT_EQ(Registry.Num(), 1u);
	}

	TEST(FAssetThumbnailContractTests, RendererRegistryUsesExactClassesAndMonotonicGenerations)
	{
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto Material = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		auto MaterialInstance = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DMaterialInstance",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Material, Error)) << Error;
		ASSERT_TRUE(Registry.Register(MaterialInstance, Error)) << Error;

		const Editor::FThumbnailRendererHandle MaterialHandle = Registry.Find("DMaterial");
		const Editor::FThumbnailRendererHandle InstanceHandle = Registry.Find("DMaterialInstance");
		EXPECT_TRUE(MaterialHandle);
		EXPECT_TRUE(InstanceHandle);
		EXPECT_NE(MaterialHandle.Generation, InstanceHandle.Generation);
		EXPECT_FALSE(Registry.Find("DMaterialInterface"));

		ASSERT_TRUE(Registry.Unregister("DMaterial", Error)) << Error;
		EXPECT_FALSE(Registry.Find("DMaterial"));
		EXPECT_FALSE(Registry.Unregister("DMaterial", Error));

		auto Replacement = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 2});
		ASSERT_TRUE(Registry.Register(Replacement, Error)) << Error;
		EXPECT_GT(Registry.Find("DMaterial").Generation, MaterialHandle.Generation);
	}

	TEST(FAssetThumbnailContractTests, RendererRegistryShutdownDropsRenderersAndClosesRegistration)
	{
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto Renderer = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DTextureCube",
			.RendererName = "Durin.TextureCubeThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer, Error)) << Error;

		Registry.Shutdown();
		EXPECT_TRUE(Registry.IsShuttingDown());
		EXPECT_EQ(Registry.Num(), 0u);
		EXPECT_FALSE(Registry.Find("DTextureCube"));
		EXPECT_FALSE(Registry.Register(Renderer, Error));
		EXPECT_NE(Error.find("shutdown"), std::string::npos);
	}

	TEST(FAssetThumbnailContractTests, ScopedRendererRegistrationRejectsDuplicatesAndAllowsLaterReplacement)
	{
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto FirstState = std::make_shared<FFakeThumbnailRendererState>();
		Editor::FThumbnailRendererRegistrationHandle First = Registry.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(FirstState), Error);
		ASSERT_TRUE(First) << Error;
		const uint64 FirstGeneration = Registry.Find("DFakeRenderedAsset").Generation;

		auto DuplicateState = std::make_shared<FFakeThumbnailRendererState>();
		Editor::FThumbnailRendererRegistrationHandle Duplicate = Registry.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(DuplicateState), Error);
		EXPECT_FALSE(Duplicate);
		EXPECT_NE(Error.find("DFakeRenderedAsset"), std::string::npos);
		EXPECT_EQ(DuplicateState->ExtensionDestructions, 1u);
		EXPECT_EQ(Registry.Find("DFakeRenderedAsset").Generation, FirstGeneration);

		First.Reset();
		EXPECT_FALSE(Registry.Find("DFakeRenderedAsset"));
		EXPECT_EQ(FirstState->ExtensionDestructions, 1u);
		auto ReplacementState = std::make_shared<FFakeThumbnailRendererState>();
		Editor::FThumbnailRendererRegistrationHandle Replacement = Registry.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(ReplacementState), Error);
		ASSERT_TRUE(Replacement) << Error;
		EXPECT_GT(Registry.Find("DFakeRenderedAsset").Generation, FirstGeneration);
	}

	TEST(FAssetThumbnailContractTests, AssetThumbnailReferencesCoalesceAndReleasePoolPins)
	{
		Editor::DThumbnailManager Manager;
		std::string Error;
		auto State = std::make_shared<FFakeThumbnailRendererState>();
		auto Registration = Manager.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(State), Error);
		ASSERT_TRUE(Registration) << Error;
		Editor::FAssetThumbnailPool Pool(
			Manager, {}, {.CacheRoot = MakeObjectStoreRoot("AssetThumbnailReferences")});
		const Editor::FAssetThumbnailRequest Request = MakeThumbnailRequest(
			"/ThumbnailTests/References/Shared", "DFakeRenderedAsset", 1);
		{
			Editor::FAssetThumbnail First(Request.Asset, 128, 96, &Pool);
			Editor::FAssetThumbnail Second(Request.Asset, 512, 512, &Pool);
			First.Request(Editor::EAssetThumbnailPriority::Prefetch);
			Second.Request(Editor::EAssetThumbnailPriority::Visible);
			const Editor::FAssetThumbnailPoolStats Stats = Pool.GetStats();
			EXPECT_EQ(Stats.QueuedJobs, 1u);
			EXPECT_EQ(Stats.PinnedEntries, 1u);
			EXPECT_EQ(Stats.Referencers, 2u);
			EXPECT_EQ(First.GetRequestedWidth(), 128u);
			EXPECT_EQ(Second.GetRequestedWidth(), 512u);
		}
		EXPECT_EQ(Pool.GetStats().PinnedEntries, 0u);
		EXPECT_EQ(Pool.GetStats().Referencers, 0u);
	}

	TEST(FAssetThumbnailContractTests, PoolEvictsUnreferencedMetadataEntriesToBudget)
	{
		Editor::DThumbnailManager Manager;
		Editor::FAssetThumbnailBudgets Budgets;
		Budgets.MaximumRetainedEntries = 2;
		Editor::FAssetThumbnailPool Pool(
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
				Editor::EAssetThumbnailPriority::Prefetch);
		}
		Pool.EndFrame();

		EXPECT_EQ(Pool.GetStats().RetainedEntries, 2u);
	}

	TEST(FAssetThumbnailContractTests, PoolRejectsWrongSizedPersistentThumbnailBeforeUpload)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("WrongSizedWarmObject");
		Editor::DThumbnailManager Manager;
		std::string Error;
		auto Renderer = std::make_shared<FTestThumbnailRenderer>(
			Editor::FThumbnailRenderingInfo{
				.AssetClassName = "DMaterial",
				.RendererName = "Durin.MaterialThumbnail",
				.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Manager.Register(Renderer, Error)) << Error;
		const Editor::FAssetThumbnailRequest Request = MakeThumbnailRequest(
			"/ThumbnailTests/WrongSizedWarm/Material", "DMaterial", 1);

		std::string CacheKey;
		{
			Editor::FAssetThumbnailRequestQueue Scheduler(Manager);
			ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
			auto Scheduled = Scheduler.TakeNext();
			ASSERT_TRUE(Scheduled);
			CacheKey = Scheduled->CacheKey;
		}
		{
			Editor::FThumbnailObjectStore Store({.CacheRoot = Root});
			ASSERT_TRUE(Store.Store(
				CacheKey, std::as_bytes(std::span{WrongSizedThumbnailPng})));
		}

		Editor::FAssetThumbnailPool Pool(Manager, {}, {.CacheRoot = Root});
		Pool.BeginFrame();
		Pool.Request(Request.Asset, Request.Priority);
		Pool.EndFrame();

		EXPECT_EQ(Pool.GetStats().Generation.Retries, 1u);
		EXPECT_EQ(Pool.Find(Request.Asset.VirtualPath).State,
			Editor::EAssetThumbnailState::Queued);
		Editor::FThumbnailObjectStore Store({.CacheRoot = Root});
		std::vector<std::byte> Bytes;
		EXPECT_EQ(Store.Load(CacheKey, Bytes), Editor::EThumbnailObjectLoadResult::Miss);
	}

	TEST(FAssetThumbnailContractTests, SchedulersCreatedBeforeAndAfterScopedRegistrationObserveItsGeneration)
	{
		Editor::DThumbnailManager Registry;
		Editor::FAssetThumbnailRequestQueue BeforeRegistration(Registry);
		std::string Error;
		auto State = std::make_shared<FFakeThumbnailRendererState>();
		Editor::FThumbnailRendererRegistrationHandle Registration = Registry.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(State), Error);
		ASSERT_TRUE(Registration) << Error;
		const uint64 Generation = Registry.Find("DFakeRenderedAsset").Generation;
		ASSERT_NE(Generation, 0u);
		Editor::FAssetThumbnailRequestQueue AfterRegistration(Registry);
		ASSERT_TRUE(BeforeRegistration.Request(MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/BeforeRegistry",
			"DFakeRenderedAsset",
			1), Error)) << Error;
		ASSERT_TRUE(AfterRegistration.Request(MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/AfterRegistry",
			"DFakeRenderedAsset",
			1), Error)) << Error;
		const std::optional<Editor::FAssetThumbnailScheduledRequest> BeforeJob =
			BeforeRegistration.TakeNext();
		const std::optional<Editor::FAssetThumbnailScheduledRequest> AfterJob =
			AfterRegistration.TakeNext();
		ASSERT_TRUE(BeforeJob);
		ASSERT_TRUE(AfterJob);
		EXPECT_EQ(BeforeJob->GenerationRequest.RendererGeneration, Generation);
		EXPECT_EQ(AfterJob->GenerationRequest.RendererGeneration, Generation);

		Registration.Reset();
		EXPECT_FALSE(BeforeRegistration.Transition(
			*BeforeJob,
			Editor::EAssetThumbnailState::Loading,
			Editor::EAssetThumbnailState::WaitingForResources));
		EXPECT_FALSE(AfterRegistration.Transition(
			*AfterJob,
			Editor::EAssetThumbnailState::Loading,
			Editor::EAssetThumbnailState::WaitingForResources));
	}

	TEST(FAssetThumbnailContractTests, RenderedCachesResolveOneServiceBeforeAndAfterRegistration)
	{
		Editor::DThumbnailManager ThumbnailManager;
		Editor::FAssetThumbnailPool BeforeRegistration(
			ThumbnailManager,
			{},
			{.CacheRoot = MakeObjectStoreRoot("CacheBeforeRegistration"),
				.ObjectExtension = ".bin"});
		std::string Error;
		auto State = std::make_shared<FFakeThumbnailRendererState>();
		Editor::FThumbnailRendererRegistrationHandle Registration = ThumbnailManager.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(State), Error);
		ASSERT_TRUE(Registration) << Error;
		Editor::FAssetThumbnailPool AfterRegistration(
			ThumbnailManager,
			{},
			{.CacheRoot = MakeObjectStoreRoot("CacheAfterRegistration"),
				.ObjectExtension = ".bin"});
		const Editor::FAssetThumbnailRequest BeforeRequest = MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/CacheBeforeRegistration",
			"DFakeRenderedAsset",
			1);
		const Editor::FAssetThumbnailRequest AfterRequest = MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/CacheAfterRegistration",
			"DFakeRenderedAsset",
			1);
		BeforeRegistration.Request(
			BeforeRequest.Asset, Editor::EAssetThumbnailPriority::Visible);
		AfterRegistration.Request(
			AfterRequest.Asset, Editor::EAssetThumbnailPriority::Visible);
		EXPECT_EQ(
			BeforeRegistration.Find(BeforeRequest.Asset.VirtualPath).State,
			Editor::EAssetThumbnailState::Queued);
		EXPECT_EQ(
			AfterRegistration.Find(AfterRequest.Asset.VirtualPath).State,
			Editor::EAssetThumbnailState::Queued);

		Registration.Reset();
		BeforeRegistration.EndFrame();
		AfterRegistration.EndFrame();
		EXPECT_NE(
			BeforeRegistration.Find(BeforeRequest.Asset.VirtualPath).State,
			Editor::EAssetThumbnailState::Loading);
		EXPECT_NE(
			AfterRegistration.Find(AfterRequest.Asset.VirtualPath).State,
			Editor::EAssetThumbnailState::Loading);
		EXPECT_EQ(State->ExtensionDestructions, 1u);
	}

	TEST(FAssetThumbnailContractTests, ParkedResourceWaitDoesNotBlockLaterRenderedWork)
	{
		Editor::DThumbnailManager ThumbnailManager;
		std::string Error;
		auto WaitingState = std::make_shared<FFakeThumbnailRendererState>();
		WaitingState->ResourcePollsBeforeReady = 100;
		auto ReadyState = std::make_shared<FFakeThumbnailRendererState>();
		auto WaitingRegistration = ThumbnailManager.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(
				WaitingState, "DWaitingRenderedAsset"), Error);
		ASSERT_TRUE(WaitingRegistration) << Error;
		auto ReadyRegistration = ThumbnailManager.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(
				ReadyState, "DReadyRenderedAsset"), Error);
		ASSERT_TRUE(ReadyRegistration) << Error;
		Editor::FAssetThumbnailBudgets Budgets;
		Budgets.ResourcePollIntervalFrames = 4;
		Editor::FAssetThumbnailPool Cache(
			ThumbnailManager,
			Budgets,
			{.CacheRoot = MakeObjectStoreRoot("ParkedResourceWait"),
				.ObjectExtension = ".bin"});
		const Editor::FAssetThumbnailRequest Waiting = MakeThumbnailRequest(
			"/ThumbnailTests/Parked/Waiting", "DWaitingRenderedAsset", 1,
			Editor::EAssetThumbnailPriority::Visible);
		const Editor::FAssetThumbnailRequest Ready = MakeThumbnailRequest(
			"/ThumbnailTests/Parked/Ready", "DReadyRenderedAsset", 1,
			Editor::EAssetThumbnailPriority::Visible);

		Cache.BeginFrame();
		Cache.Request(Waiting.Asset, Waiting.Priority);
		Cache.EndFrame();
		EXPECT_EQ(Cache.Find(Waiting.Asset.VirtualPath).State,
			Editor::EAssetThumbnailState::WaitingForResources);
		EXPECT_EQ(Cache.GetStats().ParkedResourceWaits, 1u);
		EXPECT_FALSE(Cache.GetStats().bHasActiveJob);

		Cache.BeginFrame();
		Cache.Request(Ready.Asset, Ready.Priority);
		Cache.EndFrame();
		EXPECT_EQ(Cache.Find(Waiting.Asset.VirtualPath).State,
			Editor::EAssetThumbnailState::WaitingForResources);
		EXPECT_NE(Cache.Find(Ready.Asset.VirtualPath).State,
			Editor::EAssetThumbnailState::Queued);
		EXPECT_EQ(Cache.GetStats().ParkedResourceWaits, 1u);
		EXPECT_EQ(Cache.GetStats().PeakParkedResourceWaits, 1u);
	}

	TEST(FAssetThumbnailContractTests, ParkedResourceWaitTimesOutAndReleasesSession)
	{
		Editor::DThumbnailManager ThumbnailManager;
		std::string Error;
		auto State = std::make_shared<FFakeThumbnailRendererState>();
		State->ResourcePollsBeforeReady = 100;
		auto Registration = ThumbnailManager.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(
				State, "DTimeoutRenderedAsset"), Error);
		ASSERT_TRUE(Registration) << Error;
		Editor::FAssetThumbnailBudgets Budgets;
		Budgets.ResourcePollIntervalFrames = 1;
		Budgets.MaximumResourceWaitFrames = 2;
		Editor::FAssetThumbnailPool Cache(
			ThumbnailManager,
			Budgets,
			{.CacheRoot = MakeObjectStoreRoot("ParkedResourceTimeout"),
				.ObjectExtension = ".bin"});
		const Editor::FAssetThumbnailRequest Request = MakeThumbnailRequest(
			"/ThumbnailTests/Parked/Timeout", "DTimeoutRenderedAsset", 1,
			Editor::EAssetThumbnailPriority::Visible);

		Cache.BeginFrame();
		Cache.Request(Request.Asset, Request.Priority);
		Cache.EndFrame();
		Cache.BeginFrame();
		Cache.EndFrame();
		Cache.BeginFrame();
		Cache.EndFrame();

		const Editor::FAssetThumbnailView View =
			Cache.Find(Request.Asset.VirtualPath);
		EXPECT_EQ(View.State, Editor::EAssetThumbnailState::Failed);
		EXPECT_NE(View.Diagnostic.find("Timed out"), std::string::npos);
		EXPECT_EQ(Cache.GetStats().ParkedResourceWaits, 0u);
		EXPECT_EQ(Cache.GetStats().ResourceWaitTimeouts, 1u);
		EXPECT_EQ(State->PreviewResets, 1u);
		EXPECT_EQ(State->SessionDestructions, 1u);
	}

	TEST(FAssetThumbnailContractTests, FakeThumbnailRendererRunsColdAndWarmBeforeScopedRemoval)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("FakeThumbnailRendererWarmHit");
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto State = std::make_shared<FFakeThumbnailRendererState>();
		Editor::FThumbnailRendererRegistrationHandle Registration = Registry.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(State), Error);
		ASSERT_TRUE(Registration) << Error;
		Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
		Editor::FAssetThumbnailGeneration Pipeline(
			Scheduler, {.CacheRoot = Root, .ObjectExtension = ".bin"});
		const Editor::FAssetThumbnailRequest ColdRequest = MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/ColdWarm", "DFakeRenderedAsset", 1);
		ASSERT_TRUE(Scheduler.Request(ColdRequest, Error)) << Error;
		Pipeline.BeginFrame();
		std::optional<Editor::FAssetThumbnailJob> ColdJob = Pipeline.StartNext();
		ASSERT_TRUE(ColdJob);
		Editor::IThumbnailRendererSession* Session =
			ColdJob->ScheduledJob.GenerationRequest.BeginRenderedSession(Error);
		ASSERT_NE(Session, nullptr) << Error;
		const Editor::FThumbnailRendererSessionUpdate Update = Session->Load();
		ASSERT_EQ(Update.State, Editor::EThumbnailRendererSessionState::ReadyToRender);
		FFakeThumbnailPreviewScene PreviewScene;
		ASSERT_TRUE(Session->PreparePreview(PreviewScene, Error)) << Error;
		ASSERT_TRUE(Session->ValidateRevisions(
			Update.AssetRevision, Update.ResourceRevision, Error)) << Error;
		ASSERT_TRUE(Pipeline.CompleteLoad(*ColdJob, Update.AssetRevision));
		ASSERT_TRUE(Pipeline.BeginRender(
			*ColdJob, true, Update.AssetRevision, Update.ResourceRevision));
		ASSERT_TRUE(Pipeline.CompleteRender(
			*ColdJob, Update.AssetRevision, Update.ResourceRevision));
		ASSERT_TRUE(Pipeline.CompleteReadback(
			*ColdJob, Update.AssetRevision, Update.ResourceRevision));
		const std::array<uint8, 4> Encoded = {1, 2, 3, 4};
		ASSERT_TRUE(Pipeline.CompleteEncoding(
			*ColdJob,
			Update.AssetRevision,
			Update.ResourceRevision,
			std::as_bytes(std::span{Encoded})));
		ColdJob->ScheduledJob.GenerationRequest.ReleaseRenderedSession();
		EXPECT_EQ(State->PreviewResets, 1u);

		const Editor::FAssetThumbnailRequest WarmRequest = MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/ColdWarm", "DFakeRenderedAsset", 2);
		ASSERT_TRUE(Scheduler.Request(WarmRequest, Error)) << Error;
		Editor::FAssetThumbnailStartResult Warm = Pipeline.StartNextDetailed();
		EXPECT_FALSE(Warm.ColdJob);
		ASSERT_TRUE(Warm.WarmJob);
		const auto EncodedBytes = std::as_bytes(std::span{Encoded});
		EXPECT_EQ(Warm.EncodedBytes, std::vector<std::byte>(
			EncodedBytes.begin(), EncodedBytes.end()));
		EXPECT_EQ(State->Captures, 2u);
		EXPECT_EQ(State->Sessions, 1u);
		EXPECT_EQ(Pipeline.GetStats().DiskHits, 1u);

		Registration.Reset();
		EXPECT_EQ(State->InputDestructions, 2u);
		EXPECT_EQ(State->SessionDestructions, 1u);
		EXPECT_EQ(State->ExtensionDestructions, 1u);
		EXPECT_FALSE(Scheduler.Request(MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/AfterRemoval",
			"DFakeRenderedAsset",
			3), Error));
	}

	TEST(FAssetThumbnailContractTests, ScopedRemovalReleasesQueuedInputBeforeReturning)
	{
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto State = std::make_shared<FFakeThumbnailRendererState>();
		Editor::FThumbnailRendererRegistrationHandle Registration = Registry.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(State), Error);
		ASSERT_TRUE(Registration) << Error;
		Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
		ASSERT_TRUE(Scheduler.Request(MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/QueuedRemoval",
			"DFakeRenderedAsset",
			1), Error)) << Error;
		// Capture one request without taking it into the rendered lane.
		EXPECT_FALSE(Scheduler.TakeNextGeneratedPixels());
		ASSERT_EQ(State->Captures, 1u);

		Registration.Reset();
		EXPECT_EQ(State->InputDestructions, 1u);
		EXPECT_EQ(State->ExtensionDestructions, 1u);
		EXPECT_FALSE(Scheduler.TakeNext());
	}

	TEST(FAssetThumbnailContractTests, ScopedRemovalRejectsEveryInFlightStateAndDrainsSession)
	{
		const std::array States = {
			Editor::EAssetThumbnailState::Loading,
			Editor::EAssetThumbnailState::WaitingForResources,
			Editor::EAssetThumbnailState::Rendering,
			Editor::EAssetThumbnailState::Readback,
			Editor::EAssetThumbnailState::Encoding,
			Editor::EAssetThumbnailState::Uploading};
		for (size_t StateIndex = 0; StateIndex < States.size(); ++StateIndex)
		{
			SCOPED_TRACE(static_cast<uint32>(States[StateIndex]));
			Editor::DThumbnailManager Registry;
			std::string Error;
			auto State = std::make_shared<FFakeThumbnailRendererState>();
			const std::string AssetClassName = std::format("DFakeRenderedAsset{}", StateIndex);
			Editor::FThumbnailRendererRegistrationHandle Registration = Registry.RegisterScoped(
				std::make_unique<FFakeThumbnailRenderer>(State, AssetClassName),
				Error);
			ASSERT_TRUE(Registration) << Error;
			Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
			const Editor::FAssetThumbnailRequest Request = MakeThumbnailRequest(
				std::format("/ThumbnailTests/FakeRendered/InFlight{}", StateIndex),
				AssetClassName,
				1);
			ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
			std::optional<Editor::FAssetThumbnailScheduledRequest> Job = Scheduler.TakeNext();
			ASSERT_TRUE(Job);
			ASSERT_NE(Job->GenerationRequest.BeginRenderedSession(Error), nullptr) << Error;

			Editor::EAssetThumbnailState Current = Editor::EAssetThumbnailState::Loading;
			for (const Editor::EAssetThumbnailState Next : {
				Editor::EAssetThumbnailState::WaitingForResources,
				Editor::EAssetThumbnailState::Rendering,
				Editor::EAssetThumbnailState::Readback,
				Editor::EAssetThumbnailState::Encoding,
				Editor::EAssetThumbnailState::Uploading})
			{
				if (Current == States[StateIndex]) break;
				ASSERT_TRUE(Scheduler.Transition(*Job, Current, Next, 17, 29));
				Current = Next;
			}
			ASSERT_EQ(Current, States[StateIndex]);
			ASSERT_NE(Job->GenerationRequest.GetInput(), nullptr);
			ASSERT_NE(Job->GenerationRequest.GetRenderedSession(), nullptr);

			Registration.Reset();
			EXPECT_TRUE(Job->GenerationRequest.Cancellation.IsCancelled());
			EXPECT_EQ(Job->GenerationRequest.GetInput(), nullptr);
			EXPECT_EQ(Job->GenerationRequest.GetRenderedSession(), nullptr);
			EXPECT_EQ(State->PreviewResets, 1u);
			EXPECT_EQ(State->SessionDestructions, 1u);
			EXPECT_EQ(State->InputDestructions, 1u);
			EXPECT_EQ(State->ExtensionDestructions, 1u);
			EXPECT_FALSE(Scheduler.Transition(
				*Job, Current, Editor::EAssetThumbnailState::Ready, 17, 29));
		}
	}

	TEST(FAssetThumbnailContractTests, SchedulerSkipsMissingRenderersAndRecordsRendererRejection)
	{
		Editor::DThumbnailManager Registry;
		Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
		std::string Error;
		const Editor::FAssetThumbnailRequest Missing =
			MakeThumbnailRequest("/ThumbnailTests/Unsupported", "DUnsupported", 1);
		EXPECT_FALSE(Scheduler.Request(Missing, Error));
		EXPECT_EQ(Scheduler.NumQueued(), 0u);
		EXPECT_EQ(Scheduler.Find(Missing.Asset.VirtualPath).State, Editor::EAssetThumbnailState::NotRequested);

		auto Rejecting = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1}, false);
		ASSERT_TRUE(Registry.Register(Rejecting, Error)) << Error;
		const Editor::FAssetThumbnailRequest Invalid =
			MakeThumbnailRequest("/ThumbnailTests/Invalid", "DMaterial", 2);
		ASSERT_TRUE(Scheduler.Request(Invalid, Error)) << Error;
		EXPECT_FALSE(Scheduler.TakeNext());
		const Editor::FAssetThumbnailView InvalidView = Scheduler.Find(Invalid.Asset.VirtualPath);
		EXPECT_EQ(InvalidView.State, Editor::EAssetThumbnailState::Invalid);
		EXPECT_EQ(InvalidView.RequestSerial, 2u);
		EXPECT_NE(InvalidView.Diagnostic.find("rejected"), std::string::npos);
	}

	TEST(FAssetThumbnailContractTests, SchedulerDefersAndBoundsRendererCaptureUntilWorkAdmission)
	{
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto State = std::make_shared<FFakeThumbnailRendererState>();
		auto Registration = Registry.RegisterScoped(
			std::make_unique<FFakeThumbnailRenderer>(State), Error);
		ASSERT_TRUE(Registration) << Error;
		Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
		const Editor::FAssetThumbnailRequest First = MakeThumbnailRequest(
			"/ThumbnailTests/DeferredCapture/First", "DFakeRenderedAsset", 1);
		const Editor::FAssetThumbnailRequest Second = MakeThumbnailRequest(
			"/ThumbnailTests/DeferredCapture/Second", "DFakeRenderedAsset", 1,
			Editor::EAssetThumbnailPriority::Visible);

		ASSERT_TRUE(Scheduler.Request(First, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(Second, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(Second, Error)) << Error;
		EXPECT_EQ(State->Captures, 0u);
		EXPECT_EQ(Scheduler.NumQueued(), 2u);

		const auto Admitted = Scheduler.TakeNext();
		ASSERT_TRUE(Admitted);
		EXPECT_EQ(State->Captures, 1u);
		EXPECT_EQ(Admitted->GenerationRequest.KeyInput.Asset.VirtualPath,
			Second.Asset.VirtualPath);
		EXPECT_EQ(Scheduler.NumQueued(), 1u);
	}

	TEST(FAssetThumbnailContractTests, SchedulerCoalescesAndPromotesVisibleRequests)
	{
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto Renderer = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer, Error)) << Error;
		Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
		const Editor::FAssetThumbnailRequest Prefetch =
			MakeThumbnailRequest("/ThumbnailTests/Coalesced", "DMaterial", 1);
		const Editor::FAssetThumbnailRequest Visible =
			MakeThumbnailRequest("/ThumbnailTests/Coalesced", "DMaterial", 2, Editor::EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(Scheduler.Request(Prefetch, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(Visible, Error)) << Error;
		EXPECT_EQ(Scheduler.NumQueued(), 1u);
		EXPECT_EQ(Scheduler.Find(Prefetch.Asset.VirtualPath).RequestSerial, 2u);

		std::optional<Editor::FAssetThumbnailScheduledRequest> Job = Scheduler.TakeNext();
		ASSERT_TRUE(Job);
		EXPECT_EQ(Job->Priority, Editor::EAssetThumbnailPriority::Visible);
		EXPECT_EQ(Job->GenerationRequest.RequestSerial, 2u);
		EXPECT_EQ(Scheduler.Find(Prefetch.Asset.VirtualPath).State, Editor::EAssetThumbnailState::Loading);

		const Editor::FAssetThumbnailRequest NewSerial =
			MakeThumbnailRequest("/ThumbnailTests/Coalesced", "DMaterial", 3, Editor::EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(Scheduler.Request(NewSerial, Error)) << Error;
		EXPECT_TRUE(Job->GenerationRequest.Cancellation.IsCancelled());
		EXPECT_EQ(Scheduler.NumQueued(), 1u);
		const std::optional<Editor::FAssetThumbnailScheduledRequest> Replacement = Scheduler.TakeNext();
		ASSERT_TRUE(Replacement);
		EXPECT_EQ(Replacement->GenerationRequest.RequestSerial, 3u);
	}

	TEST(FAssetThumbnailContractTests, SchedulerPrioritizesVisibleJobsAndEnforcesQueueBudget)
	{
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto Renderer = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DTextureCube",
			.RendererName = "Durin.TextureCubeThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer, Error)) << Error;
		Editor::FAssetThumbnailBudgets Budgets;
		Budgets.MaximumQueuedJobs = 2;
		Editor::FAssetThumbnailRequestQueue Scheduler(Registry, Budgets);
		const Editor::FAssetThumbnailRequest Prefetch =
			MakeThumbnailRequest("/ThumbnailTests/PrefetchCube", "DTextureCube", 1);
		const Editor::FAssetThumbnailRequest Visible =
			MakeThumbnailRequest("/ThumbnailTests/VisibleCube", "DTextureCube", 1, Editor::EAssetThumbnailPriority::Visible);
		const Editor::FAssetThumbnailRequest Overflow =
			MakeThumbnailRequest("/ThumbnailTests/OverflowCube", "DTextureCube", 1);
		ASSERT_TRUE(Scheduler.Request(Prefetch, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(Visible, Error)) << Error;
		EXPECT_FALSE(Scheduler.Request(Overflow, Error));
		EXPECT_NE(Error.find("budget"), std::string::npos);

		const std::optional<Editor::FAssetThumbnailScheduledRequest> First = Scheduler.TakeNext();
		ASSERT_TRUE(First);
		EXPECT_EQ(First->GenerationRequest.KeyInput.Asset.VirtualPath, Visible.Asset.VirtualPath);
		const std::optional<Editor::FAssetThumbnailScheduledRequest> Second = Scheduler.TakeNext();
		ASSERT_TRUE(Second);
		EXPECT_EQ(Second->GenerationRequest.KeyInput.Asset.VirtualPath, Prefetch.Asset.VirtualPath);
		EXPECT_FALSE(Scheduler.TakeNext());
	}

	TEST(FAssetThumbnailContractTests, SchedulerRejectsStaleSerialsAndCancelsReplacedWork)
	{
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto Renderer = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer, Error)) << Error;
		Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
		const Editor::FAssetThumbnailRequest Current =
			MakeThumbnailRequest("/ThumbnailTests/Replaced", "DMaterial", 2);
		ASSERT_TRUE(Scheduler.Request(Current, Error)) << Error;
		std::optional<Editor::FAssetThumbnailScheduledRequest> Active = Scheduler.TakeNext();
		ASSERT_TRUE(Active);

		const Editor::FAssetThumbnailRequest Stale =
			MakeThumbnailRequest("/ThumbnailTests/Replaced", "DMaterial", 1);
		EXPECT_FALSE(Scheduler.Request(Stale, Error));
		EXPECT_FALSE(Active->GenerationRequest.Cancellation.IsCancelled());

		const Editor::FAssetThumbnailRequest Changed =
			MakeThumbnailRequest("/ThumbnailTests/Replaced", "DMaterial", 3, Editor::EAssetThumbnailPriority::Visible, 200);
		ASSERT_TRUE(Scheduler.Request(Changed, Error)) << Error;
		EXPECT_TRUE(Active->GenerationRequest.Cancellation.IsCancelled());
		EXPECT_EQ(Scheduler.NumQueued(), 1u);
		EXPECT_EQ(Scheduler.Find(Changed.Asset.VirtualPath).RequestSerial, 3u);
	}

	TEST(FAssetThumbnailContractTests, MixedRenderedKindsSharePriorityCoalescingAndQueueBudgets)
	{
		Editor::DThumbnailManager Registry;
		std::string Error;
		for (const Editor::FThumbnailRenderingInfo Registration : {
			Editor::FThumbnailRenderingInfo{
				.AssetClassName = "DMaterial",
				.RendererName = "Durin.MaterialThumbnail",
				.GeneratorSchemaVersion = 1},
			Editor::FThumbnailRenderingInfo{
				.AssetClassName = "DTextureCube",
				.RendererName = "Durin.TextureCubeThumbnail",
				.GeneratorSchemaVersion = 1},
			Editor::FThumbnailRenderingInfo{
				.AssetClassName = "DStaticMesh",
				.RendererName = "Durin.StaticMeshThumbnail",
				.GeneratorSchemaVersion = 1}})
		{
			ASSERT_TRUE(Registry.Register(
				std::make_shared<FTestThumbnailRenderer>(Registration), Error)) << Error;
		}
		Editor::FAssetThumbnailBudgets Budgets;
		Budgets.MaximumQueuedJobs = 3;
		Editor::FAssetThumbnailRequestQueue Scheduler(Registry, Budgets);
		const Editor::FAssetThumbnailRequest Material = MakeThumbnailRequest(
			"/ThumbnailTests/Mixed/Material", "DMaterial", 1);
		const Editor::FAssetThumbnailRequest Cube = MakeThumbnailRequest(
			"/ThumbnailTests/Mixed/Cube", "DTextureCube", 1);
		const Editor::FAssetThumbnailRequest MeshPrefetch = MakeThumbnailRequest(
			"/ThumbnailTests/Mixed/Mesh", "DStaticMesh", 1);
		const Editor::FAssetThumbnailRequest MeshVisible = MakeThumbnailRequest(
			"/ThumbnailTests/Mixed/Mesh", "DStaticMesh", 2,
			Editor::EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(Scheduler.Request(Material, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(Cube, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(MeshPrefetch, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(MeshVisible, Error)) << Error;
		EXPECT_EQ(Scheduler.NumQueued(), 3u);

		const std::optional<Editor::FAssetThumbnailScheduledRequest> First = Scheduler.TakeNext();
		ASSERT_TRUE(First);
		EXPECT_EQ(
			First->GenerationRequest.KeyInput.Asset.VirtualPath,
			MeshVisible.Asset.VirtualPath);
		EXPECT_EQ(First->GenerationRequest.RequestSerial, 2u);
		const std::optional<Editor::FAssetThumbnailScheduledRequest> Second = Scheduler.TakeNext();
		const std::optional<Editor::FAssetThumbnailScheduledRequest> Third = Scheduler.TakeNext();
		ASSERT_TRUE(Second);
		ASSERT_TRUE(Third);
		EXPECT_NE(
			Second->GenerationRequest.KeyInput.Asset.AssetClassName,
			Third->GenerationRequest.KeyInput.Asset.AssetClassName);
		EXPECT_FALSE(Scheduler.TakeNext());
	}

	TEST(FAssetThumbnailContractTests, SchedulerShutdownCancelsWorkAndRejectsNewRequests)
	{
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto Renderer = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer, Error)) << Error;
		Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
		const Editor::FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/Shutdown", "DMaterial", 1);
		ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
		std::optional<Editor::FAssetThumbnailScheduledRequest> Active = Scheduler.TakeNext();
		ASSERT_TRUE(Active);

		Scheduler.Shutdown();
		EXPECT_TRUE(Scheduler.IsShuttingDown());
		EXPECT_TRUE(Active->GenerationRequest.Cancellation.IsCancelled());
		EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State, Editor::EAssetThumbnailState::NotRequested);
		EXPECT_FALSE(Scheduler.Request(Request, Error));
		EXPECT_NE(Error.find("shutdown"), std::string::npos);
	}

	TEST(FAssetThumbnailContractTests, RenderedPipelinePublishesColdOutputAndServesWarmHit)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("RenderedPipelineWarmHit");
		auto RunRequest = [&](uint64 Serial, bool bExpectWarmHit) {
			Editor::DThumbnailManager Registry;
			std::string Error;
			auto Renderer = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
				.AssetClassName = "DMaterial",
				.RendererName = "Durin.MaterialThumbnail",
				.GeneratorSchemaVersion = 1});
			EXPECT_TRUE(Registry.Register(Renderer, Error)) << Error;
			Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
			Editor::FAssetThumbnailGeneration Pipeline(
				Scheduler,
				{.CacheRoot = Root, .ObjectExtension = ".bin"});
			const Editor::FAssetThumbnailRequest Request =
				MakeThumbnailRequest("/ThumbnailTests/PersistentMaterial", "DMaterial", Serial);
			EXPECT_TRUE(Scheduler.Request(Request, Error)) << Error;
			Pipeline.BeginFrame();
			std::optional<Editor::FAssetThumbnailJob> Job = Pipeline.StartNext();
			if (bExpectWarmHit)
			{
				EXPECT_FALSE(Job);
				EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State, Editor::EAssetThumbnailState::Ready);
				const Editor::FAssetThumbnailGenerationStats Stats = Pipeline.GetStats();
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
			const std::vector<std::byte> Encoded = {
				std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
			ASSERT_TRUE(Pipeline.CompleteEncoding(*Job, 10, 20, Encoded));
			EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State, Editor::EAssetThumbnailState::Ready);
			const Editor::FAssetThumbnailGenerationStats Stats = Pipeline.GetStats();
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
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto Renderer = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer, Error)) << Error;
		Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
		Editor::FAssetThumbnailGeneration Pipeline(
			Scheduler,
			{.CacheRoot = Root, .ObjectExtension = ".png"});
		const Editor::FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/EncodedMaterial", "DMaterial", 1);
		ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
		Pipeline.BeginFrame();
		std::optional<Editor::FAssetThumbnailJob> Job = Pipeline.StartNext();
		ASSERT_TRUE(Job);
		const std::string CacheKey = Job->ScheduledJob.CacheKey;
		ASSERT_TRUE(Pipeline.CompleteLoad(*Job, 10));
		ASSERT_TRUE(Pipeline.BeginRender(*Job, true, 10, 20));
		ASSERT_TRUE(Pipeline.CompleteRender(*Job, 10, 20));
		ASSERT_TRUE(Pipeline.CompleteReadback(*Job, 10, 20));
		const std::array<uint8, 8> Pixels = {
			255, 0, 0, 255,
			0, 255, 0, 128};
		ASSERT_TRUE(Pipeline.CompletePixels(
			*Job, 10, 20, std::as_bytes(std::span{Pixels}), 2, 1));

		Editor::FThumbnailObjectStore Store({
			.CacheRoot = Root,
			.ObjectExtension = ".png"});
		std::vector<std::byte> Encoded;
		ASSERT_EQ(Store.Load(CacheKey, Encoded), Editor::EThumbnailObjectLoadResult::Hit);
		Image::FDecodedImage Decoded;
		ASSERT_TRUE(Image::DecodeImageFromMemory(Encoded, Decoded, Error)) << Error;
		EXPECT_EQ(Decoded.Width, 2u);
		EXPECT_EQ(Decoded.Height, 1u);
		const auto ExpectedPixels = std::as_bytes(std::span{Pixels});
		EXPECT_EQ(Decoded.Pixels, std::vector<std::byte>(
			ExpectedPixels.begin(), ExpectedPixels.end()));
	}

	TEST(FAssetThumbnailContractTests, RenderedPipelineRevalidatesAfterEncodingBeforePublication)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("RenderedPipelinePublicationValidation");
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto Renderer = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DStaticMesh",
			.RendererName = "Durin.StaticMeshThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer, Error)) << Error;
		Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
		Editor::FAssetThumbnailGeneration Pipeline(
			Scheduler,
			{.CacheRoot = Root, .ObjectExtension = ".png"});
		const Editor::FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/StaleStaticMesh", "DStaticMesh", 1);
		ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
		Pipeline.BeginFrame();
		std::optional<Editor::FAssetThumbnailJob> Job = Pipeline.StartNext();
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
			std::as_bytes(std::span{Pixels}),
			1,
			1,
			{},
			[] { return std::string("StaticMesh changed before publication."); }));
		EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State, Editor::EAssetThumbnailState::Failed);

		Editor::FThumbnailObjectStore Store({.CacheRoot = Root, .ObjectExtension = ".png"});
		std::vector<std::byte> Encoded;
		EXPECT_EQ(Store.Load(CacheKey, Encoded), Editor::EThumbnailObjectLoadResult::Miss);
	}

	TEST(FAssetThumbnailContractTests, GeneratedPixelsPublishWithoutPreviewRenderAllowance)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("GeneratedPixels");
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto Renderer = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DTerrainHeightmap",
			.RendererName = "TerrainHeightmapCanonicalThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer, Error)) << Error;
		Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
		Editor::FAssetThumbnailGeneration Pipeline(
			Scheduler, {.CacheRoot = Root, .ObjectExtension = ".png"},
			{.MaximumRendersPerFrame = 0});
		const Editor::FAssetThumbnailRequest Request = MakeThumbnailRequest(
			"/ThumbnailTests/GeneratedTerrain", "DTerrainHeightmap", 1);
		ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
		Pipeline.BeginFrame();
		auto Job = Pipeline.StartNext();
		ASSERT_TRUE(Job);
		const std::array<uint8, 4> Pixels{32, 32, 32, 255};
		ASSERT_TRUE(Pipeline.CompleteGeneratedPixels(
			*Job, 7, std::as_bytes(std::span{Pixels}), 1, 1));
		EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State,
			Editor::EAssetThumbnailState::Ready);
		EXPECT_EQ(Pipeline.GetStats().Renders, 0u);
	}

	TEST(FAssetThumbnailContractTests, GeneratedPixelsWithCapturedRevisionServeWarmHit)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("GeneratedPixelsWarmHit");
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto Renderer = std::make_shared<FTestThumbnailRenderer>(
			Editor::FThumbnailRenderingInfo{
				.AssetClassName = "DTerrainHeightmap",
				.RendererName = "TerrainHeightmapCanonicalThumbnail",
				.GeneratorSchemaVersion = 1},
			true,
			true);
		ASSERT_TRUE(Registry.Register(Renderer, Error)) << Error;

		const Editor::FAssetThumbnailRequest Request = MakeThumbnailRequest(
			"/ThumbnailTests/GeneratedTerrainWarmHit", "DTerrainHeightmap", 1);
		{
			Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
			Editor::FAssetThumbnailGeneration Pipeline(
				Scheduler, {.CacheRoot = Root, .ObjectExtension = ".png"});
			ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
			auto Cold = Pipeline.StartNextDetailed();
			ASSERT_TRUE(Cold.ColdJob);
			const auto& Generated =
				*Cold.ColdJob->ScheduledJob.GenerationRequest.GeneratedPixels;
			ASSERT_TRUE(Pipeline.CompleteGeneratedPixels(
				*Cold.ColdJob,
				Generated.AssetRevision,
				Generated.Pixels,
				Generated.Width,
				Generated.Height));
		}

		Editor::FAssetThumbnailRequestQueue WarmScheduler(Registry);
		Editor::FAssetThumbnailGeneration WarmPipeline(
			WarmScheduler, {.CacheRoot = Root, .ObjectExtension = ".png"});
		ASSERT_TRUE(WarmScheduler.Request(Request, Error)) << Error;
		Editor::FAssetThumbnailStartResult Warm =
			WarmPipeline.StartNextDetailed();
		EXPECT_FALSE(Warm.ColdJob);
		ASSERT_TRUE(Warm.WarmJob);
		EXPECT_EQ(WarmScheduler.Find(Request.Asset.VirtualPath).State,
			Editor::EAssetThumbnailState::Ready);
		EXPECT_EQ(WarmPipeline.GetStats().DiskHits, 1u);
	}

	TEST(FAssetThumbnailContractTests, GeneratedPixelsBypassResourceBoundRenderedJob)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("GeneratedPixelsFastLane");
		Editor::DThumbnailManager Registry;
		std::string Error;
		ASSERT_TRUE(Registry.Register(std::make_shared<FTestThumbnailRenderer>(
			Editor::FThumbnailRenderingInfo{
				.AssetClassName = "DWaitingRenderedAsset",
				.RendererName = "WaitingThumbnail",
				.GeneratorSchemaVersion = 1}), Error)) << Error;
		ASSERT_TRUE(Registry.Register(std::make_shared<FTestThumbnailRenderer>(
			Editor::FThumbnailRenderingInfo{
				.AssetClassName = "DTerrainHeightmap",
				.RendererName = "TerrainHeightmapCanonicalThumbnail",
				.GeneratorSchemaVersion = 1}, true, true), Error)) << Error;
		Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
		Editor::FAssetThumbnailGeneration Pipeline(
			Scheduler, {.CacheRoot = Root, .ObjectExtension = ".png"});
		const Editor::FAssetThumbnailRequest Waiting = MakeThumbnailRequest(
			"/ThumbnailTests/WaitingRendered", "DWaitingRenderedAsset", 1);
		const Editor::FAssetThumbnailRequest Terrain = MakeThumbnailRequest(
			"/ThumbnailTests/GeneratedTerrainFastLane", "DTerrainHeightmap", 1);
		ASSERT_TRUE(Scheduler.Request(Waiting, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(Terrain, Error)) << Error;
		Pipeline.BeginFrame();
		ASSERT_TRUE(Pipeline.StartNext());
		auto Generated = Pipeline.StartNextGeneratedPixelsDetailed();
		ASSERT_TRUE(Generated.ColdJob);
		ASSERT_TRUE(Generated.ColdJob->ScheduledJob.GenerationRequest.GeneratedPixels);
		const auto& Pixels = *Generated.ColdJob->ScheduledJob.GenerationRequest.GeneratedPixels;
		ASSERT_TRUE(Pipeline.CompleteGeneratedPixels(
			*Generated.ColdJob, Pixels.AssetRevision,
			Pixels.Pixels, Pixels.Width, Pixels.Height));
		EXPECT_EQ(Scheduler.Find(Waiting.Asset.VirtualPath).State,
			Editor::EAssetThumbnailState::Loading);
		EXPECT_EQ(Scheduler.Find(Terrain.Asset.VirtualPath).State,
			Editor::EAssetThumbnailState::Ready);
	}

	TEST(FAssetThumbnailContractTests, RenderedPipelineBoundsRendersAndRejectsStaleCompletions)
	{
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto Renderer = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DMaterial",
			.RendererName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer, Error)) << Error;
		Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
		Editor::FAssetThumbnailGeneration Pipeline(
			Scheduler,
			{.CacheRoot = MakeObjectStoreRoot("RenderedPipelineBounds"), .ObjectExtension = ".bin"});
		const Editor::FAssetThumbnailRequest FirstRequest =
			MakeThumbnailRequest("/ThumbnailTests/FirstRendered", "DMaterial", 1);
		const Editor::FAssetThumbnailRequest SecondRequest =
			MakeThumbnailRequest("/ThumbnailTests/SecondRendered", "DMaterial", 1);
		ASSERT_TRUE(Scheduler.Request(FirstRequest, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(SecondRequest, Error)) << Error;
		Pipeline.BeginFrame();
		std::optional<Editor::FAssetThumbnailJob> First = Pipeline.StartNext();
		std::optional<Editor::FAssetThumbnailJob> Second = Pipeline.StartNext();
		ASSERT_TRUE(First);
		ASSERT_TRUE(Second);
		ASSERT_TRUE(Pipeline.CompleteLoad(*First, 10));
		ASSERT_TRUE(Pipeline.CompleteLoad(*Second, 11));
		EXPECT_TRUE(Pipeline.BeginRender(*First, true, 10, 20));
		EXPECT_FALSE(Pipeline.BeginRender(*Second, true, 11, 21));
		EXPECT_EQ(Scheduler.Find(SecondRequest.Asset.VirtualPath).State,
			Editor::EAssetThumbnailState::WaitingForResources);

		Pipeline.BeginFrame();
		EXPECT_TRUE(Pipeline.BeginRender(*Second, true, 11, 21));
		EXPECT_FALSE(Pipeline.CompleteRender(*Second, 12, 21));
		EXPECT_EQ(Scheduler.Find(SecondRequest.Asset.VirtualPath).State, Editor::EAssetThumbnailState::Rendering);
		EXPECT_TRUE(Pipeline.CompleteRender(*Second, 11, 21));

		const Editor::FAssetThumbnailRequest Replacement =
			MakeThumbnailRequest("/ThumbnailTests/FirstRendered", "DMaterial", 2,
				Editor::EAssetThumbnailPriority::Visible, 200);
		ASSERT_TRUE(Scheduler.Request(Replacement, Error)) << Error;
		EXPECT_FALSE(Pipeline.CompleteRender(*First, 10, 20));
		EXPECT_TRUE(First->ScheduledJob.GenerationRequest.Cancellation.IsCancelled());
	}

	TEST(FAssetThumbnailContractTests, RenderedPipelineTracksWaitFailureCancellationAndRetry)
	{
		Editor::DThumbnailManager Registry;
		std::string Error;
		auto Renderer = std::make_shared<FTestThumbnailRenderer>(Editor::FThumbnailRenderingInfo{
			.AssetClassName = "DTextureCube",
			.RendererName = "Durin.TextureCubeThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Renderer, Error)) << Error;
		Editor::FAssetThumbnailRequestQueue Scheduler(Registry);
		Editor::FAssetThumbnailGeneration Pipeline(
			Scheduler,
			{.CacheRoot = MakeObjectStoreRoot("RenderedPipelineCounters"), .ObjectExtension = ".bin"});
		const Editor::FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/CounterCube", "DTextureCube", 1);
		ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
		Pipeline.BeginFrame();
		std::optional<Editor::FAssetThumbnailJob> Job = Pipeline.StartNext();
		ASSERT_TRUE(Job);
		ASSERT_TRUE(Pipeline.CompleteLoad(*Job, 30));
		EXPECT_TRUE(Pipeline.BeginRender(*Job, false, 30, 0));
		EXPECT_FALSE(Pipeline.BeginRender(*Job, true, 30, 0, "Cube build failed."));
		EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State, Editor::EAssetThumbnailState::Failed);

		Pipeline.RecordRetry();
		const Editor::FAssetThumbnailRequest CancelRequest =
			MakeThumbnailRequest("/ThumbnailTests/CancelledCube", "DTextureCube", 1);
		ASSERT_TRUE(Scheduler.Request(CancelRequest, Error)) << Error;
		std::optional<Editor::FAssetThumbnailJob> Cancelled = Pipeline.StartNext();
		ASSERT_TRUE(Cancelled);
		Pipeline.Cancel(*Cancelled);

		const Editor::FAssetThumbnailGenerationStats Stats = Pipeline.GetStats();
		EXPECT_EQ(Stats.ResourceWaits, 1u);
		EXPECT_EQ(Stats.Failures, 1u);
		EXPECT_EQ(Stats.Retries, 1u);
		EXPECT_EQ(Stats.Cancellations, 1u);
	}
} // namespace Durin
