#include "ExplicitMaterialProgramTestFixture.h"
#include "MaterialTestSupport.h"
#include "Materials/MaterialFunction.h"
#include "Editor/EditorTransactionTestSupport.h"

#include "DObject/DefaultObjectGraph.h"
#include "DObject/MathStructs.h"
#include "Hash/XxHash.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialExpressionBuild.h"
#include <set>
#include "Materials/MaterialCookedProgram.h"
#include "StaticMesh/StaticMeshDerivedData.h"

#include <cstring>
#include <limits>
#include <unordered_set>

namespace
{
	auto CaptureMaterialExpressions(const Durin::DMaterial& Material) -> Durin::Testing::FTestMaterialExpressionGraph
	{
		Durin::Testing::FTestMaterialExpressionGraph Graph;
		Graph.Outputs = Material.GetExpressionOutputs();
		for (const auto& Expression : Material.GetExpressionCollection().Expressions)
			Graph.Expressions.emplace_back(Durin::DuplicateObject(Expression.Get(), nullptr, Durin::NAME_None));
		return Graph;
	}

	auto MakeDefaultMaterialCompilerIR() -> Durin::FMaterialIR
	{
		Durin::FMaterialExpressionBuildContext Empty(std::span<Durin::DMaterialExpression* const>{});
		return Empty.FinishSurface({}).IR;
	}

	auto MakeSyntheticMaterialCompilerInput() -> Durin::FMaterialIRCompilerInput
	{
		InitializeDObjectSystem();
		auto Recipe = Durin::Testing::MakePBRMaterialExpressionsForTest();
		std::vector<Durin::DMaterialExpression*> Expressions;
		for (const auto& Expression : Recipe.Expressions) Expressions.push_back(Expression.Get());
		Durin::FMaterialExpressionBuildContext Context(Expressions);
		auto Built = Context.FinishSurface(Recipe.Outputs);
		check(Built);
		Durin::FMaterialIRCompilerInput Input{.IR = std::move(Built.IR), .Parameters = std::move(Built.Parameters),
			.Sources = std::move(Built.Sources)};
		Input.Environment.CompilerIdentity = "slang-test-build;target=spirv;profile=spirv_1_5";
		Input.Environment.Target = "vulkan-spirv-1.5";
		Input.Environment.Dependencies = {
			{"/Engine/MaterialTemplate.slang", {11, 12}},
			{"/Engine/StaticMeshBasePass.slang", {21, 22}}};
		return Input;
	}

	auto ReorderIndependentMaterialIRNodes(Durin::FMaterialIRCompilerInput& Input) -> void
	{
		// Detached IR requires dependencies before consumers. Reverse ready-node
		// priority to exercise equivalent orderings without violating that contract.
		const auto Count = static_cast<uint32>(Input.IR.Nodes.size());
		std::vector<uint32> Pending(Count), Remapping(Count);
		std::vector<std::vector<uint32>> Consumers(Count);
		std::set<uint32> Ready;
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			Pending[Index] = static_cast<uint32>(Input.IR.Nodes[Index].Inputs.size());
			for (const auto Dependency : Input.IR.Nodes[Index].Inputs) Consumers.at(Dependency).push_back(Index);
			if (Pending[Index] == 0) Ready.insert(Index);
		}
		std::vector<Durin::FMaterialIRNode> Nodes;
		while (!Ready.empty())
		{
			const auto Index = *Ready.rbegin(); Ready.erase(Index);
			auto Node = Input.IR.Nodes[Index];
			for (auto& Dependency : Node.Inputs) Dependency = Remapping[Dependency];
			Remapping[Index] = static_cast<uint32>(Nodes.size());
			Nodes.push_back(std::move(Node));
			for (const auto Consumer : Consumers[Index]) if (--Pending[Consumer] == 0) Ready.insert(Consumer);
		}
		check(Nodes.size() == Count);
		Input.IR.Nodes = std::move(Nodes);
		for (auto& Root : Input.IR.SurfaceRoot.Inputs)
			if (Root.bExpression) Root.ExpressionIndex = Remapping[Root.ExpressionIndex];
		if (Input.IR.SurfaceRoot.bAggregate)
			Input.IR.SurfaceRoot.AggregateExpressionIndex = Remapping[Input.IR.SurfaceRoot.AggregateExpressionIndex];
		for (auto& Source : Input.Sources) Source.ExpressionIndex = Remapping[Source.ExpressionIndex];
	}

	auto MakeExpandedMaterial(const char* Name) -> Durin::DMaterial*
	{
		auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, Name);
		if (!Material || !Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Material)) return nullptr;
		if (!FinishMaterialCompileForTest(*Material)) return nullptr;
		return Material;
	}
}

TEST(FMaterialTests, OwnedParametersRejectDuplicateIdentityAndNameAtomically)
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
	EXPECT_FALSE(Material->SetMaterialExpressions(Duplicated, {}));
	Duplicate->Metadata.Id = FGuid::NewGuid(); Duplicate->Metadata.Name = "rustamount";
	EXPECT_FALSE(Material->SetMaterialExpressions(Duplicated, {}));
	EXPECT_EQ(Material->GetParameterDefinitionSchemaRevision(), Revision);
	ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
	Owner->Metadata.Name = "Weathering";
	ASSERT_TRUE(Material->SetMaterialExpressions(Original, {}));
	EXPECT_EQ(Material->FindParameterDefinition("Weathering")->Id, Owner->Metadata.Id);
	EXPECT_EQ(Material->GetExpressionCollection().Expressions.front()->Id, Owner->Id);
	EXPECT_TRUE(Material->SetParameterValue(Owner->Metadata.Id, FMaterialParameterValue::MakeScalar(.75f)));
	EXPECT_EQ(Cast<DMaterialExpressionScalarParameter>(Material->GetExpressionCollection().Expressions.front().Get())->DefaultValue, .75f);
	ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
	EXPECT_TRUE(Material->GetParameterDefinitions().empty());
}

TEST(FMaterialTests, CustomDeclarationOverridesRetainOrphansAndRejectRetyping)
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

TEST(FMaterialTests, CustomDeclarationValidationIsBoundedAndChecksActiveDefaults)
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

TEST(FMaterialTests, StaticMeshUsesReflectedMaterialSlotSchema)
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

TEST(FMaterialProgramSchemaTests,
	TypedExpressionsAreReflectedBoundedAndDeterministicallyValid)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto First = Testing::MakePBRMaterialExpressionsForTest();
	const auto Second = Testing::MakePBRMaterialExpressionsForTest();
	ASSERT_EQ(First.Expressions.size(), Second.Expressions.size());
	EXPECT_EQ(First.Outputs, Second.Outputs);
	EXPECT_FALSE(First.Expressions.empty());
	EXPECT_LE(First.Expressions.size(), MaterialProgramMaxNodeCount);
	std::unordered_set<FGuid> NodeIds;
	size_t LinkCount = 8;
	std::vector<DMaterialExpression*> Expressions;
	for (size_t Index = 0; Index < First.Expressions.size(); ++Index)
	{
		const auto& Node = First.Expressions[Index];
		EXPECT_TRUE(Node->Id.IsValid()); EXPECT_TRUE(NodeIds.insert(Node->Id).second);
		EXPECT_LE(Node->GetAuthoredInputCount(), MaterialProgramMaxNodeInputCount);
		LinkCount += Node->GetAuthoredInputCount();
		ASSERT_EQ(Node->GetClass(), Second.Expressions[Index]->GetClass());
		Node->GetClass()->ForEachProperty([&](FProperty* Property) {
			EXPECT_TRUE(ArePropertyValuesIdentical(Property, Node.Get(), 0, Second.Expressions[Index].Get(), 0));
		});
		Expressions.push_back(Node.Get());
	}
	EXPECT_LE(LinkCount, MaterialProgramMaxLinkCount);
	const auto Validation = FMaterialExpressionBuildContext::ValidateSurface(Expressions, First.Outputs);
	EXPECT_TRUE(Validation); EXPECT_TRUE(Validation.Diagnostics.empty());
	EXPECT_NE(FMaterialExpressionCollection::StaticStruct()->FindPropertyByName("Expressions"), nullptr);
	EXPECT_NE(FMaterialExpressionSurfaceOutputs::StaticStruct()->FindPropertyByName("BaseColor"), nullptr);
	EXPECT_NE(DMaterialExpressionScalarConstant::StaticClass()->FindPropertyByName("Value"), nullptr);
	DStruct* Presentation = FMaterialGraphPresentation::StaticStruct();
	ASSERT_NE(Presentation, nullptr);
	EXPECT_NE(Presentation->FindPropertyByName("bHasMaterialOutputPosition"), nullptr);
	EXPECT_NE(Presentation->FindPropertyByName("MaterialOutputX"), nullptr);
	EXPECT_NE(Presentation->FindPropertyByName("MaterialOutputY"), nullptr);
	EXPECT_EQ(DMaterial::StaticClass()->FindPropertyByName("Program"), nullptr);
	EXPECT_NE(DMaterial::StaticClass()->FindPropertyByName("ExpressionCollection"), nullptr);
	EXPECT_EQ(DMaterialInstance::StaticClass()->FindPropertyByName("ExpressionCollection"), nullptr);
}

TEST(FMaterialProgramSchemaTests,
	TypedValidatorRejectsMalformedOwnersLinksAndBoundsDeterministically)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto ExpectFailure = [&](const Testing::FTestMaterialExpressionGraph& Graph, std::string_view Message) {
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Graph.Expressions) Expressions.push_back(Expression.Get());
		auto Validation = FMaterialExpressionBuildContext::ValidateSurface(Expressions, Graph.Outputs);
		EXPECT_FALSE(Validation);
		EXPECT_FALSE(Validation.Diagnostics.empty());
		if (!Validation.Diagnostics.empty()) EXPECT_NE(Validation.Diagnostics.front().Message.find(Message), std::string::npos)
			<< Validation.Diagnostics.front().Message;
		return Validation;
	};
	{
		auto Graph = Testing::MakePBRMaterialExpressionsForTest();
		Graph.Expressions[1]->Id = Graph.Expressions[0]->Id;
		ExpectFailure(Graph, "duplicate GUID");
	}
	{
		auto Graph = Testing::MakePBRMaterialExpressionsForTest();
		Graph.Expressions.emplace_back();
		ExpectFailure(Graph, "null owner");
	}
	{
		auto Graph = Testing::MakePBRMaterialExpressionsForTest();
		Graph.Outputs.Metallic.ExpressionId = {1, 2, 3, 4};
		ExpectFailure(Graph, "missing expression");
	}
	{
		auto Graph = Testing::MakePBRMaterialExpressionsForTest();
		Graph.Outputs.Metallic = Graph.Outputs.BaseColor;
		const auto Validation = ExpectFailure(Graph, "type");
		ASSERT_FALSE(Validation.Diagnostics.empty());
		EXPECT_EQ(Validation.Diagnostics.front().Category, EMaterialProgramDiagnosticCategory::Type);
		EXPECT_EQ(Validation.Diagnostics.front().LocationKind, EMaterialProgramDiagnosticLocationKind::SurfaceOutput);
		EXPECT_EQ(Validation.Diagnostics.front().LocationIndex, static_cast<uint32>(EMaterialSurfaceOutput::Metallic));
	}
	{
		Testing::FTestMaterialExpressionGraph Graph;
		TStrongObjectPtr<DMaterialExpressionScalarConstant> Constant(NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None));
		Constant->Id = FGuid::NewGuid(); Constant->Value = std::numeric_limits<float>::infinity();
		Graph.Expressions.emplace_back(Constant.Get());
		ExpectFailure(Graph, "finite");
	}
	{
		Testing::FTestMaterialExpressionGraph Graph;
		TStrongObjectPtr<DMaterialExpressionAdd> Invalid(NewObject<DMaterialExpressionAdd>(nullptr, NAME_None));
		Invalid->Id = FGuid::NewGuid(); Invalid->ResultType = static_cast<EMaterialProgramValueType>(255);
		Graph.Expressions.emplace_back(Invalid.Get());
		ExpectFailure(Graph, "signature");
	}
	{
		Testing::FTestMaterialExpressionGraph Graph;
		TStrongObjectPtr<DMaterialExpressionNegate> A(NewObject<DMaterialExpressionNegate>(nullptr, NAME_None));
		TStrongObjectPtr<DMaterialExpressionNegate> B(NewObject<DMaterialExpressionNegate>(nullptr, NAME_None));
		A->Id = FGuid::NewGuid(); B->Id = FGuid::NewGuid(); A->Input = {B->Id}; B->Input = {A->Id};
		Graph.Expressions.emplace_back(A.Get()); Graph.Expressions.emplace_back(B.Get());
		ExpectFailure(Graph, "cycle");
	}
	{
		Testing::FTestMaterialExpressionGraph Graph;
		FGuid Previous;
		for (uint32 Index = 0; Index <= MaterialProgramMaxDepth; ++Index)
		{
			TStrongObjectPtr<DMaterialExpressionNegate> Node(NewObject<DMaterialExpressionNegate>(nullptr, NAME_None));
			Node->Id = {0xde770001, 0, 0, Index + 1}; Node->Input = {Previous};
			Node->InputDefault = {0}; Previous = Node->Id;
			Graph.Expressions.emplace_back(Node.Get());
		}
		ExpectFailure(Graph, "depth");
	}
	{
		Testing::FTestMaterialExpressionGraph Graph;
		while (Graph.Expressions.size() <= MaterialProgramMaxNodeCount)
		{
			TStrongObjectPtr<DMaterialExpressionScalarConstant> Node(NewObject<DMaterialExpressionScalarConstant>(nullptr, NAME_None));
			Node->Id = FGuid::NewGuid(); Graph.Expressions.emplace_back(Node.Get());
		}
		ExpectFailure(Graph, "node bound");
	}
	{
		auto Graph = Testing::MakePBRMaterialExpressionsForTest();
		const auto Found = std::ranges::find_if(Graph.Expressions, [](const auto& Node) { return Cast<DMaterialExpressionParameter>(Node.Get()) != nullptr; });
		ASSERT_NE(Found, Graph.Expressions.end());
		auto* Parameter = Cast<DMaterialExpressionParameter>(Found->Get());
		Parameter->Metadata.Id = {};
		ExpectFailure(Graph, "parameter GUID");
		Parameter->Metadata.Id = FGuid::NewGuid();
		Parameter->Metadata.DisplayName.assign(MaterialProgramMaxDisplayNameBytes + 1, 'x');
		const auto Forward = ExpectFailure(Graph, "metadata");
		std::ranges::reverse(Graph.Expressions);
		const auto Reversed = ExpectFailure(Graph, "metadata");
		EXPECT_EQ(Forward.Diagnostics, Reversed.Diagnostics);
	}
}

TEST(FMaterialProgramSchemaTests,
	BaseOwnsExpressionsAndInstancesShareWithoutDuplicatingThem)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Base(NewObject<DMaterial>(nullptr, "ExpressionOwningBase"));
	TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, "ExpressionSharingInstance"));
	ASSERT_TRUE(Instance->SetParent(Base.Get()));
	auto Graph = Testing::MakePBRMaterialExpressionsForTest();
	std::ranges::reverse(Graph.Expressions);
	ASSERT_TRUE(Graph.Apply(*Base));
	const auto& Owned = Base->GetExpressionCollection().Expressions;
	ASSERT_EQ(Owned.size(), Graph.Expressions.size());
	for (size_t Index = 0; Index < Owned.size(); ++Index)
	{
		EXPECT_EQ(Owned[Index]->Id, Graph.Expressions[Index]->Id);
		EXPECT_EQ(Owned[Index]->GetOuter(), Base.Get());
		EXPECT_NE(Owned[Index].Get(), Graph.Expressions[Index].Get());
	}
	EXPECT_EQ(Instance->GetParent(), Base.Get());
	EXPECT_TRUE(GDObjectArray.GetObjectsWithOuter(Instance.Get(), EObjectQueryScope::LiveOnly).empty());
	const auto BeforeChildren = Owned;
	const auto BeforeOutputs = Base->GetExpressionOutputs();
	Graph.Outputs.BaseColorDefault.x = std::numeric_limits<float>::quiet_NaN();
	EXPECT_FALSE(Graph.Apply(*Base));
	EXPECT_EQ(Base->GetExpressionCollection().Expressions, BeforeChildren);
	EXPECT_EQ(Base->GetExpressionOutputs(), BeforeOutputs);
}

TEST(FMaterialProgramNormalizationTests,
	SnapshotIsDetachedAndExcludesDynamicParameterValues)
{
	InitializeDObjectSystem();
	auto* Material = MakeExpandedMaterial("CompilerSnapshotMaterial");
	Durin::FMaterialCompilerEnvironment Environment =
		MakeSyntheticMaterialCompilerInput().Environment;
	Durin::FMaterialIRCompilerInput Before;
	auto Validation = Durin::SnapshotMaterialCompilerInput(
		*Material, Environment, Before);
	ASSERT_TRUE(Validation);
	ASSERT_TRUE(Material->SetScalarParameterValue(
		Durin::MaterialParameters::MetallicName(), 0.87f));
	Durin::FMaterialIRCompilerInput After;
	ASSERT_TRUE((Validation = Durin::SnapshotMaterialCompilerInput(
		*Material, Environment, After)));
	EXPECT_EQ(Before.IR, After.IR);
	EXPECT_EQ(Before.Parameters, After.Parameters);
	EXPECT_EQ(Before.StaticProperties, After.StaticProperties);
	EXPECT_EQ(Before.Environment, After.Environment);
	for (const auto& Node : Before.IR.Nodes) EXPECT_TRUE(Node.HasValidPayload());
	EXPECT_EQ(Before.Parameters.size(),
		Material->GetParameterDefinitions().size());
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
	EXPECT_TRUE(Durin::NormalizeMaterialIR(Before));
}

TEST(FMaterialProgramNormalizationTests,
	DefaultProgramUsesLiteralSurfaceRootWithoutOrdinaryNodes)
{
	InitializeDObjectSystem();
	Durin::FMaterialIRCompilerInput Input = MakeSyntheticMaterialCompilerInput();
	Durin::FMaterialExpressionBuildContext Empty(std::span<Durin::DMaterialExpression* const>{});
	Input.IR = Empty.FinishSurface({}).IR;
	Input.Parameters.clear();
	Input.Sources.clear();
	const Durin::FMaterialNormalizationResult Normalized =
		Durin::NormalizeMaterialIR(Input);
	ASSERT_TRUE(Normalized);
	EXPECT_TRUE(Input.IR.Nodes.empty());
	ASSERT_TRUE(Normalized.IR.Nodes.empty());
	EXPECT_TRUE(Normalized.ActiveParameters.empty());
	EXPECT_FALSE(Normalized.IR.SurfaceRoot.bAggregate);
	EXPECT_EQ(Normalized.IR.SurfaceRoot.Inputs[0].Literal,
		(Durin::FMaterialProgramLiteral{0.5f, 0.5f, 0.5f, 0.0f}));
	EXPECT_EQ(Normalized.IR.SurfaceRoot.Inputs[1].Literal,
		(Durin::FMaterialProgramLiteral{0.0f, 0.0f, 1.0f, 0.0f}));
	std::string Source;
	std::string Error;
	ASSERT_TRUE(Durin::GenerateMaterialProgramSlang(
		Normalized.IR, Source, Error)) << Error;
	EXPECT_EQ(Source.find("BaseColorTexture.Sample"), std::string::npos);
	EXPECT_EQ(Source.find("NormalTexture.Sample"), std::string::npos);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	const Durin::FMaterialCompilerResult Compiled =
		Durin::CompileMaterialIR(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty()
		? std::string("no diagnostic") : Compiled.Diagnostics.front().Message);
	size_t ActiveBindings = 0;
	for (const Durin::FCompiledShader& Shader : Compiled.CompiledShaders)
		ActiveBindings += Shader.Reflection.ResourceBindings.size();
	std::cout << "[MaterialOutputDefaultBaseline] input_ir_nodes="
		<< Input.IR.Nodes.size()
		<< " ir_nodes=" << Normalized.IR.Nodes.size()
		<< " texture_samples=0 generated_bytes=" << Source.size()
		<< " source_hash=" << Durin::FXxHash128::HashBuffer(Source).ToString()
		<< " identity=" << Compiled.Identity.Digest.ToString()
		<< " compiled_stages=" << Compiled.CompiledShaders.size()
		<< " active_bindings=" << ActiveBindings << '\n';
}

TEST(FMaterialProgramSchemaTests, AggregateAndPropertyOutputsAreExclusive)
{
	InitializeDObjectSystem();
	auto Graph = Durin::Testing::MakePBRMaterialExpressionsForTest();
	Graph.Outputs.Surface = Graph.Outputs.BaseColor;
	std::vector<Durin::DMaterialExpression*> Expressions;
	for (const auto& Expression : Graph.Expressions) Expressions.push_back(Expression.Get());
	auto Validation = Durin::FMaterialExpressionBuildContext::ValidateSurface(Expressions, Graph.Outputs);
	EXPECT_FALSE(Validation);
	ASSERT_FALSE(Validation.Diagnostics.empty());
	EXPECT_NE(Validation.Diagnostics.front().Message.find("cannot be combined"), std::string::npos);
	EXPECT_NE(std::ranges::find(Validation.Diagnostics,
		Durin::EMaterialProgramDiagnosticCategory::Type,
		&Durin::FMaterialProgramDiagnostic::Category),
		Validation.Diagnostics.end());
}

TEST(FMaterialProgramNormalizationTests,
	EquivalentTypedInputsProduceIdenticalCanonicalIdentity)
{
	const Durin::FMaterialIRCompilerInput BaselineInput =
		MakeSyntheticMaterialCompilerInput();
	const Durin::FMaterialNormalizationResult Baseline =
		Durin::NormalizeMaterialIR(BaselineInput);
	ASSERT_TRUE(Baseline);
	EXPECT_FALSE(Baseline.CanonicalBytes.empty());
	EXPECT_LE(Baseline.CanonicalBytes.size(),
		Durin::MaterialProgramMaxCanonicalBytes);
	constexpr std::string_view Domain = "DurinMaterialProgramIR";
	ASSERT_GT(Baseline.CanonicalBytes.size(), Domain.size());
	EXPECT_EQ(std::memcmp(
		Baseline.CanonicalBytes.data(), Domain.data(), Domain.size()), 0);
	EXPECT_EQ(Baseline.CanonicalBytes[Domain.size()], std::byte{0});

	const auto ExpectEquivalent = [&](Durin::FMaterialIRCompilerInput Candidate) {
		const auto Result = Durin::NormalizeMaterialIR(Candidate);
		ASSERT_TRUE(Result);
		EXPECT_EQ(Result.IR, Baseline.IR);
		EXPECT_EQ(Result.CanonicalBytes, Baseline.CanonicalBytes);
		EXPECT_EQ(Result.Identity, Baseline.Identity);
		EXPECT_EQ(Result.ActiveParameters, Baseline.ActiveParameters);
	};

	Durin::FMaterialIRCompilerInput Reordered = BaselineInput;
	ReorderIndependentMaterialIRNodes(Reordered);
	ExpectEquivalent(std::move(Reordered));

	Durin::FMaterialIRCompilerInput Reidentified = BaselineInput;
	for (auto& Source : Reidentified.Sources) Source.NodeId = Durin::FGuid::NewGuid();
	ExpectEquivalent(std::move(Reidentified));

	Durin::FMaterialIRCompilerInput PresentationOnly = BaselineInput;
	for (auto& Source : PresentationOnly.Sources)
		Source.FunctionAssetPath = "ignored diagnostic source location";
	const auto Float3Constant = std::ranges::find_if(
		PresentationOnly.IR.Nodes, [](const auto& Node) {
			return Node.Opcode == Durin::EMaterialProgramOpcode::Constant
				&& Node.ResultType
					== Durin::EMaterialProgramValueType::Float3;
		});
	ASSERT_NE(Float3Constant, PresentationOnly.IR.Nodes.end());
	std::get<Durin::FMaterialProgramLiteral>(Float3Constant->Payload).W = 123.0f;
	ExpectEquivalent(std::move(PresentationOnly));

	Durin::FMaterialIRCompilerInput SignedZero = BaselineInput;
	const auto ZeroConstant = std::ranges::find_if(
		SignedZero.IR.Nodes, [](const auto& Node) {
			return Node.Opcode == Durin::EMaterialProgramOpcode::Constant
				&& Node.GetLiteral().X == 0.0f;
		});
	ASSERT_NE(ZeroConstant, SignedZero.IR.Nodes.end());
	std::get<Durin::FMaterialProgramLiteral>(ZeroConstant->Payload).X = -0.0f;
	ExpectEquivalent(std::move(SignedZero));

	Durin::FMaterialIRCompilerInput Swapped = BaselineInput;
	const auto Commutative = std::ranges::find_if(
		Swapped.IR.Nodes, [](const auto& Node) {
			return (Node.Opcode == Durin::EMaterialProgramOpcode::Add
				|| Node.Opcode == Durin::EMaterialProgramOpcode::Multiply
				|| Node.Opcode == Durin::EMaterialProgramOpcode::Minimum
				|| Node.Opcode == Durin::EMaterialProgramOpcode::Maximum)
				&& Node.Inputs.size() == 2;
		});
	ASSERT_NE(Commutative, Swapped.IR.Nodes.end());
	std::swap(Commutative->Inputs[0], Commutative->Inputs[1]);
	ExpectEquivalent(std::move(Swapped));

	Durin::FMaterialIRCompilerInput WithDeadNode = BaselineInput;
	Durin::FMaterialIRNode DeadNode;
	DeadNode.Opcode = Durin::EMaterialProgramOpcode::Constant;
	DeadNode.ResultType = Durin::EMaterialProgramValueType::Float;
	DeadNode.Payload = Durin::FMaterialProgramLiteral{-0.0f};
	WithDeadNode.IR.Nodes.push_back(std::move(DeadNode));
	ExpectEquivalent(std::move(WithDeadNode));
}

TEST(FMaterialProgramNormalizationTests, SharedDagKeysRemainBoundedAtMaximumDepth)
{
	using namespace Durin;
	auto Input = MakeSyntheticMaterialCompilerInput();
	FMaterialExpressionBuildContext Empty(std::span<DMaterialExpression* const>{});
	Input.IR = Empty.FinishSurface({}).IR;
	Input.Parameters.clear(); Input.Sources.clear();
	// Independent equal DAGs force structural comparisons at the depth boundary.
	std::array<uint32, 2> Roots;
	for (uint32 Branch = 0; Branch < 2; ++Branch)
	{
		uint32 Previous = 0;
		for (uint32 Level = 0; Level < MaterialProgramMaxDepth - 1; ++Level)
		{
			FMaterialIRNode Node{.Opcode = Level == 0 ? EMaterialProgramOpcode::Constant : EMaterialProgramOpcode::Add};
			if (Level == 0) Node.Payload = FMaterialProgramLiteral{.25f};
			else Node.Inputs = {Previous, Previous};
			Previous = static_cast<uint32>(Input.IR.Nodes.size());
			Input.IR.Nodes.push_back(std::move(Node));
		}
		Roots[Branch] = Previous;
	}
	Input.IR.SurfaceRoot.Inputs[2].bExpression = true;
	Input.IR.SurfaceRoot.Inputs[2].ExpressionIndex = static_cast<uint32>(Input.IR.Nodes.size());
	Input.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Add, .Inputs = {Roots[0], Roots[1]}});
	const auto Baseline = NormalizeMaterialIR(Input);
	ASSERT_TRUE(Baseline);
	EXPECT_EQ(Baseline.IR.Nodes.size(), Input.IR.Nodes.size());
	EXPECT_LT(Baseline.CanonicalBytes.size(), MaterialProgramMaxCanonicalBytes);
	std::swap(Input.IR.Nodes.back().Inputs[0], Input.IR.Nodes.back().Inputs[1]);
	ReorderIndependentMaterialIRNodes(Input);
	const auto Reordered = NormalizeMaterialIR(Input);
	ASSERT_TRUE(Reordered);
	EXPECT_EQ(Reordered.Identity, Baseline.Identity);
	EXPECT_EQ(Reordered.CanonicalBytes, Baseline.CanonicalBytes);
	for (auto& Node : Input.IR.Nodes)
		if (Node.Opcode == EMaterialProgramOpcode::Constant) Node.Payload = FMaterialProgramLiteral{.5f};
	const auto Changed = NormalizeMaterialIR(Input);
	ASSERT_TRUE(Changed);
	EXPECT_NE(Changed.Identity, Baseline.Identity);
}

TEST(FMaterialProgramNormalizationTests,
	CodeAffectingInputsParticipateInIdentityAndRuntimeStateDoesNot)
{
	const Durin::FMaterialIRCompilerInput BaselineInput =
		MakeSyntheticMaterialCompilerInput();
	const auto Baseline = Durin::NormalizeMaterialIR(BaselineInput);
	ASSERT_TRUE(Baseline);
	const auto ExpectDifferent = [&](Durin::FMaterialIRCompilerInput Candidate) {
		const auto Result = Durin::NormalizeMaterialIR(Candidate);
		ASSERT_TRUE(Result);
		EXPECT_NE(Result.Identity, Baseline.Identity);
	};

	Durin::FMaterialIRCompilerInput ProgramChange = BaselineInput;
	const auto Constant = std::ranges::find_if(
		ProgramChange.IR.Nodes, [](const auto& Node) {
			return Node.Opcode == Durin::EMaterialProgramOpcode::Constant
				&& Node.ResultType == Durin::EMaterialProgramValueType::Float;
		});
	ASSERT_NE(Constant, ProgramChange.IR.Nodes.end());
	std::get<Durin::FMaterialProgramLiteral>(Constant->Payload).X += 0.125f;
	ExpectDifferent(std::move(ProgramChange));

	Durin::FMaterialIRCompilerInput DependencyChange = BaselineInput;
	DependencyChange.Environment.Dependencies.front().ContentHash.HashLow++;
	ExpectDifferent(std::move(DependencyChange));
	Durin::FMaterialIRCompilerInput CompilerChange = BaselineInput;
	CompilerChange.Environment.CompilerIdentity += ";revision=2";
	ExpectDifferent(std::move(CompilerChange));
	Durin::FMaterialIRCompilerInput TargetChange = BaselineInput;
	TargetChange.Environment.Target = "vulkan-spirv-1.6";
	ExpectDifferent(std::move(TargetChange));
	Durin::FMaterialIRCompilerInput PassChange = BaselineInput;
	PassChange.Environment.PassContractVersion++;
	ExpectDifferent(std::move(PassChange));

	Durin::FMaterialIRCompilerInput BlendChange = BaselineInput;
	BlendChange.StaticProperties.BlendMode =
		Durin::EMaterialBlendMode::Masked;
	ExpectDifferent(std::move(BlendChange));
	Durin::FMaterialIRCompilerInput ShadingChange = BaselineInput;
	ShadingChange.StaticProperties.ShadingModel =
		Durin::EMaterialShadingModel::Unlit;
	ExpectDifferent(std::move(ShadingChange));
	Durin::FMaterialIRCompilerInput ThresholdChange = BaselineInput;
	ThresholdChange.StaticProperties.OpacityMaskThreshold = 0.5f;
	EXPECT_EQ(Durin::NormalizeMaterialIR(ThresholdChange).Identity, Baseline.Identity);
	ThresholdChange.StaticProperties.BlendMode = Durin::EMaterialBlendMode::Masked;
	const auto MaskedIdentity = Durin::NormalizeMaterialIR(ThresholdChange).Identity;
	ThresholdChange.StaticProperties.OpacityMaskThreshold = 0.75f;
	EXPECT_NE(Durin::NormalizeMaterialIR(ThresholdChange).Identity, MaskedIdentity);
	ThresholdChange.StaticProperties.OpacityMaskThreshold = -0.0f;
	const auto ZeroIdentity = Durin::NormalizeMaterialIR(ThresholdChange).Identity;
	ThresholdChange.StaticProperties.OpacityMaskThreshold = 0.0f;
	EXPECT_EQ(Durin::NormalizeMaterialIR(ThresholdChange).Identity, ZeroIdentity);

	Durin::FMaterialIRCompilerInput RuntimeOnly = BaselineInput;
	RuntimeOnly.StaticProperties.bTwoSided = true;
	RuntimeOnly.StaticProperties.DepthWritePolicy =
		Durin::EMaterialDepthWritePolicy::Enabled;
	const auto RuntimeOnlyResult =
		Durin::NormalizeMaterialIR(RuntimeOnly);
	ASSERT_TRUE(RuntimeOnlyResult);
	EXPECT_EQ(RuntimeOnlyResult.Identity, Baseline.Identity);

	Durin::FMaterialIRCompilerInput Invalid = BaselineInput;
	Invalid.Environment.Dependencies.push_back(
		Invalid.Environment.Dependencies.front());
	const auto InvalidResult = Durin::NormalizeMaterialIR(Invalid);
	EXPECT_FALSE(InvalidResult);
	ASSERT_FALSE(InvalidResult.Diagnostics.empty());
	EXPECT_EQ(InvalidResult.Diagnostics.front().Category,
		Durin::EMaterialProgramDiagnosticCategory::Normalization);
}

TEST(FMaterialProgramCompilerTests,
	CanonicalIRGeneratesStableBoundedSourceAndCompleteStages)
{
	InitializeDObjectSystem();
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::FMaterialIRCompilerInput Input = MakeSyntheticMaterialCompilerInput();
	std::string EnvironmentError;
	ASSERT_TRUE(Durin::BuildDefaultMaterialCompilerEnvironment(
		Input.Environment, EnvironmentError)) << EnvironmentError;
	ASSERT_EQ(Input.Environment.Dependencies.size(), 1u);
	EXPECT_EQ(Input.Environment.Dependencies.front().VirtualPath,
		"/Engine/MaterialCompilerEnvironment");
	EXPECT_FALSE(Input.Environment.Dependencies.front().ContentHash.IsZero());
	const auto Normalized = Durin::NormalizeMaterialIR(Input);
	ASSERT_TRUE(Normalized);
	std::string FirstSource;
	std::string SecondSource;
	std::string Error;
	ASSERT_TRUE(Durin::GenerateMaterialProgramSlang(
		Normalized.IR, FirstSource, Error)) << Error;
	ASSERT_TRUE(Durin::GenerateMaterialProgramSlang(
		Normalized.IR, SecondSource, Error)) << Error;
	EXPECT_EQ(FirstSource, SecondSource);
	EXPECT_LE(FirstSource.size(), Durin::MaterialProgramMaxCanonicalBytes);
	EXPECT_NE(FirstSource.find("module DurinGeneratedMaterial"),
		std::string::npos);
	EXPECT_EQ(FirstSource.find(Input.Environment.Dependencies.front().VirtualPath),
		std::string::npos);

	const Durin::FMaterialCompilerResult Compiled =
		Durin::CompileMaterialIR(Input, true);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty()
		? "missing diagnostic"
		: Compiled.Diagnostics.front().Message);
	EXPECT_EQ(Compiled.Identity, Normalized.Identity);
	ASSERT_EQ(Compiled.CompiledShaders.size(), 3u);
	EXPECT_EQ(Compiled.CompiledShaders[0].Reflection.ResourceBindings.size(), 24u);
	EXPECT_EQ(Compiled.CompiledShaders[1].Reflection.ResourceBindings.size(), 17u);
	EXPECT_TRUE(Compiled.CompiledShaders[2].Reflection.ResourceBindings.empty());
	std::vector CorruptedStages = Compiled.CompiledShaders;
	CorruptedStages[1].Reflection.ResourceBindings.back().BindingIndex = 99;
	std::string ReflectionError;
	EXPECT_FALSE(Durin::ValidateMaterialCompiledStages(
		CorruptedStages, Compiled.Layout));
	const Durin::FMaterialCompilerResult Warm =
		Durin::CompileMaterialIR(Input);
	ASSERT_TRUE(Warm) << (Warm.Diagnostics.empty()
		? "missing diagnostic" : Warm.Diagnostics.front().Message);
	ASSERT_EQ(Warm.CompiledShaders.size(), Compiled.CompiledShaders.size());
	uint64 SpirvBytes = 0;
	for (size_t Index = 0; Index < Compiled.CompiledShaders.size(); ++Index)
	{
		EXPECT_EQ(Warm.CompiledShaders[Index].Hash,
			Compiled.CompiledShaders[Index].Hash);
		ASSERT_TRUE(Compiled.CompiledShaders[Index].Code);
		SpirvBytes += Compiled.CompiledShaders[Index].Code->size();
	}
	Durin::FByteBuffer CookedBytes;
	ASSERT_TRUE(Durin::EncodeMaterialCookedProgram(Compiled, {},
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game, CookedBytes, Error)) << Error;
	RecordProperty("GeneratedSourceBytes", Compiled.GeneratedSource.size());
	RecordProperty("DependencyCount", Compiled.Dependencies.size());
	RecordProperty("SpirvBytes", SpirvBytes);
	RecordProperty("NormalizationMicroseconds",
		Compiled.Timings.NormalizationMicroseconds);
	RecordProperty("GenerationMicroseconds",
		Compiled.Timings.GenerationMicroseconds);
	RecordProperty("ColdCompilationMicroseconds",
		Compiled.Timings.CompilationMicroseconds);
	RecordProperty("WarmCompilationMicroseconds",
		Warm.Timings.CompilationMicroseconds);
	std::cout << "[MaterialIRCompilerBaseline] input_ir_nodes="
		<< Input.IR.Nodes.size()
		<< " ir_nodes=" << Normalized.IR.Nodes.size()
		<< " texture_samples=" << std::ranges::count_if(
			Normalized.IR.Nodes, [](const Durin::FMaterialIRNode& Node) {
				return Node.Opcode == Durin::EMaterialProgramOpcode::TextureSample2D;
			})
		<< " canonical_bytes=" << Normalized.CanonicalBytes.size()
		<< " generated_bytes=" << Compiled.GeneratedSource.size()
		<< " source_hash="
		<< Durin::FXxHash128::HashBuffer(Compiled.GeneratedSource).ToString()
		<< " identity=" << Compiled.Identity.Digest.ToString()
		<< " dependencies=" << Compiled.Dependencies.size()
		<< " spirv_bytes=" << SpirvBytes
		<< " cooked_bytes=" << CookedBytes.size()
		<< " normalize_us=" << Compiled.Timings.NormalizationMicroseconds
		<< " generate_us=" << Compiled.Timings.GenerationMicroseconds
		<< " cold_compile_us=" << Compiled.Timings.CompilationMicroseconds
		<< " warm_compile_us=" << Warm.Timings.CompilationMicroseconds << '\n';

	Durin::FMaterialIR InvalidIR = Normalized.IR;
	InvalidIR.Version++;
	std::string InvalidSource;
	EXPECT_FALSE(Durin::GenerateMaterialProgramSlang(
		InvalidIR, InvalidSource, Error));
	EXPECT_TRUE(InvalidSource.empty());
	Durin::FMaterialIRCompilerInput InvalidInput = Input;
	InvalidInput.Environment.Target.clear();
	const auto Failed = Durin::CompileMaterialIR(InvalidInput);
	EXPECT_FALSE(Failed);
	EXPECT_TRUE(Failed.CompiledShaders.empty());
	EXPECT_FALSE(Failed.Diagnostics.empty());
}

TEST(FMaterialProgramPublicationTests,
	BasePublishesCompleteProgramAndInstancesReuseDynamicIdentity)
{
	InitializeDObjectSystem();
	auto* Base = MakeExpandedMaterial("CompiledProgramBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(
		nullptr, "CompiledProgramInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	const auto Initial = Base->GetRenderData();
	ASSERT_TRUE(Initial.CompiledProgram);
	ASSERT_TRUE(Initial.PlanningPassIdentity.ShaderMap.ProgramIdentity.IsValid());
	EXPECT_EQ(Initial.CompiledProgram, Instance->GetAcceptedCompiledProgram());
	EXPECT_EQ(Initial.CompiledProgram,
		Instance->GetRenderData().CompiledProgram);

	ASSERT_TRUE(Base->SetScalarParameterValue(
		Durin::MaterialParameters::MetallicName(), 0.73f));
	const auto Dynamic = Base->GetRenderData();
	EXPECT_EQ(Dynamic.CompiledProgram, Initial.CompiledProgram);
	EXPECT_EQ(Dynamic.PlanningPassIdentity.ShaderMap.ProgramIdentity,
		Initial.PlanningPassIdentity.ShaderMap.ProgramIdentity);

	Durin::FMaterialStaticProperties PipelineOnly = Base->GetStaticProperties();
	PipelineOnly.bTwoSided = true;
	PipelineOnly.DepthWritePolicy = Durin::EMaterialDepthWritePolicy::Enabled;
	ASSERT_TRUE(Base->SetStaticProperties(PipelineOnly));
	const auto PipelineChanged = Base->GetRenderData();
	EXPECT_EQ(PipelineChanged.CompiledProgram, Initial.CompiledProgram);
	EXPECT_NE(PipelineChanged.PlanningPassIdentity,
		Initial.PlanningPassIdentity);

	Durin::FMaterialStaticProperties ShaderProperties = PipelineOnly;
	ShaderProperties.BlendMode = Durin::EMaterialBlendMode::Masked;
	ASSERT_TRUE(Base->SetStaticProperties(ShaderProperties));
	const auto ShaderChanged = Base->GetRenderData();
	ASSERT_TRUE(ShaderChanged.CompiledProgram);
	EXPECT_NE(ShaderChanged.CompiledProgram, Initial.CompiledProgram);
	EXPECT_NE(ShaderChanged.PlanningPassIdentity.ShaderMap.ProgramIdentity,
		Initial.PlanningPassIdentity.ShaderMap.ProgramIdentity);

	auto Edited = CaptureMaterialExpressions(*Base);
	Edited.Outputs.RoughnessDefault += 0.01f;
	auto Validation = Edited.Apply(*Base);
	ASSERT_TRUE(Validation);
	EXPECT_EQ(Base->GetRenderData().PlanningPassIdentity.ShaderMap.ProgramIdentity,
		ShaderChanged.PlanningPassIdentity.ShaderMap.ProgramIdentity);
	Edited = CaptureMaterialExpressions(*Base);
	Edited.Outputs.Roughness = {};
	ASSERT_TRUE(Edited.Apply(*Base));
	const auto ProgramChanged = Base->GetRenderData();
	ASSERT_TRUE(ProgramChanged.CompiledProgram);
	EXPECT_NE(ProgramChanged.PlanningPassIdentity.ShaderMap.ProgramIdentity,
		ShaderChanged.PlanningPassIdentity.ShaderMap.ProgramIdentity);
	EXPECT_EQ(ProgramChanged.CompiledProgram,
		Instance->GetAcceptedCompiledProgram());

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialTests, ReflectedPositionalMaterialOverrideUsesSharedTransactions)
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

TEST(FMaterialTests, PositionalMaterialOverridesResolveDefaultsAndSurviveMeshSwitches)
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

TEST(FMaterialTests, ReflectedParameterEditCoalescesAndInvalidatesRenderDataAcrossUndoRedo)
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

TEST(FMaterialTests, ReflectedPropertyViewTracksPresentedOwnerSeparatelyFromEditTarget)
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

TEST(FMaterialTests, ReflectedPropertyViewTracksMaterialOverrideStructureInSharedHistory)
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

TEST(FMaterialTests, ReflectedPropertyOverridesValidateAndRestoreIndependentIntent)
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

TEST(FMaterialTests, ParentTransactionsRenderFromCurrentCanonicalStorage)
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

TEST(FMaterialTests, ProductionClassDefaultsMatchFreshOrdinaryObjectGraphs)
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


TEST(FMaterialProgramCompilerTests, NormalizedIRRecompilationPreservesSourceAndStages)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto Input = MakeSyntheticMaterialCompilerInput();
	std::string Error;
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment, Error)) << Error;
	const auto Baseline = CompileMaterialIR(Input);
	ASSERT_TRUE(Baseline);
	FMaterialIRCompilerInput Direct{.IR = Baseline.IR, .Parameters = Input.Parameters,
		.StaticProperties = Input.StaticProperties, .Environment = Input.Environment};
	const auto Compiled = CompileMaterialIR(Direct);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "Missing diagnostic" : Compiled.Diagnostics.front().Message);
	EXPECT_EQ(Compiled.GeneratedSource, Baseline.GeneratedSource);
	EXPECT_EQ(Compiled.Layout, Baseline.Layout);
	ASSERT_EQ(Compiled.CompiledShaders.size(), Baseline.CompiledShaders.size());
	for (size_t Index = 0; Index < Compiled.CompiledShaders.size(); ++Index)
	{
		EXPECT_EQ(Compiled.CompiledShaders[Index].Frequency, Baseline.CompiledShaders[Index].Frequency);
		EXPECT_EQ(Compiled.CompiledShaders[Index].SourceEntryPoint, Baseline.CompiledShaders[Index].SourceEntryPoint);
		EXPECT_EQ(Compiled.CompiledShaders[Index].Hash, Baseline.CompiledShaders[Index].Hash);
	}
}

TEST(FMaterialProgramCompilerTests, DirectIRNormalizationPrunesDeadCodeAndRemapsSourcesDeterministically)
{
	using namespace Durin;
	FMaterialIRCompilerInput Input;
	Input.Environment = MakeSyntheticMaterialCompilerInput().Environment;
	for (uint32 Index = 0; Index < Input.IR.SurfaceRoot.Inputs.size(); ++Index)
		Input.IR.SurfaceRoot.Inputs[Index].Type = GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Index));
	Input.IR.Nodes = {
		{.Opcode = EMaterialProgramOpcode::Constant, .ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{.25f}},
		{.Opcode = EMaterialProgramOpcode::Constant, .ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{.5f}},
		{.Opcode = EMaterialProgramOpcode::Add, .ResultType = EMaterialProgramValueType::Float, .Inputs = {0, 1}},
		{.Opcode = EMaterialProgramOpcode::Splat3, .ResultType = EMaterialProgramValueType::Float3, .Inputs = {2}},
		{.Opcode = EMaterialProgramOpcode::Constant, .ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{9.f}}};
	Input.IR.SurfaceRoot.Inputs[0].bExpression = true;
	Input.IR.SurfaceRoot.Inputs[0].ExpressionIndex = 3;
	Input.Sources = {{.ExpressionIndex = 0, .NodeId = {1, 0, 0, 1}}, {.ExpressionIndex = 1, .NodeId = {1, 0, 0, 2}},
		{.ExpressionIndex = 4, .NodeId = {1, 0, 0, 3}}};
	for (uint32 Index = 0; Index < 32; ++Index) Input.Parameters.push_back({{2, 0, 0, Index + 1}, EMaterialParameterType::Texture});
	const auto Baseline = NormalizeMaterialIR(Input);
	ASSERT_TRUE(Baseline);
	EXPECT_EQ(Baseline.IR.Nodes.size(), 4u);
	EXPECT_EQ(Baseline.Sources.size(), 2u);
	EXPECT_TRUE(Baseline.ActiveParameters.empty());
	std::swap(Input.IR.Nodes[0], Input.IR.Nodes[1]);
	Input.IR.Nodes[2].Inputs = {1, 0};
	Input.Sources[0].ExpressionIndex = 1; Input.Sources[1].ExpressionIndex = 0;
	Input.IR.SurfaceRoot.Inputs[0].Literal = {42, 43, 44};
	std::get<FMaterialProgramLiteral>(Input.IR.Nodes[0].Payload).W = 88;
	std::get<FMaterialProgramLiteral>(Input.IR.Nodes[4].Payload).X = 99;
	const auto Reordered = NormalizeMaterialIR(Input);
	ASSERT_TRUE(Reordered);
	EXPECT_EQ(Reordered.Identity, Baseline.Identity);
	EXPECT_EQ(Reordered.IR, Baseline.IR);
	EXPECT_EQ(Reordered.CanonicalBytes, Baseline.CanonicalBytes);
	for (const auto& Source : Baseline.Sources)
	{
		const auto Found = std::ranges::find(Reordered.Sources, Source.NodeId, &FMaterialExpressionSource::NodeId);
		ASSERT_NE(Found, Reordered.Sources.end()); EXPECT_EQ(Found->ExpressionIndex, Source.ExpressionIndex);
	}
	Input.IR.Nodes[4].Payload = FMaterialProgramLiteral{std::numeric_limits<float>::quiet_NaN()};
	EXPECT_FALSE(NormalizeMaterialIR(Input));
}

TEST(FMaterialProgramCompilerTests, DetachedIRRejectsMalformedInputsWithoutAuthoredGraphReconstruction)
{
	using namespace Durin;
	const auto Layout = CompileMaterialLayout({});
	ASSERT_TRUE(Layout);
	FMaterialIR IR;
	for (uint32 Index = 0; Index < IR.SurfaceRoot.Inputs.size(); ++Index)
		IR.SurfaceRoot.Inputs[Index].Type = GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Index));
	IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Constant,
		.ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{.25f}});
	IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Saturate,
		.ResultType = EMaterialProgramValueType::Float, .Inputs = {0}});
	IR.SurfaceRoot.Inputs[3].bExpression = true;
	IR.SurfaceRoot.Inputs[3].ExpressionIndex = 1;
	ASSERT_TRUE(GenerateMaterialProgramSlang(IR, Layout.Layout));
	const auto Good = IR;
	const auto Reject = [&] {
		const auto Result = GenerateMaterialProgramSlang(IR, Layout.Layout);
		EXPECT_FALSE(Result);
		EXPECT_TRUE(Result.Source.empty());
		EXPECT_FALSE(Result.Diagnostics.empty());
		IR = Good;
	};
	IR.Nodes[1].Inputs = {1}; Reject();
	IR.Nodes[1].Inputs = {0xffffffffu}; Reject();
	IR.Nodes[1].Inputs.clear(); Reject();
	IR.Nodes[1].Opcode = EMaterialProgramOpcode::TextureCoordinates; Reject();
	IR.Nodes[0].Payload = std::monostate{}; Reject();
	IR.Nodes[1].Payload = FGuid{1, 2, 3, 4}; Reject();
	IR.Nodes[0].ResultType = EMaterialProgramValueType::Float2; Reject();
	IR.Nodes[0].Payload = FMaterialProgramLiteral{std::numeric_limits<float>::infinity()}; Reject();
	IR.Nodes[0].Opcode = EMaterialProgramOpcode::Parameter;
	IR.Nodes[0].Payload = FGuid{1, 2, 3, 4}; Reject();
	IR.Nodes[1].Opcode = EMaterialProgramOpcode::Swizzle;
	IR.Nodes[1].Payload = FMaterialIRSwizzle{1, {1}}; Reject();
	for (uint32 Index = 2; Index <= MaterialProgramMaxDepth; ++Index)
		IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Saturate,
			.ResultType = EMaterialProgramValueType::Float, .Inputs = {Index - 1}});
	Reject();
	IR.SurfaceRoot.Inputs[2].Literal.X = std::numeric_limits<float>::quiet_NaN(); Reject();
	IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Constant,
		.ResultType = EMaterialProgramValueType::Float3, .Payload = FMaterialProgramLiteral{}});
	for (uint32 Index = 0; Index < MaterialFunctionMaxExpandedLinks / 8; ++Index)
		IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::MakeSurface,
			.ResultType = EMaterialProgramValueType::Surface, .Inputs = {2, 2, 0, 0, 0, 2, 0, 0}});
	Reject();
}

TEST(FMaterialProgramCompilerTests, CompiledLayoutsAreTypedDeterministicAndDeviceBounded)
{
	using namespace Durin;
	std::vector<FMaterialCompilerParameterDeclaration> Parameters{
		{{0, 0, 0, 5}, EMaterialParameterType::Texture},
		{{0, 0, 0, 4}, EMaterialParameterType::Vector4},
		{{0, 0, 0, 3}, EMaterialParameterType::Vector},
		{{0, 0, 0, 2}, EMaterialParameterType::Vector2},
		{{0, 0, 0, 1}, EMaterialParameterType::Scalar}};
	const auto Built = CompileMaterialLayout(Parameters);
	ASSERT_TRUE(Built);
	EXPECT_TRUE(ValidateCompiledMaterialLayout(Built.Layout));
	EXPECT_EQ(Built.Layout.Identity.Version, 4u);
	EXPECT_EQ(Built.Layout.UniformPayloadSize, 80u);
	EXPECT_EQ(Built.Layout.UniformFieldCount, 4u);
	EXPECT_EQ(Built.Layout.ResourceFieldCount, 1u);
	for (uint32 Index = 0; Index < 4; ++Index)
	{
		EXPECT_EQ(Built.Layout.Fields[Index].Offset, 16u * (Index + 1));
		EXPECT_EQ(Built.Layout.Fields[Index].Size, 4u * (Index + 1));
	}
	std::ranges::reverse(Parameters);
	EXPECT_EQ(CompileMaterialLayout(Parameters).Layout, Built.Layout);
	auto Broken = Built.Layout;
	Broken.Fields[1].Offset = Broken.Fields[0].Offset;
	EXPECT_EQ(ValidateCompiledMaterialLayout(Broken).Error, EMaterialLayoutError::InvalidField);
	Broken = Built.Layout;
	Broken.Fields[1].Type = EMaterialRenderValueType::Vector4;
	EXPECT_FALSE(ValidateCompiledMaterialLayout(Broken));
	Broken = Built.Layout;
	Broken.Identity.Id = FGuid::NewGuid();
	EXPECT_EQ(ValidateCompiledMaterialLayout(Broken).Error, EMaterialLayoutError::InvalidIdentity);
	Parameters.push_back(Parameters.front());
	EXPECT_EQ(CompileMaterialLayout(Parameters).Validation.Error, EMaterialLayoutError::DuplicateParameter);
	Parameters.pop_back();
	FMaterialCompilerResourceLimits Limits;
	Limits.Samplers = 2;
	EXPECT_EQ(CompileMaterialLayout(Parameters, Limits).Validation.Error, EMaterialLayoutError::ResourceLimit);
	Limits = {};
	Limits.UniformBufferBytes = 64;
	EXPECT_EQ(CompileMaterialLayout(Parameters, Limits).Validation.Error, EMaterialLayoutError::ResourceLimit);
	Parameters.clear();
	for (uint32 Index = 0; Index < MaterialMaxParameterDefinitionCount; ++Index)
		Parameters.push_back({{0, 0, 0, Index + 1}, EMaterialParameterType::Vector4});
	EXPECT_TRUE(CompileMaterialLayout(Parameters));
	Parameters.push_back({FGuid::NewGuid(), EMaterialParameterType::Scalar});
	EXPECT_FALSE(CompileMaterialLayout(Parameters));
}

TEST(FMaterialProgramCompilerTests, CustomNumericTextureAndResourceFreeProgramsCompileWithTheirLayouts)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FMaterialIRCompilerInput Input;
	Input.IR = MakeDefaultMaterialCompilerIR();
	std::string Error;
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment, Error)) << Error;
	Input.StaticProperties.BlendMode = EMaterialBlendMode::Masked;
	const FGuid Tint{0, 0, 1, 1}, UV{0, 0, 1, 2}, Texture{0, 0, 1, 3}, Amount{0, 0, 1, 4};
	Input.Parameters = {{Tint, EMaterialParameterType::Vector4}, {UV, EMaterialParameterType::Vector2},
		{Texture, EMaterialParameterType::Texture}, {Amount, EMaterialParameterType::Scalar}};
	auto Add = [&](EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
		FGuid Parameter, std::vector<uint32> Links = {}) {
		FMaterialIRNode Node{.Opcode = Opcode, .ResultType = Type, .Inputs = std::move(Links)};
		if (Parameter.IsValid()) Node.Payload = Parameter;
		const auto Index = static_cast<uint32>(Input.IR.Nodes.size());
		Input.IR.Nodes.push_back(std::move(Node));
		return Index;
	};
	const auto TintValue = Add(EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float4, Tint);
	const auto UVValue = Add(EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float2, UV);
	const auto TextureValue = Add(EMaterialProgramOpcode::TextureParameter, EMaterialProgramValueType::Texture2D, Texture);
	const auto Sample = Add(EMaterialProgramOpcode::TextureSample2D, EMaterialProgramValueType::Float4, {}, {TextureValue, UVValue});
	const auto Product = Add(EMaterialProgramOpcode::Multiply, EMaterialProgramValueType::Float4, {}, {TintValue, Sample});
	Input.IR.SurfaceRoot.Inputs[0].bExpression = true;
	Input.IR.SurfaceRoot.Inputs[0].ExpressionIndex = Add(EMaterialProgramOpcode::TruncateToFloat3, EMaterialProgramValueType::Float3, {}, {Product});
	Input.IR.SurfaceRoot.Inputs[7].bExpression = true;
	Input.IR.SurfaceRoot.Inputs[7].ExpressionIndex = Add(EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float, Amount);
	const auto Compiled = CompileMaterialIR(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "missing diagnostic" : Compiled.Diagnostics.front().Message);
	EXPECT_EQ(Compiled.Layout.Identity.Version, 4u);
	EXPECT_EQ(Compiled.Layout.Fields.size(), 4u);
	EXPECT_EQ(Compiled.Layout.ResourceFieldCount, 1u);
	EXPECT_TRUE(ValidateMaterialCompiledStages(Compiled.CompiledShaders, Compiled.Layout));
	EXPECT_TRUE(ValidateMaterialCompilerResult(Compiled));
	FByteBuffer CookedBytes;
	ASSERT_TRUE(EncodeMaterialCookedProgram(Compiled, Input.StaticProperties,
		ECookTargetPlatform::Win64, ECookTargetProfile::Game, CookedBytes, Error)) << Error;
	FMaterialStaticProperties CookedProperties;
	std::shared_ptr<const FMaterialCompilerResult> Cooked;
	ASSERT_TRUE(DecodeMaterialCookedProgram(CookedBytes, ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, CookedProperties, Cooked, Error)) << Error;
	ASSERT_NE(Cooked, nullptr);
	EXPECT_EQ(Cooked->Layout, Compiled.Layout);
	EXPECT_EQ(Cooked->ActiveParameters, Compiled.ActiveParameters);
	EXPECT_TRUE(Cooked->IR.Nodes.empty());
	EXPECT_TRUE(Cooked->GeneratedSource.empty());
	FByteBuffer Reencoded;
	ASSERT_TRUE(EncodeMaterialCookedProgram(*Cooked, CookedProperties,
		ECookTargetPlatform::Win64, ECookTargetProfile::Game, Reencoded, Error));
	EXPECT_EQ(Reencoded, CookedBytes);
	for (uint32 Mutation = 0; Mutation < 5; ++Mutation)
	{
		auto Invalid = Compiled;
		if (Mutation == 0) Invalid.Layout.Fields.front().Offset += 4;
		if (Mutation == 1) Invalid.Layout.Identity.Id = FGuid::NewGuid();
		if (Mutation == 2) Invalid.Layout.UniformPayloadSize += 16;
		if (Mutation == 3) Invalid.ActiveParameters.pop_back();
		if (Mutation == 4) Invalid.CompiledShaders.front().BinaryEntryPoint.clear();
		EXPECT_FALSE(EncodeMaterialCookedProgram(Invalid, Input.StaticProperties,
			ECookTargetPlatform::Win64, ECookTargetProfile::Game, Reencoded, Error));
	}
	const auto AcceptedCooked = Cooked;
	for (size_t Position : {size_t{4}, CookedBytes.size() / 2, CookedBytes.size() - 1})
	{
		auto Broken = CookedBytes; Broken[Position] ^= std::byte{1};
		EXPECT_FALSE(DecodeMaterialCookedProgram(Broken, ECookTargetPlatform::Win64,
			ECookTargetProfile::Game, CookedProperties, Cooked, Error));
		EXPECT_EQ(Cooked, AcceptedCooked);
	}
	auto LegacyCooked = CookedBytes; LegacyCooked[4] = std::byte{3};
	EXPECT_FALSE(DecodeMaterialCookedProgram(LegacyCooked, ECookTargetPlatform::Win64,
		ECookTargetProfile::Game, CookedProperties, Cooked, Error));
	EXPECT_NE(Error.find("recook"), std::string::npos);

	auto InvalidResult = Compiled;
	InvalidResult.ActiveParameters.pop_back();
	EXPECT_FALSE(ValidateMaterialCompilerResult(InvalidResult));
	InvalidResult = Compiled;
	InvalidResult.CompiledShaders.pop_back();
	EXPECT_FALSE(ValidateMaterialCompilerResult(InvalidResult));
	InvalidResult = Compiled;
	InvalidResult.bSucceeded = false;
	EXPECT_FALSE(ValidateMaterialCompilerResult(InvalidResult));
	EXPECT_NE(Compiled.GeneratedSource.find("MaterialTexture0.Sample(MaterialSampler0"), std::string::npos);
	EXPECT_EQ(Compiled.GeneratedSource.find("GetMaterialUV"), std::string::npos);
	ReorderIndependentMaterialIRNodes(Input);
	std::ranges::reverse(Input.Parameters);
	const auto Reordered = NormalizeMaterialIR(Input);
	ASSERT_TRUE(Reordered);
	EXPECT_EQ(Reordered.Identity, Compiled.Identity);
	EXPECT_EQ(Reordered.Layout, Compiled.Layout);
	auto Corrupted = Compiled.CompiledShaders;
	ASSERT_FALSE(Corrupted.front().Reflection.ResourceBindings.empty());
	Corrupted.front().Reflection.ResourceBindings.front().Type = ERHIBindingType::StorageBuffer;
	EXPECT_FALSE(ValidateMaterialCompiledStages(Corrupted, Compiled.Layout));
	Corrupted = Compiled.CompiledShaders;
	Corrupted.front().Reflection.ResourceBindings.push_back(Corrupted.front().Reflection.ResourceBindings.front());
	EXPECT_FALSE(ValidateMaterialCompiledStages(Corrupted, Compiled.Layout));
	Corrupted.pop_back();
	EXPECT_FALSE(ValidateMaterialCompiledStages(Corrupted, Compiled.Layout));
	auto InvalidIR = Compiled.IR;
	InvalidIR.Nodes.front().Inputs = {0xffffffffu};
	EXPECT_FALSE(GenerateMaterialProgramSlang(InvalidIR, Compiled.Layout));

	Input.IR = MakeDefaultMaterialCompilerIR();
	Input.Parameters.clear();
	Input.StaticProperties = {.ShadingModel = EMaterialShadingModel::Unlit};
	const auto ResourceFree = CompileMaterialIR(Input);
	ASSERT_TRUE(ResourceFree) << (ResourceFree.Diagnostics.empty() ? "missing diagnostic" : ResourceFree.Diagnostics.front().Message);
	EXPECT_TRUE(ResourceFree.Layout.Fields.empty());
	for (const auto& Stage : ResourceFree.CompiledShaders)
		EXPECT_TRUE(Stage.Reflection.ResourceBindings.empty()) << Stage.SourceEntryPoint;
}

TEST(FMaterialProgramCompilerTests, ExplicitUVAndSurfaceCompositionUseOnlyAuthoredInputs)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FMaterialIRCompilerInput Input;
	Input.IR = MakeDefaultMaterialCompilerIR();
	std::string Error;
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment, Error)) << Error;
	const FGuid Channel = FGuid::NewGuid(), Angle = FGuid::NewGuid();
	Input.Parameters = {{Channel, EMaterialParameterType::Scalar}, {Angle, EMaterialParameterType::Scalar}};
	auto Add = [&](EMaterialProgramOpcode Op, EMaterialProgramValueType Type,
		std::vector<uint32> Links = {}, FGuid Parameter = {}, FMaterialProgramLiteral Literal = {}) {
		FMaterialIRNode Node{.Opcode = Op, .ResultType = Type, .Inputs = std::move(Links)};
		if (Op == EMaterialProgramOpcode::Constant) Node.Payload = Literal;
		else if (Parameter.IsValid()) Node.Payload = Parameter;
		const auto Index = static_cast<uint32>(Input.IR.Nodes.size());
		Input.IR.Nodes.push_back(std::move(Node));
		return Index;
	};
	const auto ChannelValue = Add(EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float, {}, Channel);
	const auto AngleValue = Add(EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float, {}, Angle);
	const auto UV = Add(EMaterialProgramOpcode::UVChannel, EMaterialProgramValueType::Float2, {ChannelValue});
	const auto Sin = Add(EMaterialProgramOpcode::Sine, EMaterialProgramValueType::Float, {AngleValue});
	const auto Cos = Add(EMaterialProgramOpcode::Cosine, EMaterialProgramValueType::Float, {AngleValue});
	const auto Factor = Add(EMaterialProgramOpcode::MakeFloat2, EMaterialProgramValueType::Float2, {Sin, Cos});
	const auto Product = Add(EMaterialProgramOpcode::Multiply, EMaterialProgramValueType::Float2, {UV, Factor});
	const auto X = Add(EMaterialProgramOpcode::TruncateToFloat, EMaterialProgramValueType::Float, {Product});
	const auto Color = Add(EMaterialProgramOpcode::MakeFloat3, EMaterialProgramValueType::Float3, {X, Sin, Cos});
	const auto Normal = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float3, {}, {}, {0, 0, 1, 0});
	const auto Zero = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float);
	const auto One = Add(EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float, {}, {}, {1, 0, 0, 0});
	Input.IR.SurfaceRoot.bAggregate = true;
	Input.IR.SurfaceRoot.AggregateExpressionIndex = Add(EMaterialProgramOpcode::MakeSurface, EMaterialProgramValueType::Surface,
		{Color, Normal, Zero, One, One, Color, One, One});
	const auto Compiled = CompileMaterialIR(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "missing diagnostic" : Compiled.Diagnostics.front().Message);
	ASSERT_TRUE(ValidateMaterialCompilerResult(Compiled));
	EXPECT_EQ(Compiled.Layout.Fields.size(), 2u);
	EXPECT_EQ(Compiled.Layout.ResourceFieldCount, 0u);
	EXPECT_NE(Compiled.GeneratedSource.find("SelectAuthoredUV(input,"), std::string::npos);
	EXPECT_EQ(Compiled.GeneratedSource.find("GetMaterialUV"), std::string::npos);
	EXPECT_EQ(Compiled.GeneratedSource.find("EvaluateStandardSurface"), std::string::npos);
	Input.IR.Nodes.back().Inputs.pop_back();
	EXPECT_FALSE(NormalizeMaterialIR(Input));
	Input.IR.Nodes.back().Inputs.push_back(One);
	Input.IR.Nodes[2].Inputs[0] = Color;
	EXPECT_FALSE(NormalizeMaterialIR(Input));


}


TEST(FMaterialProgramSchemaTests, RetiredIROpcodesAreRejectedWithoutMutatingInput)
{
	using namespace Durin;
	const auto Original = MakeSyntheticMaterialCompilerInput();
	for (uint8 Opcode : {uint8(3), uint8(30), uint8(255)})
	{
		auto Input = Original;
		ASSERT_FALSE(Input.IR.Nodes.empty());
		Input.IR.Nodes.front().Opcode = static_cast<EMaterialProgramOpcode>(Opcode);
		const auto Before = Input.IR;
		EXPECT_FALSE(NormalizeMaterialIR(Input));
		EXPECT_EQ(Input.IR, Before);
	}
}
