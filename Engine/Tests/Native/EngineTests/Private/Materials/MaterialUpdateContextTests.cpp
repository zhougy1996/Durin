#include "MaterialTestSupport.h"

#include "Materials/MaterialUpdateContext.h"

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
	auto* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "BatchComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, Grandchild);
	ASSERT_TRUE(Component->SetMaterialBySlotId(SecondSlot, Grandchild));
	ASSERT_TRUE(Component->SetMaterialBySlotId(ThirdSlot, Sibling));
	Component->RegisterComponent();

	auto* OtherMesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* OtherComponent = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "BatchOtherComponent");
	OtherComponent->SetStaticMesh(OtherMesh);
	OtherComponent->SetMaterial(Unrelated);
	OtherComponent->RegisterComponent();
	auto* SharedComponent = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "BatchSharedComponent");
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
	Context.AddMaterial(Base, Durin::EMaterialRenderDirtyFlags::ParameterValues);
	Context.AddMaterial(Base, Durin::EMaterialRenderDirtyFlags::ParentChain);
	Context.AddMaterial(Child, Durin::EMaterialRenderDirtyFlags::ParameterValues);
	Context.AddMaterial(nullptr, Durin::EMaterialRenderDirtyFlags::ParameterValues);
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
	ASSERT_EQ(Updated.MaterialDirtyFlags.size(), 3);
	const Durin::EMaterialRenderDirtyFlags MergedFlags =
		Durin::EMaterialRenderDirtyFlags::ParameterValues
		| Durin::EMaterialRenderDirtyFlags::ParentChain;
	for (Durin::EMaterialRenderDirtyFlags DirtyFlags : Updated.MaterialDirtyFlags)
	{
		EXPECT_TRUE(Durin::EnumHasAllFlags(DirtyFlags, MergedFlags));
	}
	EXPECT_EQ(Updated.ComponentRevision, Initial.ComponentRevision + 3);

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
