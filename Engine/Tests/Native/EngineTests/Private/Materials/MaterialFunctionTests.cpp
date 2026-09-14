#include "MaterialTestSupport.h"
#include "ExplicitMaterialProgramTestFixture.h"
#include "StandardMaterialFunctionTestFixture.h"
#include "AssetForge/Builtins/ImportedSurfaceRecipe.h"
#include "AssetForge/Builtins/PBRSurfaceMaterial.h"
#include "Materials/MaterialFunction.h"
#include "Asset/Testing.h"
#include "Asset/References.h"
#include "Asset/OfflinePreparation.h"
#include "Asset/Cook.h"
#include "AssetTools/IAssetTools.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeAssetTestSupport.h"
#include "NativeTestSupport.h"
#include "MaterialGraphDocument.h"
#include "MaterialFunctionPreview.h"
#include "MaterialEditorModule.h"
#include "Widgets/MMaterialFunctionEditor.h"
#include "Editor/WorkspaceManager.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Modules/ModuleTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"

namespace
{
	auto FunctionPort(uint32 Id, Durin::EMaterialProgramValueType Type, std::string Name)
		-> Durin::FMaterialFunctionPort
	{
		return {.Id = {0xa04759c1, 1, 2, Id}, .Type = Type, .Name = std::move(Name)};
	}

	auto AddFunctionCall(Durin::DMaterialFunction& Caller, Durin::DMaterialFunctionInterface& Callee) -> void
	{
		using namespace Durin;
		auto Graph = Caller.GetFunctionGraph();
		const FGuid CallId{0x538d091e, 1, 2, static_cast<uint32>(Graph.Nodes.size() + 1)};
		const auto& Output = Callee.GetFunctionSignature().Outputs[0];
		Graph.Nodes.push_back({.Id = CallId, .Opcode = EMaterialProgramOpcode::FunctionCall,
			.ResultType = Output.Type});
		Graph.Calls.push_back({.NodeId = CallId, .Function = &Callee, .Outputs = {{Output.Id, Output.Type}}});
		Graph.Nodes[1].Inputs[0] = {.SourceNodeId = CallId, .SourceOutputId = Output.Id};
		ASSERT_TRUE(Caller.SetFunctionGraph(std::move(Graph)));
	}
}

TEST(FMaterialFunctionTests, ExplicitMRTemplateRetainsIndependentInstanceParametersWithoutFunctions)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "PBRSurfaceMaterial_MR"));
	ASSERT_NE(Material.Get(), nullptr);
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphPresentation Presentation;
	auto Program = AssetForge::Builtins::MakePBRSurfaceMaterialMRProgram(Presentation);
	ASSERT_TRUE(Material->SetMaterialProgram(Program));
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(Presentation));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 48u);
	EXPECT_EQ(Presentation.Nodes.size(), Program.Nodes.size());
	EXPECT_TRUE(Material->GetMaterialFunctionCalls().empty());
	EXPECT_FALSE(Program.Outputs.Surface.SourceNodeId.IsValid());
	using Kind = MaterialParameters::EMaterialBuiltinParameterKind;
	for (uint32 Index = 0; Index < 8; ++Index)
	{
		const auto Role = static_cast<EMaterialSurfaceOutput>(Index);
		EXPECT_TRUE(GetMaterialSurfaceOutputLink(Program.Outputs, Role).SourceNodeId.IsValid());
		const auto Id = GetMaterialSurfaceParameterId(Role, Kind::Texture);
		const auto Sample = std::ranges::find_if(Program.Nodes, [&](const auto& Node) { return Node.Parameter.Id == Id; });
		ASSERT_NE(Sample, Program.Nodes.end());
		EXPECT_EQ(Sample->Opcode, EMaterialProgramOpcode::TextureSampleParameter2D);
		ASSERT_EQ(Sample->Inputs.size(), 1u);
		EXPECT_TRUE(Sample->Inputs.front().SourceNodeId.IsValid());
	}
	FMaterialCompilerInput Input;
	ASSERT_TRUE(SnapshotMaterialCompilerInput(*Material, {.CompilerIdentity = "ExplicitMRTemplate"}, Input));
	const auto Normalized = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(Normalized);
	EXPECT_EQ(Normalized.Layout.ResourceFieldCount, 8u);
	TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, "MRInstance"));
	ASSERT_TRUE(Instance->SetParent(Material.Get()));
	const auto MetallicId = GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::Metallic, Kind::Value);
	const auto RoughnessId = GetMaterialSurfaceParameterId(EMaterialSurfaceOutput::Roughness, Kind::Value);
	ASSERT_TRUE(Instance->SetParameterOverride(MetallicId, EMaterialParameterType::Scalar, FMaterialParameterValue::MakeScalar(.8f)));
	ASSERT_TRUE(Instance->SetParameterOverride(RoughnessId, EMaterialParameterType::Scalar, FMaterialParameterValue::MakeScalar(.2f)));
	FResolvedMaterialParameter Resolved;
	ASSERT_TRUE(Instance->ResolveParameterValue(MetallicId, Resolved));
	EXPECT_FLOAT_EQ(Resolved.Value.ScalarValue, .8f);
	ASSERT_TRUE(Instance->ResolveParameterValue(RoughnessId, Resolved));
	EXPECT_FLOAT_EQ(Resolved.Value.ScalarValue, .2f);
}

TEST(FMaterialFunctionTests, StructuralImportRecipesExposeOnlyRequiredOwners)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	std::array<FImportedSurfaceRole, 8> Roles;
	const FMaterialSurfaceOutputs Defaults;
	for (uint32 I = 0; I < 8; ++I)
		Roles[I].Value = GetMaterialSurfaceOutputDefault(Defaults, static_cast<EMaterialSurfaceOutput>(I));
	const auto Empty = MakeImportedSurfaceRecipe(Roles);
	EXPECT_TRUE(Empty.Program.Nodes.empty());
	EXPECT_TRUE(Empty.Owners.empty());
	Roles[0].Sample = FImportedSurfaceSample{.ResourceIdentity = "first"};
	Roles[0].Value = {1, 1, 1};
	const auto Plain = MakeImportedSurfaceRecipe(Roles);
	ASSERT_EQ(Plain.Program.Nodes.size(), 1u);
	EXPECT_EQ(Plain.Owners.size(), 1u);
	Roles[0].Sample->ResourceIdentity = "second";
	Roles[0].Sample->Sampler.AddressU = EMaterialSamplerAddressMode::ClampToEdge;
	const auto Renamed = MakeImportedSurfaceRecipe(Roles);
	EXPECT_EQ(Plain.CanonicalKey, Renamed.CanonicalKey);
	EXPECT_EQ(Plain.Program, Renamed.Program);
	Roles[0].Value = {.2f, .3f, .4f};
	Roles[0].Sample->UVOffset = {.25f, .5f};
	const auto Transformed = MakeImportedSurfaceRecipe(Roles);
	EXPECT_EQ(Transformed.Program.Nodes.size(), 5u);
	EXPECT_EQ(Transformed.Owners.size(), 3u);
	Roles[0].Value = {.6f, .7f, .8f};
	Roles[0].Sample->UVOffset = {.75f, .25f};
	const auto OtherValues = MakeImportedSurfaceRecipe(Roles);
	EXPECT_EQ(Transformed.CanonicalKey, OtherValues.CanonicalKey);
	EXPECT_EQ(Transformed.Program, OtherValues.Program);
	auto* Material = NewObject<DMaterial>(nullptr, "StructuralImportRecipe");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(Material->SetMaterialProgram(OtherValues.Program));
	Roles[2].Value = {1};
	Roles[3].Value = {1};
	Roles[2].Sample = FImportedSurfaceSample{.ResourceIdentity = "packed", .Usage = ETextureUsage::DataMask, .OutputIndex = 4};
	Roles[3].Sample = FImportedSurfaceSample{.ResourceIdentity = "packed", .Usage = ETextureUsage::DataMask, .OutputIndex = 3};
	const auto Packed = MakeImportedSurfaceRecipe(Roles);
	EXPECT_EQ(Packed.Program.Nodes.size(), 6u);
	EXPECT_EQ(Packed.Program.Outputs.Metallic.SourceNodeId, Packed.Program.Outputs.Roughness.SourceNodeId);
	ASSERT_TRUE(Material->SetMaterialProgram(Packed.Program));
	Roles[3].Sample->UVChannel = {1};
	const auto Split = MakeImportedSurfaceRecipe(Roles);
	EXPECT_NE(Packed.CanonicalKey, Split.CanonicalKey);
	EXPECT_NE(Split.Program.Outputs.Metallic.SourceNodeId, Split.Program.Outputs.Roughness.SourceNodeId);
	ASSERT_TRUE(Material->SetMaterialProgram(Split.Program));
	Roles[1].Sample = FImportedSurfaceSample{.ResourceIdentity = "normal", .Usage = ETextureUsage::Normal,
		.OutputIndex = 6, .bDecodeNormal = true};
	const auto Normal = MakeImportedSurfaceRecipe(Roles);
	EXPECT_EQ(Normal.Program.Nodes.size(), Split.Program.Nodes.size() + 1);
	ASSERT_TRUE(Material->SetMaterialProgram(Normal.Program));
	Roles[1].Sample.reset();
	EXPECT_EQ(MakeImportedSurfaceRecipe(Roles).Program, Split.Program);
}

TEST(FMaterialFunctionTests, ExpandedAndFunctionRecipesPreserveCompilationAndIndependentOverrides)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	auto* Frozen = NewObject<DMaterial>(nullptr, "ExpandedBaseline");
	ASSERT_NE(Frozen, nullptr);
	ASSERT_TRUE(Frozen->SetMaterialProgram(Testing::MakePBRMaterialProgramForTest()));
	ASSERT_EQ(Frozen->GetParameterDefinitions().size(), 48u);

	auto* Current = NewObject<DMaterial>(nullptr, "CurrentFunctionFixture");
	ASSERT_NE(Current, nullptr);
	Current->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(Testing::SetStandardMaterialProgramForTest(*Current));
	EXPECT_EQ(Current->GetMaterialProgram()->Nodes.size(), 65u);
	for (const auto& Node : Current->GetMaterialProgram()->Nodes)
	{
		EXPECT_NE(Node.Opcode, EMaterialProgramOpcode::Saturate);
		EXPECT_NE(Node.Opcode, EMaterialProgramOpcode::Clamp);
	}
	EXPECT_FALSE(Current->GetMaterialProgram()->Outputs.Surface.SourceNodeId.IsValid());
	ASSERT_EQ(Current->GetMaterialFunctionCalls().size(), 1u);
	EXPECT_EQ(Current->GetMaterialFunctionCalls().front().Function->GetName(), "StandardFunction7");
	for (uint32 Role = 0; Role < 8; ++Role)
		EXPECT_TRUE(GetMaterialSurfaceOutputLink(Current->GetMaterialProgram()->Outputs,
			static_cast<EMaterialSurfaceOutput>(Role)).SourceNodeId.IsValid());
	FMaterialCompilerInput FrozenInput, CurrentInput;
	const FMaterialCompilerEnvironment Environment{.CompilerIdentity = "FunctionFixtureParity"};
	ASSERT_TRUE(SnapshotMaterialCompilerInput(*Frozen, Environment, FrozenInput));
	ASSERT_TRUE(SnapshotMaterialCompilerInput(*Current, Environment, CurrentInput));
	const auto Baseline = NormalizeMaterialProgram(FrozenInput);
	const auto Candidate = NormalizeMaterialProgram(CurrentInput);
	ASSERT_TRUE(Baseline);
	ASSERT_TRUE(Candidate);
	EXPECT_NE(Baseline.CanonicalBytes, Candidate.CanonicalBytes);
	EXPECT_EQ(Baseline.Layout.Fields, Candidate.Layout.Fields);
	const auto BaselineSource = GenerateMaterialProgramSlang(Baseline.IR, Baseline.Layout);
	const auto CandidateSource = GenerateMaterialProgramSlang(Candidate.IR, Candidate.Layout);
	ASSERT_TRUE(BaselineSource);
	ASSERT_TRUE(CandidateSource);
	EXPECT_NE(BaselineSource.Source, CandidateSource.Source);
	EXPECT_NE(CandidateSource.Source.find("return EvaluateMaterialSurface(result)"), std::string::npos);
	ASSERT_EQ(Baseline.Layout.Fields.size(), 48u);
	const auto ArtifactRoot = Testing::GetTestWorkDirectory() / "MaterialAuthoringBaseline";
	std::filesystem::create_directories(ArtifactRoot);
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Baseline.CanonicalBytes, ArtifactRoot / "canonical-ir.bin"));
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(BaselineSource.Source)),
		ArtifactRoot / "generated.slang"));
	RecordProperty("BaselineExpressionCount", static_cast<int>(Baseline.IR.Nodes.size()));
	RecordProperty("BaselineParameterCount", static_cast<int>(Baseline.Layout.Fields.size()));

	// A nonidentity override on one map must never become a shared UV default.
	auto* Parent = NewObject<DMaterialInstance>(nullptr, "IndependentUVParent");
	auto* Child = NewObject<DMaterialInstance>(nullptr, "IndependentUVChild");
	ASSERT_TRUE(Parent->SetParent(Frozen));
	ASSERT_TRUE(Child->SetParent(Parent));
	using Kind = MaterialParameters::EMaterialBuiltinParameterKind;
	for (uint32 Index = 0; Index < 8; ++Index)
	{
		const auto Role = static_cast<EMaterialSurfaceOutput>(Index);
		for (const auto ParameterKind : {Kind::UVChannel, Kind::UVScale, Kind::UVOffset, Kind::UVRotation})
		{
			const auto Id = GetMaterialSurfaceParameterId(Role, ParameterKind);
			const auto* Definition = Frozen->FindParameterDefinition(Id);
			ASSERT_NE(Definition, nullptr);
			const auto Value = ParameterKind == Kind::UVScale || ParameterKind == Kind::UVOffset
				? FMaterialParameterValue::MakeVector2({1.25f + Index, -.125f * Index})
				: FMaterialParameterValue::MakeScalar(ParameterKind == Kind::UVChannel
					? static_cast<float>(Index % 4) : .2f * (Index + 1));
			ASSERT_TRUE(Parent->SetParameterOverride(Id, Definition->Type, Value));
			FResolvedMaterialParameter Resolved;
			ASSERT_TRUE(Child->ResolveParameterValue(Id, Resolved));
			EXPECT_EQ(Resolved.Value, Value);
			EXPECT_FALSE(Child->IsParameterOverrideOrphan(Id));
		}
	}
	// Switching to the new recipe must preserve inherited values by GUID.
	ASSERT_TRUE(Parent->SetParent(Current));
	for (const auto& Override : Parent->GetParameterOverrides())
	{
		FResolvedMaterialParameter Resolved;
		ASSERT_TRUE(Child->ResolveParameterValue(Override.ParameterId, Resolved));
		EXPECT_EQ(Resolved.Value, Override.Value);
		EXPECT_FALSE(Parent->IsParameterOverrideOrphan(Override.ParameterId));
	}
	MarkAsGarbage(Child);
	MarkAsGarbage(Parent);
	MarkAsGarbage(Current);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, LiteralDefaultsMatchExplicitConstantsAndValidateTypes)
{
	using namespace Durin;
	const FGuid Constant{0x98abc101, 1, 1, 2}, Product{0x98abc101, 1, 1, 3};
	FMaterialCompilerInput Compact;
	Compact.Environment.CompilerIdentity = "LiteralBindingParity";
	Compact.Program.Nodes = {{.Id = Constant, .Literal = {.X = .5f}},
		{.Id = Product, .Opcode = EMaterialProgramOpcode::Multiply, .Inputs = {{Constant}, {}},
			.InputDefaults = {{}, {.Kind = EMaterialInputDefaultKind::Literal, .Literal = {.X = .25f}}}}};
	Compact.Program.Outputs.Roughness = {Product};
	const auto Baseline = NormalizeMaterialProgram(Compact);
	ASSERT_TRUE(Baseline);
	EXPECT_TRUE(Baseline.ActiveParameters.empty());
	auto Explicit = Compact;
	const FGuid ValueNode{0x98abc101, 1, 1, 4};
	Explicit.Program.Nodes[1].Inputs[1] = {ValueNode};
	Explicit.Program.Nodes[1].InputDefaults.clear();
	Explicit.Program.Nodes.push_back({.Id = ValueNode, .Literal = {.X = .25f}});
	const auto Expanded = NormalizeMaterialProgram(Explicit);
	ASSERT_TRUE(Expanded);
	EXPECT_EQ(Baseline.CanonicalBytes, Expanded.CanonicalBytes);
	EXPECT_EQ(Baseline.Layout, Expanded.Layout);
	Compact.Program.Nodes[1].InputDefaults[1].Type = EMaterialProgramValueType::Float2;
	EXPECT_FALSE(NormalizeMaterialProgram(Compact));
	Compact.Program.Nodes[1].InputDefaults[1] = {.Kind = EMaterialInputDefaultKind::Literal,
		.Literal = {std::numeric_limits<float>::infinity()}};
	EXPECT_FALSE(NormalizeMaterialProgram(Compact));
}

TEST(FMaterialFunctionTests, CompactSamplingSharesFetchAndPreservesUVParameterReachability)
{
	using namespace Durin;
	const FGuid TextureId{0x98abc102, 1, 1, 1}, ChannelId{0x98abc102, 1, 1, 2}, SampleId{0x98abc102, 1, 1, 3};
	FMaterialCompilerInput Compact;
	Compact.Environment.CompilerIdentity = "CompactSampleParity";
	Compact.Parameters = {{TextureId, EMaterialParameterType::Texture}, {ChannelId, EMaterialParameterType::Scalar}};
	FMaterialProgramNode Sample{.Id = SampleId, .Opcode = EMaterialProgramOpcode::TextureSampleParameter2D,
		.ResultType = EMaterialProgramValueType::Float4, .Inputs = {{}}, .Parameter = {.Id = TextureId, .Type = EMaterialParameterType::Texture}};
	const FGuid ChannelNode{0x98abc102, 1, 1, 6}, UVNode{0x98abc102, 1, 1, 7};
	Sample.Inputs = {{UVNode}};
	Compact.Program.Nodes = {Sample,
		{.Id = ChannelNode, .Opcode = EMaterialProgramOpcode::Parameter, .Parameter = {.Id = ChannelId}},
		{.Id = UVNode, .Opcode = EMaterialProgramOpcode::TextureCoordinates,
			.ResultType = EMaterialProgramValueType::Float2, .Inputs = {{ChannelNode}, {}, {}, {}}}};
	Compact.Program.Outputs.BaseColor = {.SourceNodeId = SampleId, .SourceOutputIndex = 1};
	Compact.Program.Outputs.Roughness = {.SourceNodeId = SampleId, .SourceOutputIndex = 3};
	Compact.Program.Outputs.Metallic = {.SourceNodeId = SampleId, .SourceOutputIndex = 4};
	const auto Baseline = NormalizeMaterialProgram(Compact);
	ASSERT_TRUE(Baseline);
	EXPECT_EQ(std::ranges::count(Baseline.IR.Nodes, EMaterialProgramOpcode::TextureSample2D, &FMaterialIRNode::Opcode), 1);
	EXPECT_EQ(Baseline.ActiveParameters.size(), 2u);
	const auto Source = GenerateMaterialProgramSlang(Baseline.IR, Baseline.Layout);
	ASSERT_TRUE(Source);
	auto Explicit = Compact;
	const FGuid TextureNode{0x98abc102, 1, 1, 4}, CoordinatesNode{0x98abc102, 1, 1, 5};
	Explicit.Program.Nodes[0].Opcode = EMaterialProgramOpcode::TextureSample2D;
	Explicit.Program.Nodes[0].Parameter = {};
	Explicit.Program.Nodes[0].Inputs = {{TextureNode}, {UVNode}};
	Explicit.Program.Nodes[0].UVSettings = {};
	Explicit.Program.Nodes.push_back({.Id = TextureNode, .Opcode = EMaterialProgramOpcode::TextureParameter,
		.ResultType = EMaterialProgramValueType::Texture2D, .Parameter = {.Id = TextureId, .Type = EMaterialParameterType::Texture}});
	const auto Expanded = NormalizeMaterialProgram(Explicit);
	ASSERT_TRUE(Expanded);
	EXPECT_EQ(Baseline.CanonicalBytes, Expanded.CanonicalBytes);
	EXPECT_EQ(Baseline.Layout, Expanded.Layout);
	Compact.Program.Nodes.push_back({.Id = CoordinatesNode, .ResultType = EMaterialProgramValueType::Float2,
		.Literal = {.X = .25f, .Y = .75f}});
	Compact.Program.Nodes[0].Inputs[0] = {CoordinatesNode};
	const auto Connected = NormalizeMaterialProgram(Compact);
	ASSERT_TRUE(Connected);
	EXPECT_EQ(Connected.ActiveParameters.size(), 1u);
	Compact.Program.Nodes[0].Inputs[0] = {UVNode};
	EXPECT_EQ(NormalizeMaterialProgram(Compact).CanonicalBytes, Baseline.CanonicalBytes);
	Compact.Program.Outputs.Metallic.SourceOutputIndex = 7;
	EXPECT_FALSE(NormalizeMaterialProgram(Compact));
}

TEST(FMaterialFunctionTests, NormalSampleOutputMatchesExplicitDecodeWithoutExtraFetch)
{
	using namespace Durin;
	const FGuid SampleId{0x98abc104, 1, 1, 1}, TextureId{0x98abc104, 1, 1, 2}, DecodeId{0x98abc104, 1, 1, 3};
	FMaterialCompilerInput Compact;
	Compact.Environment.CompilerIdentity = "NormalSampleParity";
	Compact.Parameters = {{TextureId, EMaterialParameterType::Texture}};
	Compact.Program.Nodes = {{.Id = SampleId, .Opcode = EMaterialProgramOpcode::TextureSampleParameter2D,
		.ResultType = EMaterialProgramValueType::Float4, .Inputs = {{}},
		.Parameter = {.Id = TextureId, .Type = EMaterialParameterType::Texture, .TextureUsage = ETextureUsage::Normal}}};
	Compact.Program.Outputs.Normal = {SampleId, 8};
	Compact.Program.Outputs.Roughness = {SampleId, 3};
	const auto Normalized = NormalizeMaterialProgram(Compact);
	ASSERT_TRUE(Normalized);
	EXPECT_EQ(std::ranges::count(Normalized.IR.Nodes, EMaterialProgramOpcode::TextureSample2D, &FMaterialIRNode::Opcode), 1);
	EXPECT_EQ(std::ranges::count(Normalized.IR.Nodes, EMaterialProgramOpcode::DecodeNormalRG, &FMaterialIRNode::Opcode), 1);
	EXPECT_EQ(std::ranges::count(Normalized.IR.Nodes, EMaterialProgramOpcode::BlendNormalsRNM, &FMaterialIRNode::Opcode), 0);
	auto Explicit = Compact;
	Explicit.Program.Nodes.push_back({.Id = DecodeId, .Opcode = EMaterialProgramOpcode::DecodeNormalRG,
		.ResultType = EMaterialProgramValueType::Float3, .Inputs = {{SampleId, 6}}});
	Explicit.Program.Outputs.Normal = {DecodeId};
	const auto Expanded = NormalizeMaterialProgram(Explicit);
	ASSERT_TRUE(Expanded);
	EXPECT_EQ(Normalized.CanonicalBytes, Expanded.CanonicalBytes);
	EXPECT_EQ(Normalized.Layout, Expanded.Layout);
	Compact.Program.Outputs.Normal.SourceOutputIndex = 9;
	EXPECT_FALSE(NormalizeMaterialProgram(Compact));
}

TEST(FMaterialFunctionTests, ResourceOutputSkipsOwnerUVAndPreservesIndependentSamples)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	auto* Material = NewObject<DMaterial>(nullptr, "ResourceFanOut");
	auto* Texture = NewObject<DTexture2D>(nullptr, "OwnedTexture");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialProgramNode Channel{.Id = FGuid::NewGuid(), .Opcode = EMaterialProgramOpcode::Parameter};
	Channel.Parameter = {.Id = FGuid::NewGuid(), .Name = "UnusedOwnerUV"};
	FMaterialProgramNode Coordinates{.Id = FGuid::NewGuid(), .Opcode = EMaterialProgramOpcode::TextureCoordinates,
		.ResultType = EMaterialProgramValueType::Float2, .Inputs = {{Channel.Id}, {}, {}, {}}};
	FMaterialProgramNode Owner{.Id = FGuid::NewGuid(), .Opcode = EMaterialProgramOpcode::TextureSampleParameter2D,
		.ResultType = EMaterialProgramValueType::Float4, .Inputs = {{Coordinates.Id}}};
	Owner.Parameter = {.Id = FGuid::NewGuid(), .Name = "SharedTexture", .Type = EMaterialParameterType::Texture,
		.Value = FMaterialParameterValue::MakeTexture(Texture)};
	FMaterialProgramNode UV1{.Id = FGuid::NewGuid(), .ResultType = EMaterialProgramValueType::Float2,
		.Literal = {.1f, .2f}};
	FMaterialProgramNode UV2{.Id = FGuid::NewGuid(), .ResultType = EMaterialProgramValueType::Float2,
		.Literal = {.7f, .8f}};
	FMaterialProgramNode Sample1{.Id = FGuid::NewGuid(), .Opcode = EMaterialProgramOpcode::TextureSample2D,
		.ResultType = EMaterialProgramValueType::Float4, .Inputs = {{Owner.Id, 7}, {UV1.Id}}};
	FMaterialProgramNode Sample2{.Id = FGuid::NewGuid(), .Opcode = EMaterialProgramOpcode::TextureSample2D,
		.ResultType = EMaterialProgramValueType::Float4, .Inputs = {{Owner.Id, 7}, {UV2.Id}}};
	FMaterialProgram Program;
	Program.Nodes = {Channel, Coordinates, Owner, UV1, UV2, Sample1, Sample2};
	Program.Outputs.BaseColor = {Sample1.Id, 1};
	Program.Outputs.Emissive = {Sample2.Id, 1};
	ASSERT_TRUE(Material->SetMaterialProgram(Program));
	EXPECT_EQ(Material->GetParameterDefinitions().size(), 2u);
	const auto Dependencies = InspectMaterialParameterDependencies(Program, Material->GetParameterDefinitions());
	ASSERT_EQ(Dependencies.size(), 1u);
	EXPECT_EQ(Dependencies.front().ParameterId, Owner.Parameter.Id);
	FMaterialCompilerInput Snapshot;
	ASSERT_TRUE(SnapshotMaterialCompilerInput(*Material, {.CompilerIdentity = "ResourceFanOut"}, Snapshot));
	for (const auto& Node : Snapshot.Program.Nodes)
	{
		EXPECT_EQ(Node.Parameter.Value.TextureValue.Get(), nullptr);
		EXPECT_TRUE(Node.Parameter.Name.IsNone());
	}
	const auto Normalized = NormalizeMaterialProgram(Snapshot);
	ASSERT_TRUE(Normalized) << (Normalized.Diagnostics.empty() ? "" : Normalized.Diagnostics.front().Message);
	ASSERT_EQ(Normalized.ActiveParameters.size(), 1u);
	EXPECT_EQ(Normalized.ActiveParameters.front().Id, Owner.Parameter.Id);
	EXPECT_EQ(std::ranges::count(Normalized.IR.Nodes, EMaterialProgramOpcode::TextureSample2D, &FMaterialIRNode::Opcode), 2);
	EXPECT_EQ(std::ranges::count(Normalized.IR.Nodes, EMaterialProgramOpcode::UVChannel, &FMaterialIRNode::Opcode), 0);
	EXPECT_EQ(Normalized.Layout.ResourceFieldCount, 1u);
	MarkAsGarbage(Material); MarkAsGarbage(Texture); CollectGarbage();
}

TEST(FMaterialFunctionTests, InlineCallBindingsRespectFunctionDefaultsAndRootOwnership)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "InlineCallFunction");
	const auto In = FunctionPort(1, EMaterialProgramValueType::Float, "Value");
	const auto Out = FunctionPort(2, EMaterialProgramValueType::Float, "Result");
	FMaterialFunctionGraph Graph;
	Graph.Signature.Inputs = {In};
	Graph.Signature.Inputs[0].Default = {.Kind = EMaterialFunctionDefaultKind::Numeric, .Numeric = {.X = .75f}};
	Graph.Signature.Outputs = {Out};
	const FGuid InputNode{0x98abc103, 1, 1, 1}, OutputNode{0x98abc103, 1, 1, 2};
	Graph.Nodes = {{.Id = InputNode, .Opcode = EMaterialProgramOpcode::FunctionInput, .FunctionPortId = In.Id},
		{.Id = OutputNode, .Opcode = EMaterialProgramOpcode::FunctionOutput, .Inputs = {{InputNode}}, .FunctionPortId = Out.Id}};
	ASSERT_TRUE(Function->SetFunctionGraph(Graph));
	FMaterialCompilerInput Input;
	Input.Environment.CompilerIdentity = "InlineCallDefaults";
	const FGuid CallId{0x98abc103, 1, 1, 3};
	Input.Program.Nodes = {{.Id = CallId, .Opcode = EMaterialProgramOpcode::FunctionCall}};
	Input.Program.Outputs.Roughness = {.SourceNodeId = CallId, .SourceOutputId = Out.Id};
	Input.FunctionCalls = {{.NodeId = CallId, .FunctionPath = Function->GetObjectPath(),
		.Inputs = {{In.Id, In.Type, {}, {.Kind = EMaterialInputDefaultKind::Literal, .Literal = {.X = .25f}}}},
		.Outputs = {{Out.Id, Out.Type}}}};
	const std::array<DMaterialFunctionInterface*, 1> Roots{Function};
	ASSERT_TRUE(SnapshotMaterialFunctionClosure(Roots, Input.Functions));
	const auto Bound = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(Bound);
	ASSERT_EQ(Bound.IR.Nodes.size(), 1u);
	EXPECT_EQ(Bound.IR.Nodes[0].Literal.X, .25f);
	Input.FunctionCalls[0].Inputs[0].Default = {};
	const auto Defaulted = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(Defaulted);
	EXPECT_EQ(Defaulted.IR.Nodes[0].Literal.X, .75f);
	Graph.Signature.Inputs[0].bRequired = true;
	Graph.Signature.Inputs[0].Default = {};
	ASSERT_TRUE(Function->SetFunctionGraph(Graph));
	ASSERT_TRUE(SnapshotMaterialFunctionClosure(Roots, Input.Functions));
	EXPECT_FALSE(NormalizeMaterialProgram(Input));
	Graph.Nodes.push_back({.Id = CallId, .Opcode = EMaterialProgramOpcode::Parameter,
		.Parameter = {.Id = {0x98abc103, 2, 1, 1}, .Name = "ForbiddenOwner"}});
	EXPECT_FALSE(Function->SetFunctionGraph(Graph));
	MarkAsGarbage(Function);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, WorkspaceSavesAndReloadsFunctionsAcrossOpenDocuments)
{
	using namespace Durin;
	using namespace Durin::Editor;
	using namespace Durin::Editor::Material;
	InitializeDObjectSystem();
	const auto Root = Testing::CreateTestFixtureDirectory("FunctionWorkspace");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/FunctionWorkspace/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid());
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath FirstPath, SecondPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionWorkspace/First", FirstPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionWorkspace/Second", SecondPath));
	DMaterialFunction* First = nullptr;
	DMaterialFunction* Second = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(FirstPath, First));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(SecondPath, Second));
	ASSERT_TRUE(FMaterialGraphDocument(*Second).InsertFunctionCall(*First, 0, 300));
	ASSERT_TRUE(SavePackage(First->GetPackage()));
	ASSERT_TRUE(SavePackage(Second->GetPackage()));
	FWorkspaceManager Manager;
	DThumbnailManager Thumbnails;
	FMaterialEditorModule Module;
	FModuleTestHarness Harness("MaterialEditor");
	Harness.Start(Module);
	ASSERT_TRUE(Module.RegisterMaterialEditor(Manager, Thumbnails));
	const auto Class = DMaterialFunction::StaticClass()->GetQualifiedName().ToString();
	const auto Resource = First->GetObjectPath();
	ASSERT_TRUE(Manager.OpenAsset(Resource, Class));
	ASSERT_TRUE(Manager.OpenAsset(Resource, Class));
	ASSERT_EQ(Manager.GetDocuments().size(), 1u);
	const auto FirstTab = *Manager.GetActiveDocument();
	ASSERT_TRUE(Manager.OpenAsset(Second->GetObjectPath(), Class));
	ASSERT_EQ(Manager.GetDocuments().size(), 2u);
	const auto Workspace = Manager.FindWorkspace(FWorkspaceTypeId("MaterialFunctionEditor"));
	ASSERT_TRUE(Workspace);
	auto Signature = First->GetFunctionSignature();
	Signature.Outputs[0].Name = "Saved Surface";
	ASSERT_TRUE(FMaterialGraphDocument(*First).SetSignature(Signature));
	EXPECT_TRUE(Workspace->IsDocumentDirty(FirstTab));
	EXPECT_EQ(Manager.RequestCloseDocument(FirstTab.Id), EDocumentCloseResult::PendingConfirmation);
	ASSERT_TRUE(Workspace->SaveDocument(FirstTab));
	EXPECT_FALSE(Workspace->IsDocumentDirty(FirstTab));
	const auto CopiedCallId = Second->GetFunctionGraph().Calls[0].NodeId;
	FMaterialGraphClipboardPayload Clipboard;
	ASSERT_TRUE(FMaterialGraphDocument(*Second).CopySelection(std::span(&CopiedCallId, 1), Clipboard));
	Signature.Outputs[0].Name = "Discarded Surface";
	ASSERT_TRUE(FMaterialGraphDocument(*First).SetSignature(Signature));
	const bool Discarded = Workspace->DiscardDocument(FirstTab);
	if (!Discarded)
	{
		const auto Error = std::string(static_cast<MMaterialFunctionEditor*>(Workspace.get())->GetLastError());
		Module.UnregisterMaterialEditor(); Harness.Shutdown();
		FAIL() << Error;
	}
	DMaterialFunction* Reloaded = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(FirstPath), Reloaded));
	ASSERT_NE(Reloaded, nullptr);
	EXPECT_EQ(Reloaded->GetFunctionSignature().Outputs[0].Name, "Saved Surface");
	EXPECT_EQ(Second->GetFunctionGraph().Calls[0].Function.Get(), Reloaded);
	const auto Pasted = FMaterialGraphDocument(*Second).Paste(Clipboard, 300, 300);
	EXPECT_TRUE(Pasted) << Pasted.Message;
	EXPECT_EQ(Second->GetFunctionGraph().Calls.back().Function.Get(), Reloaded);
	EXPECT_FALSE(Workspace->IsDocumentDirty(FirstTab));
	Module.UnregisterMaterialEditor();
	Harness.Shutdown();
}

TEST(FMaterialFunctionTests, PreviewWrappersCompileEveryOutputTypeWithoutChangingTheFunction)
{
	using namespace Durin;
	using namespace Durin::Editor::Material;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	auto* Function = NewObject<DMaterialFunction>(nullptr, "PreviewFunction");
	auto* Preview = NewObject<DMaterial>(nullptr, "PreviewWrapper");
	Preview->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	for (uint32 Index = 0; Index < 6; ++Index)
	{
		const auto Type = static_cast<EMaterialProgramValueType>(Index);
		auto Input = FunctionPort(1, Type, "Input");
		Input.bRequired = true;
		const auto Output = FunctionPort(2, Type, "Output");
		FMaterialFunctionGraph Graph;
		Graph.Signature = {{Input}, {Output}};
		Graph.Nodes = {{.Id = {1, 1, 1, 1}, .Opcode = EMaterialProgramOpcode::FunctionInput,
			.ResultType = Type, .FunctionPortId = Input.Id},
			{.Id = {1, 1, 1, 2}, .Opcode = EMaterialProgramOpcode::FunctionOutput,
			.ResultType = Type, .Inputs = {{{1, 1, 1, 1}}}, .FunctionPortId = Output.Id}};
		ASSERT_TRUE(Function->SetFunctionGraph(Graph));
		const auto Revision = Function->GetFunctionRevision();
		FMaterialGraphDocumentState State;
		FMaterialStaticProperties Properties;
		const auto Built = BuildMaterialFunctionPreview(*Function, Output.Id, State, Properties);
		ASSERT_TRUE(Built) << Built.Message;
		ASSERT_EQ(State.Calls.size(), 1u);
		EXPECT_EQ(State.Calls[0].Outputs[0].OutputId, Output.Id);
		ASSERT_TRUE(FMaterialGraphDocument(*Preview).Commit(State, "Build Preview"));
		ASSERT_TRUE(Preview->SetStaticProperties(Properties));
		ASSERT_TRUE(Preview->CompileEdits());
		ASSERT_TRUE(Preview->GetAcceptedCompiledProgram());
		EXPECT_EQ(Function->GetFunctionGraph(), Graph);
		EXPECT_EQ(Function->GetFunctionRevision(), Revision);
		const auto Before = State;
		EXPECT_FALSE(BuildMaterialFunctionPreview(*Function, FGuid::NewGuid(), State, Properties));
		EXPECT_EQ(State, Before);
	}
	MarkAsGarbage(Preview); MarkAsGarbage(Function); CollectGarbage();
}

TEST(FMaterialFunctionTests, CallInsertionBindsRequiredInputsAndAdmitsNewOutputPorts)
{
	using namespace Durin;
	using namespace Durin::Editor::Material;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "RequiredCallFunction");
	auto* Material = NewObject<DMaterial>(nullptr, "RequiredCallMaterial");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Graph(*Function), Root(*Material);
	auto Port = FunctionPort(50, EMaterialProgramValueType::Float, "Required Amount");
	Port.bRequired = true;
	const auto Input = Graph.AddPort(false, Port);
	ASSERT_TRUE(Input);
	const auto Constant = Root.CreateNode({.Node = {.Literal = {.X = 0.6f}}});
	ASSERT_TRUE(Constant);
	EXPECT_FALSE(Root.InsertFunctionCall(*Function, 0, 0));
	const std::array Inputs{FMaterialFunctionInputBinding{Port.Id, Port.Type, {Constant.GeneratedNodeIds[0]}}};
	const auto Call = Root.InsertFunctionCall(*Function, 0, 0, Inputs);
	ASSERT_TRUE(Call);
	const auto Output = FunctionPort(51, EMaterialProgramValueType::Float, "New Amount");
	ASSERT_TRUE(Graph.AddPort(true, Output, {Input.GeneratedNodeIds[0]}));
	ASSERT_EQ(Material->GetMaterialFunctionCalls()[0].Outputs.size(), 1u);
	ASSERT_TRUE(Root.AssignMaterialOutput(EMaterialSurfaceOutput::Roughness, {Call.GeneratedNodeIds[0], 0, Output.Id}));
	ASSERT_EQ(Material->GetMaterialFunctionCalls()[0].Outputs.size(), 2u);
	EXPECT_EQ(Material->GetMaterialProgram()->Outputs.Roughness.SourceOutputId, Output.Id);
	MarkAsGarbage(Material); MarkAsGarbage(Function); CollectGarbage();
}

TEST(FMaterialFunctionTests, SharedDocumentsEditStableCallsAndInterfacesWithUndo)
{
	using namespace Durin;
	using namespace Durin::Editor::Material;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "CommandFunction");
	auto* Wrapper = NewObject<DMaterialFunction>(nullptr, "CommandWrapper");
	auto* Material = NewObject<DMaterial>(nullptr, "CommandMaterial");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	Tests::FTestTransactorOwner Transactions;
	FMaterialGraphDocument FunctionDocument(*Function), WrapperDocument(*Wrapper), MaterialDocument(*Material);
	auto Signature = Function->GetFunctionSignature();
	Signature.Inputs[0].Name = "Renamed Surface";
	ASSERT_TRUE(FunctionDocument.SetSignature(Signature, Transactions.Get()));
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_NE(Function->GetFunctionSignature().Inputs[0].Name, Signature.Inputs[0].Name);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Function->GetFunctionSignature(), Signature);
	const auto Nested = WrapperDocument.InsertFunctionCall(*Function, 10, 20, Transactions.Get());
	ASSERT_TRUE(Nested);
	EXPECT_FALSE(FunctionDocument.InsertFunctionCall(*Wrapper, 0, 0));
	const auto Inserted = MaterialDocument.InsertFunctionCall(*Wrapper, 30, 40, Transactions.Get());
	ASSERT_TRUE(Inserted);
	ASSERT_EQ(Material->GetMaterialFunctionCalls().size(), 1u);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_TRUE(Material->GetMaterialFunctionCalls().empty());
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Material->GetMaterialFunctionCalls()[0].NodeId, Inserted.GeneratedNodeIds[0]);
	FMaterialGraphDocumentState State;
	ASSERT_TRUE(WrapperDocument.Capture(State));
	const auto InputNode = State.Program.Nodes[0].Id;
	const auto InputPort = Function->GetFunctionSignature().Inputs[0].Id;
	ASSERT_TRUE(WrapperDocument.ConnectCallInput(Nested.GeneratedNodeIds[0], InputPort, {InputNode}, false, Transactions.Get()));
	EXPECT_EQ(Wrapper->GetFunctionGraph().Calls[0].Inputs[0].InputId, InputPort);
	ASSERT_TRUE(WrapperDocument.DisconnectCallInput(Nested.GeneratedNodeIds[0], InputPort, Transactions.Get()));
	EXPECT_TRUE(Wrapper->GetFunctionGraph().Calls[0].Inputs.empty());
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Wrapper->GetFunctionGraph().Calls[0].Inputs[0].Source.SourceNodeId, InputNode);
	ASSERT_TRUE(MaterialDocument.Capture(State));
	const auto Before = State;
	State.Program.Nodes.push_back({.Id = {73, 1, 2, 3}});
	ASSERT_TRUE(MaterialDocument.Commit(State, "Add Numeric Constant"));
	EXPECT_FALSE(MaterialDocument.ConnectCallInput(Inserted.GeneratedNodeIds[0],
		Wrapper->GetFunctionSignature().Inputs[0].Id, {{73, 1, 2, 3}}));
	EXPECT_TRUE(Material->GetMaterialFunctionCalls()[0].Inputs.empty());
	ASSERT_TRUE(MaterialDocument.Commit(Before, "Remove Numeric Constant"));
	MarkAsGarbage(Material);
	MarkAsGarbage(Wrapper);
	MarkAsGarbage(Function);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, CommandsAuthorFunctionPortsAndCompileASelectedOutput)
{
	using namespace Durin;
	using namespace Durin::Editor::Material;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	auto* Function = NewObject<DMaterialFunction>(nullptr, "AuthoredFunction");
	auto* Material = NewObject<DMaterial>(nullptr, "AuthoredFunctionCaller");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Function), Caller(*Material);
	Tests::FTestTransactorOwner Transactions;
	auto Input = FunctionPort(90, EMaterialProgramValueType::Float, "Amount");
	Input.Default = {.Kind = EMaterialFunctionDefaultKind::Numeric, .Numeric = {.X = 0.37f}};
	const auto AddedInput = Document.AddPort(false, Input, {}, -200, 0, Transactions.Get());
	ASSERT_TRUE(AddedInput);
	FMaterialGraphCreateNodeRequest Saturate{.Node = {.Opcode = EMaterialProgramOpcode::Saturate,
		.Inputs = {{AddedInput.GeneratedNodeIds[0]}}}, .X = 100};
	const auto AddedNode = Document.CreateNode(Saturate, Transactions.Get());
	ASSERT_TRUE(AddedNode);
	const auto Output = FunctionPort(91, EMaterialProgramValueType::Float, "Clamped Amount");
	const auto AddedOutput = Document.AddPort(true, Output, {AddedNode.GeneratedNodeIds[0]}, 400, 0, Transactions.Get());
	ASSERT_TRUE(AddedOutput);
	EXPECT_FALSE(Document.RemovePort(false, Input.Id));
	const auto Inserted = Caller.InsertFunctionCall(*Function, 0, 0, Transactions.Get());
	ASSERT_TRUE(Inserted);
	ASSERT_TRUE(Caller.AssignMaterialOutput(EMaterialSurfaceOutput::Roughness,
		{.SourceNodeId = Inserted.GeneratedNodeIds[0], .SourceOutputId = Output.Id}, Transactions.Get()));
	ASSERT_TRUE(Material->CompileEdits());
	ASSERT_TRUE(Material->GetAcceptedCompiledProgram());
	const auto Identity = Material->GetAcceptedCompiledProgram()->Identity;
	auto Signature = Function->GetFunctionSignature();
	std::ranges::reverse(Signature.Inputs);
	std::ranges::reverse(Signature.Outputs);
	ASSERT_TRUE(Document.SetSignature(Signature, Transactions.Get()));
	ASSERT_TRUE(Material->CompileEdits());
	EXPECT_EQ(Material->GetAcceptedCompiledProgram()->Identity, Identity);
	const auto CallId = Inserted.GeneratedNodeIds[0];
	ASSERT_TRUE(Caller.RemoveNodes(std::span(&CallId, 1), Transactions.Get()));
	EXPECT_TRUE(Material->GetMaterialFunctionCalls().empty());
	EXPECT_FALSE(Material->GetMaterialProgram()->Outputs.Roughness.SourceNodeId.IsValid());
	ASSERT_TRUE(Transactions.Get()->Undo());
	ASSERT_EQ(Material->GetMaterialFunctionCalls().size(), 1u);
	EXPECT_EQ(Material->GetMaterialProgram()->Outputs.Roughness.SourceOutputId, Output.Id);
	ASSERT_TRUE(Document.RemovePort(true, Output.Id));
	const auto NodeId = AddedNode.GeneratedNodeIds[0];
	ASSERT_TRUE(Document.RemoveNodes(std::span(&NodeId, 1)));
	ASSERT_TRUE(Document.RemovePort(false, Input.Id));
	MarkAsGarbage(Material);
	MarkAsGarbage(Function);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, DefaultsAreTypedAndInputReferencesRemainStable)
{
	using namespace Durin;
	FMaterialFunctionSignature Signature;
	Signature.Inputs = {FunctionPort(1, EMaterialProgramValueType::Float2, "UV"),
		FunctionPort(2, EMaterialProgramValueType::Float2, "MapUV")};
	Signature.Outputs = {FunctionPort(3, EMaterialProgramValueType::Float2, "Result")};
	Signature.Inputs[0].Default.Kind = EMaterialFunctionDefaultKind::UV0;
	Signature.Inputs[1].Default = {.Kind = EMaterialFunctionDefaultKind::Input,
		.InputId = Signature.Inputs[0].Id};
	ASSERT_TRUE(ValidateMaterialFunctionSignature(Signature));
	std::ranges::reverse(Signature.Inputs);
	Signature.Inputs[1].Name = "RenamedCommonUV";
	Signature.Inputs[1].DisplayOrder = 17;
	ASSERT_TRUE(ValidateMaterialFunctionSignature(Signature));
	Signature.Inputs[1].Default = {.Kind = EMaterialFunctionDefaultKind::Input,
		.InputId = Signature.Inputs[0].Id};
	const auto Cycle = ValidateMaterialFunctionSignature(Signature);
	EXPECT_FALSE(Cycle);
	ASSERT_FALSE(Cycle.Diagnostics.empty());
	EXPECT_TRUE(Cycle.Diagnostics[0].PortId.IsValid());
	Signature.Inputs[1].Default.Kind = EMaterialFunctionDefaultKind::UV0;
	Signature.Inputs.erase(Signature.Inputs.begin() + 1);
	EXPECT_FALSE(ValidateMaterialFunctionSignature(Signature));
	Signature.Inputs[0].Default.Kind = EMaterialFunctionDefaultKind::Texture;
	EXPECT_FALSE(ValidateMaterialFunctionSignature(Signature));
	Signature.Inputs[0].Type = EMaterialProgramValueType::Texture2D;
	Signature.Inputs[0].Default.TextureFallback = EMaterialTextureFallback::FlatRGNormal;
	ASSERT_TRUE(ValidateMaterialFunctionSignature(Signature));
	Signature.Inputs[0].bRequired = true;
	EXPECT_FALSE(ValidateMaterialFunctionSignature(Signature));
	Signature.Inputs[0].Default.Kind = EMaterialFunctionDefaultKind::None;
	EXPECT_TRUE(ValidateMaterialFunctionSignature(Signature));
	Signature.Inputs.resize(MaterialFunctionMaxInputs + 1);
	EXPECT_FALSE(ValidateMaterialFunctionSignature(Signature));
}

TEST(FMaterialFunctionTests, GraphValidationRejectsCyclesParametersAndInvalidTerminals)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "FunctionValidation");
	auto Graph = Function->GetFunctionGraph();
	ASSERT_TRUE(ValidateMaterialFunctionGraph(Graph));
	Graph.Nodes.back().FunctionPortId = FGuid{1, 2, 3, 4};
	EXPECT_FALSE(ValidateMaterialFunctionGraph(Graph));
	Graph = Function->GetFunctionGraph();
	Graph.Nodes.front().Opcode = EMaterialProgramOpcode::Parameter;
	EXPECT_FALSE(ValidateMaterialFunctionGraph(Graph));
	Graph = Function->GetFunctionGraph();
	Graph.Nodes.back().Inputs[0].SourceOutputId = Graph.Signature.Outputs[0].Id;
	EXPECT_FALSE(ValidateMaterialFunctionGraph(Graph));
	Graph = Function->GetFunctionGraph();
	Graph.SchemaVersion = 100;
	EXPECT_FALSE(ValidateMaterialFunctionGraph(Graph));
	Graph = Function->GetFunctionGraph();
	Graph.Nodes.push_back(Graph.Nodes.back());
	EXPECT_FALSE(ValidateMaterialFunctionGraph(Graph));
	Graph = Function->GetFunctionGraph();
	Graph.Nodes.push_back({.Id = {9, 8, 7, 6}, .Opcode = EMaterialProgramOpcode::Add,
		.ResultType = EMaterialProgramValueType::Float, .Inputs = {{{9, 8, 7, 6}}, {{9, 8, 7, 6}}}});
	EXPECT_FALSE(ValidateMaterialFunctionGraph(Graph));
	MarkAsGarbage(Function);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, CallBindingsUseGuidAndDiagnoseRemovedRetypedOrRequiredPorts)
{
	using namespace Durin;
	FMaterialFunctionSignature Signature;
	Signature.Inputs = {FunctionPort(1, EMaterialProgramValueType::Float, "Factor"),
		FunctionPort(2, EMaterialProgramValueType::Texture2D, "Map")};
	Signature.Inputs[0].bRequired = true;
	Signature.Inputs[1].Default.Kind = EMaterialFunctionDefaultKind::Texture;
	Signature.Outputs = {FunctionPort(3, EMaterialProgramValueType::Surface, "Surface"),
		FunctionPort(4, EMaterialProgramValueType::Float, "Value")};
	FMaterialFunctionCallSnapshot Call{.NodeId = {4, 3, 2, 1}, .FunctionPath = "/Functions/Pbr.Pbr",
		.Inputs = {{Signature.Inputs[0].Id, EMaterialProgramValueType::Float, {{9, 8, 7, 6}}}},
		.Outputs = {{Signature.Outputs[0].Id, EMaterialProgramValueType::Surface}}};
	ASSERT_TRUE(ValidateMaterialFunctionCallSignature(Call, Signature));
	std::ranges::reverse(Signature.Inputs);
	std::ranges::reverse(Signature.Outputs);
	Signature.Inputs[1].Name = "RenamedFactor";
	ASSERT_TRUE(ValidateMaterialFunctionCallSignature(Call, Signature));
	Signature.Outputs[1].Type = EMaterialProgramValueType::Float3;
	const auto Retyped = ValidateMaterialFunctionCallSignature(Call, Signature);
	EXPECT_FALSE(Retyped);
	ASSERT_FALSE(Retyped.Diagnostics.empty());
	EXPECT_EQ(Retyped.Diagnostics[0].NodeId, Call.NodeId);
	EXPECT_EQ(Retyped.Diagnostics[0].PortId, Call.Outputs[0].OutputId);
	EXPECT_EQ(Retyped.Diagnostics[0].FunctionAssetPath, Call.FunctionPath);
	Signature.Outputs.pop_back();
	EXPECT_FALSE(ValidateMaterialFunctionCallSignature(Call, Signature));
	Call.Outputs.clear();
	Call.Inputs.clear();
	EXPECT_FALSE(ValidateMaterialFunctionCallSignature(Call, Signature));
}

TEST(FMaterialFunctionTests, BaseTypedCallsRoundTripAndSnapshotsDoNotRetainOwners)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto Root = Testing::CreateTestFixtureDirectory("MaterialFunctionAssets");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/FunctionTests/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath CalleePath, CallerPath, AbstractPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionTests/Callee", CalleePath));
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionTests/Caller", CallerPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionTests/Abstract", AbstractPath));
	EXPECT_FALSE(IAssetTools::Get().CreatePackageLeafAssetForTesting(AbstractPath,
		DMaterialFunctionInterface::StaticClass()));
	DMaterialFunction* Callee = nullptr;
	DMaterialFunction* Caller = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(CalleePath, Callee));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(CallerPath, Caller));
	DMaterialFunctionInterface* Interface = Callee;
	ASSERT_TRUE(Interface->GetFunctionDependencies().empty());
	const auto OutputId = Interface->GetFunctionSignature().Outputs[0].Id;
	const FGuid CallId{0x341558ff, 1, 2, 3};
	auto Graph = Caller->GetFunctionGraph();
	Graph.Nodes.push_back({.Id = CallId, .Opcode = EMaterialProgramOpcode::FunctionCall,
		.ResultType = EMaterialProgramValueType::Surface});
	Graph.Nodes[1].Inputs[0] = {.SourceNodeId = CallId, .SourceOutputId = OutputId};
	Graph.Calls.push_back({.NodeId = CallId, .Function = Interface,
		.Outputs = {{OutputId, EMaterialProgramValueType::Surface}}});
	const FGuid GetId{0x341558ff, 1, 2, 4}, SetId{0x341558ff, 1, 2, 5};
	Graph.Nodes.push_back({.Id = GetId, .Opcode = EMaterialProgramOpcode::GetSurfaceAttributes,
		.ResultType = EMaterialProgramValueType::Surface,
		.Inputs = {{.SourceNodeId = CallId, .SourceOutputId = OutputId}},
		.SurfaceAttributeMask = 1u << static_cast<uint8>(EMaterialSurfaceOutput::Metallic)});
	Graph.Nodes.push_back({.Id = SetId, .Opcode = EMaterialProgramOpcode::SetSurfaceAttributes,
		.ResultType = EMaterialProgramValueType::Surface,
		.Inputs = {{.SourceNodeId = CallId, .SourceOutputId = OutputId}},
		.SurfaceAttributes = {{EMaterialSurfaceOutput::Metallic, {GetId, static_cast<uint8>(EMaterialSurfaceOutput::Metallic)}}}});
	Graph.Nodes[1].Inputs = {{SetId}};
	ASSERT_TRUE(Caller->SetFunctionGraph(Graph));
	FMaterialFunctionSnapshot Before;
	ASSERT_TRUE(Caller->BuildFunctionSnapshot(Before));
	const auto Revision = Caller->GetFunctionRevision();
	ASSERT_TRUE(Caller->SetFunctionPresentation({.Nodes = {{CallId, 140, 240}}}));
	EXPECT_EQ(Caller->GetFunctionRevision(), Revision);
	ASSERT_EQ(Caller->GetFunctionDependencies().size(), 1u);
	ASSERT_TRUE(SavePackage(Callee->GetPackage()));
	ASSERT_TRUE(SavePackage(Caller->GetPackage()));
	const auto CallerFile = FindAssetExact(CallerPath);
	ASSERT_TRUE(CallerFile);
	FAssetPackageInspection Inspection;
	ASSERT_TRUE(InspectAssetPackage(CallerFile->PhysicalPath, CallerPath, Inspection));
	std::vector<FAssetReferenceEdge> References;
	ASSERT_TRUE(ExtractAssetReferences(CallerPath, Inspection, References));
	ASSERT_EQ(References.size(), 1u);
	EXPECT_EQ(References[0].ExpectedClass, "Durin::DMaterialFunctionInterface");
	EXPECT_EQ(References[0].TargetPath.ToString(), Callee->GetObjectPath());
	Graph.Calls.clear();
	ASSERT_TRUE(UnloadPackage(CallerPath));
	ASSERT_TRUE(UnloadPackage(CalleePath));
	CollectGarbage();
	Caller = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(CallerPath), Caller));
	FMaterialFunctionSnapshot After;
	ASSERT_TRUE(Caller->BuildFunctionSnapshot(After));
	EXPECT_EQ(Before.Nodes, After.Nodes);
	EXPECT_EQ(Before.Signature, After.Signature);
	EXPECT_EQ(Before.Calls, After.Calls);
	EXPECT_EQ(Caller->GetFunctionPresentation().Nodes.size(), 1u);
	ASSERT_EQ(Caller->GetFunctionDependencies().size(), 1u);
	EXPECT_EQ(Caller->GetFunctionDependencies()[0]->GetFunctionSignature().Outputs[0].Id, OutputId);
	ASSERT_TRUE(UnloadPackage(CallerPath));
	ASSERT_TRUE(UnloadPackage(CalleePath));
	CollectGarbage();
	EXPECT_EQ(After.Calls[0].Outputs[0].OutputId, OutputId);
}

TEST(FMaterialFunctionTests, ClosureIsDetachedAndRejectsRecursionAndMissingDependencies)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Caller = NewObject<DMaterialFunction>(nullptr, "ClosureCaller");
	auto* Callee = NewObject<DMaterialFunction>(nullptr, "ClosureCallee");
	ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Caller, *Callee));
	const std::array<DMaterialFunctionInterface*, 1> Roots{Caller};
	FMaterialFunctionClosure Closure;
	ASSERT_TRUE(SnapshotMaterialFunctionClosure(Roots, Closure));
	ASSERT_EQ(Closure.Functions.size(), 2u);
	const auto Original = Closure;
	auto Graph = Callee->GetFunctionGraph();
	Graph.Signature.Inputs[0].Default.Surface.EmissiveDefault.X = 2;
	ASSERT_TRUE(Callee->SetFunctionGraph(Graph));
	FMaterialFunctionClosure Edited;
	ASSERT_TRUE(SnapshotMaterialFunctionClosure(Roots, Edited));
	EXPECT_NE(Original, Edited);
	EXPECT_EQ(Original, Closure);
	ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Callee, *Caller));
	const auto Recursive = SnapshotMaterialFunctionClosure(Roots, Closure);
	EXPECT_FALSE(Recursive);
	ASSERT_FALSE(Recursive.Diagnostics.empty());
	EXPECT_EQ(Recursive.Diagnostics[0].CallPath.size(), 2u);
	EXPECT_EQ(Closure, Original);
	Graph = Caller->GetFunctionGraph();
	Graph.Calls[0].Function = nullptr;
	ASSERT_TRUE(Caller->SetFunctionGraph(Graph));
	EXPECT_FALSE(SnapshotMaterialFunctionClosure(Roots, Closure));
	EXPECT_EQ(Closure, Original);
	MarkAsGarbage(Caller);
	MarkAsGarbage(Callee);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, ClosureBoundsIncludeRepeatedSharedSubtrees)
{
	using namespace Durin;
	InitializeDObjectSystem();
	std::vector<DMaterialFunction*> Chain;
	for (uint32 Index = 0; Index <= MaterialFunctionMaxCallDepth; ++Index)
		Chain.push_back(NewObject<DMaterialFunction>(nullptr, FName(std::format("DepthFunction{}", Index))));
	for (size_t Index = 1; Index < Chain.size(); ++Index)
		ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Chain[Index], *Chain[Index - 1]));
	// The valid subtree is visited first so memoization must retain its depth.
	const std::array<DMaterialFunctionInterface*, 2> Roots{Chain[1], Chain.back()};
	FMaterialFunctionClosure Closure;
	const auto Result = SnapshotMaterialFunctionClosure(Roots, Closure);
	EXPECT_FALSE(Result);
	ASSERT_FALSE(Result.Diagnostics.empty());
	EXPECT_EQ(Result.Diagnostics[0].Category, EMaterialProgramDiagnosticCategory::Bounds);
	EXPECT_TRUE(Closure.Functions.empty());
	const std::array<DMaterialFunctionInterface*, 1> ValidRoots{Chain[MaterialFunctionMaxCallDepth - 1]};
	ASSERT_TRUE(SnapshotMaterialFunctionClosure(ValidRoots, Closure));
	EXPECT_EQ(Closure.Functions.size(), MaterialFunctionMaxCallDepth);
	const std::vector<DMaterialFunctionInterface*> RepeatedRoots(MaterialFunctionMaxDependencies + 1, Chain[0]);
	ASSERT_TRUE(SnapshotMaterialFunctionClosure(RepeatedRoots, Closure));
	EXPECT_EQ(Closure.Functions.size(), 1u);
	for (auto* Function : Chain) MarkAsGarbage(Function);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, ExpansionPreservesIndependentInputsMultipleOutputsAndEquivalentKeys)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "ArithmeticFunction");
	FMaterialFunctionGraph Graph;
	Graph.Signature.Inputs = {FunctionPort(1, EMaterialProgramValueType::Float, "Value"),
		FunctionPort(2, EMaterialProgramValueType::Float, "Offset")};
	Graph.Signature.Inputs[0].bRequired = true;
	Graph.Signature.Inputs[1].Default = {.Kind = EMaterialFunctionDefaultKind::Numeric, .Numeric = {1}};
	Graph.Signature.Outputs = {FunctionPort(3, EMaterialProgramValueType::Float, "Sum"),
		FunctionPort(4, EMaterialProgramValueType::Float, "Original")};
	const FGuid ValueId{11, 1, 1, 1}, OffsetId{11, 1, 1, 2}, SumId{11, 1, 1, 3};
	Graph.Nodes = {
		{.Id = ValueId, .Opcode = EMaterialProgramOpcode::FunctionInput, .FunctionPortId = Graph.Signature.Inputs[0].Id},
		{.Id = OffsetId, .Opcode = EMaterialProgramOpcode::FunctionInput, .FunctionPortId = Graph.Signature.Inputs[1].Id},
		{.Id = SumId, .Opcode = EMaterialProgramOpcode::Add, .Inputs = {{ValueId}, {OffsetId}}},
		{.Id = {11, 1, 1, 4}, .Opcode = EMaterialProgramOpcode::FunctionOutput, .Inputs = {{SumId}}, .FunctionPortId = Graph.Signature.Outputs[0].Id},
		{.Id = {11, 1, 1, 5}, .Opcode = EMaterialProgramOpcode::FunctionOutput, .Inputs = {{ValueId}}, .FunctionPortId = Graph.Signature.Outputs[1].Id}};
	ASSERT_TRUE(Function->SetFunctionGraph(Graph));
	FMaterialCompilerInput Input;
	Input.Environment.CompilerIdentity = "FunctionExpansionTest";
	const FGuid FirstValue{12, 1, 1, 1}, SecondValue{12, 1, 1, 2};
	const FGuid FirstCall{12, 1, 1, 3}, SecondCall{12, 1, 1, 4};
	Input.Program.Nodes = {
		{.Id = FirstValue, .Literal = {2}}, {.Id = SecondValue, .Literal = {9}},
		{.Id = FirstCall, .Opcode = EMaterialProgramOpcode::FunctionCall},
		{.Id = SecondCall, .Opcode = EMaterialProgramOpcode::FunctionCall}};
	for (const auto& Pair : {std::pair{FirstCall, FirstValue}, std::pair{SecondCall, SecondValue}})
		Input.FunctionCalls.push_back({.NodeId = Pair.first, .FunctionPath = Function->GetObjectPath(),
			.Inputs = {{Graph.Signature.Inputs[0].Id, EMaterialProgramValueType::Float, {Pair.second}}},
			.Outputs = {{Graph.Signature.Outputs[0].Id, EMaterialProgramValueType::Float},
				{Graph.Signature.Outputs[1].Id, EMaterialProgramValueType::Float}}});
	Input.Program.Outputs.Metallic = {.SourceNodeId = FirstCall, .SourceOutputId = Graph.Signature.Outputs[0].Id};
	Input.Program.Outputs.Roughness = {.SourceNodeId = SecondCall, .SourceOutputId = Graph.Signature.Outputs[0].Id};
	Input.Program.Outputs.AmbientOcclusion = {.SourceNodeId = FirstCall, .SourceOutputId = Graph.Signature.Outputs[1].Id};
	const std::array<DMaterialFunctionInterface*, 1> Roots{Function};
	ASSERT_TRUE(SnapshotMaterialFunctionClosure(Roots, Input.Functions));
	const auto Normalized = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(Normalized) << (Normalized.Diagnostics.empty() ? "" : Normalized.Diagnostics[0].Message);
	const auto& Metal = Normalized.IR.Nodes[Normalized.IR.SurfaceRoot.Inputs[2].ExpressionIndex];
	const auto& Rough = Normalized.IR.Nodes[Normalized.IR.SurfaceRoot.Inputs[3].ExpressionIndex];
	ASSERT_EQ(Metal.Opcode, EMaterialProgramOpcode::Add);
	ASSERT_EQ(Rough.Opcode, EMaterialProgramOpcode::Add);
	const auto HasConstant = [&](const FMaterialIRNode& Node, float Value) {
		return std::ranges::any_of(Node.Inputs, [&](uint32 Index) { return Normalized.IR.Nodes[Index].Literal.X == Value; });
	};
	EXPECT_TRUE(HasConstant(Metal, 2));
	EXPECT_TRUE(HasConstant(Metal, 1));
	EXPECT_TRUE(HasConstant(Rough, 9));
	EXPECT_EQ(Normalized.IR.Nodes[Normalized.IR.SurfaceRoot.Inputs[4].ExpressionIndex].Literal.X, 2);
	EXPECT_TRUE(std::ranges::all_of(Normalized.IR.Nodes, [](const auto& Node) { return Node.Opcode < EMaterialProgramOpcode::FunctionInput; }));
	EXPECT_TRUE(std::ranges::any_of(Normalized.Sources, [&](const auto& Source) {
		return Source.NodeId == SumId && Source.FunctionAssetPath == Function->GetObjectPath()
			&& Source.CallPath == std::vector<FGuid>{FirstCall};
	}));
	std::ranges::reverse(Graph.Nodes);
	std::ranges::reverse(Graph.Signature.Inputs);
	Graph.Signature.Outputs[0].Name = "RenamedSum";
	ASSERT_TRUE(Function->SetFunctionGraph(Graph));
	ASSERT_TRUE(SnapshotMaterialFunctionClosure(Roots, Input.Functions));
	std::ranges::reverse(Input.Program.Nodes);
	std::ranges::reverse(Input.FunctionCalls);
	const auto Reordered = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(Reordered);
	EXPECT_EQ(Normalized.CanonicalBytes, Reordered.CanonicalBytes);
	EXPECT_EQ(Normalized.Identity, Reordered.Identity);
	EXPECT_TRUE(GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout));
	MarkAsGarbage(Function);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, NestedTextureDefaultsYieldToConnectedRootResource)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Leaf = NewObject<DMaterialFunction>(nullptr, "TextureLeaf");
	auto* Wrapper = NewObject<DMaterialFunction>(nullptr, "TextureWrapper");
	FMaterialFunctionGraph Graph;
	Graph.Signature.Inputs = {FunctionPort(1, EMaterialProgramValueType::Texture2D, "Texture"),
		FunctionPort(2, EMaterialProgramValueType::Float2, "UV")};
	Graph.Signature.Inputs[0].Default = {.Kind = EMaterialFunctionDefaultKind::Texture,
		.TextureFallback = EMaterialTextureFallback::Black};
	Graph.Signature.Inputs[1].Default.Kind = EMaterialFunctionDefaultKind::UV0;
	Graph.Signature.Outputs = {FunctionPort(3, EMaterialProgramValueType::Float3, "Color")};
	const FGuid TextureId{31, 1, 1, 1}, UVId{31, 1, 1, 2}, SampleId{31, 1, 1, 3}, ColorId{31, 1, 1, 4};
	Graph.Nodes = {
		{.Id = TextureId, .Opcode = EMaterialProgramOpcode::FunctionInput, .ResultType = EMaterialProgramValueType::Texture2D,
			.FunctionPortId = Graph.Signature.Inputs[0].Id},
		{.Id = UVId, .Opcode = EMaterialProgramOpcode::FunctionInput, .ResultType = EMaterialProgramValueType::Float2,
			.FunctionPortId = Graph.Signature.Inputs[1].Id},
		{.Id = SampleId, .Opcode = EMaterialProgramOpcode::TextureSample2D, .ResultType = EMaterialProgramValueType::Float4,
			.Inputs = {{TextureId}, {UVId}}},
		{.Id = ColorId, .Opcode = EMaterialProgramOpcode::Swizzle, .ResultType = EMaterialProgramValueType::Float3,
			.Inputs = {{SampleId}}, .SwizzleLength = 3, .SwizzleX = 0, .SwizzleY = 1, .SwizzleZ = 2},
		{.Id = {31, 1, 1, 5}, .Opcode = EMaterialProgramOpcode::FunctionOutput, .ResultType = EMaterialProgramValueType::Float3,
			.Inputs = {{ColorId}}, .FunctionPortId = Graph.Signature.Outputs[0].Id}};
	ASSERT_TRUE(Leaf->SetFunctionGraph(Graph));
	FMaterialFunctionGraph Outer;
	Outer.Signature.Inputs = {Graph.Signature.Inputs[0]};
	Outer.Signature.Inputs[0].Default.TextureFallback = EMaterialTextureFallback::FlatRGNormal;
	Outer.Signature.Outputs = Graph.Signature.Outputs;
	const FGuid CallId{32, 1, 1, 1};
	Outer.Nodes = {Graph.Nodes[0],
		{.Id = CallId, .Opcode = EMaterialProgramOpcode::FunctionCall},
		{.Id = {32, 1, 1, 2}, .Opcode = EMaterialProgramOpcode::FunctionOutput, .ResultType = EMaterialProgramValueType::Float3,
			.Inputs = {{.SourceNodeId = CallId, .SourceOutputId = Graph.Signature.Outputs[0].Id}},
			.FunctionPortId = Graph.Signature.Outputs[0].Id}};
	Outer.Calls = {{.NodeId = CallId, .Function = Leaf,
		.Inputs = {{Graph.Signature.Inputs[0].Id, EMaterialProgramValueType::Texture2D, {TextureId}}},
		.Outputs = {{Graph.Signature.Outputs[0].Id, EMaterialProgramValueType::Float3}}}};
	ASSERT_TRUE(Wrapper->SetFunctionGraph(Outer));
	FMaterialCompilerInput Input;
	Input.Environment.CompilerIdentity = "NestedTextureTest";
	const FGuid RootCall{33, 1, 1, 1}, ParameterNode{33, 1, 1, 2}, ParameterId{33, 1, 1, 3};
	Input.Program.Nodes = {{.Id = RootCall, .Opcode = EMaterialProgramOpcode::FunctionCall}};
	Input.Program.Outputs.BaseColor = {.SourceNodeId = RootCall, .SourceOutputId = Graph.Signature.Outputs[0].Id};
	Input.FunctionCalls = {{.NodeId = RootCall, .FunctionPath = Wrapper->GetObjectPath(),
		.Outputs = {{Graph.Signature.Outputs[0].Id, EMaterialProgramValueType::Float3}}}};
	const std::array<DMaterialFunctionInterface*, 1> Roots{Wrapper};
	ASSERT_TRUE(SnapshotMaterialFunctionClosure(Roots, Input.Functions));
	const auto Default = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(Default) << (Default.Diagnostics.empty() ? "" : Default.Diagnostics[0].Message);
	EXPECT_EQ(Default.Layout.ResourceFieldCount, 0u);
	EXPECT_TRUE(std::ranges::any_of(Default.IR.Nodes, [](const auto& Node) {
		return Node.Opcode == EMaterialProgramOpcode::Constant && Node.ResultType == EMaterialProgramValueType::Float4
			&& Node.Literal == FMaterialProgramLiteral{0.5f, 0.5f, 1, 1};
	}));
	Input.Program.Nodes.push_back({.Id = ParameterNode, .Opcode = EMaterialProgramOpcode::TextureParameter,
		.ResultType = EMaterialProgramValueType::Texture2D, .Parameter = {.Id = ParameterId, .Type = EMaterialParameterType::Texture}});
	Input.Parameters.push_back({ParameterId, EMaterialParameterType::Texture});
	Input.FunctionCalls[0].Inputs.push_back({Graph.Signature.Inputs[0].Id, EMaterialProgramValueType::Texture2D, {ParameterNode}});
	const auto Connected = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(Connected) << (Connected.Diagnostics.empty() ? "" : Connected.Diagnostics[0].Message);
	EXPECT_EQ(Connected.Layout.ResourceFieldCount, 1u);
	EXPECT_EQ(std::ranges::count(Connected.IR.Nodes, EMaterialProgramOpcode::TextureSample2D, &FMaterialIRNode::Opcode), 1);
	ASSERT_EQ(Connected.ActiveParameters.size(), 1u);
	EXPECT_EQ(Connected.ActiveParameters[0].Id, ParameterId);
	EXPECT_TRUE(GenerateMaterialProgramSlang(Connected.IR, Connected.Layout));
	// Compose the sampled texture into a Surface function and compile real shaders.
	Graph.Signature.Inputs.push_back(FunctionPort(4, EMaterialProgramValueType::Surface, "BaseSurface"));
	Graph.Signature.Inputs.back().Default.Kind = EMaterialFunctionDefaultKind::Surface;
	Graph.Signature.Outputs[0].Type = EMaterialProgramValueType::Surface;
	const FGuid SurfaceInput{34, 1, 1, 1}, SurfaceSet{34, 1, 1, 2};
	Graph.Nodes[4].ResultType = EMaterialProgramValueType::Surface;
	Graph.Nodes[4].Inputs = {{SurfaceSet}};
	Graph.Nodes.push_back({.Id = SurfaceInput, .Opcode = EMaterialProgramOpcode::FunctionInput,
		.ResultType = EMaterialProgramValueType::Surface, .FunctionPortId = Graph.Signature.Inputs.back().Id});
	Graph.Nodes.push_back({.Id = SurfaceSet, .Opcode = EMaterialProgramOpcode::SetSurfaceAttributes,
		.ResultType = EMaterialProgramValueType::Surface, .Inputs = {{SurfaceInput}},
		.SurfaceAttributes = {{EMaterialSurfaceOutput::BaseColor, {ColorId}}}});
	ASSERT_TRUE(Leaf->SetFunctionGraph(Graph));
	Input.FunctionCalls[0].FunctionPath = Leaf->GetObjectPath();
	Input.FunctionCalls[0].Outputs[0].ExpectedType = EMaterialProgramValueType::Surface;
	Input.Program.Outputs = {};
	Input.Program.Outputs.Surface = {.SourceNodeId = RootCall, .SourceOutputId = Graph.Signature.Outputs[0].Id};
	const std::array<DMaterialFunctionInterface*, 1> SurfaceRoots{Leaf};
	ASSERT_TRUE(SnapshotMaterialFunctionClosure(SurfaceRoots, Input.Functions));
	FModuleManager::Get().LoadModule("RenderCore");
	std::string Error;
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Input.Environment, Error)) << Error;
	const auto Compiled = CompileMaterialProgram(Input, true);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "" : Compiled.Diagnostics[0].Message);
	EXPECT_EQ(Compiled.Layout.ResourceFieldCount, 1u);
	EXPECT_FALSE(Compiled.CompiledShaders.empty());
	MarkAsGarbage(Wrapper);
	MarkAsGarbage(Leaf);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, RootCallsCommitAtomicallyAndSnapshotThroughInstances)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "RootSurfaceFunction");
	auto* Material = NewObject<DMaterial>(nullptr, "RootFunctionMaterial");
	auto* Instance = NewObject<DMaterialInstance>(nullptr, "RootFunctionInstance");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(Instance->SetParent(Material));
	const auto& Output = Function->GetFunctionSignature().Outputs[0];
	const FGuid CallId{41, 1, 1, 1};
	FMaterialProgram Program;
	Program.Nodes = {{.Id = CallId, .Opcode = EMaterialProgramOpcode::FunctionCall}};
	Program.Outputs.Surface = {.SourceNodeId = CallId, .SourceOutputId = Output.Id};
	std::vector<FMaterialFunctionCall> Calls{{.NodeId = CallId, .Function = Function,
		.Outputs = {{Output.Id, Output.Type}}}};
	const auto Before = *Material->GetMaterialProgram();
	EXPECT_FALSE(Material->SetMaterialProgram(Program));
	EXPECT_EQ(*Material->GetMaterialProgram(), Before);
	ASSERT_TRUE(Material->SetMaterialProgramAndFunctionCalls(Program, Calls));
	EXPECT_EQ(Instance->GetMaterialFunctionCalls().size(), 1u);
	FMaterialCompilerInput Input;
	ASSERT_TRUE(SnapshotMaterialCompilerInput(*Instance, {.CompilerIdentity = "RootFunctionTest"}, Input));
	EXPECT_EQ(Input.FunctionCalls.size(), 1u);
	EXPECT_EQ(Input.Functions.Functions.size(), 1u);
	const auto Normalized = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(Normalized) << (Normalized.Diagnostics.empty() ? "" : Normalized.Diagnostics[0].Message);
	EXPECT_TRUE(GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout));
	Calls[0].Function = nullptr;
	ASSERT_TRUE(Material->SetMaterialProgramAndFunctionCalls(Program, Calls));
	const auto OldCalls = Input.FunctionCalls;
	const auto Missing = SnapshotMaterialCompilerInput(*Material, {}, Input);
	EXPECT_FALSE(Missing);
	ASSERT_FALSE(Missing.Diagnostics.empty());
	EXPECT_EQ(Missing.Diagnostics[0].NodeId, CallId);
	EXPECT_EQ(Input.FunctionCalls, OldCalls);
	MarkAsGarbage(Instance);
	MarkAsGarbage(Material);
	MarkAsGarbage(Function);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, ImportProvenanceRoundtripsWithoutChangingGraphOrCompileRevision)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("ImportProvenance");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/ImportProvenance/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid());
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath ParentPath, InstancePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/ImportProvenance/Parent", ParentPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/ImportProvenance/Instance", InstancePath));
	DMaterial* Parent = nullptr;
	DMaterialInstance* Instance = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(ParentPath, Parent));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(InstancePath, Instance));
	Parent->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(Instance->SetParent(Parent));
	const auto Program = *Parent->GetMaterialProgram();
	const auto Revision = Parent->GetMaterialCompileStatus().AuthoredRevision;
	const FMaterialImportProvenance ParentReceipt{.RecipeId = "Durin.ImportedSurface", .RecipeVersion = 1,
		.StructuralKey = "Durin.ImportedSurface:1;d;d;d;d;d;d;d;d"};
	auto InstanceReceipt = ParentReceipt;
	InstanceReceipt.SourceIdentity = "source.gltf";
	InstanceReceipt.OutputIdentity = "scene:material:stable";
	ASSERT_TRUE(Parent->SetImportProvenance(ParentReceipt));
	ASSERT_TRUE(Instance->SetImportProvenance(InstanceReceipt));
	EXPECT_EQ(Parent->GetMaterialCompileStatus().AuthoredRevision, Revision);
	EXPECT_EQ(*Parent->GetMaterialProgram(), Program);
	EXPECT_TRUE(Parent->GetParameterDefinitions().empty());
	auto Invalid = InstanceReceipt;
	Invalid.SourceIdentity.assign(4097, 'x');
	EXPECT_FALSE(Instance->SetImportProvenance(Invalid));
	EXPECT_EQ(Instance->GetImportProvenance(), InstanceReceipt);
	ASSERT_TRUE(SavePackage(Parent->GetPackage()));
	ASSERT_TRUE(SavePackage(Instance->GetPackage()));
	ASSERT_TRUE(UnloadPackage(InstancePath));
	ASSERT_TRUE(UnloadPackage(ParentPath));
	CollectGarbage();
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(InstancePath), Instance));
	Parent = Cast<DMaterial>(Instance->GetParent());
	ASSERT_NE(Parent, nullptr);
	EXPECT_EQ(Parent->GetImportProvenance(), ParentReceipt);
	EXPECT_EQ(Instance->GetImportProvenance(), InstanceReceipt);
	EXPECT_EQ(*Parent->GetMaterialProgram(), Program);
	ASSERT_TRUE(UnloadPackage(InstancePath));
	ASSERT_TRUE(UnloadPackage(ParentPath));
	CollectGarbage();
}

TEST(FMaterialFunctionTests, CompactDefaultsRoundtripAndRejectSpoofedLegacySchemas)
{
	using namespace Durin;
	using namespace Durin::Editor::Material;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("CompactRoundtrip");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/CompactRoundtrip/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid());
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath MaterialPath, FunctionPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/CompactRoundtrip/Material", MaterialPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/CompactRoundtrip/Function", FunctionPath));
	DMaterial* Material = nullptr;
	DMaterialFunction* Function = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(MaterialPath, Material));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(FunctionPath, Function));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material), FunctionDocument(*Function);
	ASSERT_TRUE(Document.CreateNodeWithDefaultInputs({.Node = {.Opcode = EMaterialProgramOpcode::TextureSampleParameter2D,
		.ResultType = EMaterialProgramValueType::Float4, .Inputs = {{}}}}));
	ASSERT_TRUE(FunctionDocument.CreateNodeWithDefaultInputs({.Node = {.Opcode = EMaterialProgramOpcode::Multiply, .Inputs = {{}, {}}}}));
	const auto Call = Document.InsertFunctionCall(*Function, 300, 0);
	ASSERT_TRUE(Call);
	ASSERT_TRUE(Document.AssignMaterialOutput(std::nullopt, {Call.GeneratedNodeIds[0], 0, Function->GetFunctionSignature().Outputs[0].Id}));
	const auto Expected = *Material->GetMaterialProgram();
	const auto ExpectedFunction = Function->GetFunctionGraph();
	const auto TextureId = Material->GetMaterialProgram()->Nodes.front().Parameter.Id;
	ASSERT_TRUE(SavePackage(Function->GetPackage()));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ASSERT_TRUE(UnloadPackage(FunctionPath));
	CollectGarbage();
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath), Material));
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(FunctionPath), Function));
	EXPECT_EQ(*Material->GetMaterialProgram(), Expected);
	EXPECT_EQ(Function->GetFunctionGraph(), ExpectedFunction);
	EXPECT_NE(Material->FindParameterDefinition(TextureId), nullptr);
	EXPECT_EQ(Material->GetMaterialFunctionCalls()[0].Function.Get(), Function);
	// Old schema tags must not reinterpret a payload that already contains new semantics.
	const_cast<FMaterialProgram*>(Material->GetMaterialProgram())->SchemaVersion = 5;
	Material->PostLoad();
	EXPECT_EQ(Material->GetMaterialProgram()->SchemaVersion, 5u);
	const_cast<FMaterialProgram*>(Material->GetMaterialProgram())->SchemaVersion = CurrentMaterialProgramSchemaVersion;
	const_cast<FMaterialFunctionGraph&>(Function->GetFunctionGraph()).SchemaVersion = 1;
	Function->PostLoad();
	EXPECT_EQ(Function->GetFunctionGraph().SchemaVersion, 1u);
	const_cast<FMaterialFunctionGraph&>(Function->GetFunctionGraph()).SchemaVersion = CurrentMaterialFunctionSchemaVersion;
	auto Invalid = Expected;
	Invalid.Nodes.front().UVSettings.Rotation.Literal.X = std::numeric_limits<float>::infinity();
	const auto Rejected = ValidateMaterialProgramWithFunctions(Invalid, Material->GetParameterDefinitions(), Material->GetMaterialFunctionCalls());
	EXPECT_FALSE(Rejected);
	ASSERT_FALSE(Rejected.Diagnostics.empty());
	EXPECT_EQ(Rejected.Diagnostics.front().UVFieldIndex, 3u);
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ASSERT_TRUE(UnloadPackage(FunctionPath));
	CollectGarbage();
}

TEST(FMaterialFunctionTests, RejectsOldRootSchemaAndPreservesCurrentFunctionReferences)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto Root = Testing::CreateTestFixtureDirectory("RootFunctionAssets");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/RootFunctionTests/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath MaterialPath, FunctionPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/RootFunctionTests/Material", MaterialPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/RootFunctionTests/Function", FunctionPath));
	DMaterial* Material = nullptr;
	DMaterialFunction* Function = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(MaterialPath, Material));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(FunctionPath, Function));
	// Saving unsupported authored versions must fail without rewriting the graph.
	const_cast<FMaterialProgram*>(Material->GetMaterialProgram())->SchemaVersion = 4;
	EXPECT_FALSE(SavePackage(Material->GetPackage()));
	EXPECT_EQ(Material->GetMaterialProgram()->SchemaVersion, 4u);
	const_cast<FMaterialProgram*>(Material->GetMaterialProgram())->SchemaVersion = CurrentMaterialProgramSchemaVersion;
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialProgram Program;
	const FGuid CallId{42, 1, 1, 1};
	const auto& Output = Function->GetFunctionSignature().Outputs[0];
	Program.Nodes = {{.Id = CallId, .Opcode = EMaterialProgramOpcode::FunctionCall}};
	Program.Outputs.Surface = {.SourceNodeId = CallId, .SourceOutputId = Output.Id};
	ASSERT_TRUE(Material->SetMaterialProgramAndFunctionCalls(Program,
		{{.NodeId = CallId, .Function = Function, .Outputs = {{Output.Id, Output.Type}}}}));
	ASSERT_TRUE(SavePackage(Function->GetPackage()));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	FAssetPackageInspection Inspection;
	const auto File = FindAssetExact(MaterialPath);
	ASSERT_TRUE(File);
	ASSERT_TRUE(InspectAssetPackage(File->PhysicalPath, MaterialPath, Inspection));
	std::vector<FAssetReferenceEdge> References;
	ASSERT_TRUE(ExtractAssetReferences(MaterialPath, Inspection, References));
	EXPECT_TRUE(std::ranges::any_of(References, [&](const auto& Reference) {
		return Reference.ExpectedClass == "Durin::DMaterialFunctionInterface"
			&& Reference.TargetPath.ToString() == Function->GetObjectPath();
	}));
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ASSERT_TRUE(UnloadPackage(FunctionPath));
	CollectGarbage();
	Material = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath), Material));
	EXPECT_EQ(*Material->GetMaterialProgram(), Program);
	ASSERT_EQ(Material->GetMaterialFunctionCalls().size(), 1u);
	FMaterialCompilerInput Input;
	ASSERT_TRUE(SnapshotMaterialCompilerInput(*Material, {.CompilerIdentity = "RootRoundTrip"}, Input));
	EXPECT_TRUE(NormalizeMaterialProgram(Input));
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ASSERT_TRUE(UnloadPackage(FunctionPath));
	CollectGarbage();
}

TEST(FMaterialFunctionTests, NestedSurfaceOverridesAndSelectedOutputsPreserveAttributes)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Leaf = NewObject<DMaterialFunction>(nullptr, "SurfaceOverrideLeaf");
	auto* Wrapper = NewObject<DMaterialFunction>(nullptr, "SurfaceOverrideWrapper");
	auto Graph = Leaf->GetFunctionGraph();
	const FGuid ValueId{51, 1, 1, 1}, SetId{51, 1, 1, 2};
	Graph.Nodes.push_back({.Id = ValueId, .Literal = {0.75f}});
	Graph.Nodes.push_back({.Id = SetId, .Opcode = EMaterialProgramOpcode::SetSurfaceAttributes,
		.ResultType = EMaterialProgramValueType::Surface, .Inputs = {{Graph.Nodes[0].Id}},
		.SurfaceAttributes = {{EMaterialSurfaceOutput::Metallic, {ValueId}}}});
	Graph.Nodes[1].Inputs = {{SetId}};
	ASSERT_TRUE(Leaf->SetFunctionGraph(Graph));
	ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Wrapper, *Leaf));
	FMaterialCompilerInput Input;
	Input.Environment.CompilerIdentity = "SurfaceAttributeTest";
	const FGuid CallId{52, 1, 1, 1}, GetId{52, 1, 1, 2};
	const auto Output = Wrapper->GetFunctionSignature().Outputs[0];
	Input.Program.Nodes = {{.Id = CallId, .Opcode = EMaterialProgramOpcode::FunctionCall},
		{.Id = GetId, .Opcode = EMaterialProgramOpcode::GetSurfaceAttributes, .ResultType = EMaterialProgramValueType::Surface,
			.Inputs = {{.SourceNodeId = CallId, .SourceOutputId = Output.Id}},
			.SurfaceAttributeMask = (1u << static_cast<uint8>(EMaterialSurfaceOutput::Normal))
				| (1u << static_cast<uint8>(EMaterialSurfaceOutput::Metallic))}};
	Input.FunctionCalls = {{.NodeId = CallId, .FunctionPath = Wrapper->GetObjectPath(), .Outputs = {{Output.Id, Output.Type}}}};
	Input.Program.Outputs.BaseColor = {GetId, static_cast<uint8>(EMaterialSurfaceOutput::Normal)};
	Input.Program.Outputs.Roughness = {GetId, static_cast<uint8>(EMaterialSurfaceOutput::Metallic)};
	const std::array<DMaterialFunctionInterface*, 1> Roots{Wrapper};
	ASSERT_TRUE(SnapshotMaterialFunctionClosure(Roots, Input.Functions));
	const auto Normalized = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(Normalized) << (Normalized.Diagnostics.empty() ? "" : Normalized.Diagnostics[0].Message);
	EXPECT_EQ(Normalized.IR.Nodes[Normalized.IR.SurfaceRoot.Inputs[0].ExpressionIndex].Literal, (FMaterialProgramLiteral{0, 0, 1}));
	EXPECT_EQ(Normalized.IR.Nodes[Normalized.IR.SurfaceRoot.Inputs[3].ExpressionIndex].Literal.X, 0.75f);
	EXPECT_TRUE(std::ranges::all_of(Normalized.IR.Nodes, [](const auto& Node) { return Node.Opcode < EMaterialProgramOpcode::FunctionInput; }));
	EXPECT_TRUE(GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout));
	Input.Program.Nodes[1].SurfaceAttributeMask |= 1u << static_cast<uint8>(EMaterialSurfaceOutput::Roughness);
	const auto MoreVisible = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(MoreVisible);
	EXPECT_EQ(MoreVisible.Identity, Normalized.Identity);
	Input.Program.Nodes[1].SurfaceAttributeMask = 1u << static_cast<uint8>(EMaterialSurfaceOutput::Normal);
	EXPECT_FALSE(NormalizeMaterialProgram(Input));
	Graph.Nodes.back().SurfaceAttributes[0].Source = {SetId};
	EXPECT_FALSE(Leaf->SetFunctionGraph(Graph));
	MarkAsGarbage(Wrapper);
	MarkAsGarbage(Leaf);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, SurfaceOverridesSupportAllEightAttributesAndRejectInvalidBindings)
{
	using namespace Durin;
	FMaterialCompilerInput Input;
	Input.Environment.CompilerIdentity = "AllSurfaceAttributesTest";
	FMaterialProgramNode Base{.Id = {53, 1, 1, 1}, .Opcode = EMaterialProgramOpcode::MakeSurface,
		.ResultType = EMaterialProgramValueType::Surface};
	FMaterialProgramNode Set{.Id = {53, 1, 1, 2}, .Opcode = EMaterialProgramOpcode::SetSurfaceAttributes,
		.ResultType = EMaterialProgramValueType::Surface, .Inputs = {{Base.Id}}};
	for (uint8 Index = 0; Index < 8; ++Index)
	{
		const auto Attribute = static_cast<EMaterialSurfaceOutput>(Index);
		const FGuid Id{53, 1, 1, uint32(Index) + 3};
		Input.Program.Nodes.push_back({.Id = Id, .ResultType = GetMaterialSurfaceOutputType(Attribute),
			.Literal = {0.125f * Index, 0.25f, 0.5f}});
		Base.Inputs.push_back({Id});
		Set.SurfaceAttributes.push_back({Attribute, {Id}});
	}
	Input.Program.Nodes.push_back(Base);
	Input.Program.Nodes.push_back(Set);
	Input.Program.Outputs.Surface = {Set.Id};
	const auto Normalized = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(Normalized) << (Normalized.Diagnostics.empty() ? "" : Normalized.Diagnostics[0].Message);
	EXPECT_TRUE(GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout));
	EXPECT_EQ(Input.Program.Nodes.back().Inputs.size(), 1u);
	std::ranges::reverse(Input.Program.Nodes.back().SurfaceAttributes);
	const auto Reordered = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(Reordered);
	EXPECT_EQ(Normalized.Identity, Reordered.Identity);
	Input.Program.Nodes.back().SurfaceAttributes.clear();
	const auto Passthrough = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(Passthrough);
	EXPECT_EQ(Normalized.Identity, Passthrough.Identity);
	Input.Program.Nodes.back().SurfaceAttributes = {{EMaterialSurfaceOutput::Metallic, {Base.Inputs[0]}}};
	EXPECT_FALSE(NormalizeMaterialProgram(Input));
	Input.Program.Nodes.back().SurfaceAttributes = {{EMaterialSurfaceOutput::Metallic, Base.Inputs[2]},
		{EMaterialSurfaceOutput::Metallic, Base.Inputs[2]}};
	EXPECT_FALSE(NormalizeMaterialProgram(Input));
	Input.Program.Nodes.back().SurfaceAttributes = {{static_cast<EMaterialSurfaceOutput>(255), Base.Inputs[2]}};
	EXPECT_FALSE(NormalizeMaterialProgram(Input));
}

TEST(FMaterialFunctionTests, NestedDiagnosticsIdentifyOwningDocumentAndRootInvocation)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Leaf = NewObject<DMaterialFunction>(nullptr, "DiagnosticLeaf");
	auto* Wrapper = NewObject<DMaterialFunction>(nullptr, "DiagnosticWrapper");
	ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Wrapper, *Leaf));
	const FGuid RootCall{54, 1, 1, 1};
	const auto Output = Wrapper->GetFunctionSignature().Outputs[0];
	const auto NestedCall = Wrapper->GetFunctionGraph().Calls[0].NodeId;
	std::vector<FMaterialFunctionCall> Calls{{.NodeId = RootCall, .Function = Wrapper,
		.Outputs = {{Output.Id, Output.Type}}}};
	FMaterialCompilerInput Input;
	Input.Environment.CompilerIdentity = "NestedDiagnosticTest";
	Input.Program.Nodes = {{.Id = RootCall, .Opcode = EMaterialProgramOpcode::FunctionCall}};
	Input.Program.Outputs.Surface = {.SourceNodeId = RootCall, .SourceOutputId = Output.Id};
	ASSERT_TRUE(SnapshotMaterialFunctionCalls(Calls, Input.FunctionCalls, Input.Functions));
	const auto LeafSnapshot = std::ranges::find(Input.Functions.Functions, Leaf->GetObjectPath(), &FMaterialFunctionSnapshot::AssetPath);
	ASSERT_NE(LeafSnapshot, Input.Functions.Functions.end());
	const auto TerminalId = LeafSnapshot->Nodes[1].Id;
	LeafSnapshot->Nodes[1].Inputs[0].SourceNodeId = {99, 99, 99, 99};
	const auto Broken = NormalizeMaterialProgram(Input);
	EXPECT_FALSE(Broken);
	EXPECT_TRUE(std::ranges::any_of(Broken.Diagnostics, [&](const auto& Diagnostic) {
		return Diagnostic.NodeId == TerminalId && Diagnostic.FunctionAssetPath == Leaf->GetObjectPath()
			&& Diagnostic.CallPath == std::vector<FGuid>{RootCall, NestedCall};
	}));
	auto Graph = Wrapper->GetFunctionGraph();
	Graph.Calls[0].Outputs[0].OutputId = {98, 98, 98, 98};
	Graph.Nodes[1].Inputs[0].SourceOutputId = Graph.Calls[0].Outputs[0].OutputId;
	ASSERT_TRUE(Wrapper->SetFunctionGraph(Graph));
	const auto InvalidPort = SnapshotMaterialFunctionCalls(Calls, Input.FunctionCalls, Input.Functions);
	EXPECT_FALSE(InvalidPort);
	ASSERT_FALSE(InvalidPort.Diagnostics.empty());
	EXPECT_EQ(InvalidPort.Diagnostics[0].NodeId, NestedCall);
	EXPECT_EQ(InvalidPort.Diagnostics[0].FunctionAssetPath, Wrapper->GetObjectPath());
	EXPECT_EQ(InvalidPort.Diagnostics[0].CallPath, (std::vector<FGuid>{RootCall}));
	Calls.clear();
	MarkAsGarbage(Wrapper);
	MarkAsGarbage(Leaf);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, DependencyEditsPreserveAcceptedContractsAndOwnerSourceMaps)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	auto* Leaf = NewObject<DMaterialFunction>(nullptr, "LifecycleFunctionLeaf");
	auto* Wrapper = NewObject<DMaterialFunction>(nullptr, "LifecycleFunctionWrapper");
	ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Wrapper, *Leaf));
	auto* First = NewObject<DMaterial>(nullptr, "FirstFunctionCaller");
	auto* Second = NewObject<DMaterial>(nullptr, "SecondFunctionCaller");
	auto* Child = NewObject<DMaterialInstance>(nullptr, "FunctionCallerChild");
	First->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	Second->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(Child->SetParent(First));
	const auto Output = Wrapper->GetFunctionSignature().Outputs[0];
	const FGuid FirstCall{55, 1, 1, 1}, SecondCall{55, 1, 1, 2};
	for (const auto& Pair : {std::pair{First, FirstCall}, std::pair{Second, SecondCall}})
	{
		FMaterialProgram Program;
		Program.Nodes = {{.Id = Pair.second, .Opcode = EMaterialProgramOpcode::FunctionCall}};
		Program.Outputs.Surface = {.SourceNodeId = Pair.second, .SourceOutputId = Output.Id};
		ASSERT_TRUE(Pair.first->SetMaterialProgramAndFunctionCalls(Program,
			{{.NodeId = Pair.second, .Function = Wrapper, .Outputs = {{Output.Id, Output.Type}}}}));
		ASSERT_TRUE(Pair.first->CompileEdits());
	}
	const auto Accepted = First->GetAcceptedCompiledProgram();
	ASSERT_TRUE(Accepted);
	EXPECT_EQ(Second->GetAcceptedCompiledProgram(), Accepted);
	EXPECT_EQ(Child->GetAcceptedCompiledProgram(), Accepted);
	ASSERT_FALSE(First->GetAcceptedExpressionSources().empty());
	ASSERT_FALSE(Second->GetAcceptedExpressionSources().empty());
	EXPECT_EQ(First->GetAcceptedExpressionSources()[0].CallPath[0], FirstCall);
	EXPECT_EQ(Second->GetAcceptedExpressionSources()[0].CallPath[0], SecondCall);
	const auto Before = First->GetMaterialCompileStatus();
	ASSERT_TRUE(Leaf->SetFunctionPresentation({.Nodes = {{Leaf->GetFunctionGraph().Nodes[0].Id, 70, 80}}}));
	EXPECT_EQ(First->GetMaterialCompileStatus().AuthoredRevision, Before.AuthoredRevision);
	auto Graph = Leaf->GetFunctionGraph();
	Graph.Signature.Inputs[0].Default.Surface.RoughnessDefault.X = 0.21f;
	ASSERT_TRUE(Leaf->SetFunctionGraph(Graph));
	for (const DMaterialInterface* Caller : {static_cast<DMaterialInterface*>(First), static_cast<DMaterialInterface*>(Second),
		static_cast<DMaterialInterface*>(Child)})
	{
		EXPECT_EQ(Caller->GetMaterialCompileStatus().State, EMaterialCompileState::NeedsCompile);
		EXPECT_EQ(Caller->GetAcceptedCompiledProgram(), Accepted);
	}
	FMaterialCompileResult Stale{.Owner = MakeObjectHandle(First), .AuthoredRevision = Before.AuthoredRevision,
		.Generation = Before.RequestGeneration, .DependencyRevision = Before.DependencyRevision,
		.ParentChainRevision = Before.ParentChainRevision, .ProgramIdentity = Before.RequestedIdentity,
		.StaticProperties = First->GetStaticProperties(), .Target = Before.Target,
		.State = EMaterialCompileState::Ready, .CompiledProgram = Accepted};
	EXPECT_FALSE(Private::FMaterialCompilationLifecycle::Admit(*First, Stale));
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), Accepted);
	ASSERT_TRUE(First->CompileEdits());
	ASSERT_TRUE(Second->CompileEdits());
	EXPECT_NE(First->GetAcceptedCompiledProgram()->Identity, Accepted->Identity);
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), Child->GetAcceptedCompiledProgram());
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), Second->GetAcceptedCompiledProgram());
	const auto OriginalWrapper = Wrapper->GetFunctionGraph();
	auto BrokenWrapper = OriginalWrapper;
	BrokenWrapper.Calls[0].Function = nullptr;
	ASSERT_TRUE(Wrapper->SetFunctionGraph(BrokenWrapper));
	EXPECT_FALSE(First->CompileEdits());
	EXPECT_FALSE(First->GetAcceptedCompiledProgram());
	EXPECT_FALSE(Child->GetAcceptedCompiledProgram());
	EXPECT_TRUE(First->GetAcceptedExpressionSources().empty());
	ASSERT_TRUE(Wrapper->SetFunctionGraph(OriginalWrapper));
	ASSERT_TRUE(First->CompileEdits());
	EXPECT_TRUE(First->GetAcceptedCompiledProgram());
	const auto Current = First->GetMaterialCompileStatus();
	const auto CurrentProgram = First->GetAcceptedCompiledProgram();
	// Simulate a dependency changing across a missed external notification. The
	// publication boundary must compare captured versions independently of events.
	auto* FirstBinding = const_cast<FMaterialFunctionCall*>(First->GetMaterialFunctionCalls().data());
	auto* SecondBinding = const_cast<FMaterialFunctionCall*>(Second->GetMaterialFunctionCalls().data());
	FirstBinding->Function = nullptr;
	SecondBinding->Function = nullptr;
	Graph.Signature.Inputs[0].Default.Surface.RoughnessDefault.X = 0.37f;
	ASSERT_TRUE(Leaf->SetFunctionGraph(Graph));
	FirstBinding->Function = Wrapper;
	SecondBinding->Function = Wrapper;
	EXPECT_EQ(First->GetMaterialCompileStatus().AuthoredRevision, Current.AuthoredRevision);
	FMaterialCompileResult StaleClosure{.Owner = MakeObjectHandle(First), .AuthoredRevision = Current.AuthoredRevision,
		.Generation = Current.RequestGeneration, .DependencyRevision = Current.DependencyRevision,
		.ParentChainRevision = Current.ParentChainRevision, .ProgramIdentity = Current.RequestedIdentity,
		.StaticProperties = First->GetStaticProperties(), .Target = Current.Target,
		.State = EMaterialCompileState::Ready, .CompiledProgram = CurrentProgram};
	EXPECT_FALSE(Private::FMaterialCompilationLifecycle::Admit(*First, std::move(StaleClosure)));
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), CurrentProgram);
	EXPECT_EQ(First->GetMaterialCompileStatus().State, EMaterialCompileState::NeedsCompile);
	ASSERT_TRUE(First->CompileEdits());
	EXPECT_NE(First->GetAcceptedCompiledProgram()->Identity, CurrentProgram->Identity);
	MarkAsGarbage(Child);
	MarkAsGarbage(Second);
	MarkAsGarbage(First);
	MarkAsGarbage(Wrapper);
	MarkAsGarbage(Leaf);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, RelocationRefreshesNestedCallersAndDeletionHonorsReferences)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("FunctionMutationAssets");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/FunctionMutationTests/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath WrapperPath, LeafPath, MovedPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionMutationTests/Wrapper", WrapperPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionMutationTests/Leaf", LeafPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/FunctionMutationTests/Moved", MovedPath));
	DMaterialFunction* Wrapper = nullptr;
	DMaterialFunction* Leaf = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(WrapperPath, Wrapper));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(LeafPath, Leaf));
	const auto OriginalGraph = Wrapper->GetFunctionGraph();
	ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Wrapper, *Leaf));
	ASSERT_TRUE(SavePackage(Leaf->GetPackage()));
	ASSERT_TRUE(SavePackage(Wrapper->GetPackage()));
	FAssetDeletionOperation Deletion;
	EXPECT_FALSE(IAssetTools::Get().PrepareDeletion({.AssetPaths = {LeafPath}}, Deletion));
	EXPECT_TRUE(std::ranges::any_of(Deletion.GetBlockers(), [](const auto& Blocker) {
		return Blocker.Kind == EAssetDeletionBlocker::ExternalPersistentReference
			|| Blocker.Kind == EAssetDeletionBlocker::ExternalLoadedReference;
	}));
	auto* Material = NewObject<DMaterial>(nullptr, "FunctionRelocationCaller");
	TStrongObjectPtr<DMaterial> MaterialRoot(Material);
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Output = Wrapper->GetFunctionSignature().Outputs[0];
	const FGuid CallId{64, 1, 1, 1};
	FMaterialProgram Program;
	Program.Nodes = {{.Id = CallId, .Opcode = EMaterialProgramOpcode::FunctionCall}};
	Program.Outputs.Surface = {.SourceNodeId = CallId, .SourceOutputId = Output.Id};
	ASSERT_TRUE(Material->SetMaterialProgramAndFunctionCalls(Program,
		{{.NodeId = CallId, .Function = Wrapper, .Outputs = {{Output.Id, Output.Type}}}}));
	ASSERT_TRUE(Material->CompileEdits());
	const auto Accepted = Material->GetAcceptedCompiledProgram();
	const auto Revision = Material->GetMaterialCompileStatus().AuthoredRevision;
	const auto OriginalLeafPath = Leaf->GetObjectPath();
	const std::array Mappings{FAssetRelocationMapping{LeafPath, MovedPath}};
	FAssetRelocationSummary Summary;
	FAssetMutationJob Job;
	ASSERT_TRUE(PrepareAssetRelocationJob(Mappings, Summary, Job));
	ASSERT_TRUE(Job.ResumeForward());
	EXPECT_EQ(Wrapper->GetFunctionDependencies()[0].Get(), Leaf);
	EXPECT_GT(Material->GetMaterialCompileStatus().AuthoredRevision, Revision);
	EXPECT_EQ(Material->GetMaterialCompileStatus().State, EMaterialCompileState::NeedsCompile);
	EXPECT_EQ(Material->GetAcceptedCompiledProgram(), Accepted);
	ASSERT_TRUE(Material->CompileEdits());
	EXPECT_EQ(Material->GetAcceptedCompiledProgram()->Identity, Accepted->Identity);
	EXPECT_TRUE(std::ranges::any_of(Material->GetAcceptedExpressionSources(), [&](const auto& Source) {
		return Source.FunctionAssetPath == Leaf->GetObjectPath();
	}));
	EXPECT_FALSE(std::ranges::any_of(Material->GetAcceptedExpressionSources(), [&](const auto& Source) {
		return Source.FunctionAssetPath == OriginalLeafPath;
	}));
	ASSERT_TRUE(Wrapper->SetFunctionGraph(OriginalGraph));
	ASSERT_TRUE(SavePackage(Wrapper->GetPackage()));
	ASSERT_TRUE(Material->CompileEdits());
	const std::array Removed{LeafPath, MovedPath};
	FAssetDeletionOperation UnreferencedDeletion;
	const auto PreparedDeletion = IAssetTools::Get().PrepareDeletion(
		{.AssetPaths = {LeafPath, MovedPath}}, UnreferencedDeletion);
	ASSERT_TRUE(PreparedDeletion) << PreparedDeletion.Message;
	ASSERT_TRUE(Testing::RemoveAssetPackagesForTests(Removed));
	EXPECT_TRUE(Material->GetAcceptedCompiledProgram());
	MaterialRoot.Reset();
	MarkAsGarbage(Material);
	CollectGarbage();
	ASSERT_TRUE(UnloadPackage(WrapperPath));
	CollectGarbage();
}

TEST(FMaterialFunctionTests, CookFingerprintsNestedFunctionsWithoutProducingRuntimeFunctionPackages)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("CookFunctionAssets");
	std::filesystem::create_directories(Root / "Content");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/CookFunctionTests/", .Owner = EMountOwner::Test,
		.Root = Root / "Content", .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	ASSERT_TRUE(RefreshAssetRegistry());
	std::vector<FCookContributorHandle> Handles;
	std::string Error;
	ASSERT_TRUE(RegisterEngineCookContributors(Handles, Error)) << Error;
	struct FRetire { std::vector<FCookContributorHandle>& Handles; ~FRetire() { for (auto Handle : Handles) UnregisterCookContributor(Handle); } } Retire{Handles};
	FPackagePath MaterialPath, WrapperPath, LeafPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookFunctionTests/Material", MaterialPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/CookFunctionTests/Wrapper", WrapperPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/CookFunctionTests/Leaf", LeafPath));
	DMaterial* Material = nullptr;
	DMaterialFunction* Wrapper = nullptr;
	DMaterialFunction* Leaf = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(MaterialPath, Material));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(WrapperPath, Wrapper));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(LeafPath, Leaf));
	ASSERT_NO_FATAL_FAILURE(AddFunctionCall(*Wrapper, *Leaf));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Output = Wrapper->GetFunctionSignature().Outputs[0];
	const FGuid CallId{63, 1, 1, 1};
	FMaterialProgram Program;
	Program.Nodes = {{.Id = CallId, .Opcode = EMaterialProgramOpcode::FunctionCall}};
	Program.Outputs.Surface = {.SourceNodeId = CallId, .SourceOutputId = Output.Id};
	ASSERT_TRUE(Material->SetMaterialProgramAndFunctionCalls(Program,
		{{.NodeId = CallId, .Function = Wrapper, .Outputs = {{Output.Id, Output.Type}}}}));
	ASSERT_TRUE(Material->CompileEdits());
	ASSERT_TRUE(SavePackage(Leaf->GetPackage()));
	ASSERT_TRUE(SavePackage(Wrapper->GetPackage()));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	FCookRequest Request{.OutputRoot = Root / "Cooked", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {MaterialPath}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Result.Code << ": " << Result.Diagnostic;
	ASSERT_EQ(Result.Packages.size(), 1u);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Result.Diagnostic;
	ASSERT_EQ(Result.Packages.size(), 1u);
	EXPECT_EQ(Result.Packages[0].Status, ECookPackageStatus::CookHit);
	auto Edited = Leaf->GetFunctionGraph();
	Edited.Signature.Inputs[0].Default.Surface.RoughnessDefault.X = 0.27f;
	ASSERT_TRUE(Leaf->SetFunctionGraph(Edited));
	ASSERT_TRUE(SavePackage(Leaf->GetPackage()));
	ASSERT_TRUE(Material->CompileEdits());
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Result.Diagnostic;
	ASSERT_EQ(Result.Packages.size(), 1u);
	EXPECT_NE(Result.Packages[0].Status, ECookPackageStatus::CookHit);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Result.Diagnostic;
	EXPECT_EQ(Result.Packages[0].Status, ECookPackageStatus::CookHit);
	const auto ExpectedIdentity = Material->GetAcceptedCompiledProgram()->Identity;
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ASSERT_TRUE(UnloadPackage(WrapperPath));
	ASSERT_TRUE(UnloadPackage(LeafPath));
	CollectGarbage();
	const auto LeafData = FindAssetExact(LeafPath);
	ASSERT_TRUE(LeafData);
	const std::filesystem::path LeafFile = LeafData->PhysicalPath;
	const auto HiddenLeafFile = LeafFile.string() + ".unavailable";
	std::filesystem::rename(LeafFile, HiddenLeafFile);
	const auto MissingDependency = FCookCoordinator().Run(Request, Result);
	std::filesystem::rename(HiddenLeafFile, LeafFile);
	EXPECT_FALSE(MissingDependency) << "A warm Cook hit must still admit every function source.";
	ShutdownAssetManager();
	auto Configuration = FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(FAssetRuntimeConfiguration::Cooked(Request.OutputRoot, Configuration));
	ASSERT_TRUE(InitializeAssetManager(std::move(Configuration)));
	{
		const std::array CookMounts{FMountPoint{.VirtualRoot = "/CookFunctionTests/", .Owner = EMountOwner::Test,
			.Root = Request.OutputRoot / "CookFunctionTests", .bAutoScan = true}};
		Testing::FScopedMountRegistryFixture CookRegistry(CookMounts);
		ASSERT_TRUE(CookRegistry.IsValid()) << CookRegistry.GetError();
		ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		DMaterial* Loaded = nullptr;
		ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath), Loaded));
		ASSERT_NE(Loaded, nullptr);
		ASSERT_NE(Loaded->GetAcceptedCompiledProgram(), nullptr);
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->Identity, ExpectedIdentity);
		EXPECT_TRUE(Loaded->GetMaterialProgram()->Nodes.empty());
		EXPECT_TRUE(Loaded->GetMaterialFunctionCalls().empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->IR.Nodes.empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->GeneratedSource.empty());
	}
	ShutdownAssetManager();
	CollectGarbage();
	ASSERT_TRUE(InitializeAssetManager());
}

TEST(FMaterialFunctionTests, StructuralNormalParentRoundTripsDuplicatesAndCooksWithoutGraph)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("CookStructuralNormal");
	std::filesystem::create_directories(Root / "Content");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/CookNormal/", .Owner = EMountOwner::Test,
		.Root = Root / "Content", .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	ASSERT_TRUE(RefreshAssetRegistry());
	std::vector<FCookContributorHandle> Handles;
	std::string Error;
	ASSERT_TRUE(RegisterEngineCookContributors(Handles, Error)) << Error;
	struct FRetire { std::vector<FCookContributorHandle>& Handles; ~FRetire() { for (auto Handle : Handles) UnregisterCookContributor(Handle); } } Retire{Handles};
	FPackagePath MaterialPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/CookNormal/Parent", MaterialPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(MaterialPath, Material));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	std::array<FImportedSurfaceRole, 8> Roles;
	const FMaterialSurfaceOutputs Defaults;
	for (uint32 I = 0; I < Roles.size(); ++I)
		Roles[I].Value = GetMaterialSurfaceOutputDefault(Defaults, static_cast<EMaterialSurfaceOutput>(I));
	Roles[1].Sample = FImportedSurfaceSample{.ResourceIdentity = "normal", .Usage = ETextureUsage::Normal,
		.OutputIndex = 6, .bDecodeNormal = true};
	const auto Recipe = MakeImportedSurfaceRecipe(Roles);
	ASSERT_EQ(Recipe.Program.Nodes.size(), 1u);
	ASSERT_TRUE(Material->SetMaterialProgram(Recipe.Program));
	ASSERT_TRUE(Material->CompileEdits());
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	auto* Duplicate = Cast<DMaterial>(DuplicateObject(Material, nullptr, "CopiedNormalParent"));
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(*Duplicate->GetMaterialProgram(), Recipe.Program);
	MarkAsGarbage(Duplicate);
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	CollectGarbage();
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath), Material));
	EXPECT_EQ(*Material->GetMaterialProgram(), Recipe.Program);
	ASSERT_TRUE(FinishMaterialCompileForTest(*Material));
	const auto ExpectedIdentity = Material->GetAcceptedCompiledProgram()->Identity;
	FCookRequest Request{.OutputRoot = Root / "Cooked", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {MaterialPath}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Result.Code << ": " << Result.Diagnostic;
	ASSERT_EQ(Result.Packages.size(), 1u);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Result.Diagnostic;
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ShutdownAssetManager();
	CollectGarbage();
	auto Configuration = FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(FAssetRuntimeConfiguration::Cooked(Request.OutputRoot, Configuration));
	ASSERT_TRUE(InitializeAssetManager(std::move(Configuration)));
	{
		const std::array CookMounts{FMountPoint{.VirtualRoot = "/CookNormal/", .Owner = EMountOwner::Test,
			.Root = Request.OutputRoot / "CookNormal", .bAutoScan = true}};
		Testing::FScopedMountRegistryFixture CookRegistry(CookMounts);
		ASSERT_TRUE(CookRegistry.IsValid()) << CookRegistry.GetError();
		ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		DMaterial* Loaded = nullptr;
		ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath), Loaded));
		ASSERT_NE(Loaded->GetAcceptedCompiledProgram(), nullptr);
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->Identity, ExpectedIdentity);
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->Layout.ResourceFieldCount, 1u);
		EXPECT_TRUE(Loaded->GetMaterialProgram()->Nodes.empty());
		EXPECT_TRUE(Loaded->GetMaterialFunctionCalls().empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->IR.Nodes.empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->GeneratedSource.empty());
	}
	ShutdownAssetManager();
	CollectGarbage();
	ASSERT_TRUE(InitializeAssetManager());
}

TEST(FMaterialFunctionTests, ExpandedBoundsApplyBeforePruningWithoutRaisingAuthoredBounds)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Function = NewObject<DMaterialFunction>(nullptr, "BoundedExpansionFunction");
	FMaterialFunctionGraph Graph;
	Graph.Signature.Outputs = {FunctionPort(1, EMaterialProgramValueType::Float, "Result")};
	for (uint32 Index = 0; Index < 199; ++Index)
		Graph.Nodes.push_back({.Id = {21, 1, 1, Index + 1}, .Literal = {static_cast<float>(Index)}});
	Graph.Nodes.push_back({.Id = {21, 1, 1, 200}, .Opcode = EMaterialProgramOpcode::FunctionOutput,
		.Inputs = {{Graph.Nodes[0].Id}}, .FunctionPortId = Graph.Signature.Outputs[0].Id});
	ASSERT_TRUE(Function->SetFunctionGraph(Graph));
	FMaterialCompilerInput Input;
	Input.Environment.CompilerIdentity = "FunctionExpansionBounds";
	const std::array<DMaterialFunctionInterface*, 1> Roots{Function};
	ASSERT_TRUE(SnapshotMaterialFunctionClosure(Roots, Input.Functions));
	for (uint32 Index = 0; Index < 20; ++Index)
	{
		const FGuid Id{22, 1, 1, Index + 1};
		Input.Program.Nodes.push_back({.Id = Id, .Opcode = EMaterialProgramOpcode::FunctionCall});
		Input.FunctionCalls.push_back({.NodeId = Id, .FunctionPath = Function->GetObjectPath(),
			.Outputs = {{Graph.Signature.Outputs[0].Id, EMaterialProgramValueType::Float}}});
	}
	Input.Program.Outputs.Metallic = {.SourceNodeId = Input.Program.Nodes[0].Id,
		.SourceOutputId = Graph.Signature.Outputs[0].Id};
	const auto Valid = NormalizeMaterialProgram(Input);
	ASSERT_TRUE(Valid) << (Valid.Diagnostics.empty() ? "" : Valid.Diagnostics[0].Message);
	EXPECT_EQ(Valid.IR.Nodes.size(), 1u);
	Input.Program.Nodes.push_back({.Id = {22, 1, 1, 21}, .Opcode = EMaterialProgramOpcode::FunctionCall});
	Input.FunctionCalls.push_back({.NodeId = Input.Program.Nodes.back().Id, .FunctionPath = Function->GetObjectPath(),
		.Outputs = {{Graph.Signature.Outputs[0].Id, EMaterialProgramValueType::Float}}});
	const auto Excessive = NormalizeMaterialProgram(Input);
	EXPECT_FALSE(Excessive);
	ASSERT_FALSE(Excessive.Diagnostics.empty());
	EXPECT_EQ(Excessive.Diagnostics[0].Category, EMaterialProgramDiagnosticCategory::Bounds);
	Input.FunctionCalls.clear();
	Input.Program.Nodes.clear();
	Input.Program.Outputs = {};
	for (uint32 Index = 0; Index <= MaterialProgramMaxNodeCount; ++Index)
		Input.Program.Nodes.push_back({.Id = {23, 1, 1, Index + 1}});
	EXPECT_FALSE(NormalizeMaterialProgram(Input));
	MarkAsGarbage(Function);
	CollectGarbage();
}

TEST(FMaterialFunctionTests, StandardMaterialFixtureCooksAndLoadsWithoutAuthoredFunctionAssets)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("CookStandardMaterialFixture");
	const auto Source = std::filesystem::path(FPaths::EngineContentDir()) / "Materials";
	std::filesystem::create_directories(Root / "Content/Materials/Functions");
	for (const std::string_view File : {"Functions/UVTransform.dasset",
		"Functions/SampleNormal.dasset", "Functions/SampleORM.dasset", "Functions/StandardPBR.dasset",
		"Functions/StandardPBR_ORM.dasset", "Functions/ImportedSurfaceValues.dasset", "Functions/DecodeImportedNormalRG.dasset"})
		std::filesystem::copy_file(Source / File, Root / "Content/Materials" / File);
	const std::array Mounts{FMountPoint{.VirtualRoot = "/Engine/", .Owner = EMountOwner::Test,
		.Root = Root / "Content", .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
	ASSERT_TRUE(RefreshAssetRegistry());
	std::vector<FCookContributorHandle> Handles;
	std::string Error;
	ASSERT_TRUE(RegisterEngineCookContributors(Handles, Error)) << Error;
	struct FRetire { std::vector<FCookContributorHandle>& Handles; ~FRetire() { for (auto Handle : Handles) UnregisterCookContributor(Handle); } } Retire{Handles};
	// The standalone host registers this unversioned fallback for generic assets.
	// It must not make transitive function dependencies permanently uncacheable.
	const auto Generic = RegisterCookContributor(DObject::StaticClass(), {"generic-package", 1, 1,
		[](DObject& Object, std::string_view Path, FCookContext& Context) -> FAssetResult {
			std::string Error;
			if (!Context.AddPackage(std::string(Path), Object.GetPackage(), &Error))
				return {EAssetError::InvalidPackageType, Error};
			return {};
		}});
	ASSERT_NE(Generic, 0u);
	Handles.push_back(Generic);

	FPackagePath MaterialPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/Engine/Materials/FunctionFixture", MaterialPath));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(MaterialPath, Material));
	ASSERT_NE(Material, nullptr);
	AssetForge::Builtins::FStandardMaterialFunctions Functions;
	ASSERT_TRUE(AssetForge::Builtins::EnsureStandardMaterialFunctions(Functions, Error)) << Error;
	std::vector<FMaterialFunctionCall> Calls;
	FMaterialGraphPresentation Presentation;
	auto Program = Testing::MakeStandardMaterialProgramForTest(Functions, Calls, Presentation);
	ASSERT_TRUE(Material->SetMaterialProgramAndFunctionCalls(std::move(Program), std::move(Calls)));
	ASSERT_TRUE(Material->SetMaterialGraphPresentation(std::move(Presentation)));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_EQ(Material->GetMaterialFunctionCalls().size(), 1u);
	ASSERT_TRUE(FinishMaterialCompileForTest(*Material));
	const auto ExpectedIdentity = Material->GetAcceptedCompiledProgram()->Identity;
	FCookRequest Request{.OutputRoot = Root / "Cooked", .TargetPlatform = ECookTargetPlatform::Win64,
		.TargetProfile = ECookTargetProfile::Game, .ExplicitRoots = {MaterialPath}};
	FCookRunResult Result;
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Result.Code << ": " << Result.Diagnostic;
	ASSERT_EQ(Result.Packages.size(), 1u);
	ASSERT_TRUE(FCookCoordinator().Run(Request, Result)) << Result.Diagnostic;
	ASSERT_EQ(Result.Packages.size(), 1u);
	EXPECT_EQ(Result.Packages.front().Status, ECookPackageStatus::CookHit);
	EXPECT_FALSE(std::filesystem::exists(Request.OutputRoot / "Engine/Materials/Functions"));
	FPackagePath FunctionPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/Engine/Materials/Functions/StandardPBR", FunctionPath));
	auto FunctionRootRequest = Request;
	FunctionRootRequest.OutputRoot = Root / "RejectedFunctionRoot";
	FunctionRootRequest.ExplicitRoots = {FunctionPath};
	EXPECT_FALSE(FCookCoordinator().Run(FunctionRootRequest, Result));
	EXPECT_NE(Result.Diagnostic.find("authoring-only"), std::string::npos);
	EXPECT_FALSE(std::filesystem::exists(FunctionRootRequest.OutputRoot / "CookManifest.bin"));

	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ShutdownAssetManager();
	CollectGarbage();
	auto Configuration = FAssetRuntimeConfiguration::Authored();
	ASSERT_TRUE(FAssetRuntimeConfiguration::Cooked(Request.OutputRoot, Configuration));
	ASSERT_TRUE(InitializeAssetManager(std::move(Configuration)));
	{
		const std::array CookMounts{FMountPoint{.VirtualRoot = "/Engine/", .Owner = EMountOwner::Test,
			.Root = Request.OutputRoot / "Engine", .bAutoScan = true}};
		Testing::FScopedMountRegistryFixture CookRegistry(CookMounts);
		ASSERT_TRUE(CookRegistry.IsValid()) << CookRegistry.GetError();
		ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
		DMaterial* Loaded = nullptr;
		ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath), Loaded));
		ASSERT_NE(Loaded, nullptr);
		ASSERT_NE(Loaded->GetAcceptedCompiledProgram(), nullptr);
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->Identity, ExpectedIdentity);
		EXPECT_EQ(Loaded->GetAcceptedCompiledProgram()->Layout.ResourceFieldCount, 8u);
		EXPECT_TRUE(Loaded->GetMaterialProgram()->Nodes.empty());
		EXPECT_TRUE(Loaded->GetMaterialFunctionCalls().empty());
		EXPECT_TRUE(Loaded->GetAcceptedCompiledProgram()->GeneratedSource.empty());
	}
	ShutdownAssetManager();
	CollectGarbage();
	ASSERT_TRUE(InitializeAssetManager());
}
