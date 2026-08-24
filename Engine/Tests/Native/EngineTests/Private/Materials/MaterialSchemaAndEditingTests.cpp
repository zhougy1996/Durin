#include "MaterialTestSupport.h"

#include "DObject/DefaultObjectGraph.h"
#include "DObject/MathStructs.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "StaticMesh/StaticMeshDerivedData.h"

TEST(FMaterialTests, MeshAssetsShareOneReflectedMaterialSlotSchema)
{
	InitializeDObjectSystem();
	const auto GetMaterialSlotStruct = [](Durin::DClass* MeshClass) {
		auto* Slots = static_cast<Durin::FArrayProperty*>(
			MeshClass->FindPropertyByName("MaterialSlots"));
		if (!Slots || !Slots->GetInner()
			|| Slots->GetInner()->GetKind()
				!= Durin::DurinCodeGen::EPropertyGenFlags::Struct) return static_cast<Durin::DStruct*>(nullptr);
		return static_cast<Durin::FStructProperty*>(Slots->GetInner())->GetStruct();
	};

	Durin::DStruct* StaticSlot = GetMaterialSlotStruct(Durin::DStaticMesh::StaticClass());
	Durin::DStruct* SkeletalSlot = GetMaterialSlotStruct(Durin::DSkeletalMesh::StaticClass());
	ASSERT_NE(StaticSlot, nullptr);
	EXPECT_EQ(StaticSlot, SkeletalSlot);
	EXPECT_EQ(StaticSlot, Durin::FMeshMaterialSlotDefinition::StaticStruct());
	EXPECT_EQ(StaticSlot->GetQualifiedName().ToString(), "Durin::FMeshMaterialSlotDefinition");
	EXPECT_EQ(Durin::MaximumMeshMaterialSlots, 4096u);
}

TEST(FMaterialTests, StaticPropertiesHaveStableDefaultsAndInstanceInheritance)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "StaticPropertyBase");
	Durin::DMaterialInstance* Parent = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "StaticPropertyParent");
	Durin::DMaterialInstance* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "StaticPropertyChild");
	const auto* DefaultMaterial = static_cast<const Durin::DMaterial*>(
		Durin::DMaterial::StaticClass()->GetDefaultObject());
	ASSERT_NE(DefaultMaterial, nullptr)
		<< "state=" << static_cast<int>(Durin::DMaterial::StaticClass()->GetDefaultObjectState())
		<< " reason=" << static_cast<int>(Durin::DMaterial::StaticClass()->GetDefaultObjectReason());
	EXPECT_FALSE(DefaultMaterial->GetMaterialRenderProxy());
	EXPECT_TRUE(Durin::GetLoadedMaterialDependents(DefaultMaterial).empty());
	EXPECT_TRUE(Base->GetMaterialRenderProxy());

	const Durin::FMaterialStaticProperties Defaults;
	EXPECT_EQ(Base->GetStaticProperties(), Defaults);
	EXPECT_EQ(Child->GetStaticProperties(), Defaults);
	const Durin::FMaterialRenderData DefaultRenderData = Base->GetRenderData();

	Durin::FMaterialStaticProperties Properties;
	Properties.BlendMode = Durin::EMaterialBlendMode::Masked;
	Properties.ShadingModel = Durin::EMaterialShadingModel::Unlit;
	Properties.bTwoSided = true;
	Properties.DepthWritePolicy = Durin::EMaterialDepthWritePolicy::Enabled;
	Properties.OpacityMaskThreshold = 0.4f;
	ASSERT_TRUE(Base->SetStaticProperties(Properties));
	ASSERT_TRUE(Parent->SetParent(Base));
	ASSERT_TRUE(Child->SetParent(Parent));
	EXPECT_EQ(Parent->GetStaticProperties(), Properties);
	EXPECT_EQ(Child->GetStaticProperties(), Properties);
	const Durin::FMaterialRenderData BaseRenderData = Base->GetRenderData();
	const Durin::FMaterialRenderData ChildRenderData = Child->GetRenderData();
	EXPECT_NE(BaseRenderData.PlanningPassIdentity, DefaultRenderData.PlanningPassIdentity);
	EXPECT_EQ(ChildRenderData.PlanningPassIdentity, BaseRenderData.PlanningPassIdentity);
	EXPECT_EQ(BaseRenderData.PlanningPassIdentity.ShaderMap.BlendMode, Properties.BlendMode);
	EXPECT_EQ(BaseRenderData.PlanningPassIdentity.ShaderMap.ShadingModel, Properties.ShadingModel);
	EXPECT_FLOAT_EQ(
		BaseRenderData.PlanningPassIdentity.ShaderMap.OpacityMaskThreshold,
		Properties.OpacityMaskThreshold);
	EXPECT_EQ(BaseRenderData.PlanningPassIdentity.bTwoSided, Properties.bTwoSided);
	EXPECT_EQ(
		BaseRenderData.PlanningPassIdentity.DepthWritePolicy,
		Properties.DepthWritePolicy);

	Durin::FMaterialStaticProperties Invalid = Properties;
	Invalid.OpacityMaskThreshold = 1.1f;
	const uint64 InitialVersion = Base->GetRenderStateVersion();
	EXPECT_FALSE(Base->SetStaticProperties(Invalid));
	EXPECT_EQ(Base->GetStaticProperties(), Properties);
	EXPECT_EQ(Base->GetRenderStateVersion(), InitialVersion);

	std::string Error;
	EXPECT_FALSE(Durin::ValidateMaterialStaticProperties(Invalid, Error));
	EXPECT_FALSE(Error.empty());

	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Parent);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, RuntimeSchemaHasStableIdentityOrderAndMetadata)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "SchemaMaterial");
	const std::span Definitions = Material->GetParameterDefinitions();
	ASSERT_EQ(Definitions.size(), 56u);
	const std::array ConstantIds{Durin::MaterialParameters::BaseColorId, Durin::MaterialParameters::NormalId,
		Durin::MaterialParameters::MetallicId, Durin::MaterialParameters::RoughnessId,
		Durin::MaterialParameters::AmbientOcclusionId, Durin::MaterialParameters::EmissiveId,
		Durin::MaterialParameters::OpacityId, Durin::MaterialParameters::OpacityMaskId};
	std::vector<Durin::FGuid> ExpectedIds;
	for (size_t Role = 0; Role < 8; ++Role)
	{
		ExpectedIds.insert(ExpectedIds.end(), {ConstantIds[Role], Durin::MaterialParameters::TextureIds[Role],
			Durin::MaterialParameters::UVChannelIds[Role], Durin::MaterialParameters::UVScaleIds[Role],
			Durin::MaterialParameters::UVOffsetIds[Role], Durin::MaterialParameters::UVRotationIds[Role],
			Durin::MaterialParameters::SamplerStateIds[Role]});
	}
	std::unordered_set<Durin::FGuid> Ids;
	std::unordered_set<Durin::FName> Names;
	for (size_t Index = 0; Index < Definitions.size(); ++Index)
	{
		const Durin::FMaterialParameterDefinition& Definition = Definitions[Index];
		EXPECT_EQ(Definition.Id, ExpectedIds[Index]);
		EXPECT_TRUE(Ids.insert(Definition.Id).second);
		EXPECT_TRUE(Names.insert(Definition.Name).second);
		EXPECT_FALSE(Definition.Name.IsNone());
		EXPECT_FALSE(Definition.DisplayName.empty());
		EXPECT_EQ(Definition.SortOrder, static_cast<int32>(Index));
		if (Index % 7 == 3 || Index % 7 == 4)
		{
			EXPECT_EQ(Definition.Type, Durin::EMaterialParameterType::Vector2);
		}
		switch (Definition.Presentation)
		{
		case Durin::EMaterialParameterPresentation::Drag:
			EXPECT_TRUE(Definition.bHasRange);
			EXPECT_LT(Definition.MinimumValue, Definition.MaximumValue);
			break;
		case Durin::EMaterialParameterPresentation::Integer:
			EXPECT_EQ(Definition.Type, Durin::EMaterialParameterType::Scalar);
			EXPECT_FLOAT_EQ(Definition.MinimumValue, 0.0f);
			EXPECT_FLOAT_EQ(
				Definition.MaximumValue,
				std::ranges::find(Durin::MaterialParameters::UVChannelIds, Definition.Id)
					!= Durin::MaterialParameters::UVChannelIds.end()
					? 3.0f : 255.0f);
			break;
		case Durin::EMaterialParameterPresentation::Color:
			EXPECT_EQ(Definition.Type, Durin::EMaterialParameterType::Vector);
			break;
		case Durin::EMaterialParameterPresentation::AssetPicker:
			EXPECT_EQ(Definition.Type, Durin::EMaterialParameterType::Texture);
			break;
		case Durin::EMaterialParameterPresentation::Default: FAIL() << "Built-in parameters require an explicit presentation."; break;
		}
	}
	EXPECT_EQ(Material->FindParameterDefinition(Durin::MaterialParameters::OpacityId), &Definitions[42]);
	EXPECT_EQ(Material->FindParameterDefinition(Durin::FName("oPaCiTy")), &Definitions[42]);
	EXPECT_FALSE(Material->SetScalarParameterValue(Durin::MaterialParameters::BaseColorName(), 0.5f));
	EXPECT_FALSE(Material->SetVectorParameterValue(
		Durin::FName("BaseColorUVScale"), Durin::FVector3(1.0)));
	EXPECT_TRUE(Material->SetVector2ParameterValue(
		Durin::FName("BaseColorUVScale"), Durin::FVector2(2.0, 3.0)));
	EXPECT_FALSE(Material->SetScalarParameterValue(Durin::FName("UnknownParameter"), 0.5f));
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, RuntimeSchemaValidationReportsSpecificCorruption)
{
	InitializeDObjectSystem();
	std::vector Definitions = Durin::MakeCanonicalMaterialParameterDefinitions();
	std::string Error;
	EXPECT_TRUE(Durin::ValidateCanonicalMaterialParameterDefinitions(Definitions, Error));
	EXPECT_TRUE(Error.empty());

	Definitions[1].Id = Definitions[0].Id;
	EXPECT_FALSE(Durin::ValidateCanonicalMaterialParameterDefinitions(Definitions, Error));
	EXPECT_NE(Error.find("duplicate GUID"), std::string::npos);

	Definitions = Durin::MakeCanonicalMaterialParameterDefinitions();
	Definitions[2].Name = Durin::FName("RenamedOpacity");
	EXPECT_FALSE(Durin::ValidateCanonicalMaterialParameterDefinitions(Definitions, Error));
	EXPECT_NE(Error.find("canonical identity"), std::string::npos);

	Definitions = Durin::MakeCanonicalMaterialParameterDefinitions();
	std::swap(Definitions[0], Definitions[1]);
	EXPECT_FALSE(Durin::ValidateCanonicalMaterialParameterDefinitions(Definitions, Error));
	EXPECT_NE(Error.find("canonical identity"), std::string::npos);

	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "CorruptedSchemaMaterial");
	auto* Property = static_cast<Durin::FArrayProperty*>(Material->GetClass()->FindPropertyByName("ParameterDefinitions"));
	ASSERT_NE(Property, nullptr);
	auto* Opacity = static_cast<Durin::FMaterialParameterDefinition*>(Property->GetMutableElementPtr(Material, 2));
	Opacity->Type = Durin::EMaterialParameterType::Vector;
	EXPECT_FALSE(Material->PostLoad(Error));
	EXPECT_NE(Error.find("canonical identity"), std::string::npos);
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, ReflectedPositionalMaterialOverrideUsesSharedTransactions)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* First = Durin::NewObject<Durin::DMaterial>(nullptr, "FirstDetailsMaterial");
	Durin::DMaterial* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "SecondDetailsMaterial");
	First->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3));
	Second->SetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.7, 0.6, 0.5));
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Harness.CreateStaticMeshComponent("DetailsMeshComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(First);
	Component->RegisterComponent();
	const FSceneSnapshot Before = CaptureScene(Harness.Scene);

	Durin::FProperty* OverridesProperty = Component->GetClass()->FindPropertyByName("OverrideMaterials");
	ASSERT_NE(OverridesProperty, nullptr);
	Durin::FPropertyValueSnapshot Original;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(OverridesProperty, Component, 0, Original));
	ASSERT_TRUE(Component->SetMaterial(0, Second));
	ASSERT_TRUE(Durin::CapturePropertyValue(OverridesProperty, Component, 0, Proposed));
	ASSERT_TRUE(Durin::RestorePropertyValue(OverridesProperty, Component, 0, Original));
	Durin::Editor::FTransactionManager Transactions;
	Durin::Editor::FPropertyEditSession EditSession;
	ASSERT_TRUE(EditSession.Begin(
		Durin::Editor::FPropertyEditTarget::ForMember(Component, OverridesProperty),
		"Edit Material Override",
		nullptr,
		&Transactions
	));
	EXPECT_EQ(EditSession.Apply(Proposed), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(EditSession.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	const FSceneSnapshot After = CaptureScene(Harness.Scene);

	EXPECT_GT(After.ComponentRevision, Before.ComponentRevision);
	ExpectColorNear(GetMaterialBinding(After.Material).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));
	ASSERT_TRUE(Transactions.Undo());
	const FSceneSnapshot Undone = CaptureScene(Harness.Scene);
	EXPECT_GT(Undone.ComponentRevision, After.ComponentRevision);
	ExpectColorNear(GetMaterialBinding(Undone.Material).BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));
	ASSERT_TRUE(Transactions.Redo());
	const FSceneSnapshot Redone = CaptureScene(Harness.Scene);
	EXPECT_GT(Redone.ComponentRevision, Undone.ComponentRevision);
	ExpectColorNear(GetMaterialBinding(Redone.Material).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));
	Transactions.Clear();

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, PositionalMaterialOverridesResolveDefaultsAndSurviveMeshSwitches)
{
	InitializeDObjectSystem();
	Durin::DMaterial* First = Durin::NewObject<Durin::DMaterial>(nullptr, "FirstReflectedSlotMaterial");
	Durin::DMaterial* Second = Durin::NewObject<Durin::DMaterial>(nullptr, "SecondReflectedSlotMaterial");
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle(nullptr);
	AddDebugMaterialSlot(Mesh, "Second");
	auto* Slots = static_cast<Durin::FArrayProperty*>(Mesh->GetClass()->FindPropertyByName("MaterialSlots"));
	static_cast<Durin::FMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, 0))->DefaultMaterial = First;
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "ReflectedSlotMeshComponent");
	Component->SetStaticMesh(Mesh);

	EXPECT_EQ(Component->GetNumMaterials(), 2u);
	EXPECT_EQ(Component->GetMaterial(0), First);
	EXPECT_EQ(Component->GetMaterial(1), nullptr);
	EXPECT_FALSE(Component->SetMaterial(9, Second));
	EXPECT_TRUE(Component->GetOverrideMaterials().empty());
	EXPECT_FALSE(Component->SetMaterialByName(Durin::FName("Missing"), Second));
	ASSERT_TRUE(Component->SetMaterialByName(Durin::FName("Second"), Second));
	ASSERT_EQ(Component->GetOverrideMaterials().size(), 2u);
	EXPECT_EQ(Component->GetMaterialOverride(0), nullptr);
	EXPECT_EQ(Component->GetMaterialOverride(1), Second);
	EXPECT_EQ(Component->GetMaterial(1), Second);
	EXPECT_EQ(Component->GetMaterialByName(Durin::FName("Second")), Second);
	EXPECT_EQ(Component->GetMaterialByName(Durin::FName("Missing")), nullptr);
	EXPECT_TRUE(Component->SetMaterial(1, nullptr));
	EXPECT_FALSE(Component->HasMaterialOverride(1));
	EXPECT_TRUE(Component->GetOverrideMaterials().empty());

	ASSERT_TRUE(Component->SetMaterial(0, Second));
	Durin::DStaticMesh* OtherMesh = Durin::DStaticMesh::CreateDebugTriangle(nullptr);
	Component->SetStaticMesh(OtherMesh);
	EXPECT_EQ(Component->GetMaterial(0), Second);
	Component->SetStaticMesh(nullptr);
	EXPECT_EQ(Component->GetNumMaterials(), 0u);
	EXPECT_EQ(Component->GetMaterial(0), nullptr);
	Component->SetStaticMesh(Mesh);
	EXPECT_EQ(Component->GetMaterial(0), Second);
	EXPECT_TRUE(Component->ResetMaterial(0));
	EXPECT_EQ(Component->GetMaterial(0), First);
	ASSERT_TRUE(Component->SetMaterial(1, Second));
	Component->SetStaticMesh(OtherMesh);
	EXPECT_EQ(Component->GetNumMaterials(), 1u);
	EXPECT_EQ(Component->GetMaterialOverride(1), Second);
	EXPECT_EQ(Component->GetMaterial(0), nullptr);
	Component->SetStaticMesh(nullptr);
	EXPECT_EQ(Component->GetMaterialOverride(1), Second);
	Component->SetStaticMesh(Mesh);
	EXPECT_EQ(Component->GetMaterial(1), Second);
	std::string DuplicateError;
	auto* Duplicate = Durin::Cast<Durin::DStaticMeshComponent>(
		Durin::DuplicateObjectGraph(Component, nullptr, "SparseOverrideDuplicate", &DuplicateError));
	ASSERT_NE(Duplicate, nullptr) << DuplicateError;
	EXPECT_EQ(Duplicate->GetStaticMesh(), Mesh);
	EXPECT_EQ(Duplicate->GetMaterial(1), Second);
	EXPECT_EQ(Duplicate->GetOverrideMaterials().size(), 2u);
	EXPECT_TRUE(Component->ClearMaterialOverrides());
	EXPECT_TRUE(Component->GetOverrideMaterials().empty());
	EXPECT_FALSE(Component->ClearMaterialOverrides());

	Durin::MarkAsGarbage(Duplicate);
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(OtherMesh);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, StaticMeshComponentValidatesPositionalOverrides)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "CorruptOverrideMaterial");
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	Durin::DStaticMeshComponent* Component = Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "CorruptOverrideComponent");
	Component->SetStaticMesh(Mesh);
	ASSERT_TRUE(Component->SetMaterial(0, Material));
	auto* Overrides = static_cast<Durin::FArrayProperty*>(Component->GetClass()->FindPropertyByName("OverrideMaterials"));
	ASSERT_NE(Overrides, nullptr);
	std::string Error;

	auto* Inner = static_cast<Durin::FObjectProperty*>(Overrides->GetInner());
	ASSERT_NE(Inner, nullptr);
	Durin::FPropertyValueSnapshot Original;
	Durin::FPropertyValueSnapshot InvalidProposal;
	ASSERT_TRUE(Durin::CapturePropertyValue(Overrides, Component, 0, Original));
	Inner->SetObjectPropertyValue(Overrides->GetMutableElementPtr(Component, 0), Mesh);
	ASSERT_TRUE(Durin::CapturePropertyValue(Overrides, Component, 0, InvalidProposal));
	ASSERT_TRUE(Durin::RestorePropertyValue(Overrides, Component, 0, Original));
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Durin::Editor::FPropertyEditTarget::ForMember(Component, Overrides), "Corrupt Override"));
	EXPECT_EQ(Session.Apply(InvalidProposal, &Error), Durin::Editor::EPropertyEditResult::Failed);
	EXPECT_NE(Error.find("incompatible object at material index 0"), std::string::npos);
	EXPECT_EQ(Component->GetMaterialOverride(0), Material);
	EXPECT_EQ(Session.Cancel(), Durin::Editor::EPropertyEditResult::NoChange);

	Inner->SetObjectPropertyValue(Overrides->GetMutableElementPtr(Component, 0), Mesh);
	EXPECT_FALSE(Component->PostLoad(Error));
	EXPECT_NE(Error.find("incompatible object at material index 0"), std::string::npos);
	Inner->SetObjectPropertyValue(Overrides->GetMutableElementPtr(Component, 0), Material);
	Overrides->Resize(Component, Durin::MaximumMeshMaterialSlots + 1ull);
	EXPECT_FALSE(Component->PostLoad(Error));
	EXPECT_NE(Error.find("exceeding the limit"), std::string::npos);
	Overrides->Resize(Component, 2);
	Inner->SetObjectPropertyValue(Overrides->GetMutableElementPtr(Component, 0), Material);
	Inner->SetObjectPropertyValue(Overrides->GetMutableElementPtr(Component, 1), nullptr);
	EXPECT_TRUE(Component->PostLoad(Error));
	EXPECT_EQ(Component->GetOverrideMaterials().size(), 1u);

	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, ReflectedParameterEditCoalescesAndInvalidatesRenderDataAcrossUndoRedo)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "TransactionalMaterial");
	const auto Target = MakeMaterialValueTarget(Material, Durin::MaterialParameters::OpacityId, Durin::FName("ScalarValue"));
	ASSERT_TRUE(Target.has_value());
	Durin::Editor::FTransactionManager Transactions;
	Durin::Editor::FPropertyView PropertyView;
	std::string Error;
	Durin::Editor::FPropertyViewContext Context{
		.Transactions = &Transactions,
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
	};
	const uint64 BeforeVersion = Material->GetRenderStateVersion();
	EXPECT_TRUE(PropertyView.SubmitPropertyValueEdit(Context, *Target,
		[](Durin::FProperty* ValueProperty, void* Container, uint32 ArrayIndex) {
			*ValueProperty->ContainerPtrToValuePtr<float>(Container, ArrayIndex) = 0.6f;
		}, true));
	EXPECT_TRUE(PropertyView.SubmitPropertyValueEdit(Context, *Target,
		[](Durin::FProperty* ValueProperty, void* Container, uint32 ArrayIndex) {
			*ValueProperty->ContainerPtrToValuePtr<float>(Container, ArrayIndex) = 0.4f;
		}, true));
	EXPECT_GT(Material->GetRenderStateVersion(), BeforeVersion);
	PropertyView.FinishActiveEdit(&Context, false);
	EXPECT_TRUE(Error.empty());
	float Opacity = 0.0f;
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 0.4f);
	ASSERT_TRUE(Transactions.Undo());
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 1.0f);
	ASSERT_TRUE(Transactions.Redo());
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 0.4f);
	Transactions.Clear();
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, ReflectedPropertyViewTracksPresentedOwnerSeparatelyFromEditTarget)
{
	InitializeDObjectSystem();
	Durin::DMaterialInstance* Owner = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PropertyViewOwner");
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "PropertyViewTarget");
	const auto Target = MakeMaterialValueTarget(Material, Durin::MaterialParameters::OpacityId, Durin::FName("ScalarValue"));
	ASSERT_TRUE(Target.has_value());
	Durin::Editor::FTransactionManager Transactions;
	Durin::Editor::FPropertyView PropertyView;
	std::string Error;
	Durin::Editor::FPropertyViewContext Context{
		.Transactions = &Transactions,
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
	};
	PropertyView.HandleOwnerContext(Context, Owner);
	EXPECT_TRUE(PropertyView.SubmitPropertyValueEdit(Context, *Target,
		[](Durin::FProperty* ValueProperty, void* Container, uint32 ArrayIndex) {
			*ValueProperty->ContainerPtrToValuePtr<float>(Container, ArrayIndex) = 0.5f;
		}, true));
	EXPECT_TRUE(PropertyView.IsEditingObject(Material));
	PropertyView.HandleOwnerContext(Context, Owner);
	EXPECT_TRUE(PropertyView.IsEditing());

	PropertyView.HandleOwnerContext(Context, Material);
	EXPECT_FALSE(PropertyView.IsEditing());
	EXPECT_TRUE(Error.empty());
	float Opacity = 0.0f;
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 1.0f);

	Transactions.Clear();
	Durin::MarkAsGarbage(Material);
	Durin::MarkAsGarbage(Owner);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, ReflectedPropertyViewTracksMaterialOverrideStructureInSharedHistory)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "TransactionalOverrideBase");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "TransactionalOverrideInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	auto* Property = static_cast<Durin::FArrayProperty*>(Instance->GetClass()->FindPropertyByName("ParameterOverrides"));
	ASSERT_NE(Property, nullptr);
	Durin::FPropertyValueSnapshot Original;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Instance, 0, Original));
	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.5f));
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Instance, 0, Proposed));
	ASSERT_TRUE(Instance->ClearScalarParameterValue(Durin::MaterialParameters::OpacityName()));
	Durin::Editor::FTransactionManager Transactions;
	std::string Error;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(
		Durin::Editor::FPropertyEditTarget::ForMember(Instance, Property),
		"Edit Parameter Override", nullptr, &Transactions));
	EXPECT_EQ(Session.Apply(Proposed, &Error), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_TRUE(Instance->HasScalarParameterOverride(Durin::MaterialParameters::OpacityName()));
	EXPECT_TRUE(Error.empty());
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_FALSE(Instance->HasScalarParameterOverride(Durin::MaterialParameters::OpacityName()));
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_TRUE(Instance->HasScalarParameterOverride(Durin::MaterialParameters::OpacityName()));
	Transactions.Clear();
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, UnknownAndMismatchedSettersDoNotInvalidateRenderState)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "RejectedSetterMaterial");
	const uint64 Version = Material->GetRenderStateVersion();
	EXPECT_FALSE(Material->SetScalarParameterValue(Durin::FName("UnknownParameter"), 0.25f));
	EXPECT_FALSE(Material->SetScalarParameterValue(Durin::MaterialParameters::BaseColorName(), 0.25f));
	EXPECT_EQ(Material->GetRenderStateVersion(), Version);
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, ParentHookRejectsCyclesWithoutCreatingHistory)
{
	InitializeDObjectSystem();
	Durin::DMaterialInstance* First = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CycleFirst");
	Durin::DMaterialInstance* Second = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CycleSecond");
	ASSERT_TRUE(First->SetParent(Second));
	Durin::FProperty* ParentProperty = Second->GetClass()->FindPropertyByName("Parent");
	ASSERT_NE(ParentProperty, nullptr);

	Durin::FPropertyValueSnapshot Original;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(ParentProperty, Second, 0, Original));
	static_cast<Durin::FObjectProperty*>(ParentProperty)->SetObjectPropertyValue(Second, First);
	ASSERT_TRUE(Durin::CapturePropertyValue(ParentProperty, Second, 0, Proposed));
	ASSERT_TRUE(Durin::RestorePropertyValue(ParentProperty, Second, 0, Original));

	Durin::Editor::FTransactionManager Transactions;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Durin::Editor::FPropertyEditTarget::ForMember(Second, ParentProperty), "Edit Parent", nullptr, &Transactions));
	std::string Error;
	EXPECT_EQ(Session.Apply(Proposed, &Error), Durin::Editor::EPropertyEditResult::Failed);
	EXPECT_EQ(Error, "A material instance cannot create a parent cycle.");
	EXPECT_EQ(Second->GetParent(), nullptr);
	EXPECT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::NoChange);
	EXPECT_FALSE(Transactions.CanUndo());

	First->SetParent(nullptr);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, ParentTransactionsRenderFromCurrentCanonicalStorage)
{
	FRenderSceneHarness Harness;
	auto* FirstParent = Durin::NewObject<Durin::DMaterial>(nullptr, "CanonicalFirstParent");
	auto* SecondParent = Durin::NewObject<Durin::DMaterial>(nullptr, "CanonicalSecondParent");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CanonicalParentInstance");
	FirstParent->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.1, 0.2, 0.3));
	SecondParent->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.7, 0.6, 0.5));
	ASSERT_TRUE(Instance->SetParent(FirstParent));

	auto* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	auto* Component = Harness.CreateStaticMeshComponent("CanonicalParentComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(Instance);
	Component->RegisterComponent();
	const FSceneSnapshot Initial = CaptureScene(Harness.Scene);
	ExpectColorNear(GetMaterialBinding(Initial.Material).BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));

	Durin::FProperty* ParentProperty = Instance->GetClass()->FindPropertyByName("Parent");
	ASSERT_NE(ParentProperty, nullptr);
	Durin::FPropertyValueSnapshot Original;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(ParentProperty, Instance, 0, Original));
	static_cast<Durin::FObjectProperty*>(ParentProperty)->SetObjectPropertyValue(Instance, SecondParent);
	ASSERT_TRUE(Durin::CapturePropertyValue(ParentProperty, Instance, 0, Proposed));
	ASSERT_TRUE(Durin::RestorePropertyValue(ParentProperty, Instance, 0, Original));

	Durin::Editor::FTransactionManager Transactions;
	Durin::Editor::FPropertyEditSession CancelledSession;
	ASSERT_TRUE(CancelledSession.Begin(
		Durin::Editor::FPropertyEditTarget::ForMember(Instance, ParentProperty),
		"Edit Parent", nullptr, &Transactions));
	ASSERT_EQ(CancelledSession.Apply(Proposed), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Instance->GetParent(), SecondParent);
	const FSceneSnapshot Interactive = CaptureScene(Harness.Scene);
	ExpectColorNear(GetMaterialBinding(Interactive.Material).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));
	ASSERT_EQ(CancelledSession.Cancel(), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Instance->GetParent(), FirstParent);
	const FSceneSnapshot Cancelled = CaptureScene(Harness.Scene);
	ExpectColorNear(GetMaterialBinding(Cancelled.Material).BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));

	Durin::Editor::FPropertyEditSession CommittedSession;
	ASSERT_TRUE(CommittedSession.Begin(
		Durin::Editor::FPropertyEditTarget::ForMember(Instance, ParentProperty),
		"Edit Parent", nullptr, &Transactions));
	ASSERT_EQ(CommittedSession.Apply(Proposed), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_EQ(CommittedSession.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Instance->GetParent(), SecondParent);
	const FSceneSnapshot Committed = CaptureScene(Harness.Scene);
	ExpectColorNear(GetMaterialBinding(Committed.Material).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));

	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Instance->GetParent(), FirstParent);
	const FSceneSnapshot Undone = CaptureScene(Harness.Scene);
	ExpectColorNear(GetMaterialBinding(Undone.Material).BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Instance->GetParent(), SecondParent);
	const FSceneSnapshot Redone = CaptureScene(Harness.Scene);
	ExpectColorNear(GetMaterialBinding(Redone.Material).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));

	FirstParent->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.9, 0.1, 0.2));
	const FSceneSnapshot PreviousParentChanged = CaptureScene(Harness.Scene);
	EXPECT_EQ(PreviousParentChanged.ComponentRevision, Redone.ComponentRevision);
	ExpectColorNear(GetMaterialBinding(PreviousParentChanged.Material).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));
	SecondParent->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.8, 0.4));
	const FSceneSnapshot CurrentParentChanged = CaptureScene(Harness.Scene);
	EXPECT_EQ(CurrentParentChanged.ComponentRevision, PreviousParentChanged.ComponentRevision);
	ExpectColorNear(GetMaterialBinding(CurrentParentChanged.Material).BaseColor, Durin::FVector4f(0.2f, 0.8f, 0.4f, 1.0f));
	Transactions.Clear();

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(SecondParent);
	Durin::MarkAsGarbage(FirstParent);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialTests, ProductionClassDefaultsMatchFreshOrdinaryObjectGraphs)
{
	InitializeDObjectSystem();
	(void)Durin::Z_Construct_DStruct_Durin_FVector4();
	uint32 ProductionStructCount = 0;
	for (Durin::DObject* Object : Durin::GDObjectArray.GetAll(Durin::EObjectQueryScope::IncludeTemplates))
	{
		auto* Struct = Durin::Cast<Durin::DStruct>(Object);
		if (!Struct || !Struct->GetQualifiedName().ToString().starts_with("Durin::")) continue;
		++ProductionStructCount;
		EXPECT_EQ(Struct->GetDefaultState(), Durin::EDStructDefaultState::Ready)
			<< Struct->GetQualifiedName().ToString()
			<< " reason=" << static_cast<int>(Struct->GetDefaultReason());
		EXPECT_NE(Struct->GetDefaultValue(), nullptr) << Struct->GetQualifiedName().ToString();
	}
	EXPECT_GT(ProductionStructCount, 0u);

	std::vector<Durin::DClass*> Classes;
	uint32 ProductionClassCount = 0;
	for (Durin::DObject* Object : Durin::GDObjectArray.GetAll(Durin::EObjectQueryScope::IncludeTemplates))
	{
		auto* Class = Durin::Cast<Durin::DClass>(Object);
		if (!Class) continue;
		const std::string QualifiedName = Class->GetQualifiedName().ToString();
		if (!QualifiedName.starts_with("Durin::")) continue;
		++ProductionClassCount;
		EXPECT_NE(Class->GetDefaultObjectState(), Durin::EClassDefaultObjectState::Uninitialized)
			<< QualifiedName;
		EXPECT_NE(Class->GetDefaultObjectState(), Durin::EClassDefaultObjectState::Constructing)
			<< QualifiedName;
		EXPECT_NE(Class->GetDefaultObjectState(), Durin::EClassDefaultObjectState::Failed)
			<< QualifiedName << " reason=" << static_cast<int>(Class->GetDefaultObjectReason());
		if (Class->GetDefaultObjectState() == Durin::EClassDefaultObjectState::Ready) Classes.push_back(Class);
	}
	EXPECT_GT(ProductionClassCount, 0u);
	EXPECT_FALSE(Classes.empty());
	std::ranges::sort(Classes, [](const Durin::DClass* Left, const Durin::DClass* Right) {
		return Left->GetQualifiedName().ToString() > Right->GetQualifiedName().ToString();
	});

	for (Durin::DClass* Class : Classes)
	{
		const Durin::DObject* DefaultObject = Class->GetDefaultObject();
		ASSERT_NE(DefaultObject, nullptr) << Class->GetQualifiedName().ToString();
		Durin::DObject* Instance = Durin::NewObject(
			Class,
			nullptr,
			Durin::FName(std::format("Parity_{}", Class->GetShortName())));
		ASSERT_NE(Instance, nullptr) << Class->GetQualifiedName().ToString();

		Durin::FDefaultObjectGraphMap DefaultGraph;
		Durin::FDefaultObjectGraphDiagnostic GraphDiagnostic;
		ASSERT_TRUE(DefaultGraph.Build(DefaultObject, Instance, &GraphDiagnostic))
			<< Class->GetQualifiedName().ToString()
			<< " graph_reason=" << static_cast<int>(GraphDiagnostic.Reason)
			<< " path=" << GraphDiagnostic.LogicalPath;
		std::vector<const Durin::DObject*> Templates{DefaultObject};
		for (size_t TemplateIndex = 0; TemplateIndex < Templates.size(); ++TemplateIndex)
		{
			const Durin::DObject* Template = Templates[TemplateIndex];
			const Durin::DObject* Live = DefaultGraph.FindInstance(Template);
			ASSERT_NE(Live, nullptr);
			for (Durin::DObject* Child : Durin::GDObjectArray.GetObjectsWithOuter(
					 Template, Durin::EObjectQueryScope::IncludeTemplates))
			{
				Templates.push_back(Child);
			}
			Template->GetClass()->ForEachProperty([&](Durin::FProperty* Property) {
				if (Property->HasAnyPropertyFlags(Durin::EPropertyFlags::Transient)
					|| Property->NamePrivate == Durin::FName("SkyBoxSceneId")
					|| Property->NamePrivate == Durin::FName("VolumetricCloudSceneId")) return;
				for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
				{
					Durin::FPropertyIdentityDiagnostic IdentityDiagnostic;
					EXPECT_EQ(
						Durin::ComparePropertyValuesWithDefaultGraph(
							Property, Template, Index, Live, Index, DefaultGraph, &IdentityDiagnostic),
						Durin::EPropertyIdentityResult::Identical)
						<< Class->GetQualifiedName().ToString() << ":"
						<< IdentityDiagnostic.PropertyPath << " reason="
						<< static_cast<int>(IdentityDiagnostic.Reason);
				}
			}, true);
		}

		Durin::MarkObjectHierarchyAsGarbage(Instance);
	}
	Durin::CollectGarbage();
}
