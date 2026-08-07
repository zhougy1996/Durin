#include "MaterialTestSupport.h"

#include "StaticMesh/StaticMeshDerivedData.h"

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
	EXPECT_NE(BaseRenderData.PipelineIdentity, DefaultRenderData.PipelineIdentity);
	EXPECT_EQ(ChildRenderData.PipelineIdentity, BaseRenderData.PipelineIdentity);
	EXPECT_EQ(BaseRenderData.PipelineIdentity.ShaderMap.BlendMode, Properties.BlendMode);
	EXPECT_EQ(BaseRenderData.PipelineIdentity.ShaderMap.ShadingModel, Properties.ShadingModel);
	EXPECT_FLOAT_EQ(
		BaseRenderData.PipelineIdentity.ShaderMap.OpacityMaskThreshold,
		Properties.OpacityMaskThreshold);
	EXPECT_EQ(BaseRenderData.PipelineIdentity.bTwoSided, Properties.bTwoSided);
	EXPECT_EQ(
		BaseRenderData.PipelineIdentity.DepthWritePolicy,
		Properties.DepthWritePolicy);

	Durin::FMaterialStaticProperties Invalid = Properties;
	Invalid.OpacityMaskThreshold = 1.1f;
	const Durin::uint64 InitialVersion = Base->GetRenderStateVersion();
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
		EXPECT_EQ(Definition.SortOrder, static_cast<Durin::int32>(Index));
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

TEST(FMaterialTests, SchemaV1UpgradePreservesStableValuesAndLegacyInstanceOrphans)
{
	InitializeDObjectSystem();
	auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "LegacySchemaMaterial");
	auto* VersionProperty = Material->GetClass()->FindPropertyByName("ParameterSchemaVersion");
	auto* DefinitionsProperty = static_cast<Durin::FArrayProperty*>(Material->GetClass()->FindPropertyByName("ParameterDefinitions"));
	ASSERT_NE(VersionProperty, nullptr);
	ASSERT_NE(DefinitionsProperty, nullptr);
	*VersionProperty->ContainerPtrToValuePtr<Durin::uint32>(Material) = 1;
	DefinitionsProperty->Resize(Material, 3);
	auto* BaseColor = static_cast<Durin::FMaterialParameterDefinition*>(DefinitionsProperty->GetMutableElementPtr(Material, 0));
	auto* BaseTexture = static_cast<Durin::FMaterialParameterDefinition*>(DefinitionsProperty->GetMutableElementPtr(Material, 1));
	auto* Opacity = static_cast<Durin::FMaterialParameterDefinition*>(DefinitionsProperty->GetMutableElementPtr(Material, 2));
	*BaseColor = Durin::MakeCanonicalMaterialParameterDefinitions()[0];
	*BaseTexture = Durin::MakeCanonicalMaterialParameterDefinitions()[1];
	*Opacity = Durin::MakeCanonicalMaterialParameterDefinitions()[42];
	BaseColor->Value.VectorValue = Durin::FVector3(0.1, 0.2, 0.3);
	Opacity->Value.ScalarValue = 0.4f;
	std::string Error;
	ASSERT_TRUE(Material->PostLoad(Error)) << Error;
	ASSERT_EQ(Material->GetParameterDefinitions().size(), 56u);
	Durin::FVector3 LoadedColor;
	float LoadedOpacity = 0.0f;
	EXPECT_TRUE(Material->GetVectorParameterValue(Durin::MaterialParameters::BaseColorName(), LoadedColor));
	EXPECT_EQ(LoadedColor, Durin::FVector3(0.1, 0.2, 0.3));
	EXPECT_TRUE(Material->GetScalarParameterValue(Durin::MaterialParameters::OpacityName(), LoadedOpacity));
	EXPECT_FLOAT_EQ(LoadedOpacity, 0.4f);
	EXPECT_EQ(Material->FindParameterDefinition(Durin::MaterialParameters::SpecularStrengthId), nullptr);

	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "LegacySchemaInstance");
	ASSERT_TRUE(Instance->SetParent(Material));
	auto* InstanceVersion = Instance->GetClass()->FindPropertyByName("ParameterSchemaVersion");
	auto* OverridesProperty = static_cast<Durin::FArrayProperty*>(Instance->GetClass()->FindPropertyByName("ParameterOverrides"));
	ASSERT_NE(InstanceVersion, nullptr);
	ASSERT_NE(OverridesProperty, nullptr);
	*InstanceVersion->ContainerPtrToValuePtr<Durin::uint32>(Instance) = 1;
	OverridesProperty->Resize(Instance, 1);
	auto* LegacyOverride = static_cast<Durin::FMaterialParameterOverride*>(OverridesProperty->GetMutableElementPtr(Instance, 0));
	*LegacyOverride = {.ParameterId = Durin::MaterialParameters::SpecularStrengthId,
		.Type = Durin::EMaterialParameterType::Scalar,
		.Value = Durin::FMaterialParameterValue::MakeScalar(0.8f)};
	ASSERT_TRUE(Instance->PostLoad(Error)) << Error;
	EXPECT_TRUE(Instance->IsParameterOverrideOrphan(Durin::MaterialParameters::SpecularStrengthId));
	Durin::FResolvedMaterialParameter OrphanResolution;
	EXPECT_FALSE(Instance->ResolveParameterValue(Durin::MaterialParameters::SpecularStrengthId, OrphanResolution));

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, SchemaV2UpgradeMigratesUVVectorsAndPreservesAllValues)
{
	InitializeDObjectSystem();
	auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "SchemaV2Material");
	auto* VersionProperty = Material->GetClass()->FindPropertyByName("ParameterSchemaVersion");
	auto* DefinitionsProperty = static_cast<Durin::FArrayProperty*>(
		Material->GetClass()->FindPropertyByName("ParameterDefinitions"));
	ASSERT_NE(VersionProperty, nullptr);
	ASSERT_NE(DefinitionsProperty, nullptr);
	*VersionProperty->ContainerPtrToValuePtr<Durin::uint32>(Material) = 2;
	ASSERT_TRUE(Material->SetScalarParameterValue(Durin::MaterialParameters::RoughnessName(), 0.27f));
	auto* UVScale = static_cast<Durin::FMaterialParameterDefinition*>(
		DefinitionsProperty->GetMutableElementPtr(Material, 3));
	UVScale->Type = Durin::EMaterialParameterType::Vector;
	UVScale->Value = Durin::FMaterialParameterValue::MakeVector(
		Durin::FVector3(2.0, 3.0, 99.0));

	std::string Error;
	ASSERT_TRUE(Material->PostLoad(Error)) << Error;
	float Roughness = 0.0f;
	Durin::FVector2 Scale;
	ASSERT_TRUE(Material->GetScalarParameterValue(Durin::MaterialParameters::RoughnessName(), Roughness));
	ASSERT_TRUE(Material->GetVector2ParameterValue(Durin::FName("BaseColorUVScale"), Scale));
	EXPECT_FLOAT_EQ(Roughness, 0.27f);
	EXPECT_EQ(Scale, Durin::FVector2(2.0, 3.0));
	EXPECT_EQ(Material->GetParameterSchemaVersion(), Durin::CurrentMaterialParameterSchemaVersion);
	EXPECT_EQ(Material->FindParameterDefinition(Durin::MaterialParameters::UVScaleIds[0])->Type,
		Durin::EMaterialParameterType::Vector2);

	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, SchemaV2InstanceUpgradeMigratesUVVectorOverrides)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(nullptr, "SchemaV2InstanceBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "SchemaV2Instance");
	ASSERT_TRUE(Instance->SetParent(Base));
	auto* VersionProperty = Instance->GetClass()->FindPropertyByName("ParameterSchemaVersion");
	auto* OverridesProperty = static_cast<Durin::FArrayProperty*>(
		Instance->GetClass()->FindPropertyByName("ParameterOverrides"));
	ASSERT_NE(VersionProperty, nullptr);
	ASSERT_NE(OverridesProperty, nullptr);
	*VersionProperty->ContainerPtrToValuePtr<Durin::uint32>(Instance) = 2;
	OverridesProperty->Resize(Instance, 1, 0);
	auto* Override = static_cast<Durin::FMaterialParameterOverride*>(
		OverridesProperty->GetMutableElementPtr(Instance, 0));
	*Override = {
		.ParameterId = Durin::MaterialParameters::UVScaleIds[0],
		.Type = Durin::EMaterialParameterType::Vector,
		.Value = Durin::FMaterialParameterValue::MakeVector(Durin::FVector3(4.0, 5.0, 77.0)),
	};

	std::string Error;
	ASSERT_TRUE(Instance->PostLoad(Error)) << Error;
	Durin::FVector2 Scale;
	ASSERT_TRUE(Instance->GetVector2ParameterValue(Durin::FName("BaseColorUVScale"), Scale));
	EXPECT_EQ(Scale, Durin::FVector2(4.0, 5.0));
	ASSERT_EQ(Instance->GetParameterOverrides().size(), 1u);
	EXPECT_EQ(Instance->GetParameterOverrides()[0].Type, Durin::EMaterialParameterType::Vector2);
	EXPECT_EQ(Instance->GetParameterOverrides()[0].Value.Vector2Value, Durin::FVector2(4.0, 5.0));

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
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
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession EditSession;
	ASSERT_TRUE(EditSession.Begin(
		Durin::FReflectedPropertyEditTarget::ForMember(Component, OverridesProperty),
		"Edit Material Override",
		nullptr,
		&Transactions
	));
	EXPECT_EQ(EditSession.Apply(Proposed), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(EditSession.Commit(), Durin::EReflectedPropertyEditResult::Changed);
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
	static_cast<Durin::FStaticMeshMaterialSlotDefinition*>(Slots->GetMutableElementPtr(Mesh, 0))->DefaultMaterial = First;
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
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Durin::FReflectedPropertyEditTarget::ForMember(Component, Overrides), "Corrupt Override"));
	EXPECT_EQ(Session.Apply(InvalidProposal, &Error), Durin::EReflectedPropertyEditResult::Failed);
	EXPECT_NE(Error.find("incompatible object at material index 0"), std::string::npos);
	EXPECT_EQ(Component->GetMaterialOverride(0), Material);
	EXPECT_EQ(Session.Cancel(), Durin::EReflectedPropertyEditResult::NoChange);

	Inner->SetObjectPropertyValue(Overrides->GetMutableElementPtr(Component, 0), Mesh);
	EXPECT_FALSE(Component->PostLoad(Error));
	EXPECT_NE(Error.find("incompatible object at material index 0"), std::string::npos);
	Inner->SetObjectPropertyValue(Overrides->GetMutableElementPtr(Component, 0), Material);
	Overrides->Resize(Component, Durin::MaximumStaticMeshMaterialSlots + 1ull);
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
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyView PropertyView;
	std::string Error;
	Durin::FReflectedPropertyViewContext Context{
		.Transactions = &Transactions,
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
	};
	const Durin::uint64 BeforeVersion = Material->GetRenderStateVersion();
	EXPECT_TRUE(PropertyView.SubmitPropertyValueEdit(Context, *Target,
		[](Durin::FProperty* ValueProperty, void* Container, Durin::uint32 ArrayIndex) {
			*ValueProperty->ContainerPtrToValuePtr<float>(Container, ArrayIndex) = 0.6f;
		}, true));
	EXPECT_TRUE(PropertyView.SubmitPropertyValueEdit(Context, *Target,
		[](Durin::FProperty* ValueProperty, void* Container, Durin::uint32 ArrayIndex) {
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
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyView PropertyView;
	std::string Error;
	Durin::FReflectedPropertyViewContext Context{
		.Transactions = &Transactions,
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
	};
	PropertyView.HandleOwnerContext(Context, Owner);
	EXPECT_TRUE(PropertyView.SubmitPropertyValueEdit(Context, *Target,
		[](Durin::FProperty* ValueProperty, void* Container, Durin::uint32 ArrayIndex) {
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
	Durin::FEditorTransactionManager Transactions;
	std::string Error;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(
		Durin::FReflectedPropertyEditTarget::ForMember(Instance, Property),
		"Edit Parameter Override", nullptr, &Transactions));
	EXPECT_EQ(Session.Apply(Proposed, &Error), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);
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
	const Durin::uint64 Version = Material->GetRenderStateVersion();
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

	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Durin::FReflectedPropertyEditTarget::ForMember(Second, ParentProperty), "Edit Parent", nullptr, &Transactions));
	std::string Error;
	EXPECT_EQ(Session.Apply(Proposed, &Error), Durin::EReflectedPropertyEditResult::Failed);
	EXPECT_EQ(Error, "A material instance cannot create a parent cycle.");
	EXPECT_EQ(Second->GetParent(), nullptr);
	EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::NoChange);
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

	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession CancelledSession;
	ASSERT_TRUE(CancelledSession.Begin(
		Durin::FReflectedPropertyEditTarget::ForMember(Instance, ParentProperty),
		"Edit Parent", nullptr, &Transactions));
	ASSERT_EQ(CancelledSession.Apply(Proposed), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Instance->GetParent(), SecondParent);
	const FSceneSnapshot Interactive = CaptureScene(Harness.Scene);
	ExpectColorNear(GetMaterialBinding(Interactive.Material).BaseColor, Durin::FVector4f(0.7f, 0.6f, 0.5f, 1.0f));
	ASSERT_EQ(CancelledSession.Cancel(), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Instance->GetParent(), FirstParent);
	const FSceneSnapshot Cancelled = CaptureScene(Harness.Scene);
	ExpectColorNear(GetMaterialBinding(Cancelled.Material).BaseColor, Durin::FVector4f(0.1f, 0.2f, 0.3f, 1.0f));

	Durin::FReflectedPropertyEditSession CommittedSession;
	ASSERT_TRUE(CommittedSession.Begin(
		Durin::FReflectedPropertyEditTarget::ForMember(Instance, ParentProperty),
		"Edit Parent", nullptr, &Transactions));
	ASSERT_EQ(CommittedSession.Apply(Proposed), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(CommittedSession.Commit(), Durin::EReflectedPropertyEditResult::Changed);
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
	std::vector<Durin::DClass*> Classes;
	Durin::uint32 ProductionClassCount = 0;
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
	EXPECT_EQ(ProductionClassCount, 38u);
	EXPECT_EQ(Classes.size(), 25u);
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

		std::unordered_map<const Durin::DObject*, const Durin::DObject*> TemplateToLive;
		TemplateToLive.emplace(DefaultObject, Instance);
		std::function<void(const Durin::DObject*, const Durin::DObject*)> PairGraphs;
		PairGraphs = [&](const Durin::DObject* Template, const Durin::DObject* Live) {
			const std::vector<Durin::DObject*> TemplateChildren = Durin::GDObjectArray.GetObjectsWithOuter(
				Template, Durin::EObjectQueryScope::IncludeTemplates);
			const std::vector<Durin::DObject*> LiveChildren = Durin::GDObjectArray.GetObjectsWithOuter(
				Live, Durin::EObjectQueryScope::LiveOnly);
			EXPECT_EQ(TemplateChildren.size(), LiveChildren.size()) << Class->GetQualifiedName().ToString();
			for (const Durin::DObject* TemplateChild : TemplateChildren)
			{
				const auto It = std::ranges::find_if(LiveChildren, [&](const Durin::DObject* Candidate) {
					return Candidate->GetClass() == TemplateChild->GetClass()
						&& Candidate->GetFName() == TemplateChild->GetFName();
				});
				ASSERT_NE(It, LiveChildren.end())
					<< Class->GetQualifiedName().ToString() << ":" << TemplateChild->GetName();
				TemplateToLive.emplace(TemplateChild, *It);
				PairGraphs(TemplateChild, *It);
			}
		};
		PairGraphs(DefaultObject, Instance);

		for (const auto& [Template, Live] : TemplateToLive)
		{
			Template->GetClass()->ForEachProperty([&](Durin::FProperty* Property) {
				if (Property->HasAnyPropertyFlags(Durin::EPropertyFlags::Transient)
					|| Property->NamePrivate == Durin::FName("SkyBoxSceneId")) return;
				for (Durin::uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
				{
					if (Durin::ArePropertyValuesIdentical(Property, Template, Index, Live, Index)) continue;
					if (Property->ClassPrivate->IsChildOf(Durin::FObjectProperty::StaticClass()))
					{
						auto* ObjectProperty = static_cast<Durin::FObjectProperty*>(Property);
						const Durin::DObject* TemplateValue = ObjectProperty->GetObjectPropertyValue(Template, Index);
						const Durin::DObject* LiveValue = ObjectProperty->GetObjectPropertyValue(Live, Index);
						const auto PairIt = TemplateToLive.find(TemplateValue);
						EXPECT_TRUE(PairIt != TemplateToLive.end() && PairIt->second == LiveValue)
							<< Class->GetQualifiedName().ToString() << ":" << Property->NamePrivate.ToString();
						continue;
					}
					if (Property->NamePrivate == Durin::FName("OwnedComponents")) continue;
					ADD_FAILURE() << Class->GetQualifiedName().ToString() << ":"
						<< Property->NamePrivate.ToString() << " differs from its class default";
				}
			}, true);
		}

		Durin::MarkObjectHierarchyAsGarbage(Instance);
	}
	Durin::CollectGarbage();
}
