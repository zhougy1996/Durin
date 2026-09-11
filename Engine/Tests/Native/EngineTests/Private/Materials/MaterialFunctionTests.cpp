#include "MaterialTestSupport.h"
#include "Materials/MaterialFunction.h"
#include "Asset/Testing.h"
#include "Asset/References.h"
#include "AssetTools/IAssetTools.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeAssetTestSupport.h"
#include "NativeTestSupport.h"

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
		.ResultType = EMaterialProgramValueType::Texture2D, .ParameterId = ParameterId});
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

TEST(FMaterialFunctionTests, LegacyRootUpgradeAndFunctionReferencesSurvivePackageLoad)
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
	// Produce a bounded schema 4 fixture through the current package codec.
	const_cast<FMaterialProgram*>(Material->GetMaterialProgram())->SchemaVersion = 4;
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	CollectGarbage();
	Material = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath), Material));
	EXPECT_EQ(Material->GetMaterialProgram()->SchemaVersion, CurrentMaterialProgramSchemaVersion);
	EXPECT_TRUE(Material->GetPackage()->IsCanonicalResaveRecommended());
	EXPECT_FALSE(Material->GetPackage()->IsDirty());
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
