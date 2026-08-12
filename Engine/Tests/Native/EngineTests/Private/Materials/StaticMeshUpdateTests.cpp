#include "MaterialTestSupport.h"
#include "DynamicRHI.h"
#include "Components/SplineMeshComponent.h"
#include "StaticMesh/StaticMeshRenderStateRecreateContext.h"

#include <chrono>
#include <iostream>

namespace
{
	auto ReplaceWithDebugCandidate(Durin::DStaticMesh* Mesh) -> bool
	{
		Durin::DStaticMesh* Candidate =
			Durin::DStaticMesh::CreateDebugTriangle();
		if (Candidate == nullptr) return false;
		if (const Durin::FStaticMeshMaterialSlotDefinition* Slot =
			Mesh->GetMaterialSlot(0))
		{
			auto* Slots = static_cast<Durin::FArrayProperty*>(
				Candidate->GetClass()->FindPropertyByName(
					"MaterialSlots"));
			auto* CandidateSlot =
				static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(
					Slots->GetMutableElementPtr(Candidate, 0));
			CandidateSlot->DefaultMaterial = Slot->DefaultMaterial;
		}
		std::string Error;
		const bool bSucceeded =
			Mesh->ExchangeImportedState(*Candidate, Error);
		Durin::MarkAsGarbage(Candidate);
		return bSucceeded;
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

	auto CapturePrimitiveCount(Durin::FScene* Scene) -> size_t
	{
		size_t Count = 0;
		struct FCapturePrimitiveCountCommand
		{
			static constexpr auto GetName() -> const char* { return "CapturePrimitiveCount"; }
		};
		Durin::EnqueueRenderCommand<FCapturePrimitiveCountCommand>(
			[Scene, &Count](Durin::FRHICommandListImmediate&) {
				Count = Scene->GetPrimitiveSceneInfos().size();
			});
		WaitForRenderingThread();
		return Count;
	}
}

TEST(FStaticMeshRenderStateRecreateContextTests, UsesRetainedWorldSceneWithoutGlobalEngine)
{
	FRenderSceneHarness Harness;
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* Component = Harness.CreateStaticMeshComponent("RecreateWithoutGlobalEngine");
	Component->SetStaticMesh(Mesh);
	ASSERT_EQ(CapturePrimitiveCount(Harness.Scene), 1u);

	Durin::DEngine* SavedEngine = Durin::GEngine;
	Durin::GEngine = nullptr;
	const auto RecreateStart = std::chrono::steady_clock::now();
	{
		Durin::FStaticMeshRenderStateRecreateContext Context(Mesh);
		EXPECT_EQ(CapturePrimitiveCount(Harness.Scene), 0u);
	}
	EXPECT_EQ(CapturePrimitiveCount(Harness.Scene), 1u);
	const auto RecreateMicroseconds =
		std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - RecreateStart)
			.count();
	std::cout
		<< "[StaticMeshRenderStateMetric] detach_recreate_us="
		<< RecreateMicroseconds
		<< " proxies_before=1 proxies_during=0 proxies_after=1\n";
	Durin::GEngine = SavedEngine;

	Component->UnregisterComponent();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FStaticMeshRenderStateRecreateContextTests, SkipsUnregisteredReassignedAndGarbageComponents)
{
	FRenderSceneHarness Harness;
	auto* FirstMesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* SecondMesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* Registered = Harness.CreateStaticMeshComponent("RecreateRegistered");
	Registered->SetStaticMesh(FirstMesh);
	auto* Unregistered = Durin::NewObject<Durin::DStaticMeshComponent>(
		nullptr, "RecreateUnregistered");
	Unregistered->SetStaticMesh(FirstMesh);
	ASSERT_EQ(CapturePrimitiveCount(Harness.Scene), 1u);

	{
		Durin::FStaticMeshRenderStateRecreateContext Context(FirstMesh);
		EXPECT_EQ(CapturePrimitiveCount(Harness.Scene), 0u);
		Registered->SetStaticMesh(SecondMesh);
		EXPECT_EQ(CapturePrimitiveCount(Harness.Scene), 1u);
		Durin::MarkAsGarbage(Unregistered);
	}
	EXPECT_EQ(CapturePrimitiveCount(Harness.Scene), 1u);

	Registered->UnregisterComponent();
	Durin::MarkAsGarbage(Registered);
	Durin::MarkAsGarbage(SecondMesh);
	Durin::MarkAsGarbage(FirstMesh);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FStaticMeshRenderStateRecreateContextTests, RejectsReusedObjectSlotGeneration)
{
	InitializeDObjectSystem();
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::AddToRoot(Mesh);
	auto* Previous = Durin::NewObject<Durin::DStaticMeshComponent>(
		nullptr, "RecreatePreviousGeneration");
	Previous->SetStaticMesh(Mesh);
	Previous->RegisterComponent();
	const Durin::FObjectHandle PreviousHandle = Durin::MakeObjectHandle(Previous);
	auto Context = std::make_unique<Durin::FStaticMeshRenderStateRecreateContext>(Mesh);

	Durin::MarkAsGarbage(Previous);
	Durin::CollectGarbage();
	ASSERT_EQ(Durin::ResolveObjectHandle(PreviousHandle), nullptr);

	auto* Replacement = Durin::NewObject<Durin::DStaticMeshComponent>(
		nullptr, "RecreateReplacementGeneration");
	const Durin::FObjectHandle ReplacementHandle = Durin::MakeObjectHandle(Replacement);
	ASSERT_EQ(ReplacementHandle.Index, PreviousHandle.Index);
	ASSERT_NE(ReplacementHandle.Generation, PreviousHandle.Generation);
	Replacement->SetStaticMesh(Mesh);
	Replacement->RegisterComponent();
	Context.reset();
	EXPECT_TRUE(Replacement->IsRegistered());
	EXPECT_EQ(Replacement->GetStaticMesh(), Mesh);

	Replacement->UnregisterComponent();
	Durin::MarkAsGarbage(Replacement);
	Durin::RemoveFromRoot(Mesh);
	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage();
}

TEST(FStaticMeshRenderStateRecreateContextTests, RepublishesSplineMeshDerivedStateAfterSourceExchange)
{
	InitializeDObjectSystem();
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* Component = Durin::NewObject<Durin::DSplineMeshComponent>(nullptr, "SplineMeshSourceExchange");
	ASSERT_NE(Mesh, nullptr);
	ASSERT_NE(Component, nullptr);
	Component->SetStaticMesh(Mesh);
	Component->RegisterComponent();
	const auto Before = Component->GetDerivedState();
	ASSERT_TRUE(Before && Before->IsValid());
	ASSERT_TRUE(ReplaceWithDebugCandidate(Mesh));
	const auto After = Component->GetDerivedState();
	ASSERT_TRUE(After && After->IsValid());
	EXPECT_NE(After, Before);
	EXPECT_GT(After->SourceRenderResourceRevision, Before->SourceRenderResourceRevision);
	EXPECT_GT(After->DeformationRevision, Before->DeformationRevision);
	EXPECT_NE(After->CollisionInputIdentity, Before->CollisionInputIdentity);

	Component->UnregisterComponent();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage();
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

	auto* Component = Harness.CreateStaticMeshComponent("StaticMeshScanComponent");
	Component->SetStaticMesh(FirstMesh);
	Component->RegisterComponent();
	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);
	ASSERT_EQ(Initial.Materials.size(), 1);
	ExpectColorNear(GetMaterialBinding(Initial.Materials[0]).BaseColor, Durin::FVector4f(0.2f, 0.3f, 0.4f, 1.0f));

	ASSERT_TRUE(ReplaceWithDebugCandidate(SecondMesh));
	const FMaterialSlotsSnapshot UnrelatedUpdate = CaptureMaterialSlots(Harness.Scene);
	EXPECT_EQ(UnrelatedUpdate.Proxy, Initial.Proxy);

	SetDefaultMaterial(FirstMesh, 0, Second);
	ASSERT_TRUE(ReplaceWithDebugCandidate(FirstMesh));
	const FMaterialSlotsSnapshot DefaultUpdate = CaptureMaterialSlots(Harness.Scene);
	EXPECT_NE(DefaultUpdate.RenderData, Initial.RenderData);
	EXPECT_GT(
		DefaultUpdate.ComponentRevision,
		Initial.ComponentRevision);
	ExpectColorNear(GetMaterialBinding(DefaultUpdate.Materials[0]).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));

	Component->SetStaticMesh(SecondMesh);
	const FMaterialSlotsSnapshot Reassigned = CaptureMaterialSlots(Harness.Scene);
	EXPECT_NE(Reassigned.RenderData, DefaultUpdate.RenderData);
	ASSERT_TRUE(ReplaceWithDebugCandidate(FirstMesh));
	const FMaterialSlotsSnapshot PreviousMeshUpdate = CaptureMaterialSlots(Harness.Scene);
	EXPECT_EQ(PreviousMeshUpdate.Proxy, Reassigned.Proxy);
	ASSERT_TRUE(ReplaceWithDebugCandidate(SecondMesh));
	const FMaterialSlotsSnapshot CurrentMeshUpdate = CaptureMaterialSlots(Harness.Scene);
	EXPECT_NE(
		CurrentMeshUpdate.RenderData,
		Reassigned.RenderData);
	EXPECT_GT(
		CurrentMeshUpdate.ComponentRevision,
		Reassigned.ComponentRevision);

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
	auto* Component = Harness.CreateStaticMeshComponent("GarbageStaticMeshScanComponent");
	Component->SetStaticMesh(Mesh);
	Component->RegisterComponent();
	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);

	Durin::MarkAsGarbage(Component);
	ASSERT_TRUE(ReplaceWithDebugCandidate(Mesh));
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

	auto* Replacement = Harness.CreateStaticMeshComponent("ReplacementStaticMeshScanComponent");
	const Durin::FObjectHandle ReplacementHandle = Durin::MakeObjectHandle(Replacement);
	EXPECT_TRUE(ReplacementHandle.Index != PreviousHandle.Index
		|| ReplacementHandle.Generation != PreviousHandle.Generation);
	Replacement->SetStaticMesh(SecondMesh);
	Replacement->RegisterComponent();
	const FMaterialSlotsSnapshot Initial = CaptureMaterialSlots(Harness.Scene);

	ASSERT_TRUE(ReplaceWithDebugCandidate(FirstMesh));
	const FMaterialSlotsSnapshot PreviousMeshUpdate = CaptureMaterialSlots(Harness.Scene);
	EXPECT_EQ(PreviousMeshUpdate.Proxy, Initial.Proxy);
	ASSERT_TRUE(ReplaceWithDebugCandidate(SecondMesh));
	const FMaterialSlotsSnapshot CurrentMeshUpdate = CaptureMaterialSlots(Harness.Scene);
	EXPECT_NE(CurrentMeshUpdate.RenderData, Initial.RenderData);
	EXPECT_GT(
		CurrentMeshUpdate.ComponentRevision,
		Initial.ComponentRevision);

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

TEST(FStaticMeshUpdateTests, NoRHIStaticMeshDestructionNeedsNoRenderFenceSubmission)
{
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	const Durin::FObjectHandle Handle = Durin::MakeObjectHandle(Mesh);

	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectHandle(Handle), nullptr);
}
