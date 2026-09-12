#include "MaterialTestSupport.h"
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
