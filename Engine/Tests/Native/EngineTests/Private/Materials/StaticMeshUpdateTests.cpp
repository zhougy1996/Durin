#include "MaterialTestSupport.h"

namespace
{
	auto CloneRenderData(const Durin::DStaticMesh* Mesh) -> std::unique_ptr<Durin::FStaticMeshRenderData>
	{
		return std::make_unique<Durin::FStaticMeshRenderData>(*Mesh->GetRenderData());
	}

	auto SetDefaultMaterial(
		Durin::DStaticMesh* Mesh,
		Durin::uint64 SlotIndex,
		Durin::DMaterialInterface* Material
	) -> void
	{
		auto* Slots = static_cast<Durin::FArrayProperty*>(Mesh->GetClass()->FindPropertyByName("MaterialSlots"));
		ASSERT_NE(Slots, nullptr);
		auto* Slot = static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, SlotIndex));
		ASSERT_NE(Slot, nullptr);
		Slot->DefaultMaterial = Material;
	}
}

TEST(FStaticMeshUpdateTests, CurrentAssignmentsAndDefaultsDriveLoadedComponentScans)
{
	FRenderSceneHarness Harness;
	auto* First = Durin::NewObject<Durin::DMaterial>(nullptr, "StaticMeshScanFirst");
	auto* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "StaticMeshScanSecond");
	First->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.3, 0.4));
	Second->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.7, 0.6, 0.5));
	auto* FirstMesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* SecondMesh = Durin::DStaticMesh::CreateDebugTriangle();
	SetDefaultMaterial(FirstMesh, 0, First);
	SetDefaultMaterial(SecondMesh, 0, Second);

	auto* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "StaticMeshScanComponent");
	Component->SetStaticMesh(FirstMesh);
	Component->RegisterComponent();
	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Initial.Materials.size(), 1);
	ExpectColorNear(Initial.Materials[0].BaseColor, Durin::FVector4f(0.2f, 0.3f, 0.4f, 1.0f));

	SecondMesh->SetRenderData(CloneRenderData(SecondMesh));
	const FMaterialSlotsSnapshot UnrelatedUpdate = CaptureMaterialSlots(Harness.Scene);
	EXPECT_EQ(UnrelatedUpdate.Proxy, Initial.Proxy);

	SetDefaultMaterial(FirstMesh, 0, Second);
	FirstMesh->SetRenderData(CloneRenderData(FirstMesh));
	const FMaterialSlotsSnapshot DefaultUpdate = CaptureMaterialSlots(Harness.Scene);
	EXPECT_NE(DefaultUpdate.Proxy, Initial.Proxy);
	ExpectColorNear(DefaultUpdate.Materials[0].BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));

	Component->SetStaticMesh(SecondMesh);
	const FMaterialSlotsSnapshot Reassigned = CaptureMaterialSlots(Harness.Scene);
	EXPECT_NE(Reassigned.Proxy, DefaultUpdate.Proxy);
	FirstMesh->SetRenderData(CloneRenderData(FirstMesh));
	const FMaterialSlotsSnapshot PreviousMeshUpdate = CaptureMaterialSlots(Harness.Scene);
	EXPECT_EQ(PreviousMeshUpdate.Proxy, Reassigned.Proxy);
	SecondMesh->SetRenderData(CloneRenderData(SecondMesh));
	const FMaterialSlotsSnapshot CurrentMeshUpdate = CaptureMaterialSlots(Harness.Scene);
	EXPECT_NE(CurrentMeshUpdate.Proxy, Reassigned.Proxy);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(SecondMesh);
	Durin::MarkAsGarbage(FirstMesh);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FStaticMeshUpdateTests, LoadedComponentScanSkipsGarbageObjects)
{
	FRenderSceneHarness Harness;
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "GarbageStaticMeshScanComponent");
	Component->SetStaticMesh(Mesh);
	Component->RegisterComponent();
	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);

	Durin::MarkAsGarbage(Component);
	Mesh->SetRenderData(CloneRenderData(Mesh));
	const FMaterialSlotsSnapshot AfterUpdate = CaptureMaterialSlots(Harness.Scene);
	EXPECT_EQ(AfterUpdate.Proxy, Initial.Proxy);

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Mesh);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FStaticMeshUpdateTests, LaterScansResolveReusedObjectSlotsByGenerationAndCurrentMesh)
{
	FRenderSceneHarness Harness;
	auto* FirstMesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* SecondMesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* Previous = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "PreviousStaticMeshScanComponent");
	Previous->SetStaticMesh(FirstMesh);
	const Durin::FObjectHandle PreviousHandle = Durin::MakeObjectHandle(Previous);

	Durin::AddToRoot(FirstMesh);
	Durin::AddToRoot(SecondMesh);
	Durin::MarkAsGarbage(Previous);
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectHandle(PreviousHandle), nullptr);

	auto* Replacement = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "ReplacementStaticMeshScanComponent");
	const Durin::FObjectHandle ReplacementHandle = Durin::MakeObjectHandle(Replacement);
	EXPECT_EQ(ReplacementHandle.Index, PreviousHandle.Index);
	EXPECT_NE(ReplacementHandle.Generation, PreviousHandle.Generation);
	Replacement->SetStaticMesh(SecondMesh);
	Replacement->RegisterComponent();
	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);

	FirstMesh->SetRenderData(CloneRenderData(FirstMesh));
	const FMaterialSlotsSnapshot PreviousMeshUpdate = CaptureMaterialSlots(Harness.Scene);
	EXPECT_EQ(PreviousMeshUpdate.Proxy, Initial.Proxy);
	SecondMesh->SetRenderData(CloneRenderData(SecondMesh));
	const FMaterialSlotsSnapshot CurrentMeshUpdate = CaptureMaterialSlots(Harness.Scene);
	EXPECT_NE(CurrentMeshUpdate.Proxy, Initial.Proxy);

	Replacement->UnregisterComponent();
	WaitForRenderingThread();
	Durin::RemoveFromRoot(SecondMesh);
	Durin::RemoveFromRoot(FirstMesh);
	Durin::MarkAsGarbage(Replacement);
	Durin::MarkAsGarbage(SecondMesh);
	Durin::MarkAsGarbage(FirstMesh);
	Harness.Shutdown();
	Durin::CollectGarbage();
}
