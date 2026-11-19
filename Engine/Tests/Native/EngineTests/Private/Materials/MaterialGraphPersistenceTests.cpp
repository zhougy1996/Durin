#include "MaterialGraph.h"
#include "DObject/PackageFormat.h"
#include "Graph/MaterialGraphCatalog.h"
#include "Materials/MaterialShaderMap.h"
#include "Materials/MaterialCustomVersion.h"
#include "Graph/MaterialGraphInteraction.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionArithmetic.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionScene.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/MaterialExpressionTextureCoordinates.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVector4Parameter.h"
#include "Asset/OfflinePreparation.h"
#include "MaterialGraphDocument.h"
#include "Misc/UnifiedError.h"
#include "NativePackageUnloadTestSupport.h"
#include "DObject/OverridableManager.h"
#include "MaterialGraphTestSupport.h"
#include "MaterialResourceTestSupport.h"
#include "Misc/FileHelper.h"
#include "Widgets/MaterialEditingSession.h"

TEST(FMaterialGraphPersistenceTests, HiddenRootDefaultResetRetainsLiteralAndHistory)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, NAME_None));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
	const auto Pin = FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialProperty::MP_Roughness);
	auto* Root = Graph->FindNode(Pin.NodeId);
	ASSERT_TRUE(Root);
	ASSERT_TRUE(Graph->GetCommands().SetUseMaterialAttributes(true, ETransactionMode::NotTransactable));
	ASSERT_FALSE(Graph->FindInputPin(Pin));
	Tests::FTestEditorTransactorOwner History;
	const FMaterialInputDefault Value{.Kind = EMaterialInputDefaultKind::Literal, .Literal = {.X = .625f}};
	ASSERT_TRUE(Root->PinDefaultValueChanged(Pin, Value));
	ASSERT_TRUE(Root->SetPinUseConstant(Pin, false));
	const auto& Input = Material->GetExpressionOutputs().Get(EMaterialProperty::MP_Roughness);
	EXPECT_FALSE(Input.UseConstant);
	EXPECT_EQ(Input.Constant, std::vector<float>{.625f});
	ASSERT_TRUE(History->Undo());
	EXPECT_TRUE(Material->GetExpressionOutputs().Get(EMaterialProperty::MP_Roughness).UseConstant);
	ASSERT_TRUE(History->Redo());
	ASSERT_TRUE(Graph->GetCommands().SetUseMaterialAttributes(false, ETransactionMode::NotTransactable));
	const auto* Restored = Graph->FindInputPin(Pin);
	ASSERT_TRUE(Restored);
	EXPECT_FALSE(Restored->bUseConstant);
	EXPECT_EQ(Restored->RetainedConstant, Value);
}

TEST(FMaterialGraphPersistenceTests, ObjectTransactionsRefreshCompilationAndLeaveRejectedEditsClean)
{
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	Testing::FScopedMountRegistryFixture Registry;
	Testing::RegisterMountPointForTests("/ExpressionUndo/", Testing::GetTestWorkDirectory().generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ExpressionUndo/Material", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Document(*Material);
	const auto Scalar = Testing::CreateGraphConstant(Document, 0.25f);
	ASSERT_TRUE(Scalar);
	const FGuid Id = Scalar.GeneratedNodeIds.front();
	const auto Input = FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialProperty::MP_Roughness);
	const auto Source = FMaterialGraphPinAddress::Output({Id});
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	{
		TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
		Tests::FTestEditorTransactorOwner History;
		const auto Revision = [&] { return Material->GetMaterialCompileDebugInfo().AuthoredRevision; };
		const auto Literal = [&] { return Graph->FindNode(Id)->GetView().Node.GetConstantLiteral().X; };
		const auto Connected = [&] { return Material->GetExpressionOutputs().Get(EMaterialProperty::MP_Roughness).Connection.GetExpressionId() == Id; };
		const auto CleanRevision = Material->GetPackage()->GetEditRevision();
		const auto InitialCompile = Revision();
		EXPECT_FALSE(Graph->FindNode(Id)->SetConstantValue(FMaterialParameterValue::MakeScalar(std::numeric_limits<float>::infinity())));
		EXPECT_EQ(Graph->FindNode(Id)->SetConstantValue(FMaterialParameterValue::MakeScalar(.25f)).GetStatus(), EMaterialGraphCommandStatus::NoChange);
		EXPECT_FALSE(Graph->GetSchema().TryCreateConnection(*Graph, Input, FMaterialGraphPinAddress::Output({FGuid::NewGuid()})));
		auto* Root = Graph->FindNode(Input.NodeId);
		ASSERT_TRUE(Root);
		EXPECT_FALSE(Root->PinDefaultValueChanged(Input, {.Kind = EMaterialInputDefaultKind::Literal,
			.Type = EMaterialProgramValueType::Float3, .Literal = {1.f, 2.f, 3.f}}));
		EXPECT_EQ(Material->GetPackage()->GetEditRevision(), CleanRevision);
		EXPECT_FALSE(Material->GetPackage()->IsDirty());
		EXPECT_EQ(Revision(), InitialCompile);
		EXPECT_EQ(History->GetQueueLength(), 0u);

		const FMaterialInputDefault Default{.Kind = EMaterialInputDefaultKind::Literal,
			.Type = EMaterialProgramValueType::Float, .Literal = {.X = .375f}};
		const auto Original = Material->GetExpressionOutputs().Get(EMaterialProperty::MP_Roughness);
		ASSERT_TRUE(Root->PinDefaultValueChanged(Input, Default));
		EXPECT_EQ(Root->PinDefaultValueChanged(Input, Default).GetStatus(), EMaterialGraphCommandStatus::NoChange);
		EXPECT_EQ(History->GetQueueLength(), 1u);
		EXPECT_GT(Revision(), InitialCompile);
		ASSERT_TRUE(History->Undo());
		EXPECT_EQ(Material->GetExpressionOutputs().Get(EMaterialProperty::MP_Roughness), Original);
		EXPECT_FALSE(Material->GetPackage()->IsDirty());
		ASSERT_TRUE(History->Redo());
		EXPECT_EQ(Material->GetExpressionOutputs().Get(EMaterialProperty::MP_Roughness).Constant, std::vector<float>{.375f});
		EXPECT_TRUE(Graph->FindInputPin(Input)->bUseConstant);
		ASSERT_TRUE(Root->SetPinUseConstant(Input, false));
		EXPECT_FALSE(Graph->FindInputPin(Input)->bUseConstant);
		EXPECT_FLOAT_EQ(Graph->FindInputPin(Input)->RetainedConstant.Literal.X, .375f);
		EXPECT_EQ(Root->SetPinUseConstant(Input, false).GetStatus(), EMaterialGraphCommandStatus::NoChange);
		ASSERT_TRUE(History->Undo());
		EXPECT_TRUE(Graph->FindInputPin(Input)->bUseConstant);
		ASSERT_TRUE(History->Redo());
		EXPECT_FALSE(Graph->FindInputPin(Input)->bUseConstant);
		ASSERT_TRUE(History->Undo());
		ASSERT_TRUE(History->Undo());
		ASSERT_TRUE(Graph->GetSchema().TryCreateConnection(Graph->FindPin(Input), Graph->FindPin(Source)));
		EXPECT_TRUE(Connected());
		EXPECT_EQ(History->GetQueueLength(), 1u);
		auto Before = Revision();
		ASSERT_TRUE(Graph->FindNode(Id)->SetConstantValue(FMaterialParameterValue::MakeScalar(.75f)));
		EXPECT_EQ(History->GetQueueLength(), 2u);
		EXPECT_GT(Revision(), Before);
		EXPECT_FLOAT_EQ(Literal(), .75f);
		Before = Revision();
		ASSERT_TRUE(History->Undo());
		EXPECT_GT(Revision(), Before);
		EXPECT_FLOAT_EQ(Literal(), .25f);
		Before = Revision();
		ASSERT_TRUE(History->Undo());
		EXPECT_GT(Revision(), Before);
		EXPECT_FALSE(Connected());
		EXPECT_FALSE(Material->GetPackage()->IsDirty());
		Before = Revision();
		ASSERT_TRUE(History->Redo());
		EXPECT_TRUE(Connected());
		EXPECT_GT(Revision(), Before);
		Before = Revision();
		ASSERT_TRUE(History->Redo());
		EXPECT_FLOAT_EQ(Literal(), .75f);
		EXPECT_GT(Revision(), Before);
		ASSERT_TRUE(Material->CompileEdits());
	}
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Path));
}

TEST(FMaterialGraphPersistenceTests, SchemaConnectionsReplayAndReopenForMaterialAndFunction)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture Registry;
	Testing::RegisterMountPointForTests("/SchemaConnections/", Testing::GetTestWorkDirectory().generic_string() + "/");
	const auto RoundTrip = []<class T>(const char* Name) {
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(Name, Path));
		T* Asset = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Asset));
		FMaterialGraphDocument Document(*Asset);
		const auto Scalar = Testing::CreateGraphConstant(Document, 0.25f);
		const auto Saturate = Testing::CreateGraphCatalogNode(Document, EMaterialProgramOpcode::Saturate);
		ASSERT_TRUE(Scalar);
		ASSERT_TRUE(Saturate);
		const FMaterialProgramLink Link{Scalar.GeneratedNodeIds.front()};
		const auto Other = Testing::CreateGraphConstant(Document, 0.75f);
		ASSERT_TRUE(Other);
		const auto Replacement = FMaterialGraphPinAddress::Output({Other.GeneratedNodeIds.front()});
		const auto Input = FMaterialGraphPinAddress::Input(Saturate.GeneratedNodeIds.front(), 0);
		const auto Output = FMaterialGraphPinAddress::Output(Link);
		ASSERT_TRUE(SavePackage(Asset->GetPackage()));
		const auto CheckSource = [&](T& Owner, bool Connected) {
			const auto& Nodes = Owner.GetExpressionCollection().Expressions;
			auto* Sink = Cast<DMaterialExpressionSaturate>(FindMaterialExpression(Nodes, Input.NodeId));
			auto* Source = FindMaterialExpression(Nodes, Link.SourceNodeId);
			ASSERT_TRUE(Sink);
			ASSERT_TRUE(Source);
			EXPECT_EQ(Source->GetOuter(), &Owner);
			EXPECT_EQ(Sink->Input.Connection.Expression.Get(), Connected ? Source : nullptr);
			EXPECT_EQ(Sink->Input.Connection.OutputIndex, Link.SourceOutputIndex);
			EXPECT_EQ(Sink->Input.Connection.GetOutputId(), Link.SourceOutputId);
		};
		{
			Tests::FTestEditorTransactorOwner History;
			TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Asset));
			const auto& Schema = Graph->GetSchema();
			const auto Before = History->GetQueueLength();
			TStrongObjectPtr<DMaterialGraph> OtherGraph(DMaterialGraph::Create(*Asset));
			const auto CleanRevision = Asset->GetPackage()->GetEditRevision();
			EXPECT_FALSE(Schema.TryCreateConnection(OtherGraph->FindPin(Output), Graph->FindPin(Input)));
			EXPECT_FALSE(Asset->GetPackage()->IsDirty());
			EXPECT_EQ(Asset->GetPackage()->GetEditRevision(), CleanRevision);
			EXPECT_EQ(History->GetQueueLength(), Before);
			ASSERT_TRUE(Schema.CanCreateConnection(Graph->FindPin(Output), Graph->FindPin(Input)).Allows(false));
			ASSERT_TRUE(Schema.TryCreateConnection(Graph->FindPin(Output), Graph->FindPin(Input)));
			EXPECT_EQ(Graph->FindInputPin(Input)->Link, Link);
			CheckSource(*Asset, true);
			EXPECT_EQ(History->GetQueueLength(), Before + 1);
			const auto Revision = Asset->GetPackage()->GetEditRevision();
			const auto Loop = FMaterialGraphPinAddress::Output({Saturate.GeneratedNodeIds.front()});
			EXPECT_FALSE(Schema.CanCreateConnection(Graph->FindPin(Input), Graph->FindPin(Loop)).Allows(true));
			EXPECT_FALSE(Schema.TryCreateConnection(Graph->FindPin(Input), Graph->FindPin(Loop), true));
			EXPECT_EQ(Asset->GetPackage()->GetEditRevision(), Revision);
			EXPECT_EQ(History->GetQueueLength(), Before + 1);
			EXPECT_EQ(Graph->FindInputPin(Input)->Link, Link);
			CheckSource(*Asset, true);
			ASSERT_TRUE(History->Undo());
			EXPECT_FALSE(Graph->FindInputPin(Input)->Link.SourceNodeId.IsValid());
			CheckSource(*Asset, false);
			ASSERT_TRUE(History->Redo());
			EXPECT_EQ(Graph->FindInputPin(Input)->Link, Link);
			CheckSource(*Asset, true);
			ASSERT_TRUE(Schema.TryCreateConnection(Graph->FindPin(Replacement), Graph->FindPin(Input), true));
			EXPECT_EQ(History->GetQueueLength(), Before + 2);
			const auto CheckReplacement = [&] {
				const auto* Source = Graph->FindPin(Replacement);
				const auto* Sink = Graph->FindPin(Input);
				ASSERT_EQ(Sink->LinkedTo.size(), 1u);
				EXPECT_EQ(Sink->LinkedTo.front(), Source);
				EXPECT_EQ(Cast<DMaterialExpressionSaturate>(Sink->GetOwningNode()->GetMaterialExpression())->Input.Connection.Expression.Get(),
					Source->GetOwningNode()->GetMaterialExpression());
			};
			CheckReplacement();
			ASSERT_TRUE(History->Undo());
			CheckSource(*Asset, true);
			ASSERT_TRUE(History->Redo());
			CheckReplacement();
			ASSERT_TRUE(Schema.BreakPinLinks(Graph->FindPin(Replacement)));
			EXPECT_EQ(History->GetQueueLength(), Before + 3);
			CheckSource(*Asset, false);
			ASSERT_TRUE(History->Undo());
			CheckReplacement();
			ASSERT_TRUE(History->Redo());
			CheckSource(*Asset, false);
			ASSERT_TRUE(History->Undo());
			ASSERT_TRUE(History->Undo());
			CheckSource(*Asset, true);
		}
		ASSERT_TRUE(SavePackage(Asset->GetPackage()));
		ASSERT_TRUE(UnloadPackage(Path));
		auto* Reopened = LoadObject<T>(Testing::MakePackageLeafAssetObjectPathForTests(Path));
		ASSERT_TRUE(Reopened);
		{
			TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Reopened));
			ASSERT_TRUE(Graph->FindInputPin(Input));
			EXPECT_EQ(Graph->FindInputPin(Input)->Link, Link);
			CheckSource(*Reopened, true);
			EXPECT_FALSE(Reopened->GetPackage()->IsDirty());
		}
		ASSERT_TRUE(UnloadPackage(Path));
	};
	RoundTrip.operator()<DMaterial>("/SchemaConnections/Material");
	RoundTrip.operator()<DMaterialFunction>("/SchemaConnections/Function");
}

TEST(FMaterialGraphPersistenceTests, TextureCoordinatesRetainChannelLinkAndDefaultAcrossPackageRoundTrip)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root = Testing::GetTestWorkDirectory() / "TextureCoordinatesRoundTrip";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/TextureCoordinatesRoundTrip/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TextureCoordinatesRoundTrip/Base", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	auto Channel = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	Channel->Value = 1.f;
	auto Coordinates = Testing::MakeGraphExpression<DMaterialExpressionTextureCoordinates>();
	Coordinates->Channel.Constant = {2.f};
	Coordinates->Channel.Connection = {Channel.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(
		std::array<DMaterialExpression*, 2>{Channel.Get(), Coordinates.Get()}, {}));
	const auto Expected = CaptureExpressions(*Material);
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	auto* Duplicate = Cast<DMaterial>(DuplicateObject(Material, nullptr, "CopiedCoordinates").value());
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(CaptureExpressions(*Duplicate), Expected);
	MarkObjectHierarchyAsGarbage(Duplicate);
	// Release transient source expressions before unloading their duplicated package graph.
	Channel.Reset();
	Coordinates.Reset();
	ASSERT_TRUE(UnloadPackage(Path));
	Material = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedError;
		const auto Loaded = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedError);
		ASSERT_TRUE(Loaded);
		Material = Loaded;
	}
	ASSERT_NE(Material, nullptr);
	EXPECT_EQ(CaptureExpressions(*Material), Expected);
	EXPECT_FALSE(Material->GetPackage()->IsDirty());
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphPersistenceTests, CustomDeclarationsPersistAndDuplicateTheirIdentity)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root = Testing::GetTestWorkDirectory() / "CustomDeclarations";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/CustomDeclarations/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CustomDeclarations/Base", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Name = "LayerTint";
	Definition.Type = EMaterialParameterType::Vector4;
	Definition.Value = FMaterialParameterValue::MakeVector4({0.1, 0.2, 0.3, 0.4});
	auto Parameter = Testing::MakeGraphExpression<DMaterialExpressionVector4Parameter>();
	Parameter->Metadata.Id = Definition.Id; Parameter->Metadata.Name = Definition.Name;
	Parameter->DefaultValue = Definition.Value.GetVector4();
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Parameter.Get()}, {}));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	auto* Duplicate = Cast<DMaterial>(DuplicateObject(Material, nullptr, "CopiedCustomDeclarations").value());
	ASSERT_NE(Duplicate, nullptr);
	ASSERT_EQ(Duplicate->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Duplicate->GetParameterDefinitions().front(), Definition);
	MarkObjectHierarchyAsGarbage(Duplicate);
	ASSERT_TRUE(UnloadPackage(Path));
	Material = nullptr;
	{
		std::optional<Durin::UnifiedError::FError> LoadedValueError;
		auto LoadedValue = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedValueError);
		Material = LoadedValue;
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(Material, nullptr);
	ASSERT_EQ(Material->GetParameterDefinitions().size(), 1u);
	EXPECT_EQ(Material->GetParameterDefinitions().front(), Definition);
	EXPECT_FALSE(Material->GetPackage()->IsDirty());
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphPersistenceTests, LargeGraphRoundTripsWithoutLoadedOverrideLedger)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root = Testing::GetTestWorkDirectory() / "GraphWithoutLedger";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/GraphWithoutLedger/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/GraphWithoutLedger/Base", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	Testing::FTestMaterialExpressionGraph Graph;
	for (int Index = 0; Index < 65; ++Index)
	{
		FMaterialParameterDefinition Definition;
		Definition.Id = FGuid::NewGuid();
		Definition.Name = std::format("Tint{}", Index);
		Definition.Type = EMaterialParameterType::Vector4;
		Definition.Value = FMaterialParameterValue::MakeVector4({0.1, 0.0, 0.3, 1.0});
		auto Parameter = Testing::MakeGraphExpression<DMaterialExpressionVector4Parameter>();
		Parameter->Metadata.Id = Definition.Id; Parameter->Metadata.Name = Definition.Name;
		Parameter->DefaultValue = Definition.Value.GetVector4();
		Graph.Expressions.emplace_back(Parameter.Get());
	}
	ASSERT_TRUE(Graph.Apply(*Material));
	const auto Expected = CaptureExpressions(*Material);
	ASSERT_TRUE(SavePackage(Material->GetPackage(), {.Mode = EAssetPackageSaveMode::Complete}));
	const auto CompleteBytes = std::filesystem::file_size(Root / "Base.dasset");
	for (int Round = 0; Round < 2; ++Round)
	{
		std::optional<Durin::UnifiedError::FError> LoadResultError;
		ASSERT_TRUE(SavePackage(Material->GetPackage()));
		const auto DeltaBytes = std::filesystem::file_size(Root / "Base.dasset");
		EXPECT_LE(DeltaBytes, CompleteBytes);
		ASSERT_TRUE(UnloadPackage(Path));
		const auto LoadResult = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadResultError);
		Material = LoadResult;
		ASSERT_TRUE(LoadResult) << (LoadResultError ? LoadResultError->ToString() : "");
		ASSERT_NE(Material, nullptr);
		EXPECT_FALSE(Durin::FOverridableManager::HasOverriddenProperties(*Material));
		EXPECT_EQ(CaptureExpressions(*Material), Expected);
		auto* Copy = Cast<DMaterial>(DuplicateObject(Material, nullptr, "GraphWithoutLedgerCopy").value());
		ASSERT_NE(Copy, nullptr);
		EXPECT_FALSE(Durin::FOverridableManager::HasOverriddenProperties(*Copy));
		EXPECT_EQ(CaptureExpressions(*Copy), Expected);
		MarkObjectHierarchyAsGarbage(Copy);
	}
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialAssetCreationPersistenceTests, BuiltInMaterialsHaveCompletePersistentGraphPresentation)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(FMountPaths::InitDefaultMountPoints());
	const FAssetCatalogRefreshResult Refresh =
		RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(Refresh) << (Refresh.Errors.empty()
		? "Asset catalog refresh failed without a diagnostic."
		: Durin::FormatAssetRegistryError(Refresh.Errors.front()));

	for (const std::string_view PathString : {
		"/Engine/Materials/DefaultMaterial"})
	{
		std::optional<Durin::UnifiedError::FError> LoadedError;
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(PathString, Path));
		DMaterial* Material = nullptr;
		const auto Loaded = LoadObject<DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Path), nullptr, &LoadedError);
		Material = Loaded;
		ASSERT_TRUE(Loaded) << (LoadedError ? LoadedError->ToString() : "");
		ASSERT_NE(Material, nullptr);
		const FMaterialGraphPresentation& Presentation =
			Material->GetMaterialGraphPresentation();
		EXPECT_EQ(Presentation.Nodes.size(),
			Material->GetExpressionCollection().Expressions.size());
		EXPECT_NE(Material->GetOutputNode(), nullptr);
		const FMaterialGraphView View = ProjectMaterialGraph(&*Material);
		EXPECT_EQ(View.Nodes.size(), Material->GetExpressionCollection().Expressions.size());
		ASSERT_TRUE(UnloadPackage(Path));
	}
	CollectGarbage(Durin::EObjectFlags::Standalone);
}


TEST(FMaterialGraphPersistenceTests, SecondFunctionOutputRetainsItsIndexAcrossHistoryAndReload)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture Registry;
	Testing::RegisterMountPointForTests("/IndexedCall/", Testing::GetTestWorkDirectory().generic_string() + "/");
	FPackagePath MaterialPath, FunctionPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/IndexedCall/Caller", MaterialPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/IndexedCall/Callee", FunctionPath));
	DMaterial* Material = nullptr;
	DMaterialFunction* Function = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(MaterialPath, Material));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(FunctionPath, Function));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialGraphDocument Root(*Material), Body(*Function);
	const auto Call = Root.InsertFunctionCall(*Function, 0, 0, ETransactionMode::NotTransactable);
	ASSERT_TRUE(Call);
	FMaterialFunctionPort Port;
	Port.Id = FGuid::NewGuid();
	Port.Name = "Weight";
	Port.Type = EMaterialProgramValueType::Float;
	const auto Constant = Testing::CreateGraphConstant(Body, .25f);
	ASSERT_TRUE(Constant);
	ASSERT_TRUE(Body.AddPort(true, Port, {Constant.GeneratedNodeIds.front()}, 0, 0, ETransactionMode::NotTransactable));
	const auto Check = [&](DMaterial& Owner) {
		const auto& Input = Owner.GetExpressionOutputs().Get(EMaterialProperty::MP_Roughness).Connection;
		auto* Upstream = Cast<DMaterialExpressionFunctionCall>(Input.Expression.Get());
		ASSERT_TRUE(Upstream);
		EXPECT_EQ(Upstream->GetOuter(), &Owner);
		EXPECT_EQ(Upstream->Id, Call.GeneratedNodeIds.front());
		ASSERT_EQ(Upstream->Outputs.size(), 2u);
		EXPECT_EQ(Input.OutputIndex, 1);
		EXPECT_EQ(Upstream->Outputs[1].OutputId, Port.Id);
		TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(Owner));
		const auto* RootNode = Graph->FindNode(Owner.GetOutputNode()->Id);
		ASSERT_TRUE(RootNode);
		const auto Pins = RootNode->GetPins();
		const auto RootInput = std::ranges::find(Pins, static_cast<int32>(EMaterialProperty::MP_Roughness), &FMaterialGraphPin::SourceIndex);
		ASSERT_NE(RootInput, Pins.end());
		ASSERT_EQ(RootInput->LinkedTo.size(), 1u);
		const auto* Output = RootInput->LinkedTo.front();
		EXPECT_EQ(Output->SourceIndex, 1);
		EXPECT_EQ(Output->GetOwningNode(), Graph->FindNode(Upstream->Id));
		ASSERT_EQ(Output->LinkedTo.size(), 1u);
		EXPECT_EQ(Output->LinkedTo.front(), &*RootInput);
	};
	{
		Tests::FTestEditorTransactorOwner History;
		TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
		const auto Pin = FMaterialGraphPinAddress::MaterialOutput(Material->GetOutputNode()->Id, EMaterialProperty::MP_Roughness);
		const auto Source = FMaterialGraphPinAddress::Output({Call.GeneratedNodeIds.front(), 0, Port.Id});
		ASSERT_TRUE(Graph->GetSchema().TryCreateConnection(Graph->FindPin(Source), Graph->FindPin(Pin)));
		Check(*Material);
		ASSERT_TRUE(History->Undo());
		EXPECT_FALSE(Material->GetExpressionOutputs().Get(EMaterialProperty::MP_Roughness).Connection.IsConnected());
		ASSERT_TRUE(History->Redo());
		Check(*Material);
	}
	ASSERT_TRUE(SavePackage(Function->GetPackage()));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ASSERT_TRUE(Testing::EnsurePackageNotResident(FunctionPath));
	Material = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath));
	ASSERT_TRUE(Material);
	Check(*Material);
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ASSERT_TRUE(Testing::EnsurePackageNotResident(FunctionPath));
}

TEST(FMaterialGraphPersistenceTests, ShippedMaterialAndFunctionRetainAuthoredGraphAfterEditingAndReopen)
{
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	Testing::FScopedMountRegistryFixture Registry;
	Testing::RegisterMountPointForTests("/Engine/", FPaths::EngineContentDir(), true, false);
	Testing::RegisterMountPointForTests("/Plugins/Water/", (std::filesystem::path(FPaths::EngineDir()) / "Plugins/Water/Content").generic_string() + "/", true, false);
	Testing::RegisterMountPointForTests("/Game/", (std::filesystem::path(FPaths::EngineDir()) / "../Sandbox/Content").generic_string() + "/", true, false);
	const auto Scratch = Testing::GetTestWorkDirectory() / "ShippedGraphs";
	Testing::RegisterMountPointForTests("/ShippedGraphs/", Scratch.generic_string(), false);
	const auto Scan = RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(Scan) << (Scan.Errors.empty() ? "Asset scan failed" : Durin::FormatAssetRegistryError(Scan.Errors.front()));
	const auto CheckCopy = []<class T>(const char* Original, const char* Name) {
		FPackagePath SourcePath, Path;
		ASSERT_TRUE(FPackagePath::TryCreate(Original, SourcePath));
		ASSERT_TRUE(FPackagePath::TryCreate(std::string("/ShippedGraphs/") + Name, Path));
		std::optional<Durin::UnifiedError::FError> Error;
		auto* Source = LoadObject<T>(Testing::MakePackageLeafAssetObjectPathForTests(SourcePath), nullptr, &Error);
		ASSERT_TRUE(Source) << (Error ? Error->ToString() : Original);
		const auto Before = CaptureExpressions(*Source);
		const auto CheckVersion = [](const FPackagePath& PackagePath) {
			const auto Data = FindPackageExact(PackagePath);
			ASSERT_TRUE(Data);
			const auto Bytes = FFileHelper::LoadFileToArray(Data->PhysicalPath);
			ASSERT_TRUE(Bytes);
			ObjectPackage::FLinkerTables Tables;
			ASSERT_TRUE(ObjectPackage::ReadPackage(*Bytes, {}, PackagePath, Tables));
			const auto Version = std::ranges::find(Tables.CustomVersions, FMaterialGraphVersion::Guid, &FCustomVersion::Guid);
			ASSERT_NE(Version, Tables.CustomVersions.end());
			EXPECT_EQ(Version->Version, FMaterialGraphVersion::CurrentVersion);
			const auto InputSchema = std::ranges::find_if(Tables.Schemas, [](const auto& Schema) {
				return Schema.QualifiedName.ends_with("FMaterialExpressionInput");
			});
			ASSERT_NE(InputSchema, Tables.Schemas.end());
			ASSERT_EQ(InputSchema->Fields.size(), 2u);
			const auto Index = std::ranges::find(InputSchema->Fields, "OutputIndex", &ObjectPackage::FSerializedField::Name);
			ASSERT_NE(Index, InputSchema->Fields.end());
			EXPECT_EQ(Index->Type.Kind, ObjectPackage::EValueKind::I32);
			const auto Expression = std::ranges::find(InputSchema->Fields, "Expression", &ObjectPackage::FSerializedField::Name);
			ASSERT_NE(Expression, InputSchema->Fields.end());
			EXPECT_EQ(Expression->Type.Kind, ObjectPackage::EValueKind::HardReference);
		};
		CheckVersion(SourcePath);
		if (std::string_view(Name) != "MaterialCopy")
			EXPECT_TRUE(std::ranges::any_of(Before.Sources, [](const FGuid& Id) { return Id.IsValid(); }));
		auto* Package = CreatePackage(Path);
		ASSERT_TRUE(Package);
		const auto Copy = DuplicateObject(Source, Package, FName(Name));
		ASSERT_TRUE(Copy);
		auto* Asset = *Copy;
		EXPECT_EQ(CaptureExpressions(*Asset), Before);
		FGuid Id;
		{
			TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Asset));
			ASSERT_FALSE(Graph->GetNodes().empty());
			auto Position = Graph->GetNodes().front()->GetView().Presentation;
			Id = Position.NodeId;
			Position.X += 24;
			ASSERT_TRUE(Graph->MoveNodes(std::span(&Position, 1), ETransactionMode::NotTransactable));
			if constexpr (std::is_same_v<T, DMaterial>) ASSERT_TRUE(Asset->CompileEdits());
			ASSERT_TRUE(SavePackage(Package));
			ASSERT_TRUE(UnloadPackage(Path));
			auto* Reopened = LoadObject<T>(Testing::MakePackageLeafAssetObjectPathForTests(Path));
			ASSERT_TRUE(Reopened);
			CheckVersion(Path);
			EXPECT_EQ(CaptureExpressions(*Reopened), Before);
			Graph = DMaterialGraph::Create(*Reopened);
			ASSERT_TRUE(Graph->FindNode(Id));
			EXPECT_EQ(Graph->FindNode(Id)->GetView().Presentation.X, Position.X);
		}
		ASSERT_TRUE(UnloadPackage(Path));
	};
	CheckCopy.operator()<DMaterial>("/Engine/Materials/DefaultMaterial", "MaterialCopy");
	CheckCopy.operator()<DMaterial>("/Plugins/Water/Materials/DefaultWater", "WaterCopy");
	CheckCopy.operator()<DMaterial>("/Game/Materials/Metal/M_MetalRust", "MetalCopy");
	CheckCopy.operator()<DMaterialFunction>("/Engine/Materials/Functions/SampleNormal", "NormalCopy");
	CheckCopy.operator()<DMaterialFunction>("/Engine/Materials/Functions/SampleORM", "OrmCopy");
	CheckCopy.operator()<DMaterialFunction>("/Engine/Materials/Functions/UVTransform", "FunctionCopy");
}

TEST(FMaterialGraphPersistenceTests, TextureSamplingModeAndInactiveOperandsRoundTrip)
{
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root = Testing::GetTestWorkDirectory() / "TextureSamplingRoundTrip";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/TextureSamplingRoundTrip/", Root.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TextureSamplingRoundTrip/Base", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	auto Sample = Testing::MakeGraphExpression<DMaterialExpressionTextureSampleParameter2D>();
	Sample->Metadata = {.Id = FGuid::NewGuid(), .Name = "Sample"};
	Sample->ReadMode = EMaterialTextureReadMode::Level;
	Sample->MipValue.SetConstant({2.f});
	Sample->DDX.SetConstant({.125f,.25f});
	Sample->DDY.SetConstant({.5f,.75f});
	auto Cube = Testing::MakeGraphExpression<DMaterialExpressionTextureSampleParameterCube>();
	Cube->Metadata = {.Id = FGuid::NewGuid(), .Name = "Cube"};
	Cube->ReadMode = EMaterialTextureReadMode::Grad;
	Cube->DDX.SetConstant({.125f,.25f,.5f});
	Cube->DDY.SetConstant({.5f,.75f,1.f});
	Cube->DefaultValue.SamplerState.AddressW = EMaterialSamplerAddressMode::ClampToEdge;
	Cube->DefaultValue.TextureFallback = EMaterialTextureFallback::Black;
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 2>{Sample.Get(), Cube.Get()}, {}));
	const auto Expected = CaptureExpressions(*Material);
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	Sample.Reset();
	Cube.Reset();
	ASSERT_TRUE(UnloadPackage(Path));
	Material = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path));
	ASSERT_NE(Material, nullptr);
	EXPECT_EQ(CaptureExpressions(*Material), Expected);
	const auto It = std::ranges::find_if(Material->GetExpressionCollection().Expressions,
		[](const auto& E) { return Cast<DMaterialExpressionTextureSampleParameter2D>(E.Get()) != nullptr; });
	ASSERT_NE(It, Material->GetExpressionCollection().Expressions.end());
	const auto* LoadedSample = Cast<DMaterialExpressionTextureSampleParameter2D>(It->Get());
	EXPECT_EQ(LoadedSample->ReadMode, EMaterialTextureReadMode::Level);
	EXPECT_EQ(LoadedSample->DDX.Constant, (std::vector<float>{.125f,.25f}));
	EXPECT_EQ(LoadedSample->DDY.Constant, (std::vector<float>{.5f,.75f}));
	const auto CubeIt = std::ranges::find_if(Material->GetExpressionCollection().Expressions,
		[](const auto& E) { return Cast<DMaterialExpressionTextureSampleParameterCube>(E.Get()) != nullptr; });
	ASSERT_NE(CubeIt, Material->GetExpressionCollection().Expressions.end());
	const auto* LoadedCube = Cast<DMaterialExpressionTextureSampleParameterCube>(CubeIt->Get());
	EXPECT_EQ(LoadedCube->ReadMode, EMaterialTextureReadMode::Grad);
	EXPECT_EQ(LoadedCube->DefaultValue.Dimension, EMaterialTextureDimension::TextureCube);
	EXPECT_EQ(LoadedCube->DefaultValue.SamplerState.AddressW, EMaterialSamplerAddressMode::ClampToEdge);
	EXPECT_EQ(LoadedCube->DefaultValue.TextureFallback, EMaterialTextureFallback::Black);
	EXPECT_EQ(LoadedCube->DDX.Constant, (std::vector<float>{.125f,.25f,.5f}));
	const auto* Definition = Material->FindParameterDefinition("Cube");
	ASSERT_NE(Definition, nullptr);
	EXPECT_EQ(Definition->Value.GetTexture().Dimension, EMaterialTextureDimension::TextureCube);
	ASSERT_TRUE(UnloadPackage(Path));
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FMaterialGraphPersistenceTests, SceneInputDomainCoordinatesAndFilteringSurvivePackageReload)
{
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root = Testing::GetTestWorkDirectory() / "SceneInputRoundTrip";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/SceneInputRoundTrip/", Root.generic_string() + "/");
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/SceneInputRoundTrip/Effect", Path));
	DMaterial* Material = nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialConfiguration Configuration; Configuration.Domain = EMaterialDomain::PostProcess;
	Configuration.Properties.ShadingModel = EMaterialShadingModel::Unlit;
	ASSERT_TRUE(Material->SetMaterialConfiguration(Configuration));
	auto Scene = Testing::MakeGraphExpression<DMaterialExpressionSceneTexture>();
	Scene->bFiltered = false; Scene->Coordinates.SetConstant({.25f,.75f});
	auto RGB = Testing::MakeGraphExpression<DMaterialExpressionSwizzle>(); RGB->Components = {0,1,2}; RGB->Input = {Scene.Get()};
	FMaterialAttributeRoot Outputs; Outputs.Get(EMaterialProperty::MP_EmissiveColor) = {RGB.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*,2>{Scene.Get(),RGB.Get()}, Outputs));
	const auto Expected = CaptureExpressions(*Material);
	ASSERT_TRUE(SavePackage(Material->GetPackage())); Scene.Reset(); RGB.Reset();
	ASSERT_TRUE(UnloadPackage(Path));
	Material = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path)); ASSERT_NE(Material,nullptr);
	EXPECT_EQ(Material->GetDomain(),EMaterialDomain::PostProcess); EXPECT_EQ(CaptureExpressions(*Material),Expected);
	const auto Node = std::ranges::find_if(Material->GetExpressionCollection().Expressions, [](const auto& E) { return Cast<DMaterialExpressionSceneTexture>(E.Get()) != nullptr; });
	ASSERT_NE(Node,Material->GetExpressionCollection().Expressions.end());
	const auto* Loaded = Cast<DMaterialExpressionSceneTexture>(Node->Get()); EXPECT_FALSE(Loaded->bFiltered);
	EXPECT_EQ(Loaded->SceneTextureId,EMaterialSceneTextureId::PostProcessInput0);
	EXPECT_EQ(Loaded->Coordinates.Constant,(std::vector<float>{.25f,.75f}));
	ASSERT_TRUE(Material->CompileEdits()) << FormatMaterialProgramDiagnostics(Material->GetMaterialCompileDiagnostics());
	ASSERT_TRUE(Material->GetAcceptedCompiledProgram());
	EXPECT_EQ(Material->GetAcceptedCompiledProgram()->GetShaderMap().Domain,EMaterialDomain::PostProcess);
	ASSERT_TRUE(UnloadPackage(Path)); CollectGarbage(EObjectFlags::Standalone);
}

TEST(FMaterialGraphPersistenceTests, Stage4ConfigurationsUndoRedoValueUpdatesAndReopenPreserveContracts)
{
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root = Testing::GetTestWorkDirectory() / "Stage4History";
	Testing::RemoveTestWorkDirectory(Root);
	Testing::RegisterMountPointForTests("/Game/", Root.generic_string() + "/");
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/Game/MaterialContracts/M_Stage4History", Path));
	DMaterial* Material = nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	auto Weight = Testing::MakeGraphExpression<DMaterialExpressionScalarParameter>();
	Weight->Metadata = {.Id=FGuid::NewGuid(), .Name="LayerWeight"}; Weight->DefaultValue=.25f;
	auto Color = Testing::MakeGraphExpression<DMaterialExpressionVector4Parameter>();
	Color->Metadata = {.Id=FGuid::NewGuid(), .Name="Emission"}; Color->DefaultValue={.2,.4,.6,1};
	auto RGB = Testing::MakeGraphExpression<DMaterialExpressionSwizzle>(); RGB->Input={Color.Get()}; RGB->Components={0,1,2};
	const auto WeightId=Weight->Metadata.Id, ColorId=Color->Metadata.Id;
	FMaterialAttributeRoot Outputs;
	Outputs.Get(EMaterialProperty::MP_Specular)={Weight.Get()};
	Outputs.Get(EMaterialProperty::MP_ClearCoat)={Weight.Get()};
	Outputs.Get(EMaterialProperty::MP_ClearCoatRoughness)={Weight.Get()};
	Outputs.Get(EMaterialProperty::MP_EmissiveColor)={RGB.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*,3>{Weight.Get(),Color.Get(),RGB.Get()},Outputs));
	Weight.Reset(); Color.Reset(); RGB.Reset();
	std::array<FMaterialConfiguration,4> Configurations;
	Configurations[1].Properties.ShadingModel=EMaterialShadingModel::ClearCoat;
	Configurations[2].Properties.ShadingModel=EMaterialShadingModel::Unlit;
	Configurations[3].Domain=EMaterialDomain::PostProcess;
	Configurations[3].Properties.ShadingModel=EMaterialShadingModel::Unlit;
	std::vector<FMaterialProgramIdentity> Identities;
	std::string Evidence="variant,domain,model,identity,entries,value_compile_requests\n";
	for (uint32 Index=0;Index<Configurations.size();++Index)
	{
		SCOPED_TRACE(Index);
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		const auto Before=CaptureExpressions(*Material);
		const FMaterialConfiguration Previous{Material->GetDomain(),Material->GetStaticProperties()};
		Tests::FTestEditorTransactorOwner Transactions;
		FMaterialGraphDocument Document(*Material);
		std::vector<FMaterialGraphChangeSet> Changes;
		const auto Subscription=Material->GetGraphChanges().Subscribe(*Material,[&](const auto& Change) { Changes.push_back(Change); });
		ASSERT_TRUE(Document.SetConfiguration(Configurations[Index]));
		if (Index)
		{
			ASSERT_EQ(Changes.size(),1u);
			EXPECT_TRUE(std::ranges::any_of(Changes.back().Nodes,[&](const auto& N) { return N.NodeId==Material->GetOutputNode()->Id
				&& (N.Flags & EMaterialGraphNodeChange::Interface)!=EMaterialGraphNodeChange::None; }));
			ASSERT_TRUE(Transactions->Undo()); EXPECT_EQ(Material->GetDomain(),Previous.Domain); EXPECT_EQ(Material->GetStaticProperties(),Previous.Properties);
			ASSERT_TRUE(Material->CompileEdits()); ASSERT_TRUE(Material->GetAcceptedCompiledProgram());
			EXPECT_EQ(Material->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId,Identities.back());
			ASSERT_TRUE(Transactions->Redo());
		}
		Material->GetGraphChanges().Unsubscribe(Subscription);
		EXPECT_EQ(Material->GetDomain(),Configurations[Index].Domain); EXPECT_EQ(Material->GetStaticProperties(),Configurations[Index].Properties);
		EXPECT_EQ(CaptureExpressions(*Material),Before); // Inactive roots and stable parameter IDs are retained.
		ASSERT_TRUE(Transactions->TryReset());
		const auto Revision=Material->GetMaterialCompileDebugInfo().AuthoredRevision;
		auto Invalid=Configurations[Index]; Invalid.Domain=EMaterialDomain::PostProcess; Invalid.Properties.ShadingModel=EMaterialShadingModel::ClearCoat;
		EXPECT_FALSE(Document.SetConfiguration(Invalid)); EXPECT_FALSE(Transactions->CanUndo());
		Invalid.Domain=EMaterialDomain::Surface; Invalid.Properties.BlendMode=EMaterialBlendMode::Translucent;
		EXPECT_FALSE(Document.SetConfiguration(Invalid)); EXPECT_FALSE(Transactions->CanUndo());
		EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision,Revision); EXPECT_EQ(CaptureExpressions(*Material),Before);
		ASSERT_TRUE(Material->CompileEdits()) << FormatMaterialProgramDiagnostics(Material->GetMaterialCompileDiagnostics());
		const auto Program=Material->GetAcceptedCompiledProgram(); ASSERT_TRUE(Program);
		EXPECT_FALSE(std::ranges::contains(Identities,Program->GetShaderMap().ShaderMapId)); Identities.push_back(Program->GetShaderMap().ShaderMapId);
		EXPECT_EQ(Program->GetShaderMap().Domain,Configurations[Index].Domain);
		const auto OriginalDebug=Material->GetMaterialCompileDebugInfo();
		Material->SetEditCompileMode(EMaterialEditCompileMode::Immediate);
		const auto Requests=GetMaterialCompilationStatistics().AcceptedRequests;
		for (const auto Id : {WeightId,ColorId})
		{
			const auto Original=Material->FindParameterDefinition(Id)->Value;
			const auto Value=Id==WeightId ? FMaterialParameterValue::MakeScalar(.5f+Index*.0625f)
				: FMaterialParameterValue::MakeVector4({.3+Index*.1,.2,.7,1});
			FMaterialGraphParameterEditSession Edit;
			ASSERT_TRUE(Edit.Begin(*Material,Id,Transactions.Get())); ASSERT_TRUE(Edit.Apply(Value)); ASSERT_TRUE(Edit.Commit());
			const auto Updated=Testing::CaptureMaterialRenderData(*Material); EXPECT_FALSE(Updated.Representation.IsError());
			ASSERT_TRUE(Updated.CompiledProgram); EXPECT_EQ(Updated.CompiledProgram->ShaderMap,Program->ShaderMap);
			const auto Field=std::ranges::find(Updated.Representation.GetLayout().Fields,Id,&FMaterialRenderField::ParameterId);
			EXPECT_EQ(Field!=Updated.Representation.GetLayout().Fields.end(),Id==ColorId || Index<2);
			if (Field!=Updated.Representation.GetLayout().Fields.end())
			{
				float Actual=0; std::memcpy(&Actual,Updated.Representation.GetUniformPayload().data()+Field->Offset,sizeof(float));
				EXPECT_FLOAT_EQ(Actual,Id==WeightId ? Value.GetScalar() : float(Value.GetVector4().x));
			}
			ASSERT_TRUE(Transactions->Undo()); EXPECT_EQ(Material->FindParameterDefinition(Id)->Value,Original);
			ASSERT_TRUE(Transactions->Redo()); EXPECT_EQ(Material->FindParameterDefinition(Id)->Value,Value);
			ASSERT_TRUE(Material->GetAcceptedCompiledProgram()); EXPECT_EQ(Material->GetAcceptedCompiledProgram()->ShaderMap,Program->ShaderMap);
			EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision,OriginalDebug.AuthoredRevision);
			EXPECT_EQ(Material->GetMaterialCompileDebugInfo().RequestId,OriginalDebug.RequestId);
		}
		EXPECT_EQ(GetMaterialCompilationStatistics().AcceptedRequests,Requests);
		Evidence+=std::format("{},{},{},{},{},{}\n",Index,uint32(Configurations[Index].Domain),uint32(Configurations[Index].Properties.ShadingModel),Program->GetShaderMap().ShaderMapId.ToString(),Program->GetShaderMap().Entries.size(),Material->GetMaterialCompileDebugInfo().RequestId-OriginalDebug.RequestId);
		{
			TStrongObjectPtr<DMaterialInstance> Instance(DMaterialInstance::CreateDynamic(Material)); ASSERT_TRUE(Instance);
			FAssetCompilingManager::Get().FinishCompilationForObject(*Instance);
			const auto BeforeInstance=Testing::CaptureMaterialRenderData(*Instance); ASSERT_TRUE(BeforeInstance.CompiledProgram); EXPECT_FALSE(BeforeInstance.Representation.IsError());
			const auto InstanceRequests=GetMaterialCompilationStatistics().AcceptedRequests;
			if (Index<2) ASSERT_TRUE(Instance->SetParameterValue(WeightId,FMaterialParameterValue::MakeScalar(.875f)));
			else EXPECT_FALSE(Instance->SetParameterValue(WeightId,FMaterialParameterValue::MakeScalar(.875f)));
			ASSERT_TRUE(Instance->SetParameterValue(ColorId,FMaterialParameterValue::MakeVector4({.8,.2,.1,1})));
			const auto Updated=Testing::CaptureMaterialRenderData(*Instance); EXPECT_FALSE(Updated.Representation.IsError()); ASSERT_TRUE(Updated.CompiledProgram);
			EXPECT_EQ(Updated.CompiledProgram->ShaderMap,BeforeInstance.CompiledProgram->ShaderMap);
			for (const auto& Field:Updated.Representation.GetLayout().Fields)
				if (Field.ParameterId==WeightId || Field.ParameterId==ColorId) { float Value; std::memcpy(&Value,Updated.Representation.GetUniformPayload().data()+Field.Offset,sizeof(Value)); EXPECT_FLOAT_EQ(Value,Field.ParameterId==WeightId ? .875f : .8f); }
			if (Index<2) ASSERT_TRUE(Instance->ClearParameterValue(WeightId));
			ASSERT_TRUE(Instance->ClearParameterValue(ColorId));
			const auto Cleared=Testing::CaptureMaterialRenderData(*Instance);
			EXPECT_EQ(Cleared.Representation.GetContentHash(),Testing::CaptureMaterialRenderData(*Material).Representation.GetContentHash());
			EXPECT_EQ(GetMaterialCompilationStatistics().AcceptedRequests,InstanceRequests);
			MarkAsGarbage(Instance.Get());
		}
		ASSERT_TRUE(Transactions->TryReset());
		const auto Expected=CaptureExpressions(*Material);
		ASSERT_TRUE(SavePackage(Material->GetPackage()));
		ASSERT_TRUE(UnloadPackage(Path)); Material=nullptr;
		Material=LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path)); ASSERT_TRUE(Material);
		EXPECT_EQ(CaptureExpressions(*Material),Expected); EXPECT_FALSE(Material->GetPackage()->IsDirty());
		EXPECT_EQ(Material->GetDomain(),Configurations[Index].Domain); EXPECT_EQ(Material->GetStaticProperties(),Configurations[Index].Properties);
		ASSERT_TRUE(Material->CompileEdits()); ASSERT_TRUE(Material->GetAcceptedCompiledProgram());
		EXPECT_EQ(Material->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId,Program->GetShaderMap().ShaderMapId);
	}
	// A combined domain/model edit in Immediate mode is one generation, including a return to a prior identity.
	Material->SetEditCompileMode(EMaterialEditCompileMode::Immediate);
	const auto ImmediateRequests=Material->GetMaterialCompileDebugInfo().RequestId;
	const auto ImmediateRevision=Material->GetMaterialCompileDebugInfo().AuthoredRevision;
	uint32 Notifications=0;
	const auto Subscription=Material->GetGraphChanges().Subscribe(*Material,[&](const auto&) { ++Notifications; });
	ASSERT_TRUE(FMaterialGraphDocument(*Material).SetConfiguration(Configurations.front(),ETransactionMode::NotTransactable));
	FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().RequestId,ImmediateRequests+1);
	EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision,ImmediateRevision+1);
	EXPECT_EQ(Notifications,1u); Material->GetGraphChanges().Unsubscribe(Subscription);
	ASSERT_TRUE(Material->GetAcceptedCompiledProgram()); EXPECT_EQ(Material->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId,Identities.front());
	if (const auto* Directory=std::getenv("DURIN_MATERIAL_STAGE4_CLOSURE_EVIDENCE_DIR")) ASSERT_TRUE(FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Evidence)),std::filesystem::path(Directory)/"history-contracts.csv"));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Path)); CollectGarbage(EObjectFlags::Standalone);
}

TEST(FMaterialGraphPersistenceTests, Stage4WorkingCopyAppliesDomainAndModelAcrossSaveAndReopen)
{
	InitializeDObjectSystem(); FScopedOfflinePreparation Offline;
	Testing::FScopedMountRegistryFixture MountRegistry;
	const auto Root=Testing::GetTestWorkDirectory()/"Stage4Apply";
	Testing::RemoveTestWorkDirectory(Root); Testing::RegisterMountPointForTests("/Game/",Root.generic_string()+"/");
	FPackagePath Path; ASSERT_TRUE(FPackagePath::TryCreate("/Game/MaterialContracts/M_Apply",Path));
	DMaterial* Source=nullptr; ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path,Source));
	Source->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FMaterialConfiguration Initial; Initial.Properties.ShadingModel=EMaterialShadingModel::Unlit;
	ASSERT_TRUE(Source->SetMaterialConfiguration(Initial));
	FMaterialAttributeRoot Outputs; Outputs.Get(EMaterialProperty::MP_EmissiveColor).SetConstant({.25f,.5f,1.f});
	ASSERT_TRUE(Source->SetMaterialExpressions({},Outputs)); ASSERT_TRUE(SavePackage(Source->GetPackage()));
	std::array<FMaterialConfiguration,3> Configurations;
	Configurations[0]=Initial; Configurations[0].Domain=EMaterialDomain::PostProcess;
	Configurations[1].Properties.ShadingModel=EMaterialShadingModel::ClearCoat;
	for (const auto& Configuration:Configurations)
	{
		SCOPED_TRACE(uint32(Configuration.Domain));
		const auto OriginalDomain=Source->GetDomain(); const auto OriginalProperties=Source->GetStaticProperties();
		FMaterialAttributeRoot ExpectedOutputs=Outputs;
		auto ExpectedBindings = CaptureMaterialOutputBindings(ExpectedOutputs);
		{
			Tests::FTestEditorTransactorOwner Transactions;
			FMaterialEditingSession Session; std::string Error;
			ASSERT_TRUE(Session.Initialize(*Source,EMaterialEditCompileMode::Manual,Error,Transactions.Get())) << Error;
			auto* Draft=Session.GetWorkingMaterial(); ASSERT_TRUE(Draft);
			EXPECT_EQ(Draft->GetDomain(),OriginalDomain); EXPECT_EQ(Draft->GetStaticProperties(),OriginalProperties); EXPECT_FALSE(Session.HasUnappliedChanges());
			ASSERT_TRUE(FMaterialGraphDocument(*Draft).SetConfiguration(Configuration)); EXPECT_TRUE(Session.HasUnappliedChanges());
			EXPECT_EQ(Source->GetDomain(),OriginalDomain); EXPECT_EQ(Source->GetStaticProperties(),OriginalProperties); EXPECT_FALSE(Source->GetPackage()->IsDirty());
			ASSERT_TRUE(Transactions->Undo()); EXPECT_FALSE(Session.HasUnappliedChanges());
			ASSERT_TRUE(Transactions->Redo()); EXPECT_TRUE(Session.HasUnappliedChanges());
			if (Configuration.Domain==EMaterialDomain::PostProcess)
			{
				auto Scene=Testing::MakeGraphExpression<DMaterialExpressionSceneTexture>();
				auto RGB=Testing::MakeGraphExpression<DMaterialExpressionSwizzle>(); RGB->Input={Scene.Get()}; RGB->Components={0,1,2};
				ExpectedOutputs.Get(EMaterialProperty::MP_EmissiveColor)={RGB.Get()};
				ExpectedBindings = CaptureMaterialOutputBindings(ExpectedOutputs);
				ASSERT_TRUE(Draft->SetMaterialExpressions(std::array<DMaterialExpression*,2>{Scene.Get(),RGB.Get()},ExpectedOutputs));
			}
			else ASSERT_TRUE(Draft->SetMaterialExpressions({},ExpectedOutputs));
			ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
			EXPECT_FALSE(Session.HasUnappliedChanges()); EXPECT_EQ(Source->GetDomain(),Configuration.Domain); EXPECT_EQ(Source->GetStaticProperties(),Configuration.Properties);
			ASSERT_TRUE(Source->GetAcceptedCompiledProgram()); EXPECT_EQ(Source->GetAcceptedCompiledProgram()->GetShaderMap().Domain,Configuration.Domain);
			ASSERT_TRUE(SavePackage(Source->GetPackage())); Session.MarkSaved();
		}
		ASSERT_TRUE(UnloadPackage(Path)); Source=LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path)); ASSERT_TRUE(Source);
		EXPECT_EQ(Source->GetDomain(),Configuration.Domain); EXPECT_EQ(Source->GetStaticProperties(),Configuration.Properties); EXPECT_EQ(CaptureMaterialOutputBindings(Source->GetExpressionOutputs()), ExpectedBindings);
	}
	ASSERT_TRUE(UnloadPackage(Path)); CollectGarbage(EObjectFlags::Standalone);
}
