#include "StaticMeshMaterialSlotDetails.h"

#include "AssetSystem.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Package.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "Editor/EditorAssetPicker.h"
#include "EngineTestSupport.h"
#include "LevelEditorCustomizations.h"
#include "Materials/Material.h"
#include "Misc/Paths.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Texture/Texture2D.h"
#include "Workspace/LevelEditorContext.h"

#include <gtest/gtest.h>

namespace
{
	auto AddSlot(Durin::DStaticMesh* Mesh, std::string_view Name, Durin::DMaterialInterface* Default = nullptr)
		-> Durin::FGuid
	{
		auto* Slots = static_cast<Durin::FArrayProperty*>(Mesh->GetClass()->FindPropertyByName("MaterialSlots"));
		const Durin::uint64 Index = Slots->Num(Mesh);
		Slots->Resize(Mesh, Index + 1);
		auto* Slot = static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, Index));
		Slot->SlotId = Durin::FGuid::NewGuid();
		Slot->Name = Durin::FName(Name);
		Slot->SourceName = std::string(Name);
		Slot->SourceMaterialIndex = static_cast<Durin::uint32>(Index);
		Slot->DefaultMaterial = Default;
		Mesh->GetRenderData()->MaterialSlots.push_back({std::string(Name), static_cast<Durin::uint32>(Index), Slot->SlotId});
		return Slot->SlotId;
	}

	auto MakeContext(Durin::FEditorTransactionManager& Transactions, std::string& Error)
		-> Durin::FReflectedPropertyViewContext
	{
		return {.Transactions = &Transactions, .ReportError = [&Error](std::string Message) { Error = std::move(Message); }};
	}
}

TEST(FStaticMeshMaterialSlotDetailsTests, BuildsFixedRowsSourcesSearchAndOrphans)
{
	InitializeDObjectSystem();
	auto* Default = Durin::NewObject<Durin::DMaterial>(nullptr, "SlotDefault");
	auto* Override = Durin::NewObject<Durin::DMaterial>(nullptr, "SlotOverride");
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* FirstSlot = const_cast<Durin::FStaticMeshMaterialSlotDefinition*>(Mesh->GetMaterialSlot(0));
	FirstSlot->Name = Durin::FName("Body");
	FirstSlot->DefaultMaterial = Default;
	const Durin::FGuid SecondId = AddSlot(Mesh, "Glass");
	auto* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "SlotDetailsComponent");
	Component->SetStaticMesh(Mesh);
	ASSERT_TRUE(Component->SetMaterialBySlotId(SecondId, Override));

	Durin::FStaticMeshMaterialSlotDetailsModel Model(Component);
	ASSERT_TRUE(Model.HasMesh());
	ASSERT_EQ(Model.GetCurrentEntries().size(), 2u);
	EXPECT_EQ(Model.GetCurrentEntries()[0].Label, "[0] Body");
	EXPECT_EQ(Model.GetCurrentEntries()[0].Source, Durin::EStaticMeshMaterialSource::MeshDefault);
	EXPECT_EQ(Model.GetCurrentEntries()[0].Material, Default);
	EXPECT_NE(Model.GetCurrentEntries()[0].SearchKeywords.find("Body"), std::string::npos);
	EXPECT_EQ(Model.GetCurrentEntries()[1].Source, Durin::EStaticMeshMaterialSource::ComponentOverride);
	EXPECT_TRUE(Model.GetCurrentEntries()[1].bHasOverride);

	auto* OtherMesh = Durin::DStaticMesh::CreateDebugTriangle();
	Component->SetStaticMesh(OtherMesh);
	Durin::FStaticMeshMaterialSlotDetailsModel OrphanModel(Component);
	ASSERT_EQ(OrphanModel.GetCurrentEntries().size(), 1u);
	ASSERT_EQ(OrphanModel.GetOrphanEntries().size(), 1u);
	EXPECT_EQ(OrphanModel.GetOrphanEntries()[0].SlotId, SecondId);
	EXPECT_NE(OrphanModel.GetOrphanEntries()[0].SearchKeywords.find(SecondId.ToString()), std::string::npos);

	Component->SetStaticMesh(nullptr);
	Durin::FStaticMeshMaterialSlotDetailsModel EmptyModel(Component);
	EXPECT_FALSE(EmptyModel.HasMesh());
	EXPECT_TRUE(EmptyModel.GetCurrentEntries().empty());
	EXPECT_EQ(EmptyModel.GetOrphanEntries().size(), 1u);

	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(OtherMesh);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Override);
	Durin::MarkAsGarbage(Default);
	Durin::CollectGarbage();
}

TEST(FStaticMeshMaterialSlotDetailsTests, FiltersMaterialTypesAndUsesGuidScopedRootTransactions)
{
	InitializeDObjectSystem();
	auto* First = Durin::NewObject<Durin::DMaterial>(nullptr, "SlotFirstMaterial");
	auto* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "SlotSecondMaterial");
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "NotAMaterial");
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	const Durin::FGuid FirstId = Mesh->GetMaterialSlot(0)->SlotId;
	const Durin::FGuid SecondId = AddSlot(Mesh, "Second");
	auto* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "SlotTransactionComponent");
	Component->SetStaticMesh(Mesh);

	EXPECT_TRUE(Durin::FStaticMeshMaterialSlotDetailsModel::IsSupportedMaterialClass(First->GetClass()));
	EXPECT_FALSE(Durin::FStaticMeshMaterialSlotDetailsModel::IsSupportedMaterialClass(Texture->GetClass()));
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);

	Durin::FStaticMeshMaterialSlotDetailsModel Initial(Component);
	ASSERT_TRUE(Initial.AssignMaterial(PropertyView, Context, Initial.GetCurrentEntries()[0], First));
	Durin::FStaticMeshMaterialSlotDetailsModel WithFirst(Component);
	ASSERT_TRUE(WithFirst.AssignMaterial(PropertyView, Context, WithFirst.GetCurrentEntries()[1], Second));
	EXPECT_EQ(Component->GetMaterialOverride(FirstId), First);
	EXPECT_EQ(Component->GetMaterialOverride(SecondId), Second);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Component->GetMaterialOverride(FirstId), First);
	EXPECT_EQ(Component->GetMaterialOverride(SecondId), nullptr);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Component->GetMaterialOverride(SecondId), Second);

	Durin::FStaticMeshMaterialSlotDetailsModel Replace(Component);
	ASSERT_TRUE(Replace.AssignMaterial(PropertyView, Context, Replace.GetCurrentEntries()[0], Second, true));
	EXPECT_EQ(Component->GetMaterialOverride(FirstId), Second);
	ASSERT_TRUE(PropertyView.FinishActiveEdit(&Context, true));
	EXPECT_EQ(Component->GetMaterialOverride(FirstId), First);

	Durin::FStaticMeshMaterialSlotDetailsModel Reset(Component);
	ASSERT_TRUE(Reset.ResetOverride(PropertyView, Context, Reset.GetCurrentEntries()[0]));
	EXPECT_EQ(Component->GetMaterialOverride(FirstId), nullptr);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Component->GetMaterialOverride(FirstId), First);

	Component->SetStaticMesh(Durin::DStaticMesh::CreateDebugTriangle());
	Durin::FStaticMeshMaterialSlotDetailsModel Orphans(Component);
	ASSERT_EQ(Orphans.GetOrphanEntries().size(), 2u);
	const Durin::FGuid RemovedId = Orphans.GetOrphanEntries()[0].SlotId;
	ASSERT_TRUE(Orphans.RemoveOrphan(PropertyView, Context, Orphans.GetOrphanEntries()[0]));
	EXPECT_FALSE(Component->HasMaterialOverride(RemovedId));
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_TRUE(Component->HasMaterialOverride(RemovedId));
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_FALSE(Component->HasMaterialOverride(RemovedId));
	EXPECT_TRUE(Error.empty());

	Transactions.Clear();
	Durin::DStaticMesh* CurrentMesh = Component->GetStaticMesh();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(CurrentMesh);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Texture);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::CollectGarbage();
}

TEST(FStaticMeshMaterialSlotDetailsTests, CustomizationHidesCollectionsAndTransactionsDirtyThePackage)
{
	InitializeDObjectSystem();
	static const bool bMountInitialized = [] {
		const std::filesystem::path Root = std::filesystem::path(DURIN_TEST_WORK_DIR) / "StaticMeshSlotDetails";
		std::filesystem::remove_all(Root);
		Durin::PathUtilities::RegisterMountPoint("/StaticMeshSlotDetails/", Root.generic_string() + "/");
		return true;
	}();
	(void)bMountInitialized;
	Durin::FAssetPath Path;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/StaticMeshSlotDetails/DirtyComponent", Path));
	Durin::DStaticMeshComponent* Component = nullptr;
	ASSERT_TRUE(Durin::Asset::CreateAsset(Path, Component));
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "DirtySlotMaterial");
	Component->SetStaticMesh(Mesh);
	Component->GetPackage()->ClearDirty();

	Durin::FLevelEditorContext LevelContext;
	Durin::FObjectPropertyViewBuilder Builder("material");
	Durin::CreateStaticMeshComponentDetailsCustomization()->CustomizeDetails(LevelContext, Component, Builder);
	EXPECT_EQ(Builder.GetVisibleRowCount(), 1u);
	for (std::string_view Name : {"MaterialOverrides", "Materials", "Material"})
	{
		Durin::FProperty* Property = Component->GetClass()->FindPropertyByName(Name);
		ASSERT_NE(Property, nullptr);
		EXPECT_TRUE(Builder.IsPropertyHidden(*Property));
	}

	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);
	Durin::FStaticMeshMaterialSlotDetailsModel Model(Component);
	ASSERT_TRUE(Model.AssignMaterial(PropertyView, Context, Model.GetCurrentEntries()[0], Material));
	EXPECT_TRUE(Component->GetPackage()->IsDirty());
	EXPECT_TRUE(Error.empty());

	Transactions.Clear();
	Durin::MarkAsGarbage(Material);
	Durin::MarkAsGarbage(Mesh);
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Path));
	Durin::CollectGarbage();
}
