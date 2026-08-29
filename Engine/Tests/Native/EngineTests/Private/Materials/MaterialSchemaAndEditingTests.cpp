#include "MaterialTestSupport.h"

#include "DObject/DefaultObjectGraph.h"
#include "DObject/MathStructs.h"
#include "Hash/XxHash.h"
#include "Materials/MaterialProgramCompiler.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "StaticMesh/StaticMeshDerivedData.h"

#include <cstring>
#include <limits>
#include <unordered_set>

namespace
{
	auto MakeSyntheticMaterialCompilerInput()
		-> Durin::FMaterialCompilerInput
	{
		Durin::FMaterialCompilerInput Input;
		Input.Program = Durin::MakeCanonicalMaterialProgram();
		for (const Durin::FMaterialParameterDefinition& Definition
			: Durin::GetCanonicalMaterialParameterDefinitions())
			Input.Parameters.push_back({Definition.Id, Definition.Type});
		std::ranges::sort(Input.Parameters, {},
			&Durin::FMaterialCompilerParameterDeclaration::Id);
		Input.Environment.CompilerIdentity =
			"slang-test-build;target=spirv;profile=spirv_1_5";
		Input.Environment.Target = "vulkan-spirv-1.5";
		Input.Environment.Dependencies = {
			{"/Engine/MaterialTemplate.slang", {11, 12}},
			{"/Engine/StaticMeshBasePass.slang", {21, 22}}};
		return Input;
	}

	auto RemapMaterialProgramNodeIds(Durin::FMaterialProgram& Program) -> void
	{
		std::unordered_map<Durin::FGuid, Durin::FGuid> Remapping;
		for (size_t Index = 0; Index < Program.Nodes.size(); ++Index)
		{
			const uint32 Reversed = static_cast<uint32>(
				Program.Nodes.size() - Index);
			Remapping.emplace(Program.Nodes[Index].Id,
				Durin::FGuid{0xf00d0001, Reversed, Reversed * 3,
					Reversed * 7});
		}
		for (Durin::FMaterialProgramNode& Node : Program.Nodes)
		{
			Node.Id = Remapping.at(Node.Id);
			for (Durin::FMaterialProgramLink& Link : Node.Inputs)
				Link.SourceNodeId = Remapping.at(Link.SourceNodeId);
		}
		std::array<Durin::FMaterialProgramLink*, 8> Outputs{
			&Program.Outputs.BaseColor, &Program.Outputs.Normal,
			&Program.Outputs.Metallic, &Program.Outputs.Roughness,
			&Program.Outputs.AmbientOcclusion, &Program.Outputs.Emissive,
			&Program.Outputs.Opacity, &Program.Outputs.OpacityMask};
		for (Durin::FMaterialProgramLink* Output : Outputs)
			Output->SourceNodeId = Remapping.at(Output->SourceNodeId);
	}

	auto MakeExpandedMaterial(const char* Name) -> Durin::DMaterial*
	{
		auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, Name);
		Durin::FMaterialProgramValidationResult Validation;
		if (!Material || !Material->SetMaterialProgram(
			Durin::MakeCanonicalMaterialProgram(), Validation)) return nullptr;
		return Material;
	}
}

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
	EXPECT_EQ(Material->FindParameterDefinition(Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value), &Definitions[42]);
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

TEST(FMaterialProgramSchemaTests,
	CanonicalProgramIsReflectedBoundedAndDeterministicallyValid)
{
	InitializeDObjectSystem();
	const Durin::FMaterialProgram First =
		Durin::MakeCanonicalMaterialProgram();
	const Durin::FMaterialProgram Second =
		Durin::MakeCanonicalMaterialProgram();
	EXPECT_EQ(First, Second);
	EXPECT_EQ(
		First.SchemaVersion,
		Durin::CurrentMaterialProgramSchemaVersion);
	EXPECT_FALSE(First.Nodes.empty());
	EXPECT_LE(First.Nodes.size(), Durin::MaterialProgramMaxNodeCount);

	std::unordered_set<Durin::FGuid> NodeIds;
	size_t LinkCount = 8;
	for (const Durin::FMaterialProgramNode& Node : First.Nodes)
	{
		EXPECT_TRUE(Node.Id.IsValid());
		EXPECT_TRUE(NodeIds.insert(Node.Id).second);
		EXPECT_LE(
			Node.Inputs.size(),
			Durin::MaterialProgramMaxNodeInputCount);
		LinkCount += Node.Inputs.size();
	}
	EXPECT_LE(LinkCount, Durin::MaterialProgramMaxLinkCount);
	const Durin::FMaterialProgramValidationResult Validation =
		Durin::ValidateMaterialProgram(
			First, Durin::GetCanonicalMaterialParameterDefinitions());
	EXPECT_TRUE(Validation);
	EXPECT_TRUE(Validation.Diagnostics.empty());

	Durin::DStruct* ProgramStruct = Durin::FMaterialProgram::StaticStruct();
	ASSERT_NE(ProgramStruct, nullptr);
	EXPECT_EQ(
		ProgramStruct->GetQualifiedName().ToString(),
		"Durin::FMaterialProgram");
	EXPECT_NE(ProgramStruct->FindPropertyByName("SchemaVersion"), nullptr);
	EXPECT_NE(ProgramStruct->FindPropertyByName("Nodes"), nullptr);
	EXPECT_NE(ProgramStruct->FindPropertyByName("Outputs"), nullptr);
	Durin::DStruct* PresentationStruct =
		Durin::FMaterialGraphPresentation::StaticStruct();
	ASSERT_NE(PresentationStruct, nullptr);
	EXPECT_NE(PresentationStruct->FindPropertyByName("bHasMaterialOutputPosition"), nullptr);
	EXPECT_NE(PresentationStruct->FindPropertyByName("MaterialOutputX"), nullptr);
	EXPECT_NE(PresentationStruct->FindPropertyByName("MaterialOutputY"), nullptr);
	EXPECT_NE(
		Durin::DMaterial::StaticClass()->FindPropertyByName("Program"),
		nullptr);
	EXPECT_EQ(
		Durin::DMaterialInstance::StaticClass()->FindPropertyByName("Program"),
		nullptr);
}

TEST(FMaterialProgramSchemaTests,
	ValidatorRejectsSchemaGraphTypeAndBoundsFailuresDeterministically)
{
	const auto Definitions =
		Durin::GetCanonicalMaterialParameterDefinitions();
	const auto ExpectCategory = [&](const Durin::FMaterialProgram& Program,
		Durin::EMaterialProgramDiagnosticCategory Category) {
		const auto Validation =
			Durin::ValidateMaterialProgram(Program, Definitions);
		EXPECT_FALSE(Validation);
		EXPECT_NE(std::ranges::find(
			Validation.Diagnostics, Category,
			&Durin::FMaterialProgramDiagnostic::Category),
			Validation.Diagnostics.end());
		return Validation;
	};

	Durin::FMaterialProgram UnknownVersion =
		Durin::MakeCanonicalMaterialProgram();
	UnknownVersion.SchemaVersion = 999;
	ExpectCategory(
		UnknownVersion,
		Durin::EMaterialProgramDiagnosticCategory::Schema);

	Durin::FMaterialProgram InvalidEnums =
		Durin::MakeCanonicalMaterialProgram();
	InvalidEnums.Nodes.front().Opcode =
		static_cast<Durin::EMaterialProgramOpcode>(0xff);
	ExpectCategory(
		InvalidEnums,
		Durin::EMaterialProgramDiagnosticCategory::Schema);

	Durin::FMaterialProgram DuplicateIdentity =
		Durin::MakeCanonicalMaterialProgram();
	DuplicateIdentity.Nodes[1].Id = DuplicateIdentity.Nodes[0].Id;
	ExpectCategory(
		DuplicateIdentity,
		Durin::EMaterialProgramDiagnosticCategory::Schema);

	Durin::FMaterialProgram Dangling =
		Durin::MakeCanonicalMaterialProgram();
	Dangling.Nodes.front().Inputs.push_back({
		.SourceNodeId = Durin::FGuid{1, 2, 3, 4},
		.SourceOutputIndex = 0});
	ExpectCategory(
		Dangling,
		Durin::EMaterialProgramDiagnosticCategory::Graph);

	Durin::FMaterialProgram WrongOutput =
		Durin::MakeCanonicalMaterialProgram();
	WrongOutput.Outputs.Metallic = WrongOutput.Outputs.BaseColor;
	const auto WrongOutputValidation = ExpectCategory(
		WrongOutput,
		Durin::EMaterialProgramDiagnosticCategory::Type);
	const auto WrongOutputDiagnostic = std::ranges::find_if(
		WrongOutputValidation.Diagnostics, [](const auto& Diagnostic) {
			return Diagnostic.LocationKind
					== Durin::EMaterialProgramDiagnosticLocationKind::SurfaceOutput
				&& Diagnostic.LocationIndex == static_cast<uint32>(
					Durin::EMaterialSurfaceOutput::Metallic);
		});
	EXPECT_NE(
		WrongOutputDiagnostic,
		WrongOutputValidation.Diagnostics.end());

	Durin::FMaterialProgram NonFinite =
		Durin::MakeCanonicalMaterialProgram();
	const auto ConstantIt = std::ranges::find(
		NonFinite.Nodes, Durin::EMaterialProgramOpcode::Constant,
		&Durin::FMaterialProgramNode::Opcode);
	ASSERT_NE(ConstantIt, NonFinite.Nodes.end());
	ConstantIt->Literal.X = std::numeric_limits<float>::infinity();
	ExpectCategory(
		NonFinite,
		Durin::EMaterialProgramDiagnosticCategory::Type);

	Durin::FMaterialProgram UnknownParameter =
		Durin::MakeCanonicalMaterialProgram();
	const auto ParameterIt = std::ranges::find(
		UnknownParameter.Nodes, Durin::EMaterialProgramOpcode::Parameter,
		&Durin::FMaterialProgramNode::Opcode);
	ASSERT_NE(ParameterIt, UnknownParameter.Nodes.end());
	ParameterIt->ParameterId = {0xbad00001, 2, 3, 4};
	ExpectCategory(
		UnknownParameter,
		Durin::EMaterialProgramDiagnosticCategory::Type);

	Durin::FMaterialProgram Cycle =
		Durin::MakeCanonicalMaterialProgram();
	Durin::FMaterialProgramNode FirstCycle;
	FirstCycle.Id = {0xc1c1e001, 1, 1, 1};
	FirstCycle.Opcode = Durin::EMaterialProgramOpcode::Negate;
	FirstCycle.ResultType = Durin::EMaterialProgramValueType::Float;
	Durin::FMaterialProgramNode SecondCycle = FirstCycle;
	SecondCycle.Id = {0xc1c1e002, 2, 2, 2};
	FirstCycle.Inputs = {{.SourceNodeId = SecondCycle.Id}};
	SecondCycle.Inputs = {{.SourceNodeId = FirstCycle.Id}};
	Cycle.Nodes.push_back(FirstCycle);
	Cycle.Nodes.push_back(SecondCycle);
	ExpectCategory(Cycle, Durin::EMaterialProgramDiagnosticCategory::Graph);

	Durin::FMaterialProgram ExcessiveDepth =
		Durin::MakeCanonicalMaterialProgram();
	Durin::FGuid Previous = ExcessiveDepth.Outputs.Metallic.SourceNodeId;
	for (uint32 Index = 0;
		Index <= Durin::MaterialProgramMaxDepth; ++Index)
	{
		Durin::FMaterialProgramNode Node;
		Node.Id = {0xde770001, 0, 0, Index + 1};
		Node.Opcode = Durin::EMaterialProgramOpcode::Negate;
		Node.ResultType = Durin::EMaterialProgramValueType::Float;
		Node.Inputs = {{.SourceNodeId = Previous}};
		Previous = Node.Id;
		ExcessiveDepth.Nodes.push_back(std::move(Node));
	}
	ExpectCategory(
		ExcessiveDepth,
		Durin::EMaterialProgramDiagnosticCategory::Bounds);

	Durin::FMaterialProgram ExcessiveNodes =
		Durin::MakeCanonicalMaterialProgram();
	while (ExcessiveNodes.Nodes.size()
		<= Durin::MaterialProgramMaxNodeCount)
	{
		Durin::FMaterialProgramNode Node;
		const uint32 Index = static_cast<uint32>(
			ExcessiveNodes.Nodes.size());
		Node.Id = {0xb01d0001, 0, 0, Index + 1};
		ExcessiveNodes.Nodes.push_back(std::move(Node));
	}
	ExpectCategory(
		ExcessiveNodes,
		Durin::EMaterialProgramDiagnosticCategory::Bounds);

	Durin::FMaterialProgram ExcessiveInputs =
		Durin::MakeCanonicalMaterialProgram();
	ExcessiveInputs.Nodes.front().Inputs.assign(
		Durin::MaterialProgramMaxNodeInputCount + 1,
		ExcessiveInputs.Outputs.Metallic);
	ExpectCategory(
		ExcessiveInputs,
		Durin::EMaterialProgramDiagnosticCategory::Bounds);

	Durin::FMaterialProgram LongName =
		Durin::MakeCanonicalMaterialProgram();
	LongName.Nodes.front().DisplayName.assign(
		Durin::MaterialProgramMaxDisplayNameBytes + 1, 'x');
	const auto Forward = ExpectCategory(
		LongName,
		Durin::EMaterialProgramDiagnosticCategory::Bounds);
	std::ranges::reverse(LongName.Nodes);
	const auto Reversed = ExpectCategory(
		LongName,
		Durin::EMaterialProgramDiagnosticCategory::Bounds);
	EXPECT_EQ(Forward.Diagnostics, Reversed.Diagnostics);
}

TEST(FMaterialProgramSchemaTests,
	BaseOwnsProgramAndInstancesShareWithoutDuplicatingIt)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(
		nullptr, "ProgramOwningBase");
	auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(
		nullptr, "ProgramSharingInstance");
	ASSERT_TRUE(Instance->SetParent(Base));
	ASSERT_NE(Base->GetMaterialProgram(), nullptr);
	EXPECT_EQ(Instance->GetMaterialProgram(), Base->GetMaterialProgram());

	Durin::FMaterialProgram Reordered = *Base->GetMaterialProgram();
	std::ranges::reverse(Reordered.Nodes);
	Durin::FMaterialProgramValidationResult Validation;
	ASSERT_TRUE(Base->SetMaterialProgram(Reordered, Validation));
	EXPECT_TRUE(Validation);
	EXPECT_EQ(*Base->GetMaterialProgram(), Reordered);
	EXPECT_EQ(Instance->GetMaterialProgram(), Base->GetMaterialProgram());

	Durin::FMaterialProgram Invalid = Reordered;
	Invalid.Outputs.BaseColorDefault.X =
		std::numeric_limits<float>::quiet_NaN();
	const Durin::FMaterialProgram Before = *Base->GetMaterialProgram();
	EXPECT_FALSE(Base->SetMaterialProgram(std::move(Invalid), Validation));
	EXPECT_FALSE(Validation);
	EXPECT_EQ(*Base->GetMaterialProgram(), Before);

	Durin::MarkAsGarbage(Instance);
	Durin::MarkAsGarbage(Base);
	Durin::CollectGarbage();
}

TEST(FMaterialProgramNormalizationTests,
	SnapshotIsDetachedAndExcludesDynamicParameterValues)
{
	InitializeDObjectSystem();
	auto* Material = Durin::NewObject<Durin::DMaterial>(
		nullptr, "CompilerSnapshotMaterial");
	Durin::FMaterialCompilerEnvironment Environment =
		MakeSyntheticMaterialCompilerInput().Environment;
	Durin::FMaterialCompilerInput Before;
	Durin::FMaterialProgramValidationResult Validation;
	ASSERT_TRUE(Durin::SnapshotMaterialCompilerInput(
		*Material, Environment, Before, Validation));
	ASSERT_TRUE(Validation);
	ASSERT_TRUE(Material->SetScalarParameterValue(
		Durin::MaterialParameters::MetallicName(), 0.87f));
	Durin::FMaterialCompilerInput After;
	ASSERT_TRUE(Durin::SnapshotMaterialCompilerInput(
		*Material, Environment, After, Validation));
	EXPECT_EQ(Before, After);
	EXPECT_EQ(Before.Program, *Material->GetMaterialProgram());
	EXPECT_EQ(Before.Parameters.size(),
		Material->GetParameterDefinitions().size());
	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialProgramNormalizationTests,
	DefaultProgramLowersEightSurfaceFallbacksWithoutAuthoredNodes)
{
	InitializeDObjectSystem();
	Durin::FMaterialCompilerInput Input = MakeSyntheticMaterialCompilerInput();
	Input.Program = Durin::MakeDefaultMaterialProgram();
	const Durin::FMaterialProgramValidationResult Validation =
		Durin::ValidateMaterialProgram(Input.Program,
			Durin::GetCanonicalMaterialParameterDefinitions());
	ASSERT_TRUE(Validation);
	const Durin::FMaterialNormalizationResult Normalized =
		Durin::NormalizeMaterialProgram(Input);
	ASSERT_TRUE(Normalized);
	EXPECT_TRUE(Input.Program.Nodes.empty());
	ASSERT_EQ(Normalized.IR.Nodes.size(), 8u);
	ASSERT_EQ(Normalized.IR.SurfaceOutputs.size(), 8u);
	EXPECT_EQ(Normalized.IR.Nodes[0].Literal,
		(Durin::FMaterialProgramLiteral{0.5f, 0.5f, 0.5f, 0.0f}));
	EXPECT_EQ(Normalized.IR.Nodes[1].Literal,
		(Durin::FMaterialProgramLiteral{0.0f, 0.0f, 1.0f, 0.0f}));
	std::string Source;
	std::string Error;
	ASSERT_TRUE(Durin::GenerateMaterialProgramSlang(
		Normalized.IR, Source, Error)) << Error;
	EXPECT_EQ(Source.find("BaseColorTexture.Sample"), std::string::npos);
	EXPECT_EQ(Source.find("NormalTexture.Sample"), std::string::npos);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	const Durin::FMaterialCompilerResult Compiled =
		Durin::CompileMaterialProgram(Input);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty()
		? std::string("no diagnostic") : Compiled.Diagnostics.front().Message);
	size_t ActiveBindings = 0;
	for (const Durin::FCompiledShader& Shader : Compiled.CompiledShaders)
		ActiveBindings += Shader.Reflection.ResourceBindings.size();
	std::cout << "[MaterialOutputDefaultBaseline] authored_nodes="
		<< Input.Program.Nodes.size()
		<< " ir_nodes=" << Normalized.IR.Nodes.size()
		<< " texture_samples=0 generated_bytes=" << Source.size()
		<< " source_hash=" << Durin::FXxHash128::HashBuffer(Source).ToString()
		<< " identity=" << Compiled.Identity.Digest.ToString()
		<< " compiled_stages=" << Compiled.CompiledShaders.size()
		<< " active_bindings=" << ActiveBindings << '\n';
}

TEST(FMaterialProgramNormalizationTests,
	EquivalentAuthoredProgramsProduceIdenticalCanonicalIdentity)
{
	const Durin::FMaterialCompilerInput BaselineInput =
		MakeSyntheticMaterialCompilerInput();
	const Durin::FMaterialNormalizationResult Baseline =
		Durin::NormalizeMaterialProgram(BaselineInput);
	ASSERT_TRUE(Baseline);
	EXPECT_FALSE(Baseline.CanonicalBytes.empty());
	EXPECT_LE(Baseline.CanonicalBytes.size(),
		Durin::MaterialProgramMaxCanonicalBytes);
	constexpr std::string_view Domain = "DurinMaterialProgramIR";
	ASSERT_GT(Baseline.CanonicalBytes.size(), Domain.size());
	EXPECT_EQ(std::memcmp(
		Baseline.CanonicalBytes.data(), Domain.data(), Domain.size()), 0);
	EXPECT_EQ(Baseline.CanonicalBytes[Domain.size()], std::byte{0});

	const auto ExpectEquivalent = [&](Durin::FMaterialCompilerInput Candidate) {
		const auto Result = Durin::NormalizeMaterialProgram(Candidate);
		ASSERT_TRUE(Result);
		EXPECT_EQ(Result.IR, Baseline.IR);
		EXPECT_EQ(Result.CanonicalBytes, Baseline.CanonicalBytes);
		EXPECT_EQ(Result.Identity, Baseline.Identity);
	};

	Durin::FMaterialCompilerInput Reordered = BaselineInput;
	std::ranges::reverse(Reordered.Program.Nodes);
	ExpectEquivalent(std::move(Reordered));

	Durin::FMaterialCompilerInput Reidentified = BaselineInput;
	RemapMaterialProgramNodeIds(Reidentified.Program);
	ExpectEquivalent(std::move(Reidentified));

	Durin::FMaterialCompilerInput PresentationOnly = BaselineInput;
	for (Durin::FMaterialProgramNode& Node : PresentationOnly.Program.Nodes)
		Node.DisplayName = "ignored presentation label";
	const auto Float3Constant = std::ranges::find_if(
		PresentationOnly.Program.Nodes, [](const auto& Node) {
			return Node.Opcode == Durin::EMaterialProgramOpcode::Constant
				&& Node.ResultType
					== Durin::EMaterialProgramValueType::Float3;
		});
	ASSERT_NE(Float3Constant, PresentationOnly.Program.Nodes.end());
	Float3Constant->Literal.W = 123.0f;
	ExpectEquivalent(std::move(PresentationOnly));

	Durin::FMaterialCompilerInput SignedZero = BaselineInput;
	const auto ZeroConstant = std::ranges::find_if(
		SignedZero.Program.Nodes, [](const auto& Node) {
			return Node.Opcode == Durin::EMaterialProgramOpcode::Constant
				&& Node.Literal.X == 0.0f;
		});
	ASSERT_NE(ZeroConstant, SignedZero.Program.Nodes.end());
	ZeroConstant->Literal.X = -0.0f;
	ExpectEquivalent(std::move(SignedZero));

	Durin::FMaterialCompilerInput Swapped = BaselineInput;
	const auto Commutative = std::ranges::find_if(
		Swapped.Program.Nodes, [](const auto& Node) {
			return (Node.Opcode == Durin::EMaterialProgramOpcode::Add
				|| Node.Opcode == Durin::EMaterialProgramOpcode::Multiply
				|| Node.Opcode == Durin::EMaterialProgramOpcode::Minimum
				|| Node.Opcode == Durin::EMaterialProgramOpcode::Maximum)
				&& Node.Inputs.size() == 2;
		});
	ASSERT_NE(Commutative, Swapped.Program.Nodes.end());
	std::swap(Commutative->Inputs[0], Commutative->Inputs[1]);
	ExpectEquivalent(std::move(Swapped));

	Durin::FMaterialCompilerInput WithDeadNode = BaselineInput;
	Durin::FMaterialProgramNode DeadNode;
	DeadNode.Id = {0xdead0001, 1, 2, 3};
	DeadNode.Opcode = Durin::EMaterialProgramOpcode::Constant;
	DeadNode.ResultType = Durin::EMaterialProgramValueType::Float;
	DeadNode.Literal.X = -0.0f;
	WithDeadNode.Program.Nodes.push_back(std::move(DeadNode));
	ExpectEquivalent(std::move(WithDeadNode));
}

TEST(FMaterialProgramNormalizationTests,
	CodeAffectingInputsParticipateInIdentityAndRuntimeStateDoesNot)
{
	const Durin::FMaterialCompilerInput BaselineInput =
		MakeSyntheticMaterialCompilerInput();
	const auto Baseline = Durin::NormalizeMaterialProgram(BaselineInput);
	ASSERT_TRUE(Baseline);
	const auto ExpectDifferent = [&](Durin::FMaterialCompilerInput Candidate) {
		const auto Result = Durin::NormalizeMaterialProgram(Candidate);
		ASSERT_TRUE(Result);
		EXPECT_NE(Result.Identity, Baseline.Identity);
	};

	Durin::FMaterialCompilerInput ProgramChange = BaselineInput;
	const auto Constant = std::ranges::find_if(
		ProgramChange.Program.Nodes, [](const auto& Node) {
			return Node.Opcode == Durin::EMaterialProgramOpcode::Constant
				&& Node.ResultType == Durin::EMaterialProgramValueType::Float;
		});
	ASSERT_NE(Constant, ProgramChange.Program.Nodes.end());
	Constant->Literal.X += 0.125f;
	ExpectDifferent(std::move(ProgramChange));

	Durin::FMaterialCompilerInput DependencyChange = BaselineInput;
	DependencyChange.Environment.Dependencies.front().ContentHash.HashLow++;
	ExpectDifferent(std::move(DependencyChange));
	Durin::FMaterialCompilerInput CompilerChange = BaselineInput;
	CompilerChange.Environment.CompilerIdentity += ";revision=2";
	ExpectDifferent(std::move(CompilerChange));
	Durin::FMaterialCompilerInput TargetChange = BaselineInput;
	TargetChange.Environment.Target = "vulkan-spirv-1.6";
	ExpectDifferent(std::move(TargetChange));
	Durin::FMaterialCompilerInput PassChange = BaselineInput;
	PassChange.Environment.PassContractVersion++;
	ExpectDifferent(std::move(PassChange));

	Durin::FMaterialCompilerInput BlendChange = BaselineInput;
	BlendChange.StaticProperties.BlendMode =
		Durin::EMaterialBlendMode::Masked;
	ExpectDifferent(std::move(BlendChange));
	Durin::FMaterialCompilerInput ShadingChange = BaselineInput;
	ShadingChange.StaticProperties.ShadingModel =
		Durin::EMaterialShadingModel::Unlit;
	ExpectDifferent(std::move(ShadingChange));
	Durin::FMaterialCompilerInput ThresholdChange = BaselineInput;
	ThresholdChange.StaticProperties.OpacityMaskThreshold = 0.5f;
	ExpectDifferent(std::move(ThresholdChange));

	Durin::FMaterialCompilerInput RuntimeOnly = BaselineInput;
	RuntimeOnly.StaticProperties.bTwoSided = true;
	RuntimeOnly.StaticProperties.DepthWritePolicy =
		Durin::EMaterialDepthWritePolicy::Enabled;
	const auto RuntimeOnlyResult =
		Durin::NormalizeMaterialProgram(RuntimeOnly);
	ASSERT_TRUE(RuntimeOnlyResult);
	EXPECT_EQ(RuntimeOnlyResult.Identity, Baseline.Identity);

	Durin::FMaterialCompilerInput Invalid = BaselineInput;
	Invalid.Environment.Dependencies.push_back(
		Invalid.Environment.Dependencies.front());
	const auto InvalidResult = Durin::NormalizeMaterialProgram(Invalid);
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
	Durin::FMaterialCompilerInput Input = MakeSyntheticMaterialCompilerInput();
	std::string EnvironmentError;
	ASSERT_TRUE(Durin::BuildDefaultMaterialCompilerEnvironment(
		Input.Environment, EnvironmentError)) << EnvironmentError;
	ASSERT_EQ(Input.Environment.Dependencies.size(), 1u);
	EXPECT_EQ(Input.Environment.Dependencies.front().VirtualPath,
		"/Engine/MaterialCompilerEnvironment");
	EXPECT_FALSE(Input.Environment.Dependencies.front().ContentHash.IsZero());
	const auto Normalized = Durin::NormalizeMaterialProgram(Input);
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
		Durin::CompileMaterialProgram(Input, true);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty()
		? "missing diagnostic"
		: Compiled.Diagnostics.front().Message);
	EXPECT_EQ(Compiled.Identity, Normalized.Identity);
	ASSERT_EQ(Compiled.CompiledShaders.size(), 3u);
	EXPECT_EQ(Compiled.CompiledShaders[0].Reflection.ResourceBindings.size(), 24u);
	EXPECT_EQ(Compiled.CompiledShaders[1].Reflection.ResourceBindings.size(), 17u);
	EXPECT_EQ(Compiled.CompiledShaders[2].Reflection.ResourceBindings.size(), 3u);
	std::vector CorruptedStages = Compiled.CompiledShaders;
	CorruptedStages[1].Reflection.ResourceBindings.back().BindingIndex = 99;
	std::string ReflectionError;
	EXPECT_FALSE(Durin::ValidateMaterialCompiledStages(
		CorruptedStages, ReflectionError));
	EXPECT_FALSE(ReflectionError.empty());
	const Durin::FMaterialCompilerResult Warm =
		Durin::CompileMaterialProgram(Input);
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
	std::cout << "[M5MaterialCompilerBaseline] authored_nodes="
		<< Input.Program.Nodes.size()
		<< " ir_nodes=" << Normalized.IR.Nodes.size()
		<< " texture_samples=" << std::ranges::count_if(
			Normalized.IR.Nodes, [](const Durin::FMaterialIRNode& Node) {
				return Node.Opcode == Durin::EMaterialProgramOpcode::TextureSample2D;
			})
		<< " generated_bytes=" << Compiled.GeneratedSource.size()
		<< " source_hash="
		<< Durin::FXxHash128::HashBuffer(Compiled.GeneratedSource).ToString()
		<< " identity=" << Compiled.Identity.Digest.ToString()
		<< " dependencies=" << Compiled.Dependencies.size()
		<< " spirv_bytes=" << SpirvBytes
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
	Durin::FMaterialCompilerInput InvalidInput = Input;
	InvalidInput.Environment.Target.clear();
	const auto Failed = Durin::CompileMaterialProgram(InvalidInput);
	EXPECT_FALSE(Failed);
	EXPECT_TRUE(Failed.CompiledShaders.empty());
	EXPECT_FALSE(Failed.Diagnostics.empty());
}

TEST(FMaterialProgramPublicationTests,
	BasePublishesCompleteProgramAndInstancesReuseDynamicIdentity)
{
	InitializeDObjectSystem();
	auto* Base = Durin::NewObject<Durin::DMaterial>(
		nullptr, "CompiledProgramBase");
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

	Durin::FMaterialProgram Edited = *Base->GetMaterialProgram();
	Edited.Outputs.RoughnessDefault.X += 0.01f;
	Durin::FMaterialProgramValidationResult Validation;
	ASSERT_TRUE(Base->SetMaterialProgram(std::move(Edited), Validation));
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
	const auto Target = MakeMaterialValueTarget(Material, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value, Durin::FName("ScalarValue"));
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
	const auto Target = MakeMaterialValueTarget(Material, Durin::MaterialParameters::GetBuiltinParameterIds(Durin::MaterialParameters::EMaterialBuiltinParameterRole::Opacity).Value, Durin::FName("ScalarValue"));
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
	Durin::DMaterial* Base = MakeExpandedMaterial("TransactionalOverrideBase");
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
