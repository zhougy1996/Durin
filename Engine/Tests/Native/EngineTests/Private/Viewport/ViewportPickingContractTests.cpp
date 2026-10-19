#include "Editor/EditorActorSubsystemTestSupport.h"
#include "Workspace/LevelEditorContext.h"

#include "ViewportTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Components/SplineMeshComponent.h"
#include "LevelEditorViewportEditing.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Viewport/ViewportPickingService.h"


namespace
{
	struct FFakePickingState
	{
		std::vector<Durin::FHitProxyRenderRequest> Requests;
		std::function<void()> DuringPick;
		bool bFail = false;
		std::shared_ptr<const Durin::FHitProxyMap> Map;
		bool bReturnHit = true;
		double Distance = 3.0;
		std::optional<uint32> Token;
		uint32 Invalidations = 0;
	};

	class FImmediatePickingBackend final : public Durin::Editor::Level::IViewportPickingBackend
	{
	public:
		explicit FImmediatePickingBackend(std::shared_ptr<FFakePickingState> InState) : State(std::move(InState)) {}
		auto GetHitProxyMap(Durin::FSceneInterface*, Durin::FHitProxyRenderRequest Request) -> std::shared_ptr<const Durin::FHitProxyMap> override
		{
			const uint32 Token = !State->bReturnHit ? 0u : State->Token.value_or(!Request.Primitives.empty()
				? Request.Primitives.front().Id.Value : !Request.Overlays.empty() ? Request.Overlays.front().Id.Value : 0u);
			const auto Map = State->bFail ? nullptr : State->Map ? State->Map : MakeHitProxyTestMap(Request.View, Token, float(State->Distance));
			State->Requests.push_back(std::move(Request));
			if (State->DuringPick) State->DuringPick();
			return Map;
		}
		auto Invalidate() -> void override { ++State->Invalidations; }
	private:
		std::shared_ptr<FFakePickingState> State;
	};

	struct FPickingFixture
	{
		Durin::Tests::FActorEditorFixture Editor;
		Durin::DWorld* World = nullptr;
		Durin::DLevel* Level = nullptr;
		Durin::AStaticMeshActor* Actor = nullptr;
		Durin::Editor::Level::FLevelEditorViewportClient Client;
		Durin::FSceneView View;

		FPickingFixture()
		{
			InitializeDObjectSystem();
			World = Editor.GetWorld();
			Level = World->GetCurrentLevel();
			Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle(Level);
			Actor = Level->SpawnActor<Durin::AStaticMeshActor>("Target");
			expect_ne(Actor, nullptr);
			Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
			Actor->GetStaticMeshComponent()->SetWorldLocation(
				Client.GetCameraTransform().GetLocation() + Client.GetCameraTransform().GetForwardVector() * 3.0);
			Client.InitializeForLevel(Level);
			expect_true(Client.BuildViewMatrices(800, 600, View));
		}


	private:
		static auto expect_true(bool Value) -> void { if (!Value) throw std::runtime_error("fixture setup failed"); }
		template<typename T> static auto expect_ne(T* Value, std::nullptr_t) -> void
		{
			if (!Value) throw std::runtime_error("fixture setup failed");
		}
	};

}
TEST(FViewportPickingContractTests, RejectsOutsideViewAndPreservesExactPrimitiveIdentity)
{
	using namespace Durin::Editor::Level;
	FPickingFixture Fixture;
	auto State = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FImmediatePickingBackend>(State));
	EXPECT_EQ(Fixture.Client.PickViewport(Fixture.Level, Fixture.View, {-1.0f, 300.0f}).Status, EViewportPickStatus::Invalid);
	EXPECT_TRUE(State->Requests.empty());
	const auto Pick = Fixture.Client.PickViewport(Fixture.Level, Fixture.View, {400.0f, 300.0f});
	ASSERT_EQ(Pick.Status, EViewportPickStatus::Completed);
	ASSERT_TRUE(Pick.Hit);
	EXPECT_EQ(Pick.Hit->Kind, EViewportPickHitKind::SceneGeometry);
	EXPECT_EQ(Pick.Hit->Actor.Get(), Fixture.Actor);
	EXPECT_EQ(Pick.Hit->Component.Get(), Fixture.Actor->GetStaticMeshComponent());
	EXPECT_EQ(Pick.Hit->PrimitiveId, Fixture.Actor->GetStaticMeshComponent()->GetPrimitiveComponentId());
}

TEST(FViewportPickingContractTests, ReturnsBlankSpaceAndRenderingFailureImmediately)
{
	using namespace Durin::Editor::Level;
	FPickingFixture Fixture;
	auto State = std::make_shared<FFakePickingState>();
	State->bReturnHit = false;
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FImmediatePickingBackend>(State));
	const auto Blank = Fixture.Client.PickViewport(Fixture.Level, Fixture.View, {400.f, 300.f});
	EXPECT_EQ(Blank.Status, EViewportPickStatus::Completed);
	EXPECT_FALSE(Blank.Hit);
	State->bFail = true;
	EXPECT_EQ(Fixture.Client.PickViewport(Fixture.Level, Fixture.View, {400.f, 300.f}).Status, EViewportPickStatus::Failed);
}

TEST(FViewportPickingContractTests, RejectsTargetsRetiredDuringSynchronousQuery)
{
	using namespace Durin::Editor::Level;
	FPickingFixture Fixture;
	auto State = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FImmediatePickingBackend>(State));
	State->DuringPick = [&] { Fixture.Actor->SetActorHiddenInGame(true); };
	EXPECT_EQ(Fixture.Client.PickViewport(Fixture.Level, Fixture.View, {400.f, 300.f}).Status, EViewportPickStatus::Invalid);
	Fixture.Actor->SetActorHiddenInGame(false);
	State->DuringPick = [&] {
		Fixture.Actor->GetStaticMeshComponent()->UnregisterComponent();
		Fixture.Actor->GetStaticMeshComponent()->RegisterComponent();
	};
	EXPECT_EQ(Fixture.Client.PickViewport(Fixture.Level, Fixture.View, {400.f, 300.f}).Status, EViewportPickStatus::Invalid);
	State->DuringPick = [&] { EXPECT_TRUE(Fixture.Level->DestroyActor(Fixture.Actor)); Durin::CollectGarbage(); };
	EXPECT_EQ(Fixture.Client.PickViewport(Fixture.Level, Fixture.View, {400.f, 300.f}).Status, EViewportPickStatus::Invalid);
}

TEST(FViewportPickingContractTests, SelectModeDuplicateAndDeleteUseStructuralHistory)
{
	FPickingFixture Fixture;
	auto* Transactions = Fixture.Editor.GetTransactions();
	Durin::Editor::Level::FLevelEditorContext Context;
	Context.EditorHost = Fixture.Editor.GetEditor();
	Context.Synchronize(Fixture.World);
	Context.SelectActor(Fixture.Actor);
	Durin::Editor::Level::FLevelViewportEditModeManager Manager;
	Durin::Editor::Level::FLevelEditorViewportInput Input;
	Input.bDuplicate = true;
	ASSERT_TRUE(Manager.Tick(Context, Fixture.Client, Fixture.View, Input, Transactions));
	auto* Copy = Context.GetPrimarySelectedActor();
	ASSERT_NE(Copy, nullptr);
	EXPECT_NE(Copy, Fixture.Actor);
	EXPECT_TRUE(Fixture.Level->ContainsActor(Copy));
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(Fixture.Level->ContainsActor(Copy));
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_TRUE(Fixture.Level->ContainsActor(Copy));
	Context.SelectActor(Copy);
	Input.bDelete = true;
	ASSERT_TRUE(Manager.Tick(Context, Fixture.Client, Fixture.View, Input, Transactions));
	EXPECT_FALSE(Fixture.Level->ContainsActor(Copy));
	EXPECT_TRUE(Context.GetSelectedActors().empty());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_TRUE(Fixture.Level->ContainsActor(Copy));
	Manager.Shutdown(&Context);
}

TEST(FViewportPickingContractTests, SelectModeAppliesCtrlIntentWithinInputEvent)
{
	FPickingFixture Fixture;
	auto State = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FImmediatePickingBackend>(State));
	auto* Existing = Fixture.Level->SpawnActor<Durin::ACameraActor>("Existing");
	Durin::FTransform ExistingTransform;
	ExistingTransform.Translation = {1000.0, 1000.0, 1000.0};
	Existing->SetActorTransform(ExistingTransform);
	Durin::Editor::Level::FLevelEditorContext Context;
	Context.Synchronize(Fixture.World);
	Context.SelectActor(Existing);
	Durin::Editor::Level::FLevelViewportEditModeManager Manager;
	Durin::Editor::Level::FLevelEditorViewportInput Input;
	Input.bRequestSelection = true;
	Input.bCtrl = true;
	Input.MousePosition = {400.0f, 300.0f};
	ASSERT_TRUE(Manager.Tick(Context, Fixture.Client, Fixture.View, Input, nullptr));
	EXPECT_TRUE(Context.IsActorSelected(Existing));
	EXPECT_TRUE(Context.IsActorSelected(Fixture.Actor));
	Input = {};
	EXPECT_FALSE(Manager.Tick(Context, Fixture.Client, Fixture.View, Input, nullptr));
	EXPECT_EQ(State->Requests.size(), 1u);
	Manager.Shutdown(&Context);
}

TEST(FViewportPickingContractTests, KeepsViewportQueriesAndCacheInvalidationIndependent)
{
	using namespace Durin::Editor::Level;
	FPickingFixture Fixture;
	FLevelEditorViewportClient SecondClient;
	SecondClient.InitializeForLevel(Fixture.Level);
	auto FirstState = std::make_shared<FFakePickingState>();
	auto SecondState = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FImmediatePickingBackend>(FirstState));
	SecondClient.SetPickingBackendForTesting(std::make_unique<FImmediatePickingBackend>(SecondState));
	FirstState->bReturnHit = false;
	EXPECT_FALSE(Fixture.Client.PickViewport(Fixture.Level, Fixture.View, {400.f,300.f}).Hit);
	EXPECT_TRUE(SecondClient.PickViewport(Fixture.Level, Fixture.View, {400.f,300.f}).Hit);
	Fixture.Client.InitializeForLevel(Fixture.Level);
	EXPECT_EQ(FirstState->Invalidations, 1u);
	EXPECT_EQ(SecondState->Invalidations, 0u);
}

TEST(FViewportPickingContractTests, MissingRendererFailsWithoutCpuFallback)
{
	FPickingFixture Fixture;
	EXPECT_EQ(Fixture.Client.PickViewport(Fixture.Level, Fixture.View, {400.f,300.f}).Status, Durin::Editor::Level::EViewportPickStatus::Failed);
}

TEST(FViewportPickingContractTests, ResolvesOverlayElementAndRejectsRetiredHandle)
{
	using namespace Durin;
	using namespace Durin::Editor::Level;
	FPickingFixture Fixture;
	auto State = std::make_shared<FFakePickingState>();
	FViewportPickingService Service(std::make_unique<FImmediatePickingBackend>(State));
	Service.SetLevel(Fixture.Level);
	FEditorVisualizationBox Box;
	Box.Actor = Fixture.Actor;
	Box.Component = Fixture.Actor->GetStaticMeshComponent();
	Box.WorldPosition = Fixture.Actor->GetStaticMeshComponent()->GetWorldLocation();
	Box.Element = {EEditorSubElementKind::Point, FGuid::NewGuid()};
	FEditorVisualizationCollector Collector;
	Collector.AddBox(Box);
	const FViewportPickRequest Request{.Level=Fixture.Level, .View=Fixture.View,
		.ViewportPosition={400.f,300.f}, .Layers=EViewportPickLayer::EditorVisualization};
	const auto Pick = Service.Pick(Request, &Collector);
	ASSERT_EQ(Pick.Status, EViewportPickStatus::Completed);
	ASSERT_TRUE(Pick.Hit);
	EXPECT_EQ(Pick.Hit->Element, Box.Element);
	EXPECT_EQ(Pick.Hit->Component.Get(), Box.Component.Get());
	ASSERT_EQ(State->Requests.back().Overlays.size(), 1u);
	const auto& Overlay = State->Requests.back().Overlays.front();
	EXPECT_TRUE(Overlay.bForeground);
	EXPECT_NEAR((Overlay.Vertices[1].ClipPosition.x - Overlay.Vertices[0].ClipPosition.x) * 400.f,
		Box.SizePixels + 2.f*Box.HitPaddingPixels, 0.01f);
	State->DuringPick = [&] {
		Fixture.Actor->GetStaticMeshComponent()->UnregisterComponent();
		Fixture.Actor->GetStaticMeshComponent()->RegisterComponent();
	};
	EXPECT_EQ(Service.Pick(Request, &Collector).Status, EViewportPickStatus::Invalid);
}

TEST(FViewportPickingContractTests, WholeMapSharesImmutablePixelsAndRetainsIndependentSnapshots)
{
	using namespace Durin;
	const std::array Values{FHitProxyPixel{{11}, 0.f}, FHitProxyPixel{{12}, 0.f}, FHitProxyPixel{{13}, 0.f}, FHitProxyPixel{{14}, 0.f}};
	FByteBuffer Bytes(sizeof(Values));
	std::memcpy(Bytes.data(), Values.data(), Bytes.size());
	const auto First = FHitProxyMap::Create(2, 2, std::move(Bytes));
	ASSERT_TRUE(First);
	auto Second = First;
	FHitProxyPixel Pixel;
	ASSERT_TRUE(First->ReadPixel(0, 0, Pixel));
	EXPECT_EQ(Pixel.Id.Value, 11u);
	ASSERT_TRUE(Second->ReadPixel(1, 1, Pixel));
	EXPECT_EQ(Pixel.Id.Value, 14u);
	EXPECT_FALSE(First->ReadPixel(2, 0, Pixel));
	EXPECT_FALSE(First->ReadPixel(0, 2, Pixel));
	Second = FHitProxyMap::Create(1, 1, FByteBuffer(sizeof(FHitProxyPixel)));
	ASSERT_TRUE(Second->ReadPixel(0, 0, Pixel));
	EXPECT_EQ(Pixel.Id.Value, 0u);
	ASSERT_TRUE(First->ReadPixel(1, 0, Pixel));
	EXPECT_EQ(Pixel.Id.Value, 12u);
}

TEST(FViewportPickingContractTests, MapCreationValidatesExtentAndExactByteCount)
{
	using namespace Durin;
	EXPECT_FALSE(FHitProxyMap::Create(0, 1, {}));
	EXPECT_FALSE(FHitProxyMap::Create(1, 0, {}));
	EXPECT_FALSE(FHitProxyMap::Create(2, 2, FByteBuffer(4 * sizeof(FHitProxyPixel) - 1)));
	EXPECT_FALSE(FHitProxyMap::Create(2, 2, FByteBuffer(4 * sizeof(FHitProxyPixel) + 1)));
	EXPECT_FALSE(FHitProxyMap::Create(std::numeric_limits<uint32>::max(), std::numeric_limits<uint32>::max(), {}));
	EXPECT_FALSE(FHitProxyMap::IsValidExtent(std::numeric_limits<uint64>::max(), 1));
	EXPECT_FALSE(FHitProxyMap::IsValidExtent(8193, 1));
	EXPECT_FALSE(FHitProxyMap::IsValidExtent(1, 8193));
	EXPECT_FALSE(FHitProxyMap::IsValidExtent(8192, 1025));
	EXPECT_TRUE(FHitProxyMap::IsValidExtent(8192, 1024));
	EXPECT_TRUE(FHitProxyMap::Create(8192, 1, FByteBuffer(8192 * sizeof(FHitProxyPixel))));
}

TEST(FViewportPickingContractTests, DecodesGpuIdAndDistanceWordsIntoTypedPixel)
{
	using namespace Durin;
	const std::array<uint32, 2> Encoded{0xabcdef12u, 0x40800000u};
	FByteBuffer Bytes(sizeof(Encoded));
	std::memcpy(Bytes.data(), Encoded.data(), Bytes.size());
	const auto Map = FHitProxyMap::Create(1, 1, std::move(Bytes));
	ASSERT_TRUE(Map);
	FHitProxyPixel Pixel;
	ASSERT_TRUE(Map->ReadPixel(0, 0, Pixel));
	EXPECT_EQ(Pixel.Id.Value, 0xabcdef12u);
	EXPECT_EQ(Pixel.Distance, 4.f);
}

TEST(FViewportPickingContractTests, RejectsUnknownBackendTokensAndInvalidDistances)
{
	using namespace Durin::Editor::Level;
	FPickingFixture Fixture;
	auto State = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FImmediatePickingBackend>(State));
	State->Token = std::numeric_limits<uint32>::max();
	EXPECT_EQ(Fixture.Client.PickViewport(Fixture.Level, Fixture.View, {400.f,300.f}).Status, EViewportPickStatus::Failed);
	State->Token.reset();
	State->Distance = std::numeric_limits<double>::quiet_NaN();
	EXPECT_EQ(Fixture.Client.PickViewport(Fixture.Level, Fixture.View, {400.f,300.f}).Status, EViewportPickStatus::Failed);
}

TEST(FViewportPickingContractTests, SamplesRequestedPixelFromWholeMapAndHandlesMissingMaps)
{
	using namespace Durin;
	using namespace Durin::Editor::Level;
	FPickingFixture Fixture;
	auto State = std::make_shared<FFakePickingState>();
	Fixture.Client.SetPickingBackendForTesting(std::make_unique<FImmediatePickingBackend>(State));
	FSceneView View = Fixture.View;
	View.ViewportWidth = View.ViewportHeight = 2;
	const std::array Values{FHitProxyPixel{{1}, 3.f}, FHitProxyPixel{}, FHitProxyPixel{}, FHitProxyPixel{{1}, 4.f}};
	FByteBuffer Bytes(sizeof(Values));
	std::memcpy(Bytes.data(), Values.data(), Bytes.size());
	State->Map = FHitProxyMap::Create(2, 2, std::move(Bytes));
	const auto First = Fixture.Client.PickViewport(Fixture.Level, View, {0.f, 0.f}, EViewportPickLayer::SceneGeometry);
	ASSERT_TRUE(First.Hit);
	EXPECT_EQ(First.Hit->Actor.Get(), Fixture.Actor);
	EXPECT_EQ(First.Hit->Distance, 3.0);
	const auto Blank = Fixture.Client.PickViewport(Fixture.Level, View, {1.f, 0.f}, EViewportPickLayer::SceneGeometry);
	EXPECT_EQ(Blank.Status, EViewportPickStatus::Completed);
	EXPECT_FALSE(Blank.Hit);
	const auto Last = Fixture.Client.PickViewport(Fixture.Level, View, {1.f, 1.f}, EViewportPickLayer::SceneGeometry);
	ASSERT_TRUE(Last.Hit);
	EXPECT_EQ(Last.Hit->Distance, 4.0);
	State->bFail = true;
	EXPECT_EQ(Fixture.Client.PickViewport(Fixture.Level, View, {0.f, 0.f}).Status, EViewportPickStatus::Failed);
}
