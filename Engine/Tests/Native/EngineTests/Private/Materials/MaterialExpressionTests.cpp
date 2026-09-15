#include "Graph/MaterialExpressionInputs.h"
#include "MaterialTestSupport.h"
#include "Materials/MaterialExpressions.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialFunction.h"
#include "Asset/OfflinePreparation.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeAssetTestSupport.h"

TEST(FMaterialExpressionTests, TypedSnapshotIsDetachedFromCopiedInputsAndLaterEdits)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "DirectSnapshot"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialExpressionVector3Parameter> Color(NewObject<DMaterialExpressionVector3Parameter>(nullptr, "Color"));
	Color->Id = {1, 2, 3, 1}; Color->Metadata = {.Id = {4, 5, 6, 7}, .Name = "Color"}; Color->DefaultValue = {.2, .4, .6};
	TStrongObjectPtr<DMaterialExpressionScalarConstant> Roughness(NewObject<DMaterialExpressionScalarConstant>(nullptr, "Roughness"));
	Roughness->Id = {1, 2, 3, 2}; Roughness->Value = .75f;
	const std::array<DMaterialExpression*, 2> Expressions{Color.Get(), Roughness.Get()};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.BaseColor = {Color->Id}; Outputs.Roughness = {Roughness->Id};
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
	FMaterialCompilerEnvironment Environment;
	std::string Error;
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Environment, Error)) << Error;
	FMaterialIRCompilerInput Initial;
	ASSERT_TRUE(SnapshotMaterialCompilerInput(*Material, Environment, Initial));
	const auto Before = NormalizeMaterialIR(Initial);
	ASSERT_TRUE(Before);
	// Mutating a copied compiler input cannot affect either the owner or the original snapshot.
	auto Detached = Initial;
	Detached.IR.Nodes.clear();
	EXPECT_FALSE(Detached.IR == Initial.IR);
	FMaterialIRCompilerInput Snapshot;
	ASSERT_TRUE(SnapshotMaterialCompilerInput(*Material, Environment, Snapshot));
	const auto Direct = NormalizeMaterialIR(Snapshot);
	ASSERT_TRUE(Direct);
	EXPECT_EQ(Direct.CanonicalBytes, Before.CanonicalBytes);
	EXPECT_EQ(Snapshot.IR, Initial.IR);
	EXPECT_EQ(Direct.Identity, Before.Identity);
	EXPECT_EQ(Direct.Layout, Before.Layout);
	const auto Captured = Snapshot.IR;
	auto* Owned = Cast<DMaterialExpressionScalarConstant>(Material->GetExpressionCollection().Expressions[1].Get());
	Owned->Value = .125f;
	EXPECT_EQ(Snapshot.IR, Captured);
	FMaterialIRCompilerInput Changed;
	ASSERT_TRUE(SnapshotMaterialCompilerInput(*Material, Environment, Changed));
	EXPECT_NE(Changed.IR, Captured);
	const auto Retained = Changed.IR;
	Owned->Value = std::numeric_limits<float>::infinity();
	EXPECT_FALSE(SnapshotMaterialCompilerInput(*Material, Environment, Changed));
	EXPECT_EQ(Changed.IR, Retained);
}

TEST(FMaterialExpressionTests, SurfaceSnapshotRejectsDisconnectedInvalidNodesAndAuthoredLinkOverflow)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterialExpressionScalarConstant> Constant(NewObject<DMaterialExpressionScalarConstant>(nullptr, "DeadConstant"));
	Constant->Id = {1, 2, 3, 4}; Constant->Value = std::numeric_limits<float>::infinity();
	const std::array<DMaterialExpression*, 1> Expressions{Constant.Get()};
	FMaterialExpressionBuildContext Invalid(Expressions);
	const auto Rejected = Invalid.FinishSurface({});
	EXPECT_FALSE(Rejected); EXPECT_TRUE(Rejected.IR.Nodes.empty()); EXPECT_TRUE(Rejected.Parameters.empty());
	Constant->Value = .5f;
	FMaterialExpressionSurfaceOutputs Wrong;
	Wrong.BaseColor = {Constant->Id};
	FMaterialExpressionBuildContext WrongType(Expressions);
	EXPECT_FALSE(WrongType.FinishSurface(Wrong));
	Wrong.BaseColor = {}; Wrong.RoughnessDefault = std::numeric_limits<float>::quiet_NaN();
	FMaterialExpressionBuildContext BadDefault(Expressions);
	EXPECT_FALSE(BadDefault.FinishSurface(Wrong));
	std::vector<TStrongObjectPtr<DMaterialExpressionMakeSurface>> Owners;
	std::vector<DMaterialExpression*> Dense;
	for (uint32 Index = 0; Index < MaterialProgramMaxLinkCount / 8 + 1; ++Index)
	{
		Owners.emplace_back(NewObject<DMaterialExpressionMakeSurface>(nullptr, FName(std::format("Dense{}", Index))));
		Owners.back()->Id = {2, 3, 4, Index + 1}; Dense.push_back(Owners.back().Get());
	}
	FMaterialExpressionBuildContext TooManyLinks(Dense);
	EXPECT_FALSE(TooManyLinks.FinishSurface({}));
}

TEST(FMaterialExpressionTests, MaterialPersistsTypedOutputsAndOwnedParameterDefaults)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("OwnedMaterialExpressions");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/OwnedMaterials/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()); ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/OwnedMaterials/Material", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	TStrongObjectPtr<DMaterialExpressionVector3Parameter> Parameter(NewObject<DMaterialExpressionVector3Parameter>(nullptr, "Color"));
	Parameter->Id = {1, 2, 3, 4}; Parameter->Metadata.Id = {5, 6, 7, 8}; Parameter->Metadata.Name = "Color";
	Parameter->DefaultValue = {.2, .3, .4};
	const auto ParameterId = Parameter->Metadata.Id;
	const std::array<DMaterialExpression*, 1> Expressions{Parameter.Get()};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.BaseColor = {Parameter->Id}; Outputs.RoughnessDefault = .375f;
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
	EXPECT_EQ(DMaterial::StaticClass()->FindPropertyByName("Program"), nullptr);
	EXPECT_EQ(DMaterial::StaticClass()->FindPropertyByName("FunctionCalls"), nullptr);
	ASSERT_EQ(Material->GetExpressionCollection().Expressions.size(), 1u);
	EXPECT_NE(Material->GetExpressionCollection().Expressions[0].Get(), Parameter.Get());
	Parameter->DefaultValue = {.9, .9, .9};
	const auto Applied = Material->GetExpressionCollection().Expressions;
	const auto AuthoredRevision = Material->GetMaterialProgramRevision();
	auto BrokenOutputs = Outputs; BrokenOutputs.Normal = {{99, 1, 1, 1}};
	EXPECT_FALSE(Material->SetMaterialExpressions(Expressions, BrokenOutputs));
	EXPECT_EQ(Material->GetExpressionCollection().Expressions, Applied);
	EXPECT_EQ(Material->GetMaterialProgramRevision(), AuthoredRevision);
	ASSERT_TRUE(Material->SetParameterValue(ParameterId, FMaterialParameterValue::MakeVector({.6, .7, .8})));
	const auto* Owned = Cast<DMaterialExpressionVector3Parameter>(Applied[0].Get());
	ASSERT_NE(Owned, nullptr); EXPECT_EQ(Owned->DefaultValue, FVector3(.6, .7, .8));
	const auto Saved = SavePackage(Material->GetPackage());
	ASSERT_TRUE(Saved) << Saved.Message;
	ASSERT_TRUE(UnloadPackage(Path)); CollectGarbage(); Material = nullptr;
	const auto Loaded = LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Material);
	ASSERT_TRUE(Loaded) << Loaded.Message;
	EXPECT_EQ(Material->GetExpressionOutputs(), Outputs);
	ASSERT_EQ(GDObjectArray.GetObjectsWithOuter(Material, EObjectQueryScope::LiveOnly).size(), 1u);
	Owned = Cast<DMaterialExpressionVector3Parameter>(Material->GetExpressionCollection().Expressions[0].Get());
	ASSERT_NE(Owned, nullptr); EXPECT_EQ(Owned->DefaultValue, FVector3(.6, .7, .8));
	ASSERT_NE(Material->FindParameterDefinition(ParameterId), nullptr);
	EXPECT_EQ(Material->FindParameterDefinition(ParameterId)->Value.GetVector(), Owned->DefaultValue);
	TStrongObjectPtr<DMaterial> Duplicate(Cast<DMaterial>(DuplicateObject(Material, nullptr, "IndependentMaterial")));
	ASSERT_TRUE(Duplicate);
	EXPECT_EQ(Duplicate->GetExpressionOutputs(), Outputs);
	ASSERT_EQ(Duplicate->GetExpressionCollection().Expressions.size(), 1u);
	EXPECT_NE(Duplicate->GetExpressionCollection().Expressions[0].Get(), Owned);
	EXPECT_EQ(Duplicate->GetExpressionCollection().Expressions[0]->GetOuter(), Duplicate.Get());
	std::string Error;
	ASSERT_TRUE(Material->ValidateLoadedObjectGraph({}, Error)) << Error;
	TStrongObjectPtr<DMaterialExpressionScalarConstant> Orphan(NewObject<DMaterialExpressionScalarConstant>(Material, "Orphan"));
	EXPECT_FALSE(Material->ValidateLoadedObjectGraph({}, Error));
	Orphan->SetOuterPrivate(nullptr);
	ASSERT_TRUE(UnloadPackage(Path)); CollectGarbage();
}

TEST(FMaterialExpressionTests, MaterialCanReplaceItsOwnExpressionsAndClearOwnedChildren)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "ReplacingOwnedExpressions"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialExpressionScalarConstant> Source(NewObject<DMaterialExpressionScalarConstant>(nullptr, "Roughness"));
	Source->Id = {1, 2, 3, 4}; Source->Value = .75f;
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.Roughness = {Source->Id};
	const std::array<DMaterialExpression*, 1> Initial{Source.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(Initial, Outputs));
	TStrongObjectPtr<DMaterialExpression> Previous(Material->GetExpressionCollection().Expressions[0].Get());
	const std::array<DMaterialExpression*, 1> Existing{Previous.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(Existing, Outputs));
	TStrongObjectPtr<DMaterialExpression> Current(Material->GetExpressionCollection().Expressions[0].Get());
	EXPECT_NE(Current.Get(), Previous.Get());
	EXPECT_EQ(Current->Id, Previous->Id);
	EXPECT_EQ(Current->GetOuter(), Material.Get());
	EXPECT_NE(Previous->GetOuter(), Material.Get());
	EXPECT_EQ(Cast<DMaterialExpressionScalarConstant>(Current.Get())->Value, .75f);
	std::string Error;
	ASSERT_TRUE(Material->ValidateLoadedObjectGraph({}, Error)) << Error;
	ASSERT_TRUE(Material->SetMaterialExpressions({}, {}));
	EXPECT_TRUE(Material->GetExpressionCollection().Expressions.empty());
	EXPECT_NE(Current->GetOuter(), Material.Get());
	EXPECT_TRUE(GDObjectArray.GetObjectsWithOuter(Material.Get(), EObjectQueryScope::LiveOnly).empty());
	ASSERT_TRUE(Material->ValidateLoadedObjectGraph({}, Error)) << Error;
}

TEST(FMaterialExpressionTests, FunctionPersistsOnlyOwnedExpressionsAndAppliesIndependentCopies)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("OwnedFunctionExpressions");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/OwnedFunctions/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid()); ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/OwnedFunctions/Function", Path));
	DMaterialFunction* Function = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Function));
	const auto Previous = Function->GetExpressionCollection().Expressions;
	ASSERT_EQ(Previous.size(), 2u);
	TStrongObjectPtr<DObject> Working(NewObject<DObject>(nullptr, "WorkingFunctionGraph"));
	auto* Constant = NewObject<DMaterialExpressionVector2Constant>(Working.Get(), "Constant");
	auto* Output = NewObject<DMaterialExpressionFunctionOutput>(Working.Get(), "Output");
	Constant->Id = {1, 2, 3, 1}; Constant->Value = {.2, .4};
	Output->Id = {1, 2, 3, 2}; Output->PortId = {4, 5, 6, 7}; Output->Source = {Constant->Id};
	const auto OutputPortId = Output->PortId;
	FMaterialFunctionSignature Signature;
	Signature.Outputs = {{.Id = Output->PortId, .Type = EMaterialProgramValueType::Float2, .Name = "Value"}};
	const std::array<DMaterialExpression*, 2> Expressions{Constant, Output};
	ASSERT_TRUE(Function->SetFunctionExpressions(Signature, Expressions));
	EXPECT_EQ(DMaterialFunction::StaticClass()->FindPropertyByName("Graph"), nullptr);
	for (const auto& PreviousExpression : Previous) EXPECT_NE(PreviousExpression->GetOuter(), Function);
	ASSERT_EQ(GDObjectArray.GetObjectsWithOuter(Function, EObjectQueryScope::LiveOnly).size(), 2u);
	Constant->Value.x = .9;
	const auto* Owned = Cast<DMaterialExpressionVector2Constant>(Function->GetExpressionCollection().Expressions[0].Get());
	ASSERT_NE(Owned, nullptr); EXPECT_EQ(Owned->Value.x, .2);
	ASSERT_TRUE(Function->SetFunctionPresentation({.Nodes = {{Constant->Id, 30, 40, "Retained value"}}}));
	ASSERT_TRUE(Function->SetFunctionPresentation({.Nodes = {{Constant->Id, 30, 40}}}));
	EXPECT_TRUE(Function->GetFunctionPresentation().Nodes[0].DisplayName.empty());
	ASSERT_TRUE(Function->SetFunctionPresentation({.Nodes = {{Constant->Id, 30, 40, "Retained value"}}}));
	EXPECT_EQ(Function->GetFunctionPresentation().Nodes[0].DisplayName, "Retained value");
	const auto PersistedValue = Cast<DMaterialExpressionVector2Constant>(Function->GetExpressionCollection().Expressions[0].Get())->Value;
	const auto Saved = SavePackage(Function->GetPackage());
	ASSERT_TRUE(Saved) << Saved.Message;
	ASSERT_TRUE(UnloadPackage(Path)); CollectGarbage(); Function = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Function));
	ASSERT_EQ(Function->GetExpressionCollection().Expressions.size(), 2u);
	EXPECT_EQ(Function->GetFunctionPresentation().Nodes[0].DisplayName, "Retained value");
	ASSERT_EQ(GDObjectArray.GetObjectsWithOuter(Function, EObjectQueryScope::LiveOnly).size(), 2u);
	Owned = Cast<DMaterialExpressionVector2Constant>(Function->GetExpressionCollection().Expressions[0].Get());
	ASSERT_NE(Owned, nullptr); EXPECT_EQ(Owned->Value, PersistedValue);
	TStrongObjectPtr<DMaterialFunction> Duplicate(Cast<DMaterialFunction>(DuplicateObject(Function, nullptr, "IndependentFunction")));
	ASSERT_TRUE(Duplicate);
	EXPECT_NE(Duplicate->GetExpressionCollection().Expressions[0].Get(), Function->GetExpressionCollection().Expressions[0].Get());
	EXPECT_EQ(Duplicate->GetFunctionSignature(), Function->GetFunctionSignature());
	EXPECT_EQ(Duplicate->GetFunctionPresentation().Nodes, Function->GetFunctionPresentation().Nodes);
	const auto& OriginalExpressions = Function->GetExpressionCollection().Expressions;
	const auto& DuplicateExpressions = Duplicate->GetExpressionCollection().Expressions;
	ASSERT_EQ(OriginalExpressions.size(), DuplicateExpressions.size());
	for (size_t Index = 0; Index < OriginalExpressions.size(); ++Index)
	{
		ASSERT_EQ(OriginalExpressions[Index]->GetClass(), DuplicateExpressions[Index]->GetClass());
		OriginalExpressions[Index]->GetClass()->ForEachProperty([&](FProperty* Property) {
			EXPECT_TRUE(ArePropertyValuesIdentical(Property, OriginalExpressions[Index].Get(), 0, DuplicateExpressions[Index].Get(), 0));
		});
	}
	TStrongObjectPtr<DMaterialExpressionFunctionCall> Call(NewObject<DMaterialExpressionFunctionCall>(nullptr, "OwnedCall"));
	Call->Id = {8, 9, 10, 11}; Call->Function = Function; Call->Outputs = {{OutputPortId, EMaterialProgramValueType::Float2}};
	const std::array<DMaterialExpression*, 1> Calls{Call.Get()};
	const std::array Roots{FMaterialExpressionInput{Call->Id, 0, OutputPortId}};
	const auto Built = BuildMaterialExpressionGraph(Calls, Roots, {.FindFunction = [](const DMaterialFunctionInterface& Owner)
		-> std::optional<FMaterialExpressionFunctionBody> {
		if (const auto* Concrete = Cast<DMaterialFunction>(&Owner)) return Concrete->GetExpressionBody();
		return std::nullopt;
	}});
	ASSERT_TRUE(Built);
	EXPECT_EQ(Built.IR.Nodes[Built.Roots[0]].GetLiteral(), (FMaterialProgramLiteral{.2f, .4f}));
	Call->Function = nullptr;
	ASSERT_TRUE(UnloadPackage(Path)); CollectGarbage();
}

TEST(FMaterialExpressionTests, FunctionRejectsInvalidCandidatesAndUncollectedChildren)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, "ValidatedFunction"));
	const auto Before = Function->GetFunctionSignature();
	const auto BeforeChildren = Function->GetExpressionCollection().Expressions;
	const auto Revision = Function->GetFunctionRevision();
	TStrongObjectPtr<DMaterialExpressionFunctionOutput> Output(NewObject<DMaterialExpressionFunctionOutput>(nullptr, "InvalidOutput"));
	Output->Id = {1, 2, 3, 4}; Output->PortId = Function->GetFunctionSignature().Outputs[0].Id;
	Output->Source = {Output->Id};
	const std::array<DMaterialExpression*, 1> Invalid{Output.Get()};
	EXPECT_FALSE(Function->SetFunctionExpressions(Function->GetFunctionSignature(), Invalid));
	EXPECT_EQ(Function->GetFunctionSignature(), Before);
	EXPECT_EQ(Function->GetExpressionCollection().Expressions, BeforeChildren);
	EXPECT_EQ(Function->GetFunctionRevision(), Revision);
	std::string Error;
	ASSERT_TRUE(Function->ValidateLoadedObjectGraph({}, Error)) << Error;
	TStrongObjectPtr<DMaterialExpressionScalarConstant> Orphan(NewObject<DMaterialExpressionScalarConstant>(Function.Get(), "Uncollected"));
	Orphan->Id = {5, 6, 7, 8};
	EXPECT_FALSE(Function->ValidateLoadedObjectGraph({}, Error));
	Orphan->SetOuterPrivate(nullptr);
	ASSERT_TRUE(Function->ValidateLoadedObjectGraph({}, Error)) << Error;
	BeforeChildren[0]->SetOuterPrivate(Orphan.Get());
	EXPECT_FALSE(Function->ValidateLoadedObjectGraph({}, Error));
	BeforeChildren[0]->SetOuterPrivate(Function.Get());
	ASSERT_TRUE(Function->ValidateLoadedObjectGraph({}, Error)) << Error;
}

TEST(FMaterialExpressionTests, BuildFunctionInvocationsBindGuidPortsAndRetainIndependentDefaults)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, "DirectFunction"));
	auto* Input = NewObject<DMaterialExpressionFunctionInput>(Function.Get(), "Input");
	auto* Add = NewObject<DMaterialExpressionAdd>(Function.Get(), "Add");
	auto* OutputA = NewObject<DMaterialExpressionFunctionOutput>(Function.Get(), "OutputA");
	auto* OutputB = NewObject<DMaterialExpressionFunctionOutput>(Function.Get(), "OutputB");
	Input->Id = {1, 0, 0, 1}; Input->PortId = {2, 0, 0, 1};
	Add->Id = {1, 0, 0, 2}; Add->A = {Input->Id}; Add->BDefault = {.125f};
	OutputA->Id = {1, 0, 0, 3}; OutputA->PortId = {2, 0, 0, 2}; OutputA->Source = {Input->Id};
	OutputB->Id = {1, 0, 0, 4}; OutputB->PortId = {2, 0, 0, 3}; OutputB->Source = {Add->Id};
	FMaterialExpressionFunctionBody Body;
	Body.Signature.Inputs = {{.Id = Input->PortId, .Type = EMaterialProgramValueType::Float, .Name = "Value",
		.Default = {.Kind = EMaterialFunctionDefaultKind::Numeric, .Numeric = {.75f}}}};
	Body.Signature.Outputs = {{.Id = OutputA->PortId, .Type = EMaterialProgramValueType::Float, .Name = "A"},
		{.Id = OutputB->PortId, .Type = EMaterialProgramValueType::Float, .Name = "B"}};
	Body.Expressions = {Input, Add, OutputA, OutputB}; Body.AssetPath = "/Direct/Function"; Body.Revision = 7;
	TStrongObjectPtr<DMaterialExpressionScalarConstant> Constant(NewObject<DMaterialExpressionScalarConstant>(nullptr, "Constant"));
	TStrongObjectPtr<DMaterialExpressionFunctionCall> CallA(NewObject<DMaterialExpressionFunctionCall>(nullptr, "CallA"));
	TStrongObjectPtr<DMaterialExpressionFunctionCall> CallB(NewObject<DMaterialExpressionFunctionCall>(nullptr, "CallB"));
	Constant->Id = {3, 0, 0, 1}; Constant->Value = .5f;
	CallA->Id = {3, 0, 0, 2}; CallA->Function = Function.Get();
	CallA->Inputs = {{.InputId = Input->PortId, .Input = {Constant->Id}, .InputDefault = {.25f}}};
	CallA->Outputs = {{OutputA->PortId, EMaterialProgramValueType::Float}, {OutputB->PortId, EMaterialProgramValueType::Float}};
	CallB->Id = {3, 0, 0, 3}; CallB->Function = Function.Get(); CallB->Outputs = CallA->Outputs;
	const std::array<DMaterialExpression*, 3> Graph{Constant.Get(), CallA.Get(), CallB.Get()};
	const std::array Roots{FMaterialExpressionInput{CallA->Id, 0, OutputA->PortId},
		FMaterialExpressionInput{CallA->Id, 0, OutputB->PortId}, FMaterialExpressionInput{CallB->Id, 0, OutputA->PortId}};
	uint32 Captures = 0;
	FMaterialExpressionBuildEnvironment Environment{.FindFunction = [&](const DMaterialFunctionInterface& Owner)
		-> std::optional<FMaterialExpressionFunctionBody> { ++Captures; return &Owner == Function.Get() ? std::optional(Body) : std::nullopt; }};
	const auto Built = BuildMaterialExpressionGraph(Graph, Roots, Environment);
	ASSERT_TRUE(Built);
	EXPECT_EQ(Captures, 1u);
	ASSERT_EQ(Built.Roots.size(), 3u);
	EXPECT_FLOAT_EQ(Built.IR.Nodes[Built.Roots[0]].GetLiteral().X, .5f);
	EXPECT_EQ(Built.IR.Nodes[Built.Roots[1]].Opcode, EMaterialProgramOpcode::Add);
	EXPECT_FLOAT_EQ(Built.IR.Nodes[Built.Roots[2]].GetLiteral().X, .75f);
	ASSERT_EQ(Built.Dependencies.size(), 1u);
	EXPECT_EQ(Built.Dependencies[0].Revision, 7u);
	EXPECT_TRUE(std::ranges::any_of(Built.Sources, [&](const auto& Source) {
		return Source.FunctionAssetPath == Body.AssetPath && Source.CallPath == std::vector<FGuid>{CallB->Id}; }));
	CallA->Inputs[0].Input = {};
	const auto Disconnected = BuildMaterialExpressionGraph(Graph, Roots, Environment);
	ASSERT_TRUE(Disconnected);
	EXPECT_FLOAT_EQ(Disconnected.IR.Nodes[Disconnected.Roots[0]].GetLiteral().X, .25f);
	EXPECT_FLOAT_EQ(Built.IR.Nodes[Built.Roots[0]].GetLiteral().X, .5f);
	CallA->Inputs[0].InputDefault = {1, 2};
	const auto Invalid = BuildMaterialExpressionGraph(Graph, Roots, Environment);
	EXPECT_FALSE(Invalid); EXPECT_TRUE(Invalid.IR.Nodes.empty()); EXPECT_TRUE(Invalid.Dependencies.empty());
	CallA->Inputs[0].InputDefault = {.25f};
	Body.Signature.Inputs[0].bRequired = true; Body.Signature.Inputs[0].Default = {};
	EXPECT_FALSE(BuildMaterialExpressionGraph(Graph, Roots, Environment));
	Body.Signature.Inputs[0].bRequired = false; Body.Signature.Inputs[0].Default = {.Kind = EMaterialFunctionDefaultKind::Numeric};
	Body.Expressions.pop_back();
	EXPECT_FALSE(BuildMaterialExpressionGraph(Graph, Roots, Environment));
}

TEST(FMaterialExpressionTests, BuildNestedTextureDefaultsAreValuesAndRecursionIsRejected)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	TStrongObjectPtr<DMaterialFunction> Inner(NewObject<DMaterialFunction>(nullptr, "Inner"));
	TStrongObjectPtr<DMaterialFunction> Outer(NewObject<DMaterialFunction>(nullptr, "Outer"));
	auto* Input = NewObject<DMaterialExpressionFunctionInput>(Inner.Get(), "TextureInput");
	auto* Output = NewObject<DMaterialExpressionFunctionOutput>(Inner.Get(), "TextureOutput");
	Input->Id = {1, 0, 0, 1}; Input->PortId = {2, 0, 0, 1};
	Output->Id = {1, 0, 0, 2}; Output->PortId = {2, 0, 0, 2}; Output->Source = {Input->Id};
	FMaterialExpressionFunctionBody InnerBody;
	InnerBody.Expressions = {Input, Output}; InnerBody.AssetPath = "/Direct/Inner";
	InnerBody.Signature.Inputs = {{.Id = Input->PortId, .Type = EMaterialProgramValueType::Texture2D, .Name = "Texture",
		.Default = {.Kind = EMaterialFunctionDefaultKind::Texture, .TextureFallback = EMaterialTextureFallback::FlatRGNormal}}};
	InnerBody.Signature.Outputs = {{.Id = Output->PortId, .Type = EMaterialProgramValueType::Texture2D, .Name = "Texture"}};
	auto* Nested = NewObject<DMaterialExpressionFunctionCall>(Outer.Get(), "Nested");
	auto* OuterOutput = NewObject<DMaterialExpressionFunctionOutput>(Outer.Get(), "OuterOutput");
	Nested->Id = {3, 0, 0, 1}; Nested->Function = Inner.Get(); Nested->Outputs = {{Output->PortId, EMaterialProgramValueType::Texture2D}};
	OuterOutput->Id = {3, 0, 0, 2}; OuterOutput->PortId = {4, 0, 0, 1}; OuterOutput->Source = {Nested->Id, 0, Output->PortId};
	FMaterialExpressionFunctionBody OuterBody;
	OuterBody.Expressions = {Nested, OuterOutput}; OuterBody.AssetPath = "/Direct/Outer";
	OuterBody.Signature.Outputs = {{.Id = OuterOutput->PortId, .Type = EMaterialProgramValueType::Texture2D, .Name = "Texture"}};
	TStrongObjectPtr<DMaterialExpressionFunctionCall> Call(NewObject<DMaterialExpressionFunctionCall>(nullptr, "Call"));
	TStrongObjectPtr<DMaterialExpressionTextureSample2D> Sample(NewObject<DMaterialExpressionTextureSample2D>(nullptr, "Sample"));
	Call->Id = {5, 0, 0, 1}; Call->Function = Outer.Get(); Call->Outputs = {{OuterOutput->PortId, EMaterialProgramValueType::Texture2D}};
	Sample->Id = {5, 0, 0, 2}; Sample->Texture = {Call->Id, 0, OuterOutput->PortId};
	const std::array<DMaterialExpression*, 2> Graph{Call.Get(), Sample.Get()};
	const std::array Roots{FMaterialExpressionInput{Sample->Id}};
	FMaterialExpressionBuildEnvironment Environment{.FindFunction = [&](const DMaterialFunctionInterface& Owner)
		-> std::optional<FMaterialExpressionFunctionBody> {
		if (&Owner == Inner.Get()) return InnerBody;
		if (&Owner == Outer.Get()) return OuterBody;
		return std::nullopt;
	}};
	const auto Built = BuildMaterialExpressionGraph(Graph, Roots, Environment);
	ASSERT_TRUE(Built);
	EXPECT_EQ(Built.Dependencies.size(), 2u);
	EXPECT_TRUE(Built.Parameters.empty());
	EXPECT_EQ(std::ranges::count(Built.IR.Nodes, EMaterialProgramOpcode::TextureSample2D, &FMaterialIRNode::Opcode), 0);
	EXPECT_EQ(Built.IR.Nodes[Built.Roots[0]].GetLiteral(), (FMaterialProgramLiteral{.5f, .5f, 1, 1}));
	const std::array TextureRoots{Sample->Texture};
	EXPECT_FALSE(BuildMaterialExpressionGraph(Graph, TextureRoots, Environment));
	auto* Dead = NewObject<DMaterialExpressionScalarConstant>(Inner.Get(), "Dead");
	Dead->Id = {1, 0, 0, 3}; Dead->Value = std::numeric_limits<float>::quiet_NaN();
	InnerBody.Expressions.push_back(Dead);
	EXPECT_FALSE(BuildMaterialExpressionGraph(Graph, Roots, Environment));
	InnerBody.Expressions.pop_back();
	Sample->UV = {{9, 9, 9, 9}};
	EXPECT_FALSE(BuildMaterialExpressionGraph(Graph, Roots, Environment));
	Sample->UV = {};
	Nested->Function = Outer.Get(); Nested->Outputs = Call->Outputs; OuterOutput->Source.OutputId = OuterOutput->PortId;
	const auto Recursive = BuildMaterialExpressionGraph(Graph, Roots, Environment);
	EXPECT_FALSE(Recursive); EXPECT_TRUE(Recursive.IR.Nodes.empty());
	ASSERT_FALSE(Recursive.Diagnostics.empty());
	EXPECT_EQ(Recursive.Diagnostics[0].CallPath, std::vector<FGuid>{Call->Id});
}

TEST(FMaterialExpressionTests, BuildFunctionDefaultsPreserveAliasUVAndSurfaceSemantics)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, "DefaultKinds"));
	FMaterialExpressionFunctionBody Body;
	Body.AssetPath = "/Direct/DefaultKinds";
	const FGuid ScalarId{1, 0, 0, 1}, AliasId{1, 0, 0, 2}, UVId{1, 0, 0, 3}, SurfaceId{1, 0, 0, 4};
	Body.Signature.Inputs = {
		{.Id = ScalarId, .Type = EMaterialProgramValueType::Float, .Name = "Scalar", .Default = {.Kind = EMaterialFunctionDefaultKind::Numeric, .Numeric = {.625f}}},
		{.Id = AliasId, .Type = EMaterialProgramValueType::Float, .Name = "Alias", .Default = {.Kind = EMaterialFunctionDefaultKind::Input, .InputId = ScalarId}},
		{.Id = UVId, .Type = EMaterialProgramValueType::Float2, .Name = "UV", .Default = {.Kind = EMaterialFunctionDefaultKind::UV0}},
		{.Id = SurfaceId, .Type = EMaterialProgramValueType::Surface, .Name = "Surface", .Default = {.Kind = EMaterialFunctionDefaultKind::Surface}}};
	GetMaterialSurfaceOutputDefault(Body.Signature.Inputs[3].Default.Surface, EMaterialSurfaceOutput::BaseColor) = {.2f, .3f, .4f};
	TStrongObjectPtr<DMaterialExpressionFunctionCall> Call(NewObject<DMaterialExpressionFunctionCall>(nullptr, "DefaultsCall"));
	Call->Id = {3, 0, 0, 1}; Call->Function = Function.Get();
	std::vector<FMaterialExpressionInput> Roots;
	for (uint32 Index = 1; Index < 4; ++Index)
	{
		auto* Input = NewObject<DMaterialExpressionFunctionInput>(Function.Get(), FName(std::format("Input{}", Index)));
		auto* Output = NewObject<DMaterialExpressionFunctionOutput>(Function.Get(), FName(std::format("Output{}", Index)));
		Input->Id = {2, 0, 0, Index}; Input->PortId = Body.Signature.Inputs[Index].Id;
		Output->Id = {2, 0, 1, Index}; Output->PortId = {1, 0, 1, Index}; Output->Source = {Input->Id};
		Body.Expressions.push_back(Input); Body.Expressions.push_back(Output);
		Body.Signature.Outputs.push_back({.Id = Output->PortId, .Type = Body.Signature.Inputs[Index].Type, .Name = std::format("Output{}", Index)});
		Call->Outputs.push_back({Output->PortId, Body.Signature.Inputs[Index].Type});
		Roots.push_back({Call->Id, 0, Output->PortId});
	}
	const std::array<DMaterialExpression*, 1> Graph{Call.Get()};
	FMaterialExpressionBuildEnvironment Environment{.FindFunction = [&](const DMaterialFunctionInterface&) { return std::optional(Body); }};
	const auto Built = BuildMaterialExpressionGraph(Graph, Roots, Environment);
	ASSERT_TRUE(Built);
	EXPECT_FLOAT_EQ(Built.IR.Nodes[Built.Roots[0]].GetLiteral().X, .625f);
	const auto& UV = Built.IR.Nodes[Built.Roots[1]];
	EXPECT_EQ(UV.Opcode, EMaterialProgramOpcode::UVChannel);
	EXPECT_FLOAT_EQ(Built.IR.Nodes[UV.Inputs[0]].GetLiteral().X, 0.f);
	const auto& Surface = Built.IR.Nodes[Built.Roots[2]];
	EXPECT_EQ(Surface.Opcode, EMaterialProgramOpcode::MakeSurface);
	EXPECT_EQ(Built.IR.Nodes[Surface.Inputs[0]].GetLiteral(), (FMaterialProgramLiteral{.2f, .3f, .4f}));
	Body.Signature.Inputs[0].Default = {.Kind = EMaterialFunctionDefaultKind::Input, .InputId = AliasId};
	const auto Cyclic = BuildMaterialExpressionGraph(Graph, Roots, Environment);
	EXPECT_FALSE(Cyclic); EXPECT_TRUE(Cyclic.IR.Nodes.empty());
	ASSERT_FALSE(Cyclic.Diagnostics.empty());
	EXPECT_TRUE(Cyclic.Diagnostics[0].PortId.IsValid());
	EXPECT_EQ(Cyclic.Diagnostics[0].FunctionAssetPath, Body.AssetPath);
	EXPECT_EQ(Cyclic.Diagnostics[0].CallPath, std::vector<FGuid>{Call->Id});
}

TEST(FMaterialExpressionTests, BuildEmitsDetachedNumericIRAndRejectsInvalidGraphsAtomically)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterialExpressionScalarConstant> Constant(NewObject<DMaterialExpressionScalarConstant>(nullptr, "BuildConstant"));
	TStrongObjectPtr<DMaterialExpressionAdd> Add(NewObject<DMaterialExpressionAdd>(nullptr, "BuildAdd"));
	Constant->Id = {1, 2, 3, 1}; Constant->Value = .25f;
	Add->Id = {1, 2, 3, 2}; Add->A = {Constant->Id}; Add->ADefault = {.75f}; Add->BDefault = {.5f};
	const std::array<DMaterialExpression*, 2> Expressions{Constant.Get(), Add.Get()};
	const std::array Roots{FMaterialExpressionInput{Add->Id}, FMaterialExpressionInput{Add->Id}};
	const auto Built = BuildMaterialExpressionGraph(Expressions, Roots);
	ASSERT_TRUE(Built);
	ASSERT_EQ(Built.IR.Nodes.size(), 3u);
	EXPECT_EQ(Built.Roots[0], Built.Roots[1]);
	EXPECT_EQ(Built.IR.Nodes.back().Opcode, EMaterialProgramOpcode::Add);
	EXPECT_FLOAT_EQ(Built.IR.Nodes[0].GetLiteral().X, .25f);
	EXPECT_FLOAT_EQ(Built.IR.Nodes[1].GetLiteral().X, .5f);
	EXPECT_EQ(Built.Sources.back().NodeId, Add->Id);
	Constant->Value = .125f;
	EXPECT_FLOAT_EQ(Built.IR.Nodes[0].GetLiteral().X, .25f);
	Add->A = {};
	const auto Disconnected = BuildMaterialExpressionGraph(Expressions, Roots);
	ASSERT_TRUE(Disconnected);
	EXPECT_FLOAT_EQ(Disconnected.IR.Nodes[Disconnected.IR.Nodes[Disconnected.Roots[0]].Inputs[0]].GetLiteral().X, .75f);
	const auto Reject = [&] {
		const auto Failed = BuildMaterialExpressionGraph(Expressions, Roots);
		EXPECT_FALSE(Failed);
		EXPECT_TRUE(Failed.IR.Nodes.empty());
		EXPECT_TRUE(Failed.Roots.empty());
		EXPECT_TRUE(Failed.Parameters.empty());
		EXPECT_TRUE(Failed.Sources.empty());
	};
	Add->A = {Add->Id}; Reject();
	Add->A = {{9, 9, 9, 9}}; Reject();
	Add->A = {Constant->Id, 1}; Reject();
	Add->A = {Constant->Id}; Add->ADefault = {1, 2}; Reject();
	Add->ADefault = {std::numeric_limits<float>::infinity()}; Reject();
}

TEST(FMaterialExpressionTests, BuildSamplesAndSurfaceAttributesWithoutProgramNodes)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterialExpressionTextureSampleParameter2D> Sample(NewObject<DMaterialExpressionTextureSampleParameter2D>(nullptr, "BuildSample"));
	TStrongObjectPtr<DMaterialExpressionMakeSurface> Surface(NewObject<DMaterialExpressionMakeSurface>(nullptr, "BuildSurface"));
	TStrongObjectPtr<DMaterialExpressionGetSurfaceAttributes> Get(NewObject<DMaterialExpressionGetSurfaceAttributes>(nullptr, "BuildGet"));
	Sample->Id = {1, 2, 3, 1}; Sample->Metadata.Id = {4, 5, 6, 7}; Sample->Metadata.Name = "Sample";
	Surface->Id = {1, 2, 3, 2};
	Surface->BaseColor = {Sample->Id, 1}; Surface->Normal = {Sample->Id, 8};
	Surface->MetallicDefault = {0}; Surface->RoughnessDefault = {.5f};
	Surface->AmbientOcclusionDefault = {1}; Surface->EmissiveDefault = {0, 0, 0};
	Surface->OpacityDefault = {1}; Surface->OpacityMaskDefault = {1};
	Get->Id = {1, 2, 3, 3}; Get->Surface = {Surface->Id};
	const std::array<DMaterialExpression*, 3> Expressions{Sample.Get(), Surface.Get(), Get.Get()};
	const std::array Roots{FMaterialExpressionInput{Surface->Id}, FMaterialExpressionInput{Get->Id, 0}, FMaterialExpressionInput{Sample->Id, 7}};
	auto Built = BuildMaterialExpressionGraph(Expressions, Roots);
	ASSERT_TRUE(Built);
	EXPECT_EQ(std::ranges::count(Built.IR.Nodes, EMaterialProgramOpcode::TextureSample2D, &FMaterialIRNode::Opcode), 1);
	EXPECT_EQ(Built.IR.Nodes[Built.Roots[1]].ResultType, EMaterialProgramValueType::Float3);
	EXPECT_EQ(Built.IR.Nodes[Built.Roots[2]].ResultType, EMaterialProgramValueType::Texture2D);
	ASSERT_EQ(Built.Parameters.size(), 1u);
	EXPECT_EQ(Built.Parameters[0].Id, Sample->Metadata.Id);
	Built.IR.SurfaceRoot.bAggregate = true;
	Built.IR.SurfaceRoot.AggregateExpressionIndex = Built.Roots[0];
	const auto Layout = CompileMaterialLayout(Built.Parameters);
	ASSERT_TRUE(Layout);
	const auto Source = GenerateMaterialProgramSlang(Built.IR, Layout.Layout);
	ASSERT_TRUE(Source);
	EXPECT_NE(Source.Source.find("Sample("), std::string::npos);
	FMaterialIRCompilerInput CompilerInput{.IR = Built.IR, .Parameters = Built.Parameters, .Sources = Built.Sources};
	std::string Error;
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(CompilerInput.Environment, Error)) << Error;
	const auto Compiled = CompileMaterialIR(CompilerInput);
	ASSERT_TRUE(Compiled) << (Compiled.Diagnostics.empty() ? "Missing diagnostic" : Compiled.Diagnostics.front().Message);
	EXPECT_EQ(Compiled.CompiledShaders.size(), 3u);
	Get->AttributeMask = 2;
	EXPECT_FALSE(BuildMaterialExpressionGraph(Expressions, Roots));
}

TEST(FMaterialExpressionTests, TypedOwnersRoundTripAndDuplicateOnlyApplicableFields)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto Root = Testing::CreateTestFixtureDirectory("MaterialExpressions");
	const std::array Mounts{FMountPoint{.VirtualRoot = "/ExpressionTests/", .Owner = EMountOwner::Test,
		.Root = Root, .bAutoScan = true, .bContentWritable = true}};
	Testing::FScopedMountRegistryFixture Registry(Mounts);
	ASSERT_TRUE(Registry.IsValid());
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ExpressionTests/Owner", Path));
	DMaterial* Owner = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Owner));
	Owner->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DObject> Working(NewObject<DObject>(nullptr, "TypedWorkingGraph"));
	auto* Scalar = NewObject<DMaterialExpressionScalarParameter>(Working.Get(), "Scalar");
	Scalar->Id = {1, 2, 3, 1};
	Scalar->Metadata = {.Id = {4, 5, 6, 1}, .Name = "Scalar", .DisplayName = "Scalar default"};
	Scalar->DefaultValue = .375f;
	Scalar->bHasRange = true;
	Scalar->MaximumValue = 1.0f;
	auto* Vector = NewObject<DMaterialExpressionVector4Parameter>(Working.Get(), "Vector");
	Vector->Id = {1, 2, 3, 2};
	Vector->Metadata = {.Id = {4, 5, 6, 2}, .Name = "Vector"};
	Vector->DefaultValue = {.2, .4, .6, .8};
	auto* Texture = NewObject<DMaterialExpressionTextureParameter>(Working.Get(), "Texture");
	Texture->Id = {1, 2, 3, 3};
	Texture->Metadata = {.Id = {4, 5, 6, 3}, .Name = "Texture"};
	Texture->DefaultValue.SamplerState.AddressV = EMaterialSamplerAddressMode::ClampToEdge;
	Texture->DefaultValue.TextureFallback = EMaterialTextureFallback::Black;
	EXPECT_EQ(Scalar->GetClass()->FindPropertyByName("TextureUsage"), nullptr);
	EXPECT_EQ(Vector->GetClass()->FindPropertyByName("MinimumValue"), nullptr);
	EXPECT_EQ(Texture->GetClass()->FindPropertyByName("Inputs"), nullptr);
	EXPECT_EQ(Texture->GetClass()->FindPropertyByName("Parameter"), nullptr);
	const auto Before = Vector->GetParameterDefinition();
	EXPECT_EQ(Before.Type, EMaterialParameterType::Vector4);
	EXPECT_EQ(Before.Value.GetVector4(), Vector->DefaultValue);
	auto* Copy = Cast<DMaterialExpressionVector4Parameter>(DuplicateObject(Vector, Working.Get(), "Copy"));
	ASSERT_NE(Copy, nullptr);
	EXPECT_EQ(Copy->Metadata, Vector->Metadata);
	Copy->Id = {1, 2, 3, 5}; Copy->Metadata.Id = {4, 5, 6, 5}; Copy->Metadata.Name = "Copy";
	Copy->DefaultValue.x = .9;
	EXPECT_EQ(Vector->DefaultValue.x, .2);
	auto* Add = NewObject<DMaterialExpressionAdd>(Working.Get(), "Add");
	Add->Id = {1, 2, 3, 4};
	Add->A = {Scalar->Id};
	Add->ADefault = {.125f};
	Add->BDefault = {.75f};
	const auto SavedInput = Add->A;
	const std::array<DMaterialExpression*, 5> Expressions{Scalar, Vector, Texture, Copy, Add};
	ASSERT_TRUE(Owner->SetMaterialExpressions(Expressions, {}));
	ASSERT_TRUE(SavePackage(Owner->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage();
	Owner = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Owner));
	const auto Children = GDObjectArray.GetObjectsWithOuter(Owner, EObjectQueryScope::LiveOnly);
	ASSERT_EQ(Children.size(), 5u);
	bool bFoundVector = false, bFoundScalar = false, bFoundTexture = false, bFoundAdd = false;
	for (DObject* Child : Children)
	{
		if (auto* Value = Cast<DMaterialExpressionAdd>(Child))
		{
			EXPECT_EQ(Value->A, SavedInput);
			EXPECT_EQ(Value->ADefault, (std::vector<float>{.125f}));
			EXPECT_EQ(Value->BDefault, (std::vector<float>{.75f}));
			bFoundAdd = true;
		}
		if (auto* Value = Cast<DMaterialExpressionVector4Parameter>(Child); Value && Value->Metadata.Name == "Vector")
		{
			EXPECT_EQ(Value->GetParameterDefinition(), Before);
			bFoundVector = true;
		}
		if (auto* Value = Cast<DMaterialExpressionScalarParameter>(Child))
		{
			EXPECT_FLOAT_EQ(Value->DefaultValue, .375f);
			EXPECT_TRUE(Value->bHasRange);
			EXPECT_FLOAT_EQ(Value->MaximumValue, 1.0f);
			bFoundScalar = true;
		}
		if (auto* Value = Cast<DMaterialExpressionTextureParameter>(Child))
		{
			EXPECT_EQ(Value->DefaultValue.TextureFallback, EMaterialTextureFallback::Black);
			EXPECT_EQ(Value->DefaultValue.SamplerState.AddressV, EMaterialSamplerAddressMode::ClampToEdge);
			bFoundTexture = true;
		}
	}
	EXPECT_TRUE(bFoundVector && bFoundScalar && bFoundTexture && bFoundAdd);
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage();
}

TEST(FMaterialExpressionTests, NumericDefaultsRemainConnectedAndBuildRejectsWidthErrors)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterialExpressionVector3Constant> Source(NewObject<DMaterialExpressionVector3Constant>(nullptr, "Source"));
	TStrongObjectPtr<DMaterialExpressionLerp> Lerp(NewObject<DMaterialExpressionLerp>(nullptr, "Lerp"));
	Source->Id = {5, 6, 7, 8}; Source->Value = {1, 2, 3};
	Lerp->Id = {1, 2, 3, 4};
	Lerp->ResultType = EMaterialProgramValueType::Float3;
	Lerp->A = {Source->Id};
	Lerp->ADefault = {.1f, .2f, .3f};
	Lerp->BDefault = {.4f, .5f, .6f};
	Lerp->AlphaDefault = {.25f};
	const std::array<DMaterialExpression*, 2> Expressions{Source.Get(), Lerp.Get()};
	const std::array Roots{FMaterialExpressionInput{Lerp->Id}};
	ASSERT_TRUE(BuildMaterialExpressionGraph(Expressions, Roots));
	Lerp->A = {};
	const auto Disconnected = BuildMaterialExpressionGraph(Expressions, Roots);
	ASSERT_TRUE(Disconnected);
	const auto& LerpNode = Disconnected.IR.Nodes[Disconnected.Roots[0]];
	EXPECT_EQ(Disconnected.IR.Nodes[LerpNode.Inputs[0]].GetLiteral(), (FMaterialProgramLiteral{.1f, .2f, .3f}));
	Lerp->A = {Source->Id};
	Lerp->AlphaDefault = {.2f, .3f};
	EXPECT_FALSE(BuildMaterialExpressionGraph(Expressions, Roots));
	Lerp->AlphaDefault = {std::numeric_limits<float>::infinity()};
	EXPECT_FALSE(BuildMaterialExpressionGraph(Expressions, Roots));
	TStrongObjectPtr<DMaterialExpressionSwizzle> Swizzle(NewObject<DMaterialExpressionSwizzle>(nullptr, "Swizzle"));
	Swizzle->Id = {1, 2, 3, 5};
	Swizzle->InputDefault = {1, 2, 3, 4};
	Swizzle->Components = {2, 0};
	const std::array<DMaterialExpression*, 1> Swizzles{Swizzle.Get()};
	const std::array SwizzleRoots{FMaterialExpressionInput{Swizzle->Id}};
	const auto Built = BuildMaterialExpressionGraph(Swizzles, SwizzleRoots);
	ASSERT_TRUE(Built);
	EXPECT_EQ(Built.IR.Nodes[Built.Roots[0]].ResultType, EMaterialProgramValueType::Float2);
	EXPECT_EQ(Built.IR.Nodes[Built.Roots[0]].GetSwizzle().Components[0], 2);
	EXPECT_EQ(Built.IR.Nodes[Built.Roots[0]].GetSwizzle().Components[1], 0);
	Swizzle->Components.push_back(4);
	EXPECT_FALSE(BuildMaterialExpressionGraph(Swizzles, SwizzleRoots));
}

TEST(FMaterialExpressionTests, EveryMappedConcreteClassExposesApplicableInputs)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, "CatalogFunction"));
	struct FEntry { DClass* Class; EMaterialProgramOpcode Opcode; EMaterialProgramValueType Type; };
	const std::array Entries{
		FEntry{DMaterialExpressionScalarConstant::StaticClass(), EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionScalarParameter::StaticClass(), EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionVector2Constant::StaticClass(), EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float2},
		FEntry{DMaterialExpressionVector2Parameter::StaticClass(), EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float2},
		FEntry{DMaterialExpressionVector3Constant::StaticClass(), EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float3},
		FEntry{DMaterialExpressionVector3Parameter::StaticClass(), EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float3},
		FEntry{DMaterialExpressionVector4Constant::StaticClass(), EMaterialProgramOpcode::Constant, EMaterialProgramValueType::Float4},
		FEntry{DMaterialExpressionVector4Parameter::StaticClass(), EMaterialProgramOpcode::Parameter, EMaterialProgramValueType::Float4},
		FEntry{DMaterialExpressionTextureParameter::StaticClass(), EMaterialProgramOpcode::TextureParameter, EMaterialProgramValueType::Texture2D},
		FEntry{DMaterialExpressionAdd::StaticClass(), EMaterialProgramOpcode::Add, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionSubtract::StaticClass(), EMaterialProgramOpcode::Subtract, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionMultiply::StaticClass(), EMaterialProgramOpcode::Multiply, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionDivide::StaticClass(), EMaterialProgramOpcode::Divide, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionMinimum::StaticClass(), EMaterialProgramOpcode::Minimum, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionMaximum::StaticClass(), EMaterialProgramOpcode::Maximum, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionNegate::StaticClass(), EMaterialProgramOpcode::Negate, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionOneMinus::StaticClass(), EMaterialProgramOpcode::OneMinus, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionAbsolute::StaticClass(), EMaterialProgramOpcode::Absolute, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionSaturate::StaticClass(), EMaterialProgramOpcode::Saturate, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionClamp::StaticClass(), EMaterialProgramOpcode::Clamp, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionLerp::StaticClass(), EMaterialProgramOpcode::Lerp, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionSine::StaticClass(), EMaterialProgramOpcode::Sine, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionCosine::StaticClass(), EMaterialProgramOpcode::Cosine, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionNormalize::StaticClass(), EMaterialProgramOpcode::Normalize, EMaterialProgramValueType::Float3},
		FEntry{DMaterialExpressionMakeVector2::StaticClass(), EMaterialProgramOpcode::MakeFloat2, EMaterialProgramValueType::Float2},
		FEntry{DMaterialExpressionSplat2::StaticClass(), EMaterialProgramOpcode::Splat2, EMaterialProgramValueType::Float2},
		FEntry{DMaterialExpressionMakeVector3::StaticClass(), EMaterialProgramOpcode::MakeFloat3, EMaterialProgramValueType::Float3},
		FEntry{DMaterialExpressionSplat3::StaticClass(), EMaterialProgramOpcode::Splat3, EMaterialProgramValueType::Float3},
		FEntry{DMaterialExpressionMakeVector4::StaticClass(), EMaterialProgramOpcode::MakeFloat4, EMaterialProgramValueType::Float4},
		FEntry{DMaterialExpressionSplat4::StaticClass(), EMaterialProgramOpcode::Splat4, EMaterialProgramValueType::Float4},
		FEntry{DMaterialExpressionDecodeNormalRG::StaticClass(), EMaterialProgramOpcode::DecodeNormalRG, EMaterialProgramValueType::Float3},
		FEntry{DMaterialExpressionBlendNormalsRNM::StaticClass(), EMaterialProgramOpcode::BlendNormalsRNM, EMaterialProgramValueType::Float3},
		FEntry{DMaterialExpressionUVChannel::StaticClass(), EMaterialProgramOpcode::UVChannel, EMaterialProgramValueType::Float2},
		FEntry{DMaterialExpressionSwizzle::StaticClass(), EMaterialProgramOpcode::Swizzle, EMaterialProgramValueType::Float},
		FEntry{DMaterialExpressionTextureSample2D::StaticClass(), EMaterialProgramOpcode::TextureSample2D, EMaterialProgramValueType::Float4},
		FEntry{DMaterialExpressionTextureSampleParameter2D::StaticClass(), EMaterialProgramOpcode::TextureSampleParameter2D, EMaterialProgramValueType::Float4},
		FEntry{DMaterialExpressionTextureCoordinates::StaticClass(), EMaterialProgramOpcode::TextureCoordinates, EMaterialProgramValueType::Float2},
		FEntry{DMaterialExpressionMakeSurface::StaticClass(), EMaterialProgramOpcode::MakeSurface, EMaterialProgramValueType::Surface},
		FEntry{DMaterialExpressionGetSurfaceAttributes::StaticClass(), EMaterialProgramOpcode::GetSurfaceAttributes, EMaterialProgramValueType::Surface},
		FEntry{DMaterialExpressionSetSurfaceAttributes::StaticClass(), EMaterialProgramOpcode::SetSurfaceAttributes, EMaterialProgramValueType::Surface},
		FEntry{DMaterialExpressionFunctionInput::StaticClass(), EMaterialProgramOpcode::FunctionInput, EMaterialProgramValueType::Surface},
		FEntry{DMaterialExpressionFunctionOutput::StaticClass(), EMaterialProgramOpcode::FunctionOutput, EMaterialProgramValueType::Surface},
		FEntry{DMaterialExpressionFunctionCall::StaticClass(), EMaterialProgramOpcode::FunctionCall, EMaterialProgramValueType::Surface}};
	uint32 Index = 0;
	std::unordered_set<EMaterialProgramOpcode> Covered;
	for (const auto& Entry : Entries)
	{
		TStrongObjectPtr<DMaterialExpression> Expression(Cast<DMaterialExpression>(
			NewObject(Entry.Class, nullptr, FName("CatalogExpression" + std::to_string(++Index)))));
		ASSERT_NE(Expression.Get(), nullptr);
		Expression->Id = {1, 2, 3, Index};
		if (auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()))
			Parameter->Metadata = {.Id = {4, 5, 6, Index}, .Name = "Parameter"};
		if (auto* Normalize = Cast<DMaterialExpressionNormalize>(Expression.Get()))
			Normalize->ResultType = EMaterialProgramValueType::Float3;
		if (auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression.Get()))
			Input->PortId = Function->GetFunctionSignature().Inputs[0].Id;
		if (auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression.Get()))
			Output->PortId = Function->GetFunctionSignature().Outputs[0].Id;
		if (auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get()))
		{
			Call->Function = Function.Get();
			Call->Outputs = {{Function->GetFunctionSignature().Outputs[0].Id, EMaterialProgramValueType::Surface}};
		}
		EXPECT_EQ(Expression->GetClass()->FindPropertyByName("Opcode"), nullptr);
		EXPECT_EQ(Expression->GetClass()->FindPropertyByName("Parameter"), nullptr);
		if (auto* Surface = Cast<DMaterialExpressionSetSurfaceAttributes>(Expression.Get()))
			Surface->Attributes.push_back({EMaterialSurfaceOutput::Roughness, {}});
		if (auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get()))
			Call->Inputs.push_back({Function->GetFunctionSignature().Inputs[0].Id, EMaterialProgramValueType::Surface, {}});
		uint32 Visited = 0;
		Durin::Editor::Material::VisitMaterialExpressionInputs(*Expression, [&](uint32 Pin, FMaterialExpressionInput& Input) {
			Input = {{11, 12, Index, Pin + 1}}; ++Visited;
		});
		EXPECT_EQ(Visited, Expression->GetAuthoredInputCount());
		Durin::Editor::Material::VisitMaterialExpressionInputs(*Expression, [&](uint32 Pin, FMaterialExpressionInput& Input) {
			EXPECT_EQ(Input.ExpressionId, (FGuid{11, 12, Index, Pin + 1}));
		});
		Covered.insert(Entry.Opcode);
	}
	// No enum member has numeric value 3 or 30.
	for (uint32 Opcode = 0; Opcode <= static_cast<uint32>(EMaterialProgramOpcode::TextureCoordinates); ++Opcode)
		if (Opcode != 3 && Opcode != 30 && !(Opcode >= 25 && Opcode <= 27)) EXPECT_TRUE(Covered.contains(static_cast<EMaterialProgramOpcode>(Opcode))) << Opcode;
}

TEST(FMaterialExpressionTests, LocalAuthoringValidationPreservesMissingDependenciesAndRejectsInvalidLinks)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "LocalValidation"));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialExpressionVector3Constant> Value(NewObject<DMaterialExpressionVector3Constant>(nullptr, "Value"));
	Value->Id = FGuid::NewGuid();
	TStrongObjectPtr<DMaterialExpressionFunctionCall> Call(NewObject<DMaterialExpressionFunctionCall>(nullptr, "MissingDependency"));
	Call->Id = FGuid::NewGuid();
	const auto InputId = FGuid::NewGuid(), OutputId = FGuid::NewGuid();
	Call->Inputs = {{InputId, EMaterialProgramValueType::Float3, {Value->Id}}};
	Call->Outputs = {{OutputId, EMaterialProgramValueType::Float3}};
	const std::array<DMaterialExpression*, 2> Expressions{Value.Get(), Call.Get()};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.BaseColor = {Call->Id, 0, OutputId};
	ASSERT_TRUE(FMaterialExpressionBuildContext::ValidateSurface(Expressions, Outputs));
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
	FMaterialIRCompilerInput Snapshot;
	EXPECT_FALSE(SnapshotMaterialCompilerInput(*Material, {}, Snapshot));
	const auto Children = Material->GetExpressionCollection().Expressions;
	const auto Revision = Material->GetMaterialProgramRevision();
	Call->Inputs[0].Input = {Call->Id, 0, OutputId};
	EXPECT_FALSE(FMaterialExpressionBuildContext::ValidateSurface(Expressions, Outputs));
	EXPECT_FALSE(Material->SetMaterialExpressions(Expressions, Outputs));
	EXPECT_EQ(Material->GetExpressionCollection().Expressions, Children);
	EXPECT_EQ(Material->GetMaterialProgramRevision(), Revision);
	Call->Inputs[0].Input = {Value->Id};
	Call->Inputs[0].InputId = OutputId;
	EXPECT_FALSE(FMaterialExpressionBuildContext::ValidateSurface(Expressions, Outputs));
}

TEST(FMaterialExpressionTests, LocalFunctionValidationUsesDeclaredPortsWithoutAnInvocation)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	const auto InputId = FGuid::NewGuid(), OutputId = FGuid::NewGuid();
	FMaterialFunctionSignature Signature;
	Signature.Inputs = {{.Id = InputId, .Type = EMaterialProgramValueType::Float3, .Name = "Input", .bRequired = true}};
	Signature.Outputs = {{.Id = OutputId, .Type = EMaterialProgramValueType::Float3, .Name = "Output"}};
	TStrongObjectPtr<DMaterialExpressionFunctionInput> Input(NewObject<DMaterialExpressionFunctionInput>(nullptr, "Input"));
	Input->Id = FGuid::NewGuid(); Input->PortId = InputId;
	TStrongObjectPtr<DMaterialExpressionFunctionOutput> Output(NewObject<DMaterialExpressionFunctionOutput>(nullptr, "Output"));
	Output->Id = FGuid::NewGuid(); Output->PortId = OutputId; Output->Source = {Input->Id};
	const std::array<DMaterialExpression*, 2> Expressions{Input.Get(), Output.Get()};
	ASSERT_TRUE(FMaterialExpressionBuildContext::ValidateFunction(Expressions, Signature));
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, "LocalFunction"));
	ASSERT_TRUE(Function->SetFunctionExpressions(Signature, Expressions));
	Input->PortId = FGuid::NewGuid();
	EXPECT_FALSE(FMaterialExpressionBuildContext::ValidateFunction(Expressions, Signature));
	Input->PortId = InputId;
	Signature.Outputs[0].Type = EMaterialProgramValueType::Float;
	EXPECT_FALSE(FMaterialExpressionBuildContext::ValidateFunction(Expressions, Signature));
	Signature.Outputs[0].Type = EMaterialProgramValueType::Float3;
	EXPECT_FALSE(FMaterialExpressionBuildContext::ValidateFunction(std::span(Expressions).first(1), Signature));
}

TEST(FMaterialExpressionTests, ParameterAdmissionSharesDeclarationCountAndIdentityBounds)
{
	using namespace Durin;
	InitializeDObjectSystem();
	std::vector<TStrongObjectPtr<DMaterialExpressionScalarParameter>> Owners;
	std::vector<DMaterialExpression*> Expressions;
	for (uint32 Index = 0; Index <= MaterialMaxParameterDefinitionCount; ++Index)
	{
		auto* Parameter = NewObject<DMaterialExpressionScalarParameter>(nullptr, NAME_None);
		Parameter->Id = FGuid::NewGuid();
		Parameter->Metadata = {.Id = FGuid::NewGuid(), .Name = FName(std::format("Parameter{}", Index))};
		Owners.emplace_back(Parameter);
		Expressions.push_back(Parameter);
	}
	EXPECT_TRUE(FMaterialExpressionBuildContext::ValidateSurface(std::span(Expressions).first(MaterialMaxParameterDefinitionCount), {}));
	EXPECT_FALSE(FMaterialExpressionBuildContext::ValidateSurface(Expressions, {}));
	Expressions.pop_back();
	Owners.front()->Metadata.Id = Owners[1]->Id;
	EXPECT_FALSE(FMaterialExpressionBuildContext::ValidateSurface(Expressions, {}));
}

TEST(FMaterialExpressionTests, LocalCallsAdmitAllPortTypesAtTheAuthoredNodeBound)
{
	using namespace Durin;
	InitializeDObjectSystem();
	std::vector<TStrongObjectPtr<DMaterialExpressionFunctionCall>> Owners;
	std::vector<DMaterialExpression*> Expressions;
	for (uint32 Index = 0; Index < MaterialProgramMaxNodeCount; ++Index)
	{
		auto* Call = NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None);
		Call->Id = FGuid::NewGuid();
		for (uint32 Port = 0; Port < MaterialFunctionMaxOutputs; ++Port)
			Call->Outputs.push_back({FGuid::NewGuid(), static_cast<EMaterialProgramValueType>(Port % 6)});
		Owners.emplace_back(Call);
		Expressions.push_back(Call);
	}
	const auto Validation = FMaterialExpressionBuildContext::ValidateSurface(Expressions, {});
	ASSERT_TRUE(Validation) << (Validation.Diagnostics.empty() ? "" : Validation.Diagnostics.front().Message);
}

TEST(FMaterialExpressionTests, AuthoringFingerprintTracksCallPortsAndExcludesParameterDefaults)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	TStrongObjectPtr<DMaterialExpressionVector3Parameter> Parameter(NewObject<DMaterialExpressionVector3Parameter>(nullptr, NAME_None));
	Parameter->Id = FGuid::NewGuid(); Parameter->Metadata = {.Id = FGuid::NewGuid(), .Name = "Color"};
	TStrongObjectPtr<DMaterialExpressionFunctionCall> Call(NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None));
	Call->Id = FGuid::NewGuid();
	Call->Inputs = {{FGuid::NewGuid(), EMaterialProgramValueType::Float3, {Parameter->Id}}};
	Call->Outputs = {{FGuid::NewGuid(), EMaterialProgramValueType::Float3}, {FGuid::NewGuid(), EMaterialProgramValueType::Float3}};
	const std::array<DMaterialExpression*, 2> Expressions{Parameter.Get(), Call.Get()};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.BaseColor = {Call->Id, 0, Call->Outputs[0].OutputId};
	FXxHash128 Before, After;
	ASSERT_TRUE(FMaterialExpressionBuildContext::ValidateSurface(Expressions, Outputs, &Before));
	Parameter->DefaultValue = {.2, .4, .6}; Parameter->Metadata.DisplayName = "Display color";
	ASSERT_TRUE(FMaterialExpressionBuildContext::ValidateSurface(Expressions, Outputs, &After));
	EXPECT_EQ(After, Before);
	Outputs.BaseColor.OutputId = Call->Outputs[1].OutputId;
	ASSERT_TRUE(FMaterialExpressionBuildContext::ValidateSurface(Expressions, Outputs, &After));
	EXPECT_NE(After, Before);
	Outputs.BaseColor.OutputId = Call->Outputs[0].OutputId;
	const auto InputId = Call->Inputs[0].InputId;
	Call->Inputs[0].InputId = FGuid::NewGuid();
	ASSERT_TRUE(FMaterialExpressionBuildContext::ValidateSurface(Expressions, Outputs, &After));
	EXPECT_NE(After, Before);
	Call->Inputs[0].InputId = InputId;
	TStrongObjectPtr<DMaterialFunction> Callee(NewObject<DMaterialFunction>(nullptr, NAME_None));
	Call->Function = Callee.Get();
	ASSERT_TRUE(FMaterialExpressionBuildContext::ValidateSurface(Expressions, Outputs, &After));
	EXPECT_NE(After, Before);
	const auto Retained = After;
	Parameter->DefaultValue.x = std::numeric_limits<float>::infinity();
	EXPECT_FALSE(FMaterialExpressionBuildContext::ValidateSurface(Expressions, Outputs, &After));
	EXPECT_EQ(After, Retained);
}
