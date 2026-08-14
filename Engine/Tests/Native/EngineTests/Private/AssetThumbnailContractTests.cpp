#include <gtest/gtest.h>

#include "EngineTestSupport.h"
#include "Image/ImageDecoder.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "Thumbnail/AssetThumbnailProvider.h"
#include "Thumbnail/AssetThumbnailObjectStore.h"
#include "Thumbnail/AssetThumbnailScheduler.h"
#include "Thumbnail/RenderedAssetThumbnailExtension.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"
#include "Thumbnail/RenderedAssetThumbnailPipeline.h"
#include "Thumbnail/RenderedAssetThumbnailPreviewScene.h"
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
				.ProviderName = "Durin.MaterialThumbnail",
				.GeneratorSchemaVersion = Editor::FRenderedAssetThumbnailVisualContract::SchemaVersion,
				.Output = {},
				.PreviewFixtureIdentity = std::string(Editor::FRenderedAssetThumbnailVisualContract::SphereVirtualPath),
				.PreviewFixtureVersion = Editor::FRenderedAssetThumbnailVisualContract::SphereFixtureVersion,
				.ShaderContractVersion = 1,
				.Dependencies = {
					MakePackage("/ThumbnailTests/Textures/BaseColor", "DTexture2D", 5, 1024, 200),
					MakePackage("/ThumbnailTests/Materials/Parent", "DMaterial", 7, 2048, 150),
				},
			};
		}

		class FTestThumbnailProvider final : public Editor::IAssetThumbnailProvider
		{
		public:
			explicit FTestThumbnailProvider(
				Editor::FAssetThumbnailProviderRegistration InRegistration,
				bool bInCaptureSucceeds = true)
				: Registration(std::move(InRegistration))
				, bCaptureSucceeds(bInCaptureSucceeds)
			{
			}

			auto GetRegistration() const -> Editor::FAssetThumbnailProviderRegistration override
			{
				return Registration;
			}

			auto CaptureGenerationRequest(
				const Editor::FAssetThumbnailRequest& Request,
				uint64 ProviderGeneration,
				Editor::FAssetThumbnailGenerationRequest& OutRequest,
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
			Editor::FAssetThumbnailProviderRegistration Registration;
			bool bCaptureSucceeds = true;
		};

		struct FFakeRenderedExtensionState
		{
			uint32 Captures = 0;
			uint32 Sessions = 0;
			uint32 PreviewPreparations = 0;
			uint32 PreviewResets = 0;
			uint32 InputDestructions = 0;
			uint32 SessionDestructions = 0;
			uint32 ExtensionDestructions = 0;
		};

		class FFakeRenderedThumbnailInput final : public Editor::IAssetThumbnailGenerationInput
		{
		public:
			explicit FFakeRenderedThumbnailInput(
				std::shared_ptr<FFakeRenderedExtensionState> InState)
				: State(std::move(InState))
			{
			}

			~FFakeRenderedThumbnailInput() override
			{
				++State->InputDestructions;
			}

		private:
			std::shared_ptr<FFakeRenderedExtensionState> State;
		};

		class FFakeRenderedThumbnailPreviewScene final
			: public Editor::IRenderedAssetThumbnailPreviewScene
		{
		public:
			auto GetWorld() -> DWorld* override { return nullptr; }

			auto SetView(
				const Editor::FRenderedAssetThumbnailPreviewView& View,
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

			Editor::FRenderedAssetThumbnailPreviewView LastView;
			std::optional<FViewEnvironmentOverride> LastEnvironment;
		};

		class FFakeRenderedThumbnailSession final
			: public Editor::IRenderedAssetThumbnailGenerationSession
		{
		public:
			explicit FFakeRenderedThumbnailSession(
				std::shared_ptr<FFakeRenderedExtensionState> InState)
				: State(std::move(InState))
			{
			}

			~FFakeRenderedThumbnailSession() override
			{
				++State->SessionDestructions;
			}

			auto Load() -> Editor::FRenderedAssetThumbnailSessionUpdate override
			{
				return {
					.State = Editor::ERenderedAssetThumbnailSessionState::ReadyToRender,
					.AssetRevision = 17,
					.ResourceRevision = 29};
			}

			auto PollResources() -> Editor::FRenderedAssetThumbnailSessionUpdate override
			{
				return Load();
			}

			auto PreparePreview(
				Editor::IRenderedAssetThumbnailPreviewScene& PreviewScene,
				std::string& OutError) -> bool override
			{
				Editor::FRenderedAssetThumbnailPreviewView View;
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
			std::shared_ptr<FFakeRenderedExtensionState> State;
			bool bReset = false;
		};

		class FFakeRenderedThumbnailExtension final
			: public Editor::IRenderedAssetThumbnailExtension
		{
		public:
			explicit FFakeRenderedThumbnailExtension(
				std::shared_ptr<FFakeRenderedExtensionState> InState,
				std::string InAssetClassName = "DFakeRenderedAsset")
				: State(std::move(InState))
				, AssetClassName(std::move(InAssetClassName))
			{
			}

			~FFakeRenderedThumbnailExtension() override
			{
				++State->ExtensionDestructions;
			}

			auto GetRegistration() const -> Editor::FAssetThumbnailProviderRegistration override
			{
				return {
					.AssetClassName = AssetClassName,
					.ProviderName = "Durin.Tests.FakeRenderedThumbnail",
					.GeneratorSchemaVersion = 1};
			}

			auto CaptureGenerationRequest(
				const Editor::FAssetThumbnailRequest& Request,
				uint64 ProviderGeneration,
				Editor::FAssetThumbnailGenerationRequest& OutRequest,
				std::string& OutError) -> bool override
			{
				++State->Captures;
				OutRequest.KeyInput = {
					.Asset = Request.Asset,
					.ProviderName = "Durin.Tests.FakeRenderedThumbnail",
					.GeneratorSchemaVersion = 1,
					.Output = {.Width = 1, .Height = 1},
					.PreviewFixtureIdentity = "/Tests/FakeRenderedThumbnail",
					.PreviewFixtureVersion = 1,
					.ShaderContractVersion = 1};
				OutRequest.Input =
					std::make_shared<FFakeRenderedThumbnailInput>(State);
				OutRequest.ProviderGeneration = ProviderGeneration;
				OutRequest.RequestSerial = Request.RequestSerial;
				OutError.clear();
				return true;
			}

			auto CreateGenerationSession(
				const Editor::FAssetThumbnailGenerationRequest& Request,
				const Editor::IAssetThumbnailGenerationInput& Input,
				std::string& OutError)
				-> std::unique_ptr<Editor::IRenderedAssetThumbnailGenerationSession> override
			{
				(void)Request;
				if (dynamic_cast<const FFakeRenderedThumbnailInput*>(&Input) == nullptr)
				{
					OutError = "The fake rendered-thumbnail input type is invalid.";
					return nullptr;
				}
				++State->Sessions;
				OutError.clear();
				return std::make_unique<FFakeRenderedThumbnailSession>(State);
			}

		private:
			std::shared_ptr<FFakeRenderedExtensionState> State;
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
			const Editor::StaticMesh::FStaticMeshAssetThumbnailViewInput Input{
				.LocalBounds = Bounds,
				.OutputAspectRatio = AspectRatio};
			Editor::StaticMesh::FStaticMeshAssetThumbnailView View;
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
		EXPECT_EQ(Editor::StaticMesh::FStaticMeshAssetThumbnailContract::AssetClassName, "DStaticMesh");
		EXPECT_EQ(Editor::StaticMesh::FStaticMeshAssetThumbnailContract::ProviderName, "Durin.StaticMeshThumbnail");
		EXPECT_EQ(Editor::StaticMesh::FStaticMeshAssetThumbnailContract::GeneratorSchemaVersion, 2u);
		EXPECT_EQ(
			Editor::StaticMesh::FStaticMeshAssetThumbnailContract::PreviewFixtureIdentity,
			"/Engine/Editor/StaticMeshPreview/LOD0DefaultMaterials");
		EXPECT_EQ(Editor::StaticMesh::FStaticMeshAssetThumbnailContract::PreviewFixtureVersion, 2u);
		EXPECT_EQ(Editor::StaticMesh::FStaticMeshAssetThumbnailContract::ShaderContractVersion, 1u);
		EXPECT_EQ(Editor::StaticMesh::FStaticMeshAssetThumbnailContract::ImageMargin, 0.04);
		EXPECT_EQ(Editor::StaticMesh::FStaticMeshAssetThumbnailContract::LODIndex, 0u);
		EXPECT_FALSE(Editor::StaticMesh::FStaticMeshAssetThumbnailContract::bOutputOpaque);
		const Editor::StaticMesh::FStaticMeshAssetThumbnailViewInput ViewInput;
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
		const Editor::StaticMesh::FStaticMeshAssetThumbnailViewInput Input{
			.LocalBounds = FBox(FVector3(3.0, -8.0, 2.0), FVector3(11.0, 4.0, 6.0)),
			.OutputAspectRatio = 1.25};
		Editor::StaticMesh::FStaticMeshAssetThumbnailView First;
		Editor::StaticMesh::FStaticMeshAssetThumbnailView Second;
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
		auto ExpectRejected = [](Editor::StaticMesh::FStaticMeshAssetThumbnailViewInput Input) {
			Editor::StaticMesh::FStaticMeshAssetThumbnailView View;
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
		const Editor::FRenderedAssetThumbnailVisualContract Visual;
		EXPECT_EQ(Visual.Output.Width, Visual.Output.Height);
		EXPECT_EQ(Visual.Output.Width, 256u);
		EXPECT_EQ(Editor::FRenderedAssetThumbnailVisualContract::SphereVirtualPath,
			"/Engine/Models/Sphere");
		EXPECT_EQ(Editor::FRenderedAssetThumbnailVisualContract::SphereFixtureVersion, 1u);
		EXPECT_EQ(Editor::FRenderedAssetThumbnailVisualContract::OutputEncoding, "PNG");
		EXPECT_EQ(Editor::FRenderedAssetThumbnailVisualContract::OutputColorSpace, "sRGB");
		EXPECT_TRUE(Visual.bOutputOpaque);
		EXPECT_EQ(Visual.CubeDirectionConvention,
			Editor::EAssetThumbnailCubeDirectionConvention::WorldSpaceReflectionVector);

		const Editor::FAssetThumbnailBudgets Budgets;
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
			Editor::FAssetThumbnailObjectStore Store({
				.CacheRoot = Root,
				.ObjectExtension = ".bin"});
			EXPECT_FALSE(Store.Store("../unsafe", Payload));
			ASSERT_TRUE(Store.Store("material-key-01", Payload));
		}
		Editor::FAssetThumbnailObjectStore WarmStore({
			.CacheRoot = Root,
			.ObjectExtension = ".bin"});
		std::vector<uint8> Loaded;
		EXPECT_EQ(WarmStore.Load("material-key-01", Loaded), Editor::EAssetThumbnailObjectLoadResult::Hit);
		EXPECT_EQ(Loaded, Payload);
		EXPECT_EQ(WarmStore.GetStats().CacheHits, 1u);
		EXPECT_EQ(WarmStore.Load("../unsafe", Loaded), Editor::EAssetThumbnailObjectLoadResult::Invalid);
	}

	TEST(FAssetThumbnailContractTests, ObjectStoreCleansMissingObjectsAndHonorsDiskBudget)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("Cleanup");
		const std::vector<uint8> Payload(80, 7);
		{
			Editor::FAssetThumbnailObjectStore Store({
				.CacheRoot = Root,
				.DiskBudgetBytes = 100,
				.ObjectExtension = ".bin"});
			ASSERT_TRUE(Store.Store("object-key-01", Payload));
			ASSERT_TRUE(Store.Store("object-key-02", Payload));
			EXPECT_EQ(Store.GetStats().Evictions, 1u);
			std::vector<uint8> Loaded;
			EXPECT_EQ(Store.Load("object-key-01", Loaded), Editor::EAssetThumbnailObjectLoadResult::Miss);
			EXPECT_EQ(Store.Load("object-key-02", Loaded), Editor::EAssetThumbnailObjectLoadResult::Hit);
		}
		for (const auto& Entry : std::filesystem::recursive_directory_iterator(Root / "Objects"))
			if (Entry.path().extension() == ".bin") std::filesystem::remove(Entry.path());
		Editor::FAssetThumbnailObjectStore MissingObjectStore({
			.CacheRoot = Root,
			.DiskBudgetBytes = 100,
			.ObjectExtension = ".bin"});
		EXPECT_GE(MissingObjectStore.GetStats().Regenerations, 1u);
	}

	TEST(FAssetThumbnailContractTests, BudgetSelectionEvictsOldestUnpinnedAllocations)
	{
		const std::vector<Editor::FAssetThumbnailBudgetEntry> Entries = {
			{.Key = "visible", .Bytes = 60, .LastUsed = 1, .bPinned = true},
			{.Key = "oldest", .Bytes = 40, .LastUsed = 2},
			{.Key = "newest", .Bytes = 30, .LastUsed = 3},
		};
		EXPECT_EQ(Editor::SelectAssetThumbnailBudgetEvictions(Entries, 100),
			std::vector<std::string>({"oldest"}));
		EXPECT_EQ(Editor::SelectAssetThumbnailBudgetEvictions(Entries, 50),
			(std::vector<std::string>{"oldest", "newest"}));
		EXPECT_TRUE(Editor::SelectAssetThumbnailBudgetEvictions(Entries, 200).empty());
	}

	TEST(FAssetThumbnailContractTests, ProviderRegistryRejectsInvalidAndDuplicateRegistrations)
	{
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		EXPECT_FALSE(Registry.Register(nullptr, Error));
		EXPECT_FALSE(Error.empty());

		auto Invalid = std::make_shared<FTestThumbnailProvider>(
			Editor::FAssetThumbnailProviderRegistration{.AssetClassName = "DMaterial"});
		EXPECT_FALSE(Registry.Register(Invalid, Error));
		EXPECT_FALSE(Error.empty());

		auto Material = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Material, Error)) << Error;
		EXPECT_EQ(Registry.Num(), 1u);

		auto Duplicate = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.OtherMaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		EXPECT_FALSE(Registry.Register(Duplicate, Error));
		EXPECT_NE(Error.find("DMaterial"), std::string::npos);
		EXPECT_EQ(Registry.Num(), 1u);
	}

	TEST(FAssetThumbnailContractTests, ProviderRegistryUsesExactClassesAndMonotonicGenerations)
	{
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Material = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		auto MaterialInstance = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterialInstance",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Material, Error)) << Error;
		ASSERT_TRUE(Registry.Register(MaterialInstance, Error)) << Error;

		const Editor::FAssetThumbnailProviderHandle MaterialHandle = Registry.Find("DMaterial");
		const Editor::FAssetThumbnailProviderHandle InstanceHandle = Registry.Find("DMaterialInstance");
		EXPECT_TRUE(MaterialHandle);
		EXPECT_TRUE(InstanceHandle);
		EXPECT_NE(MaterialHandle.Generation, InstanceHandle.Generation);
		EXPECT_FALSE(Registry.Find("DMaterialInterface"));

		ASSERT_TRUE(Registry.Unregister("DMaterial", Error)) << Error;
		EXPECT_FALSE(Registry.Find("DMaterial"));
		EXPECT_FALSE(Registry.Unregister("DMaterial", Error));

		auto Replacement = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 2});
		ASSERT_TRUE(Registry.Register(Replacement, Error)) << Error;
		EXPECT_GT(Registry.Find("DMaterial").Generation, MaterialHandle.Generation);
	}

	TEST(FAssetThumbnailContractTests, ProviderRegistryShutdownDropsProvidersAndClosesRegistration)
	{
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
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

	TEST(FAssetThumbnailContractTests, ScopedProviderRegistrationRejectsDuplicatesAndAllowsLaterReplacement)
	{
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto FirstState = std::make_shared<FFakeRenderedExtensionState>();
		Editor::FAssetThumbnailProviderRegistrationHandle First = Registry.RegisterScoped(
			std::make_unique<FFakeRenderedThumbnailExtension>(FirstState), Error);
		ASSERT_TRUE(First) << Error;
		const uint64 FirstGeneration = Registry.Find("DFakeRenderedAsset").Generation;

		auto DuplicateState = std::make_shared<FFakeRenderedExtensionState>();
		Editor::FAssetThumbnailProviderRegistrationHandle Duplicate = Registry.RegisterScoped(
			std::make_unique<FFakeRenderedThumbnailExtension>(DuplicateState), Error);
		EXPECT_FALSE(Duplicate);
		EXPECT_NE(Error.find("DFakeRenderedAsset"), std::string::npos);
		EXPECT_EQ(DuplicateState->ExtensionDestructions, 1u);
		EXPECT_EQ(Registry.Find("DFakeRenderedAsset").Generation, FirstGeneration);

		First.Reset();
		EXPECT_FALSE(Registry.Find("DFakeRenderedAsset"));
		EXPECT_EQ(FirstState->ExtensionDestructions, 1u);
		auto ReplacementState = std::make_shared<FFakeRenderedExtensionState>();
		Editor::FAssetThumbnailProviderRegistrationHandle Replacement = Registry.RegisterScoped(
			std::make_unique<FFakeRenderedThumbnailExtension>(ReplacementState), Error);
		ASSERT_TRUE(Replacement) << Error;
		EXPECT_GT(Registry.Find("DFakeRenderedAsset").Generation, FirstGeneration);
	}

	TEST(FAssetThumbnailContractTests, SchedulersCreatedBeforeAndAfterScopedRegistrationObserveItsGeneration)
	{
		Editor::FAssetThumbnailProviderRegistry Registry;
		Editor::FAssetThumbnailScheduler BeforeRegistration(Registry);
		std::string Error;
		auto State = std::make_shared<FFakeRenderedExtensionState>();
		Editor::FAssetThumbnailProviderRegistrationHandle Registration = Registry.RegisterScoped(
			std::make_unique<FFakeRenderedThumbnailExtension>(State), Error);
		ASSERT_TRUE(Registration) << Error;
		const uint64 Generation = Registry.Find("DFakeRenderedAsset").Generation;
		ASSERT_NE(Generation, 0u);
		Editor::FAssetThumbnailScheduler AfterRegistration(Registry);
		ASSERT_TRUE(BeforeRegistration.Request(MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/BeforeRegistry",
			"DFakeRenderedAsset",
			1), Error)) << Error;
		ASSERT_TRUE(AfterRegistration.Request(MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/AfterRegistry",
			"DFakeRenderedAsset",
			1), Error)) << Error;
		const std::optional<Editor::FAssetThumbnailScheduledJob> BeforeJob =
			BeforeRegistration.TakeNext();
		const std::optional<Editor::FAssetThumbnailScheduledJob> AfterJob =
			AfterRegistration.TakeNext();
		ASSERT_TRUE(BeforeJob);
		ASSERT_TRUE(AfterJob);
		EXPECT_EQ(BeforeJob->GenerationRequest.ProviderGeneration, Generation);
		EXPECT_EQ(AfterJob->GenerationRequest.ProviderGeneration, Generation);

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
		Editor::FRenderedAssetThumbnailService Service;
		Editor::FRenderedAssetThumbnailCache BeforeRegistration(
			Service,
			{},
			{.CacheRoot = MakeObjectStoreRoot("CacheBeforeRegistration"),
				.ObjectExtension = ".bin"});
		std::string Error;
		auto State = std::make_shared<FFakeRenderedExtensionState>();
		Editor::FAssetThumbnailProviderRegistrationHandle Registration = Service.RegisterScoped(
			std::make_unique<FFakeRenderedThumbnailExtension>(State), Error);
		ASSERT_TRUE(Registration) << Error;
		Editor::FRenderedAssetThumbnailCache AfterRegistration(
			Service,
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

	TEST(FAssetThumbnailContractTests, FakeRenderedExtensionRunsColdAndWarmBeforeScopedRemoval)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("FakeRenderedExtensionWarmHit");
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto State = std::make_shared<FFakeRenderedExtensionState>();
		Editor::FAssetThumbnailProviderRegistrationHandle Registration = Registry.RegisterScoped(
			std::make_unique<FFakeRenderedThumbnailExtension>(State), Error);
		ASSERT_TRUE(Registration) << Error;
		Editor::FAssetThumbnailScheduler Scheduler(Registry);
		Editor::FRenderedAssetThumbnailPipeline Pipeline(
			Scheduler, {.CacheRoot = Root, .ObjectExtension = ".bin"});
		const Editor::FAssetThumbnailRequest ColdRequest = MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/ColdWarm", "DFakeRenderedAsset", 1);
		ASSERT_TRUE(Scheduler.Request(ColdRequest, Error)) << Error;
		Pipeline.BeginFrame();
		std::optional<Editor::FRenderedAssetThumbnailJob> ColdJob = Pipeline.StartNext();
		ASSERT_TRUE(ColdJob);
		Editor::IRenderedAssetThumbnailGenerationSession* Session =
			ColdJob->ScheduledJob.GenerationRequest.BeginRenderedSession(Error);
		ASSERT_NE(Session, nullptr) << Error;
		const Editor::FRenderedAssetThumbnailSessionUpdate Update = Session->Load();
		ASSERT_EQ(Update.State, Editor::ERenderedAssetThumbnailSessionState::ReadyToRender);
		FFakeRenderedThumbnailPreviewScene PreviewScene;
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
			Encoded));
		ColdJob->ScheduledJob.GenerationRequest.ReleaseRenderedSession();
		EXPECT_EQ(State->PreviewResets, 1u);

		const Editor::FAssetThumbnailRequest WarmRequest = MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/ColdWarm", "DFakeRenderedAsset", 2);
		ASSERT_TRUE(Scheduler.Request(WarmRequest, Error)) << Error;
		Editor::FRenderedAssetThumbnailStartResult Warm = Pipeline.StartNextDetailed();
		EXPECT_FALSE(Warm.ColdJob);
		ASSERT_TRUE(Warm.WarmJob);
		EXPECT_EQ(Warm.EncodedBytes, std::vector<uint8>(Encoded.begin(), Encoded.end()));
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
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto State = std::make_shared<FFakeRenderedExtensionState>();
		Editor::FAssetThumbnailProviderRegistrationHandle Registration = Registry.RegisterScoped(
			std::make_unique<FFakeRenderedThumbnailExtension>(State), Error);
		ASSERT_TRUE(Registration) << Error;
		Editor::FAssetThumbnailScheduler Scheduler(Registry);
		ASSERT_TRUE(Scheduler.Request(MakeThumbnailRequest(
			"/ThumbnailTests/FakeRendered/QueuedRemoval",
			"DFakeRenderedAsset",
			1), Error)) << Error;

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
			Editor::FAssetThumbnailProviderRegistry Registry;
			std::string Error;
			auto State = std::make_shared<FFakeRenderedExtensionState>();
			const std::string AssetClassName = std::format("DFakeRenderedAsset{}", StateIndex);
			Editor::FAssetThumbnailProviderRegistrationHandle Registration = Registry.RegisterScoped(
				std::make_unique<FFakeRenderedThumbnailExtension>(State, AssetClassName),
				Error);
			ASSERT_TRUE(Registration) << Error;
			Editor::FAssetThumbnailScheduler Scheduler(Registry);
			const Editor::FAssetThumbnailRequest Request = MakeThumbnailRequest(
				std::format("/ThumbnailTests/FakeRendered/InFlight{}", StateIndex),
				AssetClassName,
				1);
			ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
			std::optional<Editor::FAssetThumbnailScheduledJob> Job = Scheduler.TakeNext();
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

	TEST(FAssetThumbnailContractTests, SchedulerSkipsMissingProvidersAndRecordsProviderRejection)
	{
		Editor::FAssetThumbnailProviderRegistry Registry;
		Editor::FAssetThumbnailScheduler Scheduler(Registry);
		std::string Error;
		const Editor::FAssetThumbnailRequest Missing =
			MakeThumbnailRequest("/ThumbnailTests/Unsupported", "DUnsupported", 1);
		EXPECT_FALSE(Scheduler.Request(Missing, Error));
		EXPECT_EQ(Scheduler.NumQueued(), 0u);
		EXPECT_EQ(Scheduler.Find(Missing.Asset.VirtualPath).State, Editor::EAssetThumbnailState::NotRequested);

		auto Rejecting = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1}, false);
		ASSERT_TRUE(Registry.Register(Rejecting, Error)) << Error;
		const Editor::FAssetThumbnailRequest Invalid =
			MakeThumbnailRequest("/ThumbnailTests/Invalid", "DMaterial", 2);
		EXPECT_FALSE(Scheduler.Request(Invalid, Error));
		const Editor::FAssetThumbnailView InvalidView = Scheduler.Find(Invalid.Asset.VirtualPath);
		EXPECT_EQ(InvalidView.State, Editor::EAssetThumbnailState::Invalid);
		EXPECT_EQ(InvalidView.RequestSerial, 2u);
		EXPECT_NE(InvalidView.Diagnostic.find("rejected"), std::string::npos);
	}

	TEST(FAssetThumbnailContractTests, SchedulerCoalescesAndPromotesVisibleRequests)
	{
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		Editor::FAssetThumbnailScheduler Scheduler(Registry);
		const Editor::FAssetThumbnailRequest Prefetch =
			MakeThumbnailRequest("/ThumbnailTests/Coalesced", "DMaterial", 1);
		const Editor::FAssetThumbnailRequest Visible =
			MakeThumbnailRequest("/ThumbnailTests/Coalesced", "DMaterial", 2, Editor::EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(Scheduler.Request(Prefetch, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(Visible, Error)) << Error;
		EXPECT_EQ(Scheduler.NumQueued(), 1u);
		EXPECT_EQ(Scheduler.Find(Prefetch.Asset.VirtualPath).RequestSerial, 2u);

		std::optional<Editor::FAssetThumbnailScheduledJob> Job = Scheduler.TakeNext();
		ASSERT_TRUE(Job);
		EXPECT_EQ(Job->Priority, Editor::EAssetThumbnailPriority::Visible);
		EXPECT_EQ(Job->GenerationRequest.RequestSerial, 2u);
		EXPECT_EQ(Scheduler.Find(Prefetch.Asset.VirtualPath).State, Editor::EAssetThumbnailState::Loading);

		const Editor::FAssetThumbnailRequest NewSerial =
			MakeThumbnailRequest("/ThumbnailTests/Coalesced", "DMaterial", 3, Editor::EAssetThumbnailPriority::Visible);
		ASSERT_TRUE(Scheduler.Request(NewSerial, Error)) << Error;
		EXPECT_TRUE(Job->GenerationRequest.Cancellation.IsCancelled());
		EXPECT_EQ(Scheduler.NumQueued(), 1u);
		const std::optional<Editor::FAssetThumbnailScheduledJob> Replacement = Scheduler.TakeNext();
		ASSERT_TRUE(Replacement);
		EXPECT_EQ(Replacement->GenerationRequest.RequestSerial, 3u);
	}

	TEST(FAssetThumbnailContractTests, SchedulerPrioritizesVisibleJobsAndEnforcesQueueBudget)
	{
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
			.AssetClassName = "DTextureCube",
			.ProviderName = "Durin.TextureCubeThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		Editor::FAssetThumbnailBudgets Budgets;
		Budgets.MaximumQueuedJobs = 2;
		Editor::FAssetThumbnailScheduler Scheduler(Registry, Budgets);
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

		const std::optional<Editor::FAssetThumbnailScheduledJob> First = Scheduler.TakeNext();
		ASSERT_TRUE(First);
		EXPECT_EQ(First->GenerationRequest.KeyInput.Asset.VirtualPath, Visible.Asset.VirtualPath);
		const std::optional<Editor::FAssetThumbnailScheduledJob> Second = Scheduler.TakeNext();
		ASSERT_TRUE(Second);
		EXPECT_EQ(Second->GenerationRequest.KeyInput.Asset.VirtualPath, Prefetch.Asset.VirtualPath);
		EXPECT_FALSE(Scheduler.TakeNext());
	}

	TEST(FAssetThumbnailContractTests, SchedulerRejectsStaleSerialsAndCancelsReplacedWork)
	{
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		Editor::FAssetThumbnailScheduler Scheduler(Registry);
		const Editor::FAssetThumbnailRequest Current =
			MakeThumbnailRequest("/ThumbnailTests/Replaced", "DMaterial", 2);
		ASSERT_TRUE(Scheduler.Request(Current, Error)) << Error;
		std::optional<Editor::FAssetThumbnailScheduledJob> Active = Scheduler.TakeNext();
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
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		for (const Editor::FAssetThumbnailProviderRegistration Registration : {
			Editor::FAssetThumbnailProviderRegistration{
				.AssetClassName = "DMaterial",
				.ProviderName = "Durin.MaterialThumbnail",
				.GeneratorSchemaVersion = 1},
			Editor::FAssetThumbnailProviderRegistration{
				.AssetClassName = "DTextureCube",
				.ProviderName = "Durin.TextureCubeThumbnail",
				.GeneratorSchemaVersion = 1},
			Editor::FAssetThumbnailProviderRegistration{
				.AssetClassName = "DStaticMesh",
				.ProviderName = "Durin.StaticMeshThumbnail",
				.GeneratorSchemaVersion = 1}})
		{
			ASSERT_TRUE(Registry.Register(
				std::make_shared<FTestThumbnailProvider>(Registration), Error)) << Error;
		}
		Editor::FAssetThumbnailBudgets Budgets;
		Budgets.MaximumQueuedJobs = 3;
		Editor::FAssetThumbnailScheduler Scheduler(Registry, Budgets);
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

		const std::optional<Editor::FAssetThumbnailScheduledJob> First = Scheduler.TakeNext();
		ASSERT_TRUE(First);
		EXPECT_EQ(
			First->GenerationRequest.KeyInput.Asset.VirtualPath,
			MeshVisible.Asset.VirtualPath);
		EXPECT_EQ(First->GenerationRequest.RequestSerial, 2u);
		const std::optional<Editor::FAssetThumbnailScheduledJob> Second = Scheduler.TakeNext();
		const std::optional<Editor::FAssetThumbnailScheduledJob> Third = Scheduler.TakeNext();
		ASSERT_TRUE(Second);
		ASSERT_TRUE(Third);
		EXPECT_NE(
			Second->GenerationRequest.KeyInput.Asset.AssetClassName,
			Third->GenerationRequest.KeyInput.Asset.AssetClassName);
		EXPECT_FALSE(Scheduler.TakeNext());
	}

	TEST(FAssetThumbnailContractTests, SchedulerShutdownCancelsWorkAndRejectsNewRequests)
	{
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		Editor::FAssetThumbnailScheduler Scheduler(Registry);
		const Editor::FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/Shutdown", "DMaterial", 1);
		ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
		std::optional<Editor::FAssetThumbnailScheduledJob> Active = Scheduler.TakeNext();
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
			Editor::FAssetThumbnailProviderRegistry Registry;
			std::string Error;
			auto Provider = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
				.AssetClassName = "DMaterial",
				.ProviderName = "Durin.MaterialThumbnail",
				.GeneratorSchemaVersion = 1});
			EXPECT_TRUE(Registry.Register(Provider, Error)) << Error;
			Editor::FAssetThumbnailScheduler Scheduler(Registry);
			Editor::FRenderedAssetThumbnailPipeline Pipeline(
				Scheduler,
				{.CacheRoot = Root, .ObjectExtension = ".bin"});
			const Editor::FAssetThumbnailRequest Request =
				MakeThumbnailRequest("/ThumbnailTests/PersistentMaterial", "DMaterial", Serial);
			EXPECT_TRUE(Scheduler.Request(Request, Error)) << Error;
			Pipeline.BeginFrame();
			std::optional<Editor::FRenderedAssetThumbnailJob> Job = Pipeline.StartNext();
			if (bExpectWarmHit)
			{
				EXPECT_FALSE(Job);
				EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State, Editor::EAssetThumbnailState::Ready);
				const Editor::FRenderedAssetThumbnailPipelineStats Stats = Pipeline.GetStats();
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
			EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State, Editor::EAssetThumbnailState::Ready);
			const Editor::FRenderedAssetThumbnailPipelineStats Stats = Pipeline.GetStats();
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
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		Editor::FAssetThumbnailScheduler Scheduler(Registry);
		Editor::FRenderedAssetThumbnailPipeline Pipeline(
			Scheduler,
			{.CacheRoot = Root, .ObjectExtension = ".png"});
		const Editor::FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/EncodedMaterial", "DMaterial", 1);
		ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
		Pipeline.BeginFrame();
		std::optional<Editor::FRenderedAssetThumbnailJob> Job = Pipeline.StartNext();
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

		Editor::FAssetThumbnailObjectStore Store({
			.CacheRoot = Root,
			.ObjectExtension = ".png"});
		std::vector<uint8> Encoded;
		ASSERT_EQ(Store.Load(CacheKey, Encoded), Editor::EAssetThumbnailObjectLoadResult::Hit);
		Image::FDecodedImage Decoded;
		ASSERT_TRUE(Image::DecodeImageFromMemory(Encoded, Decoded, Error)) << Error;
		EXPECT_EQ(Decoded.Width, 2u);
		EXPECT_EQ(Decoded.Height, 1u);
		EXPECT_EQ(Decoded.Pixels, std::vector<uint8>(Pixels.begin(), Pixels.end()));
	}

	TEST(FAssetThumbnailContractTests, RenderedPipelineRevalidatesAfterEncodingBeforePublication)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("RenderedPipelinePublicationValidation");
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
			.AssetClassName = "DStaticMesh",
			.ProviderName = "Durin.StaticMeshThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		Editor::FAssetThumbnailScheduler Scheduler(Registry);
		Editor::FRenderedAssetThumbnailPipeline Pipeline(
			Scheduler,
			{.CacheRoot = Root, .ObjectExtension = ".png"});
		const Editor::FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/StaleStaticMesh", "DStaticMesh", 1);
		ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
		Pipeline.BeginFrame();
		std::optional<Editor::FRenderedAssetThumbnailJob> Job = Pipeline.StartNext();
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
		EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State, Editor::EAssetThumbnailState::Failed);

		Editor::FAssetThumbnailObjectStore Store({.CacheRoot = Root, .ObjectExtension = ".png"});
		std::vector<uint8> Encoded;
		EXPECT_EQ(Store.Load(CacheKey, Encoded), Editor::EAssetThumbnailObjectLoadResult::Miss);
	}

	TEST(FAssetThumbnailContractTests, GeneratedPixelsPublishWithoutPreviewRenderAllowance)
	{
		const std::filesystem::path Root = MakeObjectStoreRoot("GeneratedPixels");
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
			.AssetClassName = "DTerrainHeightmap",
			.ProviderName = "TerrainHeightmapCanonicalThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		Editor::FAssetThumbnailScheduler Scheduler(Registry);
		Editor::FRenderedAssetThumbnailPipeline Pipeline(
			Scheduler, {.CacheRoot = Root, .ObjectExtension = ".png"},
			{.MaximumRendersPerFrame = 0});
		const Editor::FAssetThumbnailRequest Request = MakeThumbnailRequest(
			"/ThumbnailTests/GeneratedTerrain", "DTerrainHeightmap", 1);
		ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
		Pipeline.BeginFrame();
		auto Job = Pipeline.StartNext();
		ASSERT_TRUE(Job);
		const std::array<uint8, 4> Pixels{32, 32, 32, 255};
		ASSERT_TRUE(Pipeline.CompleteGeneratedPixels(*Job, 7, Pixels, 1, 1));
		EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State,
			Editor::EAssetThumbnailState::Ready);
		EXPECT_EQ(Pipeline.GetStats().Renders, 0u);
	}

	TEST(FAssetThumbnailContractTests, RenderedPipelineBoundsRendersAndRejectsStaleCompletions)
	{
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
			.AssetClassName = "DMaterial",
			.ProviderName = "Durin.MaterialThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		Editor::FAssetThumbnailScheduler Scheduler(Registry);
		Editor::FRenderedAssetThumbnailPipeline Pipeline(
			Scheduler,
			{.CacheRoot = MakeObjectStoreRoot("RenderedPipelineBounds"), .ObjectExtension = ".bin"});
		const Editor::FAssetThumbnailRequest FirstRequest =
			MakeThumbnailRequest("/ThumbnailTests/FirstRendered", "DMaterial", 1);
		const Editor::FAssetThumbnailRequest SecondRequest =
			MakeThumbnailRequest("/ThumbnailTests/SecondRendered", "DMaterial", 1);
		ASSERT_TRUE(Scheduler.Request(FirstRequest, Error)) << Error;
		ASSERT_TRUE(Scheduler.Request(SecondRequest, Error)) << Error;
		Pipeline.BeginFrame();
		std::optional<Editor::FRenderedAssetThumbnailJob> First = Pipeline.StartNext();
		std::optional<Editor::FRenderedAssetThumbnailJob> Second = Pipeline.StartNext();
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
		Editor::FAssetThumbnailProviderRegistry Registry;
		std::string Error;
		auto Provider = std::make_shared<FTestThumbnailProvider>(Editor::FAssetThumbnailProviderRegistration{
			.AssetClassName = "DTextureCube",
			.ProviderName = "Durin.TextureCubeThumbnail",
			.GeneratorSchemaVersion = 1});
		ASSERT_TRUE(Registry.Register(Provider, Error)) << Error;
		Editor::FAssetThumbnailScheduler Scheduler(Registry);
		Editor::FRenderedAssetThumbnailPipeline Pipeline(
			Scheduler,
			{.CacheRoot = MakeObjectStoreRoot("RenderedPipelineCounters"), .ObjectExtension = ".bin"});
		const Editor::FAssetThumbnailRequest Request =
			MakeThumbnailRequest("/ThumbnailTests/CounterCube", "DTextureCube", 1);
		ASSERT_TRUE(Scheduler.Request(Request, Error)) << Error;
		Pipeline.BeginFrame();
		std::optional<Editor::FRenderedAssetThumbnailJob> Job = Pipeline.StartNext();
		ASSERT_TRUE(Job);
		ASSERT_TRUE(Pipeline.CompleteLoad(*Job, 30));
		EXPECT_TRUE(Pipeline.BeginRender(*Job, false, 30, 0));
		EXPECT_FALSE(Pipeline.BeginRender(*Job, true, 30, 0, "Cube build failed."));
		EXPECT_EQ(Scheduler.Find(Request.Asset.VirtualPath).State, Editor::EAssetThumbnailState::Failed);

		Pipeline.RecordRetry();
		const Editor::FAssetThumbnailRequest CancelRequest =
			MakeThumbnailRequest("/ThumbnailTests/CancelledCube", "DTextureCube", 1);
		ASSERT_TRUE(Scheduler.Request(CancelRequest, Error)) << Error;
		std::optional<Editor::FRenderedAssetThumbnailJob> Cancelled = Pipeline.StartNext();
		ASSERT_TRUE(Cancelled);
		Pipeline.Cancel(*Cancelled);

		const Editor::FRenderedAssetThumbnailPipelineStats Stats = Pipeline.GetStats();
		EXPECT_EQ(Stats.ResourceWaits, 1u);
		EXPECT_EQ(Stats.Failures, 1u);
		EXPECT_EQ(Stats.Retries, 1u);
		EXPECT_EQ(Stats.Cancellations, 1u);
	}
} // namespace Durin
