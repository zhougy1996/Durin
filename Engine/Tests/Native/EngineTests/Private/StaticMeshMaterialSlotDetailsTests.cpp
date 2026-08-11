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
#include "NativeTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMeshTestAccess.h"
#include "Texture/Texture2D.h"
#include "Workspace/LevelEditorContext.h"

#include <gtest/gtest.h>

namespace
{
	auto AddSlot(Durin::DStaticMesh* Mesh, std::string_view Name, Durin::DMaterialInterface* Default = nullptr)
		-> Durin::uint32
	{
		auto* Slots = static_cast<Durin::FArrayProperty*>(Mesh->GetClass()->FindPropertyByName("MaterialSlots"));
		const Durin::uint64 Index = Slots->Num(Mesh);
		Slots->Resize(Mesh, Index + 1);
		auto* Slot = static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, Index));
		Slot->Name = Durin::FName(Name);
		Slot->SourceName = std::string(Name);
		Slot->SourceMaterialIndex = static_cast<Durin::uint32>(Index);
		Slot->DefaultMaterial = Default;
		Durin::FStaticMeshTestAccess::GetMutableRenderData(Mesh)
			->MaterialSlots.push_back(
				{std::string(Name),
					static_cast<Durin::uint32>(Index)});
		return static_cast<Durin::uint32>(Index);
	}

	auto MakeContext(Durin::Editor::FTransactionManager& Transactions, std::string& Error)
		-> Durin::Editor::FPropertyViewContext
	{
		return {.Transactions = &Transactions, .ReportError = [&Error](std::string Message) { Error = std::move(Message); }};
	}
}

TEST(FStaticMeshMaterialSlotDetailsTests, BuildsFixedRowsSourcesAndKeepsDormantOverridesHidden)
{
	InitializeDObjectSystem();
	auto* Default = Durin::NewObject<Durin::DMaterial>(nullptr, "SlotDefault");
	auto* Override = Durin::NewObject<Durin::DMaterial>(nullptr, "SlotOverride");
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* FirstSlot = const_cast<Durin::FStaticMeshMaterialSlotDefinition*>(Mesh->GetMaterialSlot(0));
	FirstSlot->Name = Durin::FName("Body");
	FirstSlot->DefaultMaterial = Default;
	const Durin::uint32 SecondIndex = AddSlot(Mesh, "Glass");
	auto* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "SlotDetailsComponent");
	Component->SetStaticMesh(Mesh);
	ASSERT_TRUE(Component->SetMaterial(SecondIndex, Override));

	Durin::FStaticMeshMaterialSlotDetailsModel Model(Component);
	ASSERT_TRUE(Model.HasMesh());
	ASSERT_EQ(Model.GetCurrentEntries().size(), 2u);
	EXPECT_EQ(Model.GetCurrentEntries()[0].Label, "[0] Body");
	EXPECT_EQ(Model.GetCurrentEntries()[0].Source, Durin::EStaticMeshMaterialSource::MeshDefault);
	EXPECT_EQ(Model.GetCurrentEntries()[0].Material, Default);
	EXPECT_NE(Model.GetCurrentEntries()[0].SearchKeywords.find("Body"), std::string::npos);
	EXPECT_EQ(Model.GetCurrentEntries()[1].Source, Durin::EStaticMeshMaterialSource::ComponentOverride);
	EXPECT_TRUE(Model.GetCurrentEntries()[1].bHasOverride);
	EXPECT_TRUE(Model.HasStoredOverrides());

	auto* OtherMesh = Durin::DStaticMesh::CreateDebugTriangle();
	Component->SetStaticMesh(OtherMesh);
	Durin::FStaticMeshMaterialSlotDetailsModel SmallerMeshModel(Component);
	ASSERT_EQ(SmallerMeshModel.GetCurrentEntries().size(), 1u);
	EXPECT_FALSE(SmallerMeshModel.GetCurrentEntries()[0].bHasOverride);
	EXPECT_EQ(
		SmallerMeshModel.GetCurrentEntries()[0].Source,
		Durin::EStaticMeshMaterialSource::EngineDefault);
	EXPECT_TRUE(SmallerMeshModel.HasStoredOverrides());

	Component->SetStaticMesh(nullptr);
	Durin::FStaticMeshMaterialSlotDetailsModel EmptyModel(Component);
	EXPECT_FALSE(EmptyModel.HasMesh());
	EXPECT_TRUE(EmptyModel.GetCurrentEntries().empty());
	EXPECT_TRUE(EmptyModel.HasStoredOverrides());

	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(OtherMesh);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Override);
	Durin::MarkAsGarbage(Default);
	Durin::CollectGarbage();
}

TEST(FStaticMeshMaterialSlotDetailsTests, FiltersMaterialTypesAndUsesIndexScopedRootTransactions)
{
	InitializeDObjectSystem();
	auto* First = Durin::NewObject<Durin::DMaterial>(nullptr, "SlotFirstMaterial");
	auto* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "SlotSecondMaterial");
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "NotAMaterial");
	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	const Durin::uint32 SecondIndex = AddSlot(Mesh, "Second");
	auto* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "SlotTransactionComponent");
	Component->SetStaticMesh(Mesh);

	EXPECT_TRUE(Durin::FStaticMeshMaterialSlotDetailsModel::IsSupportedMaterialClass(First->GetClass()));
	EXPECT_FALSE(Durin::FStaticMeshMaterialSlotDetailsModel::IsSupportedMaterialClass(Texture->GetClass()));
	Durin::Editor::FTransactionManager Transactions;
	Durin::Editor::FPropertyView PropertyView;
	std::string Error;
	const auto Context = MakeContext(Transactions, Error);

	Durin::FStaticMeshMaterialSlotDetailsModel Initial(Component);
	ASSERT_TRUE(Initial.AssignMaterial(PropertyView, Context, Initial.GetCurrentEntries()[0], First));
	Durin::FStaticMeshMaterialSlotDetailsModel WithFirst(Component);
	ASSERT_TRUE(WithFirst.AssignMaterial(PropertyView, Context, WithFirst.GetCurrentEntries()[1], Second));
	EXPECT_EQ(Component->GetMaterialOverride(0), First);
	EXPECT_EQ(Component->GetMaterialOverride(SecondIndex), Second);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Component->GetMaterialOverride(0), First);
	EXPECT_EQ(Component->GetMaterialOverride(SecondIndex), nullptr);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Component->GetMaterialOverride(SecondIndex), Second);

	Durin::FStaticMeshMaterialSlotDetailsModel Replace(Component);
	ASSERT_TRUE(Replace.AssignMaterial(PropertyView, Context, Replace.GetCurrentEntries()[0], Second, true));
	EXPECT_EQ(Component->GetMaterialOverride(0), Second);
	ASSERT_TRUE(PropertyView.FinishActiveEdit(&Context, true));
	EXPECT_EQ(Component->GetMaterialOverride(0), First);

	Durin::FStaticMeshMaterialSlotDetailsModel Reset(Component);
	ASSERT_TRUE(Reset.ResetOverride(PropertyView, Context, Reset.GetCurrentEntries()[0]));
	EXPECT_EQ(Component->GetMaterialOverride(0), nullptr);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Component->GetMaterialOverride(0), First);

	Component->SetStaticMesh(Durin::DStaticMesh::CreateDebugTriangle());
	Durin::FStaticMeshMaterialSlotDetailsModel SmallerMesh(Component);
	ASSERT_TRUE(SmallerMesh.HasStoredOverrides());
	ASSERT_TRUE(SmallerMesh.ClearOverrides(PropertyView, Context));
	EXPECT_TRUE(Component->GetOverrideMaterials().empty());
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Component->GetMaterialOverride(0), First);
	EXPECT_EQ(Component->GetMaterialOverride(1), Second);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_TRUE(Component->GetOverrideMaterials().empty());
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
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "StaticMeshSlotDetails";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPointForTests("/StaticMeshSlotDetails/", Root.generic_string() + "/");
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
	Durin::FProperty* OverridesProperty = Component->GetClass()->FindPropertyByName("OverrideMaterials");
	ASSERT_NE(OverridesProperty, nullptr);
	EXPECT_TRUE(Builder.IsPropertyHidden(*OverridesProperty));
	EXPECT_EQ(Component->GetClass()->FindPropertyByName("Material"), nullptr);
	EXPECT_EQ(Component->GetClass()->FindPropertyByName("Materials"), nullptr);
	EXPECT_EQ(Component->GetClass()->FindPropertyByName("MaterialOverrides"), nullptr);
	EXPECT_EQ(Component->GetClass()->FindPropertyByName("MaterialOverridesVersion"), nullptr);

	Durin::Editor::FTransactionManager Transactions;
	Durin::Editor::FPropertyView PropertyView;
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
