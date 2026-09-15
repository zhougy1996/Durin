#include "MaterialProgramTestFixture.h"

TEST(FMaterialPropertyEditingTests, OwnedParametersShareIdentityAndRejectConflictingNamesAtomically)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "OwnedParameters"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialExpressionScalarParameter> Owner(NewObject<DMaterialExpressionScalarParameter>(nullptr, NAME_None));
	Owner->Id = FGuid::NewGuid(); Owner->Metadata.Id = FGuid::NewGuid();
	Owner->Metadata.Name = "RustAmount"; Owner->DefaultValue = .25f;
	const std::array<DMaterialExpression*, 1> Original{Owner.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(Original, {}));
	ASSERT_EQ(Material->GetParameterDefinitions().size(), 1u);
	const auto Revision = Material->GetParameterDefinitionSchemaRevision();
	TStrongObjectPtr<DMaterialExpressionScalarParameter> Duplicate(Cast<DMaterialExpressionScalarParameter>(DuplicateObject(Owner.Get(), nullptr, NAME_None)));
	Duplicate->Id = FGuid::NewGuid();
	const std::array<DMaterialExpression*, 2> Duplicated{Owner.Get(), Duplicate.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(Duplicated, {}));
	Duplicate->Metadata.Id = FGuid::NewGuid(); Duplicate->Metadata.Name = "rustamount";
	EXPECT_FALSE(Material->SetMaterialExpressions(Duplicated, {}));
	EXPECT_EQ(Material->GetParameterDefinitionSchemaRevision(), Revision);
	ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 2u);
	Owner->Metadata.Name = "Weathering";
	ASSERT_TRUE(Material->SetMaterialExpressions(Original, {}));
	EXPECT_EQ(Material->FindParameterDefinition("Weathering")->Id, Owner->Metadata.Id);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.front()->Id, Owner->Id);
	EXPECT_TRUE(Material->SetParameterValue(Owner->Metadata.Id, FMaterialParameterValue::MakeScalar(.75f)));
	EXPECT_EQ(Cast<DMaterialExpressionScalarParameter>(Material->GetExpressionCollection().Expressions.front().Get())->DefaultValue, .75f);
	ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
}

TEST(FMaterialPropertyEditingTests, CustomDeclarationOverridesRetainOrphansAndRejectRetyping)
{
	InitializeDObjectSystem();
	auto* Root = Durin::NewObject<Durin::DMaterial>(nullptr, "CustomOverrideRoot");
	auto* Parent = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CustomOverrideParent");
	auto* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CustomOverrideChild");
	Durin::FMaterialParameterDefinition Definition;
	Definition.Id = Durin::FGuid::NewGuid();
	Definition.Name = "RustAmount";
	Definition.Value.GetScalar() = 0.25f;
	Durin::TStrongObjectPtr<Durin::DMaterialExpressionScalarParameter> Parameter(Durin::NewObject<Durin::DMaterialExpressionScalarParameter>(nullptr, Durin::NAME_None));
	Parameter->Id = Durin::FGuid::NewGuid(); Parameter->Metadata.Id = Definition.Id;
	Parameter->Metadata.Name = Definition.Name; Parameter->DefaultValue = .25f;
	const std::array<Durin::DMaterialExpression*, 1> Original{Parameter.Get()};
	const Durin::FMaterialExpressionSurfaceOutputs Outputs{.Roughness = {Parameter->Id}};
	ASSERT_TRUE(Root->SetMaterialExpressions(Original, Outputs));
	ASSERT_TRUE(Parent->SetParent(Root));
	ASSERT_TRUE(Child->SetParent(Parent));
	ASSERT_TRUE(Parent->SetParameterValue(Definition.Id, Durin::FMaterialParameterValue::MakeScalar(0.75f)));
	float Value = 0;
	ASSERT_TRUE(Child->GetScalarParameterValue(Definition.Name, Value));
	EXPECT_EQ(Value, 0.75f);
	Durin::TStrongObjectPtr<Durin::DMaterialExpressionVector3Parameter> Retyped(Durin::NewObject<Durin::DMaterialExpressionVector3Parameter>(nullptr, Durin::NAME_None));
	Retyped->Id = Parameter->Id; Retyped->Metadata = Parameter->Metadata; Retyped->DefaultValue = {.2, .3, .4};
	const std::array<Durin::DMaterialExpression*, 1> Replacement{Retyped.Get()};
	const auto BeforeChildren = Root->GetExpressionCollection().Expressions;
	// A vector owner cannot feed the scalar roughness output.
	EXPECT_FALSE(Root->SetMaterialExpressions(Replacement, Outputs));
	EXPECT_EQ(Root->GetExpressionCollection().Expressions, BeforeChildren);
	EXPECT_EQ(Root->GetExpressionOutputs(), Outputs);
	ASSERT_TRUE(Root->SetMaterialExpressions(Replacement, {}));
	EXPECT_TRUE(Parent->IsParameterValueOrphan(Definition.Id));
	Durin::FVector3 Vector;
	ASSERT_TRUE(Child->GetVectorParameterValue(Definition.Name, Vector));
	EXPECT_EQ(Vector, Durin::FVector3(.2, .3, .4));
	ASSERT_TRUE(Root->SetMaterialExpressions({}, {}));
	EXPECT_TRUE(Parent->IsParameterValueOrphan(Definition.Id));
	EXPECT_EQ(Parent->GetLocalParameterValueCount(), 1u);
	ASSERT_TRUE(Root->SetMaterialExpressions(Original, Outputs));
	EXPECT_FALSE(Parent->IsParameterValueOrphan(Definition.Id));
	ASSERT_TRUE(Child->GetScalarParameterValue(Definition.Name, Value));
	EXPECT_EQ(Value, 0.75f);
	auto* OverridesProperty = Parent->GetClass()->FindPropertyByName("ScalarParameterValues");
	ASSERT_NE(OverridesProperty, nullptr);
	auto* Overrides = OverridesProperty->ContainerPtrToValuePtr<
		std::vector<Durin::FMaterialScalarParameterValue>>(Parent);
	Overrides->clear();
	auto* VectorProperty = Parent->GetClass()->FindPropertyByName("VectorParameterValues");
	ASSERT_NE(VectorProperty, nullptr);
	VectorProperty->ContainerPtrToValuePtr<std::vector<Durin::FMaterialVectorParameterValue>>(Parent)
		->push_back({Definition.Id, Durin::FVector4f(.2f, .3f, .4f, 0.0f), Durin::EMaterialParameterType::Vector});
	Parent->PostLoad();
	EXPECT_EQ(Parent->GetLocalParameterValueCount(), 1u);
	EXPECT_TRUE(Parent->IsParameterValueOrphan(Definition.Id));
	ASSERT_TRUE(Child->GetScalarParameterValue(Definition.Name, Value));
	EXPECT_EQ(Value, 0.25f);
	ASSERT_TRUE(Parent->ClearParameterValue(Definition.Id));
	ASSERT_TRUE(Child->GetScalarParameterValue(Definition.Name, Value));
	EXPECT_EQ(Value, 0.25f);
	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Parent);
	Durin::MarkAsGarbage(Root);
}

TEST(FMaterialPropertyEditingTests, CustomDeclarationValidationIsBoundedAndChecksActiveDefaults)
{
	Durin::FMaterialParameterDefinition Definition;
	Definition.Id = Durin::FGuid::NewGuid();
	Definition.Name = "Independent";
	EXPECT_TRUE(Durin::ValidateMaterialParameterDefinitions({}));
	std::vector Definitions{Definition};
	EXPECT_TRUE(Durin::ValidateMaterialParameterDefinitions(Definitions));
	Definitions.front().Value.GetScalar() = std::numeric_limits<float>::infinity();
	EXPECT_EQ(Durin::ValidateMaterialParameterDefinitions(Definitions).Error,
		Durin::EMaterialParameterError::InvalidDefault);
	Definitions.front() = Definition;
	Definitions.push_back(Definition);
	Definitions.back().Id = Durin::FGuid::NewGuid();
	Definitions.back().Name = "independent";
	const auto Duplicate = Durin::ValidateMaterialParameterDefinitions(Definitions);
	EXPECT_EQ(Duplicate.Error, Durin::EMaterialParameterError::DuplicateName);
	EXPECT_EQ(Duplicate.ParameterId, Definitions.back().Id);
	Definitions.assign(Durin::MaterialMaxParameterDefinitionCount + 1, Definition);
	EXPECT_EQ(Durin::ValidateMaterialParameterDefinitions(Definitions).Error,
		Durin::EMaterialParameterError::TooManyDefinitions);
}

TEST(FMaterialPropertyEditingTests, StaticMeshUsesReflectedMaterialSlotSchema)
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
	ASSERT_NE(StaticSlot, nullptr);
	EXPECT_EQ(StaticSlot, Durin::FMeshMaterialSlotDefinition::StaticStruct());
	EXPECT_EQ(StaticSlot->GetQualifiedName().ToString(), "Durin::FMeshMaterialSlotDefinition");
	EXPECT_EQ(Durin::MaximumMeshMaterialSlots, 4096u);
}

TEST(FMaterialPropertyEditingTests, StaticPropertiesHaveStableDefaultsAndInstanceInheritance)
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

TEST(FMaterialPropertyEditingTests, RuntimeSchemaHasStableIdentityOrderAndMetadata)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = MakeExpandedMaterial("SchemaMaterial");
	const std::span Definitions = Material->GetParameterDefinitions();
	ASSERT_EQ(Definitions.size(), 48u);
	using Durin::MaterialParameters::EMaterialBuiltinParameterKind;
	using Durin::MaterialParameters::EMaterialBuiltinParameterRole;
	const std::array SurfaceOutputs{
		Durin::EMaterialSurfaceOutput::BaseColor,
		Durin::EMaterialSurfaceOutput::Normal,
		Durin::EMaterialSurfaceOutput::Metallic,
		Durin::EMaterialSurfaceOutput::Roughness,
		Durin::EMaterialSurfaceOutput::AmbientOcclusion,
		Durin::EMaterialSurfaceOutput::Emissive,
		Durin::EMaterialSurfaceOutput::Opacity,
		Durin::EMaterialSurfaceOutput::OpacityMask,
	};
	std::vector<Durin::FGuid> ExpectedIds;
	for (size_t RoleIndex = 0;
		RoleIndex < Durin::MaterialParameters::BuiltinParameterRoleCount;
		++RoleIndex)
	{
		const auto Role = static_cast<EMaterialBuiltinParameterRole>(RoleIndex);
		for (size_t KindIndex = 0;
			KindIndex < Durin::MaterialParameters::BuiltinParameterKindCount;
			++KindIndex)
		{
			const auto Kind = static_cast<EMaterialBuiltinParameterKind>(KindIndex);
			const Durin::FGuid Id =
				Durin::MaterialParameters::GetBuiltinParameterId(Role, Kind);
			ExpectedIds.push_back(Id);
			EXPECT_EQ(Durin::GetMaterialSurfaceParameterId(
				SurfaceOutputs[RoleIndex], Kind), Id);
		}
	}
	EXPECT_FALSE(Durin::GetMaterialSurfaceParameterId(
		static_cast<Durin::EMaterialSurfaceOutput>(255),
		EMaterialBuiltinParameterKind::Value).IsValid());
	EXPECT_FALSE(Durin::GetMaterialSurfaceParameterId(
		Durin::EMaterialSurfaceOutput::BaseColor,
		EMaterialBuiltinParameterKind::Count).IsValid());
	EXPECT_FALSE(Durin::MaterialParameters::GetBuiltinParameterId(
		static_cast<EMaterialBuiltinParameterRole>(255),
		EMaterialBuiltinParameterKind::Value).IsValid());
	std::ranges::sort(ExpectedIds);
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
		const auto& Recipe = Durin::GetPBRMaterialParameterDefinitions();
		const auto Expected = std::ranges::find(Recipe, Definition.Id, &Durin::FMaterialParameterDefinition::Id);
		ASSERT_NE(Expected, Recipe.end());
		EXPECT_EQ(Definition, *Expected);
		if (Durin::MaterialParameters::IsBuiltinParameter(Definition.Id, EMaterialBuiltinParameterKind::UVScale)
			|| Durin::MaterialParameters::IsBuiltinParameter(Definition.Id, EMaterialBuiltinParameterKind::UVOffset))
		{
			EXPECT_EQ(Definition.Type, Durin::EMaterialParameterType::Vector2);
		}
		switch (Definition.Presentation)
		{
		case Durin::EMaterialParameterPresentation::Drag:
			if (Definition.Type == Durin::EMaterialParameterType::Scalar)
			{
				EXPECT_TRUE(Definition.bHasRange);
				EXPECT_LT(Definition.MinimumValue, Definition.MaximumValue);
			}
			else EXPECT_FALSE(Definition.bHasRange);
			break;
		case Durin::EMaterialParameterPresentation::Integer:
			EXPECT_EQ(Definition.Type, Durin::EMaterialParameterType::Scalar);
			EXPECT_FLOAT_EQ(Definition.MinimumValue, 0.0f);
			EXPECT_FLOAT_EQ(
				Definition.MaximumValue,
				Durin::MaterialParameters::IsBuiltinParameter(
					Definition.Id,
					Durin::MaterialParameters::EMaterialBuiltinParameterKind::UVChannel)
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
	const auto* Opacity = Material->FindParameterDefinition(
		Durin::MaterialParameters::GetBuiltinParameterIds(
			Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value);
	EXPECT_NE(Opacity, nullptr);
	EXPECT_EQ(Material->FindParameterDefinition(Durin::FName("oPaCiTy")), Opacity);
	EXPECT_FALSE(Material->SetScalarParameterValue(Durin::MaterialParameters::BaseColorName(), 0.5f));
	EXPECT_FALSE(Material->SetVectorParameterValue(
		Durin::FName("BaseColorUVScale"), Durin::FVector3(1.0)));
	EXPECT_TRUE(Material->SetVector2ParameterValue(
		Durin::FName("BaseColorUVScale"), Durin::FVector2(2.0, 3.0)));
	EXPECT_FALSE(Material->SetScalarParameterValue(Durin::FName("UnknownParameter"), 0.5f));
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialPropertyEditingTests, ReflectedPositionalMaterialOverrideUsesSharedTransactions)
{
	FRenderSceneHarness Harness;
	Durin::DMaterial* First = MakeExpandedMaterial("FirstDetailsMaterial");
	Durin::DMaterial* Second = MakeExpandedMaterial("SecondDetailsMaterial");
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
	Durin::Tests::FTestTransactorOwner Transactions;
	Durin::Editor::FPropertyEditSession EditSession;
	ASSERT_TRUE(EditSession.Begin(
		Durin::Editor::FPropertyEditTarget::ForMember(Component, OverridesProperty),
		"Edit Material Override",
		nullptr,
		Transactions.Get()
	));
	EXPECT_EQ(EditSession.Apply(Proposed), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(EditSession.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	const FSceneSnapshot After = CaptureScene(Harness.Scene);

	EXPECT_GT(After.ComponentRevision, Before.ComponentRevision);
	ExpectColorNear(GetMaterialBinding(After.Material).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));
	ASSERT_TRUE(Transactions->Undo());
	const FSceneSnapshot Undone = CaptureScene(Harness.Scene);
	EXPECT_GT(Undone.ComponentRevision, After.ComponentRevision);
	ExpectColorNear(GetMaterialBinding(Undone.Material).BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));
	ASSERT_TRUE(Transactions->Redo());
	const FSceneSnapshot Redone = CaptureScene(Harness.Scene);
	EXPECT_GT(Redone.ComponentRevision, Undone.ComponentRevision);
	ExpectColorNear(GetMaterialBinding(Redone.Material).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));
	EXPECT_TRUE(Transactions->Reset());

	Component->UnregisterComponent();
	WaitForRenderingThread();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Harness.Shutdown();
	Durin::CollectGarbage();
}

TEST(FMaterialPropertyEditingTests, PositionalMaterialOverridesResolveDefaultsAndSurviveMeshSwitches)
{
	InitializeDObjectSystem();
	Durin::DMaterial* First = MakeExpandedMaterial("FirstReflectedSlotMaterial");
	Durin::DMaterial* Second = MakeExpandedMaterial("SecondReflectedSlotMaterial");
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
	auto* Duplicate = Durin::Cast<Durin::DStaticMeshComponent>(
		Durin::DuplicateObject(Component, nullptr, "SparseOverrideDuplicate"));
	ASSERT_NE(Duplicate, nullptr);
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

TEST(FMaterialPropertyEditingTests, StaticMeshComponentValidatesPositionalOverrides)
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
	Component->PostLoad();
	EXPECT_TRUE(Component->GetOverrideMaterials().empty());
	EXPECT_EQ(Component->GetMaterialOverride(0), nullptr);
	Overrides->Resize(Component, Durin::MaximumMeshMaterialSlots + 1ull);
	Component->PostLoad();
	EXPECT_TRUE(Component->GetOverrideMaterials().empty());
	Overrides->Resize(Component, 2);
	Inner->SetObjectPropertyValue(Overrides->GetMutableElementPtr(Component, 0), Material);
	Inner->SetObjectPropertyValue(Overrides->GetMutableElementPtr(Component, 1), nullptr);
	Component->PostLoad();
	EXPECT_EQ(Component->GetOverrideMaterials().size(), 1u);

	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialPropertyEditingTests, ReflectedParameterEditCoalescesAndInvalidatesRenderDataAcrossUndoRedo)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Material = MakeExpandedMaterial("TransactionalMaterial");
	const auto Target = MakeMaterialValueTarget(Material, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value, Durin::FName("ScalarValue"));
	ASSERT_TRUE(Target.has_value());
	Durin::Tests::FTestTransactorOwner Transactions;
	Durin::Editor::FPropertyView PropertyView;
	std::string Error;
	Durin::Editor::FPropertyViewContext Context{
		.Transactor = Transactions.Get(),
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
	ASSERT_TRUE(Transactions->Undo());
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 1.0f);
	ASSERT_TRUE(Transactions->Redo());
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 0.4f);
	EXPECT_TRUE(Transactions->Reset());
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialPropertyEditingTests, ReflectedPropertyViewTracksPresentedOwnerSeparatelyFromEditTarget)
{
	InitializeDObjectSystem();
	Durin::DMaterialInstance* Owner = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PropertyViewOwner");
	Durin::DMaterial* Material = MakeExpandedMaterial("PropertyViewTarget");
	const auto Target = MakeMaterialValueTarget(Material, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value, Durin::FName("ScalarValue"));
	ASSERT_TRUE(Target.has_value());
	Durin::Tests::FTestTransactorOwner Transactions;
	Durin::Editor::FPropertyView PropertyView;
	std::string Error;
	Durin::Editor::FPropertyViewContext Context{
		.Transactor = Transactions.Get(),
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
	};
	PropertyView.HandleOwnerContext(Context, Owner);
	EXPECT_TRUE(PropertyView.SubmitPropertyValueEdit(Context, *Target,
		[](Durin::FProperty* ValueProperty, void* Container, uint32 ArrayIndex) {
			*ValueProperty->ContainerPtrToValuePtr<float>(Container, ArrayIndex) = 0.5f;
		}, true));
	EXPECT_TRUE(std::ranges::any_of(Material->GetExpressionCollection().Expressions,
		[&](const auto& Expression) { return PropertyView.IsEditingObject(Expression.Get()); }));
	PropertyView.HandleOwnerContext(Context, Owner);
	EXPECT_TRUE(PropertyView.IsEditing());

	PropertyView.HandleOwnerContext(Context, Material);
	EXPECT_FALSE(PropertyView.IsEditing());
	EXPECT_TRUE(Error.empty());
	float Opacity = 0.0f;
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), Opacity));
	EXPECT_FLOAT_EQ(Opacity, 1.0f);

	EXPECT_TRUE(Transactions->Reset());
	Durin::MarkAsGarbage(Material);
	Durin::MarkAsGarbage(Owner);
	Durin::CollectGarbage();
}

TEST(FMaterialPropertyEditingTests, ReflectedPropertyViewTracksMaterialOverrideStructureInSharedHistory)
{
	InitializeDObjectSystem();
	Durin::DMaterial* Base = MakeExpandedMaterial("TransactionalOverrideBase");
	Durin::DMaterialInstance* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "TransactionalOverrideInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	auto* Property = static_cast<Durin::FArrayProperty*>(Instance->GetClass()->FindPropertyByName("ScalarParameterValues"));
	ASSERT_NE(Property, nullptr);
	Durin::FPropertyValueSnapshot Original;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Instance, 0, Original));
	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::MaterialParameters::OpacityName(), 0.5f));
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Instance, 0, Proposed));
	ASSERT_TRUE(Instance->ClearScalarParameterValue(Durin::MaterialParameters::OpacityName()));
	Durin::Tests::FTestTransactorOwner Transactions;
	std::string Error;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(
		Durin::Editor::FPropertyEditTarget::ForMember(Instance, Property),
		"Edit Parameter Override", nullptr, Transactions.Get()));
	EXPECT_EQ(Session.Apply(Proposed, &Error), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_TRUE(Instance->HasLocalScalarParameterValue(Durin::MaterialParameters::OpacityName()));
	EXPECT_TRUE(Error.empty());
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(Instance->HasLocalScalarParameterValue(Durin::MaterialParameters::OpacityName()));
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_TRUE(Instance->HasLocalScalarParameterValue(Durin::MaterialParameters::OpacityName()));
	EXPECT_TRUE(Transactions->Reset());
	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialPropertyEditingTests, ReflectedPropertyOverridesValidateAndRestoreIndependentIntent)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Base = MakeExpandedMaterial("ReflectedPropertyBase");
	ASSERT_NE(Base, nullptr);
	auto* Instance = NewObject<DMaterialInstance>(nullptr, "ReflectedPropertyInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	auto* Property = Instance->GetClass()->FindPropertyByName("PropertyOverrides");
	ASSERT_NE(Property, nullptr);
	FMaterialPropertyOverrides Overrides;
	Overrides.bOverrideBlendMode = true;
	Overrides.bOverrideTwoSided = true;
	Overrides.Values.bTwoSided = true;
	ASSERT_TRUE(Instance->SetPropertyOverrides(Overrides));
	FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(CapturePropertyValue(Property, Instance, 0, Proposed));
	ASSERT_TRUE(Instance->SetPropertyOverrides({}));
	Tests::FTestTransactorOwner Transactions;
	Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Editor::FPropertyEditTarget::ForMember(Instance, Property),
		"Edit Rendering Overrides", nullptr, Transactions.Get()));
	const auto Before = Instance->GetRenderStateVersion();
	std::string Error;
	EXPECT_EQ(Session.Apply(Proposed, &Error), Editor::EPropertyEditResult::Changed) << Error;
	EXPECT_EQ(Session.Commit(), Editor::EPropertyEditResult::Changed);
	EXPECT_GT(Instance->GetRenderStateVersion(), Before);
	EXPECT_EQ(Instance->GetPropertyOverrides(), Overrides);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_FALSE(Instance->GetPropertyOverrides().HasAnyOverride());
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(Instance->GetPropertyOverrides(), Overrides);
	EXPECT_TRUE(Transactions->Reset());
	*Property->ContainerPtrToValuePtr<FMaterialPropertyOverrides>(Instance) = Overrides;
	Property->ContainerPtrToValuePtr<FMaterialPropertyOverrides>(Instance)->Values.OpacityMaskThreshold = 2.0f;
	FPropertyValueSnapshot Invalid;
	ASSERT_TRUE(CapturePropertyValue(Property, Instance, 0, Invalid));
	ASSERT_TRUE(Instance->SetPropertyOverrides(Overrides));
	Editor::FPropertyEditSession Rejected;
	ASSERT_TRUE(Rejected.Begin(Editor::FPropertyEditTarget::ForMember(Instance, Property),
		"Invalid Rendering Overrides", nullptr, Transactions.Get()));
	EXPECT_NE(Rejected.Apply(Invalid, &Error), Editor::EPropertyEditResult::Changed);
	EXPECT_FALSE(Error.empty());
	Rejected.Cancel();
	EXPECT_EQ(Instance->GetPropertyOverrides(), Overrides);
	EXPECT_FALSE(Transactions->CanUndo());
	EXPECT_TRUE(Transactions->Reset());
	MarkAsGarbage(Instance);
	MarkAsGarbage(Base);
	CollectGarbage();
}

TEST(FMaterialPropertyEditingTests, UnknownAndMismatchedSettersDoNotInvalidateRenderState)
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

TEST(FMaterialPropertyEditingTests, ParentHookRejectsCyclesWithoutCreatingHistory)
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

	Durin::Tests::FTestTransactorOwner Transactions;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Durin::Editor::FPropertyEditTarget::ForMember(Second, ParentProperty),
		"Edit Parent", nullptr, Transactions.Get()));
	std::string Error;
	EXPECT_EQ(Session.Apply(Proposed, &Error), Durin::Editor::EPropertyEditResult::Failed);
	EXPECT_EQ(Error, "A material instance cannot create a parent cycle.");
	EXPECT_EQ(Second->GetParent(), nullptr);
	EXPECT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::NoChange);
	EXPECT_FALSE(Transactions->CanUndo());

	First->SetParent(nullptr);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::CollectGarbage();
}

TEST(FMaterialPropertyEditingTests, ParentTransactionsRenderFromCurrentCanonicalStorage)
{
	FRenderSceneHarness Harness;
	auto* FirstParent = MakeExpandedMaterial("CanonicalFirstParent");
	auto* SecondParent = MakeExpandedMaterial("CanonicalSecondParent");
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

	Durin::Tests::FTestTransactorOwner Transactions;
	Durin::Editor::FPropertyEditSession CancelledSession;
	ASSERT_TRUE(CancelledSession.Begin(
		Durin::Editor::FPropertyEditTarget::ForMember(Instance, ParentProperty),
		"Edit Parent", nullptr, Transactions.Get()));
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
		"Edit Parent", nullptr, Transactions.Get()));
	ASSERT_EQ(CommittedSession.Apply(Proposed), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_EQ(CommittedSession.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Instance->GetParent(), SecondParent);
	const FSceneSnapshot Committed = CaptureScene(Harness.Scene);
	ExpectColorNear(GetMaterialBinding(Committed.Material).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));

	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(Instance->GetParent(), FirstParent);
	const FSceneSnapshot Undone = CaptureScene(Harness.Scene);
	ExpectColorNear(GetMaterialBinding(Undone.Material).BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));
	ASSERT_TRUE(Transactions->Redo());
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
	EXPECT_TRUE(Transactions->Reset());

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

TEST(FMaterialPropertyEditingTests, ProductionClassDefaultsMatchFreshOrdinaryObjectGraphs)
{
	InitializeDObjectSystem();
	(void)Durin::Z_Construct_DStruct_FVector4();
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
				// Dynamic authored collections are explicitly serialized and have no fixed CDO children.
				if (Class == Durin::DMaterialFunction::StaticClass()
					&& Property->NamePrivate == Durin::FName("ExpressionCollection")) return;
				if (Property->HasAnyPropertyFlags(Durin::EPropertyFlags::Transient)
					|| Property->NamePrivate == Durin::FName("VolumetricCloudSceneId")
					|| Property->NamePrivate == Durin::FName("SkyLightSceneId")
					|| Property->NamePrivate == Durin::FName("ProceduralSkySceneId")) return;
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
