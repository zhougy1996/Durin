#include "MaterialTestSupport.h"

#include "Materials/MaterialUpdateContext.h"

#include <future>

TEST(FMaterialUpdateContextTests, FlushMergesRootsAndScansLoadedComponentsOnce)
{
	FRenderSceneHarness Harness;
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "BatchBase");
	auto* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "BatchChild");
	auto* Grandchild = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "BatchGrandchild");
	auto* Sibling = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "BatchSibling");
	auto* Unrelated = Durin::NewObject<Durin::DMaterial>(nullptr, "BatchUnrelated");
	ASSERT_TRUE(Child->SetParent(Base));
	ASSERT_TRUE(Grandchild->SetParent(Child));
	ASSERT_TRUE(Sibling->SetParent(Base));

	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	const Durin::FGuid SecondSlot = AddDebugMaterialSlot(Mesh, "SecondBatchSlot");
	const Durin::FGuid ThirdSlot = AddDebugMaterialSlot(Mesh, "ThirdBatchSlot");
	auto* Component = Harness.CreateStaticMeshComponent("BatchComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, Grandchild);
	ASSERT_TRUE(Component->SetMaterialBySlotId(SecondSlot, Grandchild));
	ASSERT_TRUE(Component->SetMaterialBySlotId(ThirdSlot, Sibling));
	Component->RegisterComponent();

	auto* OtherMesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* OtherComponent = Harness.CreateStaticMeshComponent("BatchOtherComponent");
	OtherComponent->SetStaticMesh(OtherMesh);
	OtherComponent->SetMaterial(Unrelated);
	OtherComponent->RegisterComponent();
	auto* SharedComponent = Harness.CreateStaticMeshComponent("BatchSharedComponent");
	SharedComponent->SetStaticMesh(Mesh);
	SharedComponent->SetMaterial(0, Sibling);
	ASSERT_TRUE(SharedComponent->SetMaterialBySlotId(SecondSlot, Grandchild));
	ASSERT_TRUE(SharedComponent->SetMaterialBySlotId(ThirdSlot, Unrelated));
	SharedComponent->RegisterComponent();
	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);

	const std::vector<Durin::DObject*> ObjectsBeforeFlush = Durin::GDObjectArray.Snapshot();
	const Durin::uint64 ExpectedComponentCount = static_cast<Durin::uint64>(std::ranges::count_if(
		ObjectsBeforeFlush,
		[](Durin::DObject* Object) {
			return Durin::IsValid(Durin::Cast<Durin::DStaticMeshComponent>(Object));
		}));
	const Durin::uint64 ExpectedMaterialCount = static_cast<Durin::uint64>(std::ranges::count_if(
		ObjectsBeforeFlush,
		[](Durin::DObject* Object) {
			return Durin::IsValid(Durin::Cast<Durin::DMaterialInterface>(Object));
		}));
	const Durin::uint64 BaseVersion = Base->GetRenderStateVersion();
	const Durin::uint64 ChildVersion = Child->GetRenderStateVersion();
	const Durin::uint64 GrandchildVersion = Grandchild->GetRenderStateVersion();
	const Durin::uint64 SiblingVersion = Sibling->GetRenderStateVersion();
	const Durin::uint64 UnrelatedVersion = Unrelated->GetRenderStateVersion();

	Durin::FMaterialUpdateContext Context;
	Context.AddMaterial(Base, Durin::EMaterialRenderDirtyFlags::DynamicParameters);
	Context.AddMaterial(
		Base,
		Durin::EMaterialRenderDirtyFlags::ShaderMap
			| Durin::EMaterialRenderDirtyFlags::PipelineState);
	Context.AddMaterial(Base, Durin::EMaterialRenderDirtyFlags::ParentChain);
	Context.AddMaterial(Child, Durin::EMaterialRenderDirtyFlags::DynamicParameters);
	Context.AddMaterial(nullptr, Durin::EMaterialRenderDirtyFlags::DynamicParameters);
	Context.Flush();

	const Durin::FMaterialUpdateCounters Counters = Context.GetCounters();
	EXPECT_EQ(Counters.RootCount, 2);
	EXPECT_EQ(Counters.ObjectSnapshotCount, 1);
	EXPECT_EQ(Counters.ScannedObjectCount, ObjectsBeforeFlush.size());
	EXPECT_EQ(Counters.TestedMaterialCount, ExpectedMaterialCount);
	EXPECT_EQ(Counters.AffectedMaterialCount, 4);
	EXPECT_EQ(Counters.ScannedComponentCount, ExpectedComponentCount);
	EXPECT_EQ(Counters.UpdatedSlotCount, 5);
	const Durin::FMaterialUpdateCounters LatestCounters = Durin::GetLastMaterialUpdateCounters();
	EXPECT_EQ(LatestCounters.RootCount, Counters.RootCount);
	EXPECT_EQ(LatestCounters.ObjectSnapshotCount, Counters.ObjectSnapshotCount);
	EXPECT_EQ(LatestCounters.ScannedObjectCount, Counters.ScannedObjectCount);
	EXPECT_EQ(LatestCounters.TestedMaterialCount, Counters.TestedMaterialCount);
	EXPECT_EQ(LatestCounters.AffectedMaterialCount, Counters.AffectedMaterialCount);
	EXPECT_EQ(LatestCounters.ScannedComponentCount, Counters.ScannedComponentCount);
	EXPECT_EQ(LatestCounters.UpdatedSlotCount, Counters.UpdatedSlotCount);
	EXPECT_EQ(Base->GetRenderStateVersion(), BaseVersion + 1);
	EXPECT_EQ(Child->GetRenderStateVersion(), ChildVersion + 1);
	EXPECT_EQ(Grandchild->GetRenderStateVersion(), GrandchildVersion + 1);
	EXPECT_EQ(Sibling->GetRenderStateVersion(), SiblingVersion + 1);
	EXPECT_EQ(Unrelated->GetRenderStateVersion(), UnrelatedVersion);

	Context.Flush();
	EXPECT_EQ(Context.GetCounters().ObjectSnapshotCount, 1);
	EXPECT_EQ(Base->GetRenderStateVersion(), BaseVersion + 1);
	EXPECT_EQ(Grandchild->GetRenderStateVersion(), GrandchildVersion + 1);

	const FMaterialSlotsSnapshot Updated = CaptureMaterialSlots(Harness.Scene);
	EXPECT_EQ(Updated.Proxy, Initial.Proxy);
	EXPECT_EQ(Updated.MaterialProxies, Initial.MaterialProxies);
	EXPECT_EQ(Updated.ComponentRevision, Initial.ComponentRevision);

	SharedComponent->UnregisterComponent();
	OtherComponent->UnregisterComponent();
	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(SharedComponent);
	Durin::MarkAsGarbage(OtherComponent);
	Durin::MarkAsGarbage(OtherMesh);
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Unrelated);
	Durin::MarkAsGarbage(Sibling);
	Durin::MarkAsGarbage(Grandchild);
	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Base);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialUpdateContextTests, ScanCostGrowsWithUnrelatedLoadedObjectsAndComponents)
{
	FRenderSceneHarness Harness;
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "ScalingBase");
	auto* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "ScalingChild");
	auto* Grandchild = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "ScalingGrandchild");
	ASSERT_TRUE(Child->SetParent(Base));
	ASSERT_TRUE(Grandchild->SetParent(Child));

	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* Component = Harness.CreateStaticMeshComponent("ScalingAffectedComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Grandchild);
	Component->RegisterComponent();

	Durin::FMaterialUpdateContext BaselineContext;
	BaselineContext.AddMaterial(Base, Durin::EMaterialRenderDirtyFlags::DynamicParameters);
	BaselineContext.Flush();
	const Durin::FMaterialUpdateCounters Baseline = BaselineContext.GetCounters();
	EXPECT_EQ(Baseline.RootCount, 1);
	EXPECT_EQ(Baseline.ObjectSnapshotCount, 1);
	EXPECT_EQ(Baseline.AffectedMaterialCount, 3);
	EXPECT_EQ(Baseline.UpdatedSlotCount, 1);

	constexpr Durin::uint64 UnrelatedCount = 12;
	auto* UnrelatedMesh = Durin::DStaticMesh::CreateDebugTriangle();
	std::vector<Durin::DMaterial*> UnrelatedMaterials;
	UnrelatedMaterials.reserve(UnrelatedCount);
	for (Durin::uint64 Index = 0; Index < UnrelatedCount; ++Index)
	{
		auto* Material = Durin::NewObject<Durin::DMaterial>(
			nullptr, Durin::FName(std::format("ScalingUnrelatedMaterial{}", Index)));
		auto* UnrelatedComponent = Harness.CreateStaticMeshComponent(
			Durin::FName(std::format("ScalingUnrelatedComponent{}", Index)));
		ASSERT_NE(UnrelatedComponent, nullptr);
		UnrelatedComponent->SetStaticMesh(UnrelatedMesh);
		UnrelatedComponent->SetMaterial(Material);
		UnrelatedMaterials.push_back(Material);
	}

	Durin::FMaterialUpdateContext ExpandedContext;
	ExpandedContext.AddMaterial(Base, Durin::EMaterialRenderDirtyFlags::DynamicParameters);
	ExpandedContext.Flush();
	const Durin::FMaterialUpdateCounters Expanded = ExpandedContext.GetCounters();
	EXPECT_EQ(Expanded.RootCount, Baseline.RootCount);
	EXPECT_EQ(Expanded.ObjectSnapshotCount, Baseline.ObjectSnapshotCount);
	EXPECT_EQ(Expanded.AffectedMaterialCount, Baseline.AffectedMaterialCount);
	EXPECT_EQ(Expanded.UpdatedSlotCount, Baseline.UpdatedSlotCount);
	EXPECT_EQ(
		Expanded.TestedMaterialCount,
		Baseline.TestedMaterialCount + UnrelatedCount);
	EXPECT_EQ(
		Expanded.ScannedComponentCount,
		Baseline.ScannedComponentCount + UnrelatedCount);
	EXPECT_GE(
		Expanded.ScannedObjectCount,
		Baseline.ScannedObjectCount + UnrelatedCount * 3);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Harness.Shutdown();
	Durin::MarkAsGarbage(UnrelatedMesh);
	for (Durin::DMaterial* Material : UnrelatedMaterials)
	{
		Durin::MarkAsGarbage(Material);
	}
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Grandchild);
	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialUpdateContextTests, AcceptedRenderUpdateSurvivesMaterialDestruction)
{
	FRenderSceneHarness Harness;
	auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "QueuedDestructionMaterial");
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* Component = Harness.CreateStaticMeshComponent("QueuedDestructionComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Material);
	Component->RegisterComponent();
	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);

	auto CommandStarted = std::make_shared<std::promise<void>>();
	std::future<void> CommandStartedFuture = CommandStarted->get_future();
	auto AllowCommandCompletion = std::make_shared<std::promise<void>>();
	std::shared_future<void> AllowCommandCompletionFuture =
		AllowCommandCompletion->get_future().share();
	struct FBlockMaterialRenderUpdateCommand
	{
		static constexpr auto GetName() -> const char*
		{
			return "BlockMaterialRenderUpdate";
		}
	};
	Durin::EnqueueRenderCommand<FBlockMaterialRenderUpdateCommand>(
		[CommandStarted, AllowCommandCompletionFuture](Durin::FRHICommandListImmediate&) {
			CommandStarted->set_value();
			AllowCommandCompletionFuture.wait();
		});
	CommandStartedFuture.wait();

	const bool bChanged = Material->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(),
		Durin::FVector3(0.15, 0.35, 0.65));
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
	const bool bMaterialDestroyed = !Durin::GDObjectArray.Contains(Material);
	AllowCommandCompletion->set_value();

	const FMaterialSlotsSnapshot Updated = CaptureMaterialSlots(Harness.Scene);
	EXPECT_TRUE(bChanged);
	EXPECT_TRUE(bMaterialDestroyed);
	EXPECT_EQ(Updated.Proxy, Initial.Proxy);
	EXPECT_EQ(Updated.MaterialProxies, Initial.MaterialProxies);
	EXPECT_EQ(Updated.ComponentRevision, Initial.ComponentRevision);
	ASSERT_EQ(Updated.Materials.size(), 1u);
	ExpectColorNear(
		Updated.Materials[0].BaseColor,
		Durin::FVector4f(0.15f, 0.35f, 0.65f, 1.0f));

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Harness.Shutdown();
	Durin::CollectGarbage();
}
