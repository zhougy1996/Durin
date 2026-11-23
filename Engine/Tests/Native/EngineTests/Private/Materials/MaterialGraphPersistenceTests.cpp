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
#include "Graph/MaterialEditingOperations.h"
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
	const auto Pin = FMaterialGraphPinAddress::MaterialOutput(EMaterialProperty::MP_Roughness);
	auto* Root = Graph->GetRootNode();
	ASSERT_TRUE(Root);
	ASSERT_TRUE(MaterialEditing::SetUseMaterialAttributes(*Material, true, ETransactionMode::NotTransactable));
	ASSERT_FALSE(Graph->FindInputPin(Pin));
	EXPECT_FALSE(Material->IsRootInputVisible(EMaterialProperty::MP_Roughness));
	const auto Hidden = Material->GetExpressionInputDescription(EMaterialProperty::MP_Roughness);
	ASSERT_TRUE(Hidden && Hidden->Numeric);
	EXPECT_EQ(Material->GetExpressionInputForProperty(EMaterialProperty::MP_Roughness), &Hidden->Numeric->Connection);
	EXPECT_TRUE(Material->IsRootInputVisible(EMaterialProperty::MP_MaterialAttributes));
	EXPECT_FALSE(Material->GetExpressionInputDescription(static_cast<EMaterialProperty>(254)));
	EXPECT_EQ(Material->GetNumericInputForProperty(EMaterialProperty::MP_MaterialAttributes), std::nullopt);
	EXPECT_EQ(Material->GetNumericInputForProperty(static_cast<EMaterialProperty>(254)), std::nullopt);
	Tests::FTestEditorTransactorOwner History;
	const FMaterialInputDefault Value{.Kind = EMaterialInputDefaultKind::Literal, .Literal = {.X = .625f}};
	ASSERT_TRUE(Root->PinDefaultValueChanged(Pin, Value));
	ASSERT_TRUE(Root->SetPinUseConstant(Pin, false));
	const auto Input = *Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness);
	EXPECT_FALSE(Input.UseConstant);
	EXPECT_TRUE(std::ranges::equal(Input.Constant, std::array{.625f}));
	ASSERT_TRUE(History->Undo());
	EXPECT_TRUE(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->UseConstant);
	ASSERT_TRUE(History->Redo());
	ASSERT_TRUE(MaterialEditing::SetUseMaterialAttributes(*Material, false, ETransactionMode::NotTransactable));
	const auto* Restored = Graph->FindInputPin(Pin);
	ASSERT_TRUE(Restored);
	EXPECT_FALSE(Restored->bUseConstant);
	EXPECT_EQ(Restored->RetainedConstant, Value);
}

TEST(FMaterialGraphPersistenceTests, DisconnectedNormalIgnoresRetainedConstantAcrossReload)
{
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	Testing::FScopedMountRegistryFixture Registry;
	Testing::RegisterMountPointForTests("/NormalRoot/", Testing::GetTestWorkDirectory().generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/NormalRoot/Material", Path));
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Property = EMaterialProperty::MP_Normal;
	const auto Pin = FMaterialGraphPinAddress::MaterialOutput(Property);
	const FMaterialProgramLiteral Flat{0.f, 0.f, 1.f};
	auto Normal = Testing::MakeGraphExpression<DMaterialExpressionVector3Constant>();
	Normal->Value = FVector3(0.f, 1.f, 0.f);
	Normal->bTangentSpaceNormal = true;
	FMaterialRootInputs Inputs;
	Inputs.Get(Property).SetConstant({1.f, 0.f, 0.f});
	Inputs.Get(Property).Connection = {Normal.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*, 1>{Normal.Get()}, Inputs));
	const auto CheckNormal = [&](bool Connected) {
		const auto Capture = SnapshotMaterialCompilerInput(*Material, {.CompilerIdentity = "NormalRoot"});
		ASSERT_TRUE(Capture);
		const auto& IR = Capture.Snapshot->Input.IR;
		const auto& Root = IR.GetAttribute(Property);
		ASSERT_EQ(Root.bExpression, Connected);
		if (Connected) EXPECT_EQ(IR.Nodes.at(Root.ExpressionIndex).GetLiteral(), (FMaterialProgramLiteral{0.f, 1.f, 0.f}));
		else EXPECT_EQ(Root.Literal, Flat);
	};
	{
		TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
		Tests::FTestEditorTransactorOwner History;
		CheckNormal(true);
		EXPECT_FALSE(Graph->GetRootNode()->InlineInputNode(Pin));
		ASSERT_TRUE(Graph->GetSchema().BreakPinLinks(*Graph, Pin));
		CheckNormal(false);
		ASSERT_TRUE(History->Undo());
		CheckNormal(true);
		ASSERT_TRUE(History->Redo());
		ASSERT_TRUE(SavePackage(Material->GetPackage()));
	}
	ASSERT_TRUE(UnloadPackage(Path));
	Material = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path));
	ASSERT_TRUE(Material);
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	CheckNormal(false);
	const auto Stored = *Material->GetNumericInputForProperty(Property);
	EXPECT_TRUE(Stored.UseConstant);
	EXPECT_TRUE(std::ranges::equal(Stored.Constant, std::array{1.f, 0.f, 0.f}));
	{
		TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
		const auto* View = Graph->FindInputPin(Pin);
		ASSERT_TRUE(View);
		EXPECT_FALSE(View->bSupportsConstant);
		EXPECT_EQ(View->InlineDefault.Literal, Flat);
		auto* Root = Graph->GetRootNode();
		EXPECT_FALSE(Root->SetPinUseConstant(Pin, false, ETransactionMode::NotTransactable));
		EXPECT_FALSE(Root->PinDefaultValueChanged(Pin, View->InlineDefault, ETransactionMode::NotTransactable));
		ASSERT_TRUE(Root->ExtractInputDefault(Pin, ETransactionMode::NotTransactable));
		const auto* Extracted = Cast<DMaterialExpressionVector3Constant>(Material->GetExpressionInputForProperty(Property)->Expression.Get());
		ASSERT_TRUE(Extracted);
		EXPECT_TRUE(Extracted->bTangentSpaceNormal);
		EXPECT_EQ(Extracted->Value, FVector3(0, 0, 1));
	}
	ASSERT_TRUE(Testing::EnsurePackageNotResident(Path, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FMaterialGraphPersistenceTests, MixedRootAndExpressionMovementReplaysWithPropertyEditsAndReload)
{
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	Testing::FScopedMountRegistryFixture Registry;
	Testing::RegisterMountPointForTests("/RootMove/", Testing::GetTestWorkDirectory().generic_string() + "/");
	for (bool MoveFirst : {false, true})
	{
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(MoveFirst ? "/RootMove/LayoutFirst" : "/RootMove/PropertyFirst", Path));
		DMaterial* Material = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Material));
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		const auto Key = FMaterialGraphPinAddress::MaterialOutput(EMaterialProperty::MP_Roughness);
		FGuid Scalar;
		{
			TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
			auto Created = Testing::CreateGraphConstant(*Graph, .75f, 40, 60);
			ASSERT_TRUE(Created);
			Scalar = *Created;
			const auto Initial = Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Capture();
			const auto InitialPosition = Material->GetRootPosition();
			ASSERT_TRUE(Graph->GetSchema().TryCreateConnection(*Graph, Key, FMaterialGraphPinAddress::Output({Scalar}), true, ETransactionMode::NotTransactable));
			EXPECT_EQ(Graph->FindInputPin(Key)->Link.SourceNodeId, Scalar);
			ASSERT_TRUE(Graph->GetSchema().BreakPinLinks(*Graph, Key, ETransactionMode::NotTransactable));
			EXPECT_EQ(*Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness), Initial);
			ASSERT_TRUE(SavePackage(Material->GetPackage()));
			Tests::FTestEditorTransactorOwner History;
			const std::array Moves{FMaterialGraphNodePosition{EMaterialGraphEndpoint::MaterialRoot, 600, -80},
				FMaterialGraphNodePosition{Scalar, 300, 160}};
			const auto Move = [&] {
				const auto Revision = Material->GetMaterialCompileDebugInfo().AuthoredRevision;
				std::vector<FMaterialGraphChangeSet> Changes;
				const auto Handle = Material->GetGraphChanges().Subscribe(*Material, [&](const auto& Change) { Changes.push_back(Change); });
				const bool Moved = Graph->MoveSelection(Moves);
				Material->GetGraphChanges().Unsubscribe(Handle);
				ASSERT_TRUE(Moved);
				ASSERT_EQ(Changes.size(), 1u);
				EXPECT_TRUE(Changes.front().Has(EMaterialGraphChange::RootPosition));
				ASSERT_EQ(Changes.front().Nodes.size(), 1u);
				EXPECT_EQ(Changes.front().Nodes.front().NodeId, Scalar);
				EXPECT_EQ(Material->GetMaterialCompileDebugInfo().AuthoredRevision, Revision);
			};
			{
				FScopedTransaction Transaction("Edit root and move selection");
				if (MoveFirst) Move();
				ASSERT_TRUE(Graph->GetRootNode()->PinDefaultValueChanged(Key,
					{.Kind = EMaterialInputDefaultKind::Literal, .Literal = {.X = .375f}}));
				ASSERT_TRUE(Graph->GetSchema().TryCreateConnection(*Graph, Key, FMaterialGraphPinAddress::Output({Scalar})));
				if (!MoveFirst) Move();
			}
			ASSERT_EQ(History->GetQueueLength(), 1u);
			ASSERT_TRUE(History->Undo());
			EXPECT_EQ(Material->GetRootPosition(), InitialPosition);
			EXPECT_EQ(Graph->FindNode(Scalar)->GetView().Presentation.X, 40);
			EXPECT_EQ(*Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness), Initial);
			EXPECT_FALSE(Material->GetPackage()->IsDirty());
			ASSERT_TRUE(History->Redo());
			ASSERT_TRUE(Graph->FindInputPin(Key));
			EXPECT_EQ(Graph->FindInputPin(Key)->Link.SourceNodeId, Scalar);
			ASSERT_TRUE(Graph->GetSchema().BreakPinLinks(*Graph, Key));
			EXPECT_TRUE(Graph->FindInputPin(Key)->bUseConstant);
			EXPECT_FLOAT_EQ(Graph->FindInputPin(Key)->InlineDefault.Literal.X, .375f);
			ASSERT_TRUE(History->Undo());
			ASSERT_TRUE(SavePackage(Material->GetPackage()));
		}
		ASSERT_TRUE(UnloadPackage(Path));
		Material = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path));
		ASSERT_TRUE(Material);
		{
			TStrongObjectPtr<DMaterialGraph> Reopened(DMaterialGraph::Create(*Material));
			ASSERT_TRUE(Reopened->GetRootNode());
			EXPECT_EQ(Material->GetRootPosition(), (std::pair<int32, int32>{600, -80}));
			EXPECT_EQ(Reopened->FindNode(Scalar)->GetView().Presentation.Y, 160);
			const auto* Pin = Reopened->FindInputPin(Key);
			ASSERT_TRUE(Pin);
			EXPECT_EQ(Pin->Link.SourceNodeId, Scalar);
			EXPECT_TRUE(Pin->bUseConstant);
			EXPECT_FLOAT_EQ(Pin->RetainedConstant.Literal.X, .375f);
			EXPECT_EQ(Material->GetMaterialGraphPresentation().Nodes.size(), 1u);
		}
		ASSERT_TRUE(UnloadPackage(Path));
	}
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
	TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Material));
	const auto Scalar = Testing::CreateGraphConstant(*AuthoringGraph, 0.25f);
	ASSERT_TRUE(Scalar);
	const FGuid Id = Scalar.value();
	const auto Input = FMaterialGraphPinAddress::MaterialOutput(EMaterialProperty::MP_Roughness);
	const auto Source = FMaterialGraphPinAddress::Output({Id});
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	{
		TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
		Tests::FTestEditorTransactorOwner History;
		const auto Revision = [&] { return Material->GetMaterialCompileDebugInfo().AuthoredRevision; };
		const auto Literal = [&] { return Graph->FindNode(Id)->GetView().Node.GetConstantLiteral().X; };
		const auto Connected = [&] { return Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Connection.GetExpressionId() == Id; };
		const auto CleanRevision = Material->GetPackage()->GetEditRevision();
		const auto InitialCompile = Revision();
		EXPECT_FALSE(Graph->FindNode(Id)->SetConstantValue(FMaterialParameterValue::MakeScalar(std::numeric_limits<float>::infinity())));
		EXPECT_EQ(Graph->FindNode(Id)->SetConstantValue(FMaterialParameterValue::MakeScalar(.25f)).GetStatus(), EMaterialGraphCommandStatus::NoChange);
		EXPECT_FALSE(Graph->GetSchema().TryCreateConnection(*Graph, Input, FMaterialGraphPinAddress::Output({FGuid::NewGuid()})));
		auto* Root = Graph->GetRootNode();
		ASSERT_TRUE(Root);
		EXPECT_FALSE(Root->PinDefaultValueChanged(Input, {.Kind = EMaterialInputDefaultKind::Literal,
			.Type = EMaterialProgramValueType::Float3, .Literal = {1.f, 2.f, 3.f}}));
		EXPECT_EQ(Material->GetPackage()->GetEditRevision(), CleanRevision);
		EXPECT_FALSE(Material->GetPackage()->IsDirty());
		EXPECT_EQ(Revision(), InitialCompile);
		EXPECT_EQ(History->GetQueueLength(), 0u);

		const FMaterialInputDefault Default{.Kind = EMaterialInputDefaultKind::Literal,
			.Type = EMaterialProgramValueType::Float, .Literal = {.X = .375f}};
		const auto Original = Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Capture();
		ASSERT_TRUE(Root->PinDefaultValueChanged(Input, Default));
		EXPECT_EQ(Root->PinDefaultValueChanged(Input, Default).GetStatus(), EMaterialGraphCommandStatus::NoChange);
		EXPECT_EQ(History->GetQueueLength(), 1u);
		EXPECT_GT(Revision(), InitialCompile);
		ASSERT_TRUE(History->Undo());
		EXPECT_EQ(*Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness), Original);
		EXPECT_FALSE(Material->GetPackage()->IsDirty());
		ASSERT_TRUE(History->Redo());
		EXPECT_TRUE(std::ranges::equal(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Constant, std::array{.375f}));
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
		TStrongObjectPtr<DMaterialGraph> AuthoringGraph(DMaterialGraph::Create(*Asset));
		const auto Scalar = Testing::CreateGraphConstant(*AuthoringGraph, 0.25f);
		const auto Saturate = Testing::CreateGraphCatalogNode(*AuthoringGraph, EMaterialProgramOpcode::Saturate);
		ASSERT_TRUE(Scalar);
		ASSERT_TRUE(Saturate);
		const FMaterialProgramLink Link{Scalar.value()};
		const auto Other = Testing::CreateGraphConstant(*AuthoringGraph, 0.75f);
		ASSERT_TRUE(Other);
		const auto Replacement = FMaterialGraphPinAddress::Output({Other.value()});
		const auto Input = FMaterialGraphPinAddress::Input(Saturate->front(), 0);
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
			EXPECT_FALSE(Schema.CanCreateConnection(OtherGraph->FindPin(Output), Graph->FindPin(Input)).Reason.empty());
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
			std::string Error = "previous error";
			EXPECT_FALSE(Schema.TryCreateConnection(Graph->FindPin(Output), Graph->FindPin(Input), false, ETransactionMode::Record, &Error));
			EXPECT_TRUE(Error.empty());
			EXPECT_EQ(History->GetQueueLength(), Before + 1);
			EXPECT_EQ(Asset->GetPackage()->GetEditRevision(), Revision);
			const auto Loop = FMaterialGraphPinAddress::Output({Saturate->front()});
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
			const auto DisconnectedRevision = Asset->GetPackage()->GetEditRevision();
			EXPECT_FALSE(Schema.BreakPinLinks(nullptr, ETransactionMode::Record, &Error));
			EXPECT_FALSE(Error.empty());
			EXPECT_FALSE(Schema.BreakPinLinks(Graph->FindPin(Replacement), ETransactionMode::Record, &Error));
			EXPECT_TRUE(Error.empty());
			EXPECT_FALSE(Schema.BreakPinLinks(Graph->FindPin(Input), ETransactionMode::Record, &Error));
			EXPECT_TRUE(Error.empty());
			EXPECT_EQ(Asset->GetPackage()->GetEditRevision(), DisconnectedRevision);
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
		EXPECT_TRUE(Material->GetExpressionCollection().Expressions.empty());
		EXPECT_EQ(Material->GetRootPosition(), (std::pair<int32, int32>{1600, 400}));
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
	TStrongObjectPtr<DMaterialGraph> RootAuthoringGraph(DMaterialGraph::Create(*Material));
	TStrongObjectPtr<DMaterialGraph> BodyAuthoringGraph(DMaterialGraph::Create(*Function));
	const auto Call = RootAuthoringGraph->InsertFunctionCall(*Function, 0, 0, ETransactionMode::NotTransactable).transform(Testing::GraphNodeId);
	ASSERT_TRUE(Call);
	FMaterialFunctionPort Port;
	Port.Id = FGuid::NewGuid();
	Port.Name = "Weight";
	Port.Type = EMaterialProgramValueType::Float;
	const auto Constant = Testing::CreateGraphConstant(*BodyAuthoringGraph, .25f);
	ASSERT_TRUE(Constant);
	ASSERT_TRUE(BodyAuthoringGraph->AddPort(true, Port, {Constant.value()}, 0, 0, ETransactionMode::NotTransactable));
	const auto Check = [&](DMaterial& Owner) {
		const auto& Input = Owner.GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Connection;
		auto* Upstream = Cast<DMaterialExpressionFunctionCall>(Input.Expression.Get());
		ASSERT_TRUE(Upstream);
		EXPECT_EQ(Upstream->GetOuter(), &Owner);
		EXPECT_EQ(Upstream->Id, Call.value());
		ASSERT_EQ(Upstream->FunctionOutputs.size(), 2u);
		EXPECT_EQ(Input.OutputIndex, 1);
		EXPECT_EQ(Upstream->FunctionOutputs[1].OutputId, Port.Id);
		TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(Owner));
		const auto* RootNode = Graph->GetRootNode();
		ASSERT_TRUE(RootNode);
		const auto* RootInput = Graph->FindPin(FMaterialGraphPinAddress::MaterialOutput(EMaterialProperty::MP_Roughness));
		ASSERT_TRUE(RootInput);
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
		const auto Pin = FMaterialGraphPinAddress::MaterialOutput(EMaterialProperty::MP_Roughness);
		const auto Source = FMaterialGraphPinAddress::Output({Call.value(), 0, Port.Id});
		ASSERT_TRUE(Graph->GetSchema().TryCreateConnection(Graph->FindPin(Source), Graph->FindPin(Pin)));
		Check(*Material);
		ASSERT_TRUE(History->Undo());
		EXPECT_FALSE(Material->GetNumericInputForProperty(EMaterialProperty::MP_Roughness)->Connection.IsConnected());
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

TEST(FMaterialGraphPersistenceTests, LegacyOwnerLayoutsMigrateCoordinatesAndLabels)
{
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	Testing::FScopedMountRegistryFixture Registry;
	Testing::RegisterMountPointForTests("/LegacyLayout/", Testing::GetTestWorkDirectory().generic_string() + "/");
	const auto Check = []<class T>(std::string_view PathName, const char* Field, const char* Type, uint32 Version) {
		using namespace ObjectPackage;
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(PathName, Path));
		T* Asset = nullptr;
		ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Asset));
		FGuid Id;
		{
			TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Asset));
			const auto Created = Testing::CreateGraphConstant(*Graph, .5f);
			ASSERT_TRUE(Created);
			Id = *Created;
		}
		ASSERT_TRUE(SavePackage(Asset->GetPackage()));
		const auto File = FindPackageExact(Path);
		ASSERT_TRUE(File);
		auto Bytes = FFileHelper::LoadFileToArray(File->PhysicalPath);
		ASSERT_TRUE(Bytes);
		FLinkerTables Tables;
		ASSERT_TRUE(ReadPackage(*Bytes, {}, Path, Tables));
		ASSERT_TRUE(UnloadPackage(Path));

		const FSerializedType Int{.Kind = EValueKind::I32};
		const FSerializedType UInt{.Kind = EValueKind::U32};
		const FSerializedType Guid{.Kind = EValueKind::Guid};
		const FSerializedType String{.Kind = EValueKind::String};
		const FSerializedType Node{.Kind = EValueKind::Struct, .QualifiedName = "Durin::FMaterialGraphNodePresentation",
			.Children = {String, Guid, Int, Int}};
		const FSerializedType Nodes{.Kind = EValueKind::Array, .Children = {Node}};
		const FSerializedType Layout{.Kind = EValueKind::Struct, .QualifiedName = Type, .Children = {Nodes, UInt}};
		Tables.Schemas.push_back({Node.QualifiedName, {{"NodeId", Guid}, {"X", Int}, {"Y", Int}, {"DisplayName", String}}});
		Tables.Schemas.push_back({Layout.QualifiedName, {{"SchemaVersion", UInt}, {"Nodes", Nodes}}});
		const auto Owner = T::StaticClass()->GetQualifiedName().ToString();
		const auto OwnerSchema = std::ranges::find(Tables.Schemas, Owner, &FSerializedSchema::QualifiedName);
		ASSERT_NE(OwnerSchema, Tables.Schemas.end());
		OwnerSchema->Fields.push_back({Field, Layout});
		for (auto& Export : Tables.Exports)
			std::erase_if(Export.Properties, [](const auto& Property) { return Property.FieldName == "EditorLayout"; });
		const auto AssetExport = std::ranges::find_if(Tables.Exports, [&](const auto& Export) {
			return std::ranges::any_of(Export.Properties, [&](const auto& Property) { return Property.DeclaringType == Owner; });
		});
		ASSERT_NE(AssetExport, Tables.Exports.end());
		FSerializedValue LegacyNode{.Elements = {{.Guid = Id}, {.Signed = 137}, {.Signed = -246}, {.Text = "Legacy node label"}},
			.FieldTypes = std::vector{Guid, Int, Int, String}, .FieldNames = {"NodeId", "X", "Y", "DisplayName"}};
		AssetExport->Properties.push_back({.DeclaringType = Owner, .FieldName = Field, .Type = Layout,
			.Value = {.Elements = {{.Unsigned = Version}, {.Elements = {std::move(LegacyNode)}}},
				.FieldTypes = std::vector{UInt, Nodes}, .FieldNames = {"SchemaVersion", "Nodes"}}});
		FByteBuffer Bulk;
		const auto Written = WritePackage(Tables, *Bytes, Bulk);
		ASSERT_TRUE(Written) << FormatPackageError(Written);
		ASSERT_TRUE(Bulk.empty());
		ASSERT_TRUE(FFileHelper::SaveArrayToFileAtomically(*Bytes, File->PhysicalPath));

		const FMaterialExpressionLayout Expected{true, 137, -246, "Legacy node label"};
		for (int Load = 0; Load < 2; ++Load)
		{
			Asset = LoadObject<T>(Testing::MakePackageLeafAssetObjectPathForTests(Path));
			ASSERT_TRUE(Asset);
			const auto* Expression = FindMaterialExpression(Asset->GetExpressionCollection().Expressions, Id);
			ASSERT_TRUE(Expression);
			EXPECT_EQ(Expression->EditorLayout, Expected);
			if (Load == 0) ASSERT_TRUE(SavePackage(Asset->GetPackage()));
			ASSERT_TRUE(UnloadPackage(Path));
		}
	};
	Check.operator()<DMaterial>("/LegacyLayout/Material", "GraphPresentation", "Durin::FMaterialGraphPresentation",
		CurrentMaterialGraphPresentationSchemaVersion);
	Check.operator()<DMaterialFunction>("/LegacyLayout/Function", "Presentation", "Durin::FMaterialFunctionPresentation",
		CurrentMaterialFunctionPresentationSchemaVersion);
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
		TStrongObjectPtr<T> KeepSource(Source);
		const auto Before = CaptureExpressions(*Source);
		const auto ReadLayout = [](const T& Asset) {
			if constexpr (std::is_same_v<T, DMaterial>) return Asset.GetMaterialGraphPresentation().Nodes;
			else return Asset.GetFunctionPresentation().Nodes;
		};
		const auto BeforeLayout = ReadLayout(*Source);
		const auto CheckVersion = [](const FPackagePath& PackagePath, bool bResaved) {
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
			const auto Name = std::is_same_v<T, DMaterial> ? "GraphPresentation" : "Presentation";
			const auto Owner = std::is_same_v<T, DMaterial> ? "Durin::DMaterial" : "Durin::DMaterialFunction";
			const auto Schema = std::ranges::find(Tables.Schemas, Owner, &ObjectPackage::FSerializedSchema::QualifiedName);
			ASSERT_NE(Schema, Tables.Schemas.end());
			EXPECT_EQ(std::ranges::contains(Schema->Fields, Name, &ObjectPackage::FSerializedField::Name), !bResaved);
		};
		CheckVersion(SourcePath, false);
		if (std::string_view(Name) == "MetalCopy")
		{
			ASSERT_EQ(BeforeLayout.size(), 4u);
			for (size_t Index = 0; Index < BeforeLayout.size(); ++Index)
			{
				EXPECT_EQ(BeforeLayout[Index].X, 0);
				EXPECT_EQ(BeforeLayout[Index].Y, static_cast<int32>(Index) * 240);
			}
		}
		if constexpr (std::is_same_v<T, DMaterialFunction>)
			EXPECT_TRUE(std::ranges::any_of(Before.Sources, [](const FGuid& Id) { return Id.IsValid(); }));
		auto* Package = CreatePackage(Path);
		ASSERT_TRUE(Package);
		const auto Copy = DuplicateObject(Source, Package, FName(Name));
		ASSERT_TRUE(Copy);
		auto* Asset = *Copy;
		EXPECT_EQ(CaptureExpressions(*Asset), Before);
		EXPECT_EQ(ReadLayout(*Asset), BeforeLayout);
		const auto CheckMaterial = [&](const DMaterial& Candidate) {
			if constexpr (std::is_same_v<T, DMaterial>)
			{
				const auto Root = Candidate.CaptureRootInputs();
				EXPECT_EQ(CaptureMaterialOutputBindings(Root), CaptureMaterialOutputBindings(*Source));
				const auto CheckOwner = [&](const FMaterialExpressionInput& Input) {
					if (Input.IsConnected()) EXPECT_EQ(Input.Expression->GetOuter(), &Candidate);
				};
				CheckOwner(Root.Attributes);
				for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions()) CheckOwner(Root.Get(Field.Property).Connection);
			}
		};
		std::optional<FMaterialGraphNodePresentation> MovedNode;
		if (!Asset->GetExpressionCollection().Expressions.empty())
		{
			TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Asset));
			ASSERT_FALSE(Graph->GetNodes().empty());
			MovedNode = Graph->FindNode(Asset->GetExpressionCollection().Expressions.front()->Id)->GetView().Presentation;
			MovedNode->X += 24;
			ASSERT_TRUE(Graph->MoveNodes(std::span(&*MovedNode, 1), ETransactionMode::NotTransactable));
			auto Layout = ReadLayout(*Asset);
			const auto Position = std::ranges::find(Layout, MovedNode->NodeId, &FMaterialGraphNodePresentation::NodeId);
			ASSERT_NE(Position, Layout.end());
			Position->DisplayName = "Saved node label";
			if constexpr (std::is_same_v<T, DMaterial>)
				ASSERT_EQ(Asset->SetMaterialGraphPresentation({.Nodes = std::move(Layout)}), EMaterialGraphPresentationResult::Changed);
			else
				ASSERT_TRUE(Asset->SetFunctionPresentation({.Nodes = std::move(Layout)}));
		}
		if constexpr (std::is_same_v<T, DMaterial>)
		{
			CheckMaterial(*Asset);
			const auto [X, Y] = Source->GetRootPosition();
			EXPECT_EQ(Asset->GetRootPosition(), Source->GetRootPosition());
			Asset->SetRootPosition(X + 24, Y);
			ASSERT_TRUE(Asset->CompileEdits());
		}
		const auto SavedLayout = ReadLayout(*Asset);
		ASSERT_TRUE(SavePackage(Package));
		ASSERT_TRUE(UnloadPackage(Path));
		auto* Reopened = LoadObject<T>(Testing::MakePackageLeafAssetObjectPathForTests(Path));
		ASSERT_TRUE(Reopened);
		CheckVersion(Path, true);
		EXPECT_EQ(CaptureExpressions(*Reopened), Before);
		EXPECT_EQ(ReadLayout(*Reopened), SavedLayout);
		if (MovedNode)
		{
			TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Reopened));
			ASSERT_TRUE(Graph->FindNode(MovedNode->NodeId));
			EXPECT_EQ(Graph->FindNode(MovedNode->NodeId)->GetView().Presentation.X, MovedNode->X);
		}
		if constexpr (std::is_same_v<T, DMaterial>)
		{
			CheckMaterial(*Reopened);
			EXPECT_EQ(Reopened->GetRootPosition().first, Source->GetRootPosition().first + 24);
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

TEST(FMaterialGraphPersistenceTests, PackedFunctionAndRetainedIndividualInputsRoundTripThroughInstance)
{
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	Testing::FScopedMountRegistryFixture Registry;
	Testing::RegisterMountPointForTests("/RootBaseline/", Testing::GetTestWorkDirectory().generic_string() + "/");
	FPackagePath FunctionPath, MaterialPath, InstancePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/RootBaseline/AttributeFunction", FunctionPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/RootBaseline/Surface", MaterialPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/RootBaseline/Instance", InstancePath));
	DMaterialFunction* Function = nullptr;
	DMaterial* Material = nullptr;
	DMaterialInstance* Instance = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(FunctionPath, Function));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(MaterialPath, Material));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(InstancePath, Instance));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	auto Weight = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>({71, 1, 1, 1});
	Weight->Value = .625f;
	auto Make = Testing::MakeGraphExpression<DMaterialExpressionMakeMaterialAttributes>({71, 1, 1, 2});
	Make->Metallic = {Weight.Get()};
	auto RoughnessValue = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>({71, 1, 1, 4});
	RoughnessValue->Value = .25f;
	Make->Roughness = {RoughnessValue.Get()};
	auto Output = Testing::MakeGraphExpression<DMaterialExpressionFunctionOutput>({71, 1, 1, 3});
	Output->Port = Function->GetFunctionSignature().Outputs.front();
	Output->Port.Id = {71, 2, 1, 1};
	Output->Port.Name = "SurfaceAttributes";
	Output->Source = {Make.Get()};
	const std::array<DMaterialExpression*, 4> Body{Weight.Get(), Make.Get(), Output.Get(), RoughnessValue.Get()};
	ASSERT_TRUE(Function->SetFunctionExpressions(Body));
	auto Call = Testing::MakeGraphExpression<DMaterialExpressionFunctionCall>({71, 3, 1, 1});
	ASSERT_TRUE(Call->SetMaterialFunction(Function));
	auto Individual = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>({71, 3, 1, 2});
	Individual->Value = .75f;
	FMaterialRootInputs Root;
	Root.Attributes = {Call.Get(), 0, Output->Port.Id};
	Root.Get(EMaterialProperty::MP_Roughness).SetConstant({.375f});
	Root.Get(EMaterialProperty::MP_Roughness).Connection = {Individual.Get()};
	const std::array<DMaterialExpression*, 2> Nodes{Call.Get(), Individual.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(Nodes, Root));
	Material->SetRootPosition(520, -80);
	ASSERT_TRUE(Instance->SetParent(Material));
	ASSERT_TRUE(SavePackage(Function->GetPackage()));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	ASSERT_TRUE(SavePackage(Instance->GetPackage()));
	Call->Function = nullptr; // Release the detached draft's reference to the saved function.
	ASSERT_TRUE(UnloadPackage(InstancePath));
	ASSERT_TRUE(Testing::EnsurePackageNotResident(MaterialPath));
	ASSERT_TRUE(Testing::EnsurePackageNotResident(FunctionPath));

	Instance = LoadObject<DMaterialInstance>(Testing::MakePackageLeafAssetObjectPathForTests(InstancePath));
	ASSERT_TRUE(Instance);
	Material = Cast<DMaterial>(Instance->GetParent());
	ASSERT_TRUE(Material);
	EXPECT_EQ(Material->GetObjectPath(), "/RootBaseline/Surface.Surface");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto& Stored = Material->CaptureRootInputs();
	EXPECT_FALSE(Stored.bUseMaterialAttributes);
	EXPECT_EQ(Stored.Attributes.GetExpressionId(), Call->Id);
	EXPECT_EQ(Stored.Attributes.GetOutputId(), Output->Port.Id);
	const auto& Roughness = Stored.Get(EMaterialProperty::MP_Roughness);
	EXPECT_EQ(Roughness.Connection.GetExpressionId(), Individual->Id);
	EXPECT_TRUE(Roughness.UseConstant);
	EXPECT_EQ(Roughness.Constant, std::vector<float>{.375f});
	EXPECT_EQ(Material->GetRootPosition(), (std::pair<int32, int32>{520, -80}));
	for (bool Packed : {false, true})
	{
		if (Packed) ASSERT_TRUE(MaterialEditing::SetUseMaterialAttributes(*Material, true, ETransactionMode::NotTransactable));
		const auto Snapshot = SnapshotMaterialCompilerInput(*Instance, {.CompilerIdentity = "RootInputBaseline"});
		ASSERT_TRUE(Snapshot) << FormatMaterialProgramDiagnostics(Snapshot.Diagnostics);
		const auto Normalized = MIR::Normalize(Snapshot.Snapshot->Input);
		ASSERT_TRUE(Normalized) << FormatMaterialProgramDiagnostics(Normalized.Diagnostics);
		const auto Generated = GenerateMaterialProgramSlang(Normalized.IR, Normalized.Layout);
		ASSERT_TRUE(Generated);
		const auto& IR = Normalized.IR;
		const auto Literal = [&](EMaterialProperty Property) {
			const auto& Attribute = IR.GetAttribute(Property);
			return Attribute.bExpression ? IR.Nodes.at(Attribute.ExpressionIndex).GetLiteral().X : Attribute.Literal.X;
		};
		EXPECT_FLOAT_EQ(Literal(EMaterialProperty::MP_Roughness), Packed ? .25f : .75f);
		if (Packed) EXPECT_FLOAT_EQ(Literal(EMaterialProperty::MP_Metallic), .625f);
	}
	ASSERT_TRUE(UnloadPackage(InstancePath));
	ASSERT_TRUE(Testing::EnsurePackageNotResident(MaterialPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Testing::EnsurePackageNotResident(FunctionPath));
}

TEST(FMaterialGraphPersistenceTests, PostProcessCompilesStoredRootModeAcrossDomainChangesAndReload)
{
	InitializeDObjectSystem();
	FScopedOfflinePreparation Offline;
	Testing::FScopedMountRegistryFixture Registry;
	Testing::RegisterMountPointForTests("/DomainRoots/", Testing::GetTestWorkDirectory().generic_string() + "/");
	FPackagePath FunctionPath, MaterialPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/DomainRoots/Weight", FunctionPath));
	ASSERT_TRUE(FPackagePath::TryCreate("/DomainRoots/Material", MaterialPath));
	DMaterialFunction* Function = nullptr;
	DMaterial* Material = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(FunctionPath, Function));
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(MaterialPath, Material));
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);

	auto Weight = Testing::MakeGraphExpression<DMaterialExpressionScalarConstant>();
	Weight->Value = .625f;
	auto Output = Testing::MakeGraphExpression<DMaterialExpressionFunctionOutput>();
	Output->Port = Function->GetFunctionSignature().Outputs.front();
	Output->Port.Type = EMaterialProgramValueType::Float;
	Output->Source = {Weight.Get()};
	ASSERT_TRUE(Function->SetFunctionExpressions(std::array<DMaterialExpression*, 2>{Weight.Get(), Output.Get()}));
	auto Call = Testing::MakeGraphExpression<DMaterialExpressionFunctionCall>();
	ASSERT_TRUE(Call->SetMaterialFunction(Function));
	const auto Roughness = EMaterialProperty::MP_Roughness;
	const auto Emissive = EMaterialProperty::MP_EmissiveColor;
	FMaterialRootInputs Roots;
	Roots.Get(Emissive).SetConstant({.125f, .25f, .5f});
	Roots.Get(Roughness).SetConstant({.375f});
	Roots.Get(Roughness).Connection = {Call.Get(), 0, Output->Port.Id};
	auto Make = Testing::MakeGraphExpression<DMaterialExpressionMakeMaterialAttributes>();
	Make->Roughness = Roots.Get(Roughness).Connection;
	auto Glow = Testing::MakeGraphExpression<DMaterialExpressionVector3Constant>();
	Glow->Value = FVector3(.75f, .5f, .25f);
	Make->EmissiveColor = {Glow.Get()};
	Roots.Attributes = {Make.Get()};
	const std::array<DMaterialExpression*, 3> Expressions{Call.Get(), Make.Get(), Glow.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Roots));
	const FMaterialCompilerEnvironment Environment{.CompilerIdentity = "DomainRoots"};
	const auto Surface = SnapshotMaterialCompilerInput(*Material, Environment);
	ASSERT_TRUE(Surface);
	ASSERT_EQ(Surface.Snapshot->FunctionOwners.size(), 1u);
	const auto Original = MIR::Normalize(Surface.Snapshot->Input);
	ASSERT_TRUE(Original) << FormatMaterialProgramDiagnostics(Original.Diagnostics);

	ASSERT_TRUE(MaterialEditing::SetUseMaterialAttributes(*Material, true, ETransactionMode::NotTransactable));
	FMaterialConfiguration Effect{.Domain = EMaterialDomain::PostProcess};
	Effect.Properties.ShadingModel = EMaterialShadingModel::Unlit;
	ASSERT_TRUE(Material->SetMaterialConfiguration(Effect));
	auto Invalid = Roots;
	Invalid.Get(Roughness).Connection.OutputIndex = 1;
	EXPECT_FALSE(Material->SetMaterialExpressions(Expressions, Invalid));
	ASSERT_TRUE(SavePackage(Function->GetPackage()));
	ASSERT_TRUE(SavePackage(Material->GetPackage()));
	Call->Function = nullptr;
	ASSERT_TRUE(UnloadPackage(MaterialPath));
	ASSERT_TRUE(Testing::EnsurePackageNotResident(FunctionPath));
	Material = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(MaterialPath));
	ASSERT_TRUE(Material);
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_EQ(Material->GetDomain(), EMaterialDomain::PostProcess);
	ASSERT_TRUE(Material->UsesMaterialAttributes());
	EXPECT_FALSE(Material->CanEditMaterialAttributesMode());
	EXPECT_FALSE(MaterialEditing::SetUseMaterialAttributes(*Material, false, ETransactionMode::NotTransactable));
	const auto Hidden = *Material->GetNumericInputForProperty(Roughness);
	ASSERT_EQ(Hidden.Connection.GetExpressionId(), Call->Id);
	ASSERT_EQ(Hidden.Connection.GetOutputId(), Output->Port.Id);
	EXPECT_TRUE(std::ranges::equal(Hidden.Constant, std::array{.375f}));

	TStrongObjectPtr<DMaterial> Reference(NewObject<DMaterial>(nullptr, NAME_None));
	Reference->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(Reference->SetMaterialConfiguration(Effect));
	FMaterialRootInputs ReferenceInputs;
	ReferenceInputs.Get(Emissive) = Roots.Get(Emissive);
	ASSERT_TRUE(Reference->SetMaterialExpressions({}, ReferenceInputs));
	const auto Clean = SnapshotMaterialCompilerInput(*Reference, Environment);
	ASSERT_TRUE(Clean);
	const auto Expected = MIR::Normalize(Clean.Snapshot->Input);
	ASSERT_TRUE(Expected);
	auto* LoadedCall = Cast<DMaterialExpressionFunctionCall>(Hidden.Connection.Expression.Get());
	ASSERT_TRUE(LoadedCall);
	TStrongObjectPtr<DMaterialFunctionInterface> SavedFunction(LoadedCall->Function.Get());
	ASSERT_TRUE(SavedFunction);
	const auto PackedPin = FMaterialGraphPinAddress::MaterialOutput(EMaterialProperty::MP_MaterialAttributes);
	const auto ColorPin = FMaterialGraphPinAddress::MaterialOutput(Emissive);
	{
		TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
		EXPECT_TRUE(Graph->FindInputPin(PackedPin));
		EXPECT_FALSE(Graph->FindInputPin(ColorPin));
		const auto Packed = SnapshotMaterialCompilerInput(*Material, Environment);
		ASSERT_TRUE(Packed);
		EXPECT_EQ(Packed.Snapshot->FunctionOwners.size(), 1u);
		const auto& IR = Packed.Snapshot->Input.IR;
		const auto& Color = IR.GetAttribute(Emissive);
		ASSERT_TRUE(Color.bExpression);
		EXPECT_EQ(IR.Nodes.at(Color.ExpressionIndex).GetLiteral(), (FMaterialProgramLiteral{.75f, .5f, .25f}));
		Tests::FTestEditorTransactorOwner History;
		ASSERT_TRUE(Graph->GetSchema().BreakPinLinks(*Graph, PackedPin));
		const auto Open = SnapshotMaterialCompilerInput(*Material, Environment);
		ASSERT_TRUE(Open);
		EXPECT_TRUE(Open.Snapshot->FunctionOwners.empty());
		const auto& Default = Open.Snapshot->Input.IR.GetAttribute(Emissive);
		EXPECT_FALSE(Default.bExpression);
		EXPECT_EQ(Default.Literal, FMaterialProgramLiteral{});
		ASSERT_TRUE(History->Undo());
		EXPECT_TRUE(Material->GetExpressionInputForProperty(PackedPin.Property)->IsConnected());
	}
	ASSERT_TRUE(Material->SetMaterialConfiguration({}));
	ASSERT_TRUE(MaterialEditing::SetUseMaterialAttributes(*Material, false, ETransactionMode::NotTransactable));
	ASSERT_TRUE(Material->SetMaterialConfiguration(Effect));
	EXPECT_TRUE(Material->IsRootInputVisible(Emissive));
	EXPECT_FALSE(Material->IsRootInputVisible(PackedPin.Property));
	LoadedCall = Cast<DMaterialExpressionFunctionCall>(Material->GetExpressionInputForProperty(Roughness)->Expression.Get());
	ASSERT_TRUE(LoadedCall);
	LoadedCall->Function = nullptr;
	const auto Individual = SnapshotMaterialCompilerInput(*Material, Environment);
	ASSERT_TRUE(Individual);
	EXPECT_TRUE(Individual.Snapshot->FunctionOwners.empty());
	EXPECT_TRUE(Individual.Snapshot->Input.Sources.empty());
	const auto IndividualCompiled = MIR::Normalize(Individual.Snapshot->Input);
	ASSERT_TRUE(IndividualCompiled);
	EXPECT_EQ(IndividualCompiled.Identity, Expected.Identity);
	LoadedCall->Function = SavedFunction.Get();
	ASSERT_TRUE(Material->SetMaterialConfiguration({}));
	ASSERT_TRUE(MaterialEditing::SetUseMaterialAttributes(*Material, true, ETransactionMode::NotTransactable));
	const auto PackedSurface = SnapshotMaterialCompilerInput(*Material, Environment);
	ASSERT_TRUE(PackedSurface);
	EXPECT_EQ(PackedSurface.Snapshot->FunctionOwners.size(), 1u);
	const auto& PackedIR = PackedSurface.Snapshot->Input.IR;
	const auto& Normal = PackedIR.GetAttribute(EMaterialProperty::MP_Normal);
	ASSERT_TRUE(Normal.bExpression);
	EXPECT_EQ(PackedIR.Nodes[Normal.ExpressionIndex].GetLiteral().Z, 1.f);
	ASSERT_TRUE(MaterialEditing::SetUseMaterialAttributes(*Material, false, ETransactionMode::NotTransactable));
	LoadedCall = Cast<DMaterialExpressionFunctionCall>(Material->GetExpressionInputForProperty(Roughness)->Expression.Get());
	ASSERT_TRUE(LoadedCall);
	LoadedCall->Function = nullptr;
	EXPECT_FALSE(SnapshotMaterialCompilerInput(*Material, Environment));
	{
		TStrongObjectPtr<DMaterialInstance> Instance(NewObject<DMaterialInstance>(nullptr, NAME_None));
		FMaterialPropertyOverrides Overrides;
		Overrides.bOverrideShadingModel = true;
		Overrides.Values.ShadingModel = EMaterialShadingModel::Unlit;
		ASSERT_TRUE(Instance->SetParentAndPropertyOverrides(Material, Overrides));
		const auto Unlit = SnapshotMaterialCompilerInput(*Instance, Environment);
		ASSERT_TRUE(Unlit) << FormatMaterialProgramDiagnostics(Unlit.Diagnostics);
		EXPECT_TRUE(Unlit.Snapshot->FunctionOwners.empty());
		ASSERT_TRUE(Instance->SetParent(nullptr));
	}
	LoadedCall->Function = SavedFunction.Get();
	const auto Restored = SnapshotMaterialCompilerInput(*Material, Environment);
	ASSERT_TRUE(Restored);
	EXPECT_EQ(Restored.Snapshot->FunctionOwners.size(), 1u);
	const auto Compiled = MIR::Normalize(Restored.Snapshot->Input);
	ASSERT_TRUE(Compiled);
	EXPECT_EQ(Compiled.Identity, Original.Identity);
	SavedFunction.Reset();
	ASSERT_TRUE(Testing::EnsurePackageNotResident(MaterialPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
	ASSERT_TRUE(Testing::EnsurePackageNotResident(FunctionPath));
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
	FMaterialRootInputs Outputs; Outputs.Get(EMaterialProperty::MP_EmissiveColor) = {RGB.Get()};
	ASSERT_TRUE(Material->SetMaterialExpressions(std::array<DMaterialExpression*,2>{Scene.Get(),RGB.Get()}, Outputs));
	const auto Expected = CaptureExpressions(*Material);
	ASSERT_TRUE(SavePackage(Material->GetPackage())); Scene.Reset(); RGB.Reset();
	ASSERT_TRUE(UnloadPackage(Path));
	Material = LoadObject<DMaterial>(Testing::MakePackageLeafAssetObjectPathForTests(Path)); ASSERT_NE(Material,nullptr);
	EXPECT_EQ(Material->GetDomain(),EMaterialDomain::PostProcess); EXPECT_EQ(CaptureExpressions(*Material),Expected);
	{
		TStrongObjectPtr<DMaterialGraph> Graph(DMaterialGraph::Create(*Material));
		const auto Emissive = FMaterialGraphPinAddress::MaterialOutput(EMaterialProperty::MP_EmissiveColor);
		const auto* Pin = Graph->FindPin(Emissive);
		ASSERT_TRUE(Pin);
		EXPECT_EQ(Pin->SourceIndex, 0);
		ASSERT_EQ(Pin->LinkedTo.size(), 1u);
		EXPECT_EQ(Pin->LinkedTo.front()->GetOwningNode()->GetMaterialExpression(),
			Material->GetExpressionInputForProperty(Emissive.Property)->Expression.Get());
		EXPECT_FALSE(Graph->FindPin(FMaterialGraphPinAddress::MaterialOutput(EMaterialProperty::MP_BaseColor)));
		EXPECT_TRUE(Material->GetExpressionInputForProperty(EMaterialProperty::MP_BaseColor));
	}
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
	FMaterialRootInputs Outputs;
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

		std::vector<FMaterialGraphChangeSet> Changes;
		const auto Subscription=Material->GetGraphChanges().Subscribe(*Material,[&](const auto& Change) { Changes.push_back(Change); });
		ASSERT_TRUE(MaterialEditing::SetConfiguration(*Material, Configurations[Index]));
		if (Index)
		{
			ASSERT_EQ(Changes.size(),1u);
			EXPECT_TRUE(Changes.back().Has(EMaterialGraphChange::RootInputs));
			ASSERT_TRUE(Transactions->Undo()); EXPECT_EQ(Material->GetDomain(),Previous.Domain); EXPECT_EQ(Material->GetStaticProperties(),Previous.Properties);
			ASSERT_TRUE(Material->CompileEdits()); ASSERT_TRUE(Material->GetAcceptedCompiledProgram());
			EXPECT_EQ(Material->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId,Identities.back());
			ASSERT_TRUE(Transactions->Redo());
		}
		Material->GetGraphChanges().Unsubscribe(Subscription);
		EXPECT_EQ(Material->GetDomain(),Configurations[Index].Domain); EXPECT_EQ(Material->GetStaticProperties(),Configurations[Index].Properties);
		EXPECT_EQ(CaptureExpressions(*Material),Before); // Inactive roots and stable parameter IDs are retained.
		Transactions->Reset();
		const auto Revision=Material->GetMaterialCompileDebugInfo().AuthoredRevision;
		auto Invalid=Configurations[Index]; Invalid.Domain=EMaterialDomain::PostProcess; Invalid.Properties.ShadingModel=EMaterialShadingModel::ClearCoat;
		EXPECT_FALSE(MaterialEditing::SetConfiguration(*Material, Invalid)); EXPECT_FALSE(Transactions->CanUndo());
		Invalid.Domain=EMaterialDomain::Surface; Invalid.Properties.BlendMode=EMaterialBlendMode::Translucent;
		EXPECT_FALSE(MaterialEditing::SetConfiguration(*Material, Invalid)); EXPECT_FALSE(Transactions->CanUndo());
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
		Transactions->Reset();
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
	ASSERT_TRUE(MaterialEditing::SetConfiguration(*Material, Configurations.front(),ETransactionMode::NotTransactable));
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
	FMaterialRootInputs Outputs; Outputs.Get(EMaterialProperty::MP_EmissiveColor).SetConstant({.25f,.5f,1.f});
	ASSERT_TRUE(Source->SetMaterialExpressions({},Outputs)); ASSERT_TRUE(SavePackage(Source->GetPackage()));
	std::array<FMaterialConfiguration,3> Configurations;
	Configurations[0]=Initial; Configurations[0].Domain=EMaterialDomain::PostProcess;
	Configurations[1].Properties.ShadingModel=EMaterialShadingModel::ClearCoat;
	for (const auto& Configuration:Configurations)
	{
		SCOPED_TRACE(uint32(Configuration.Domain));
		const auto OriginalDomain=Source->GetDomain(); const auto OriginalProperties=Source->GetStaticProperties();
		FMaterialRootInputs ExpectedOutputs=Outputs;
		auto ExpectedBindings = CaptureMaterialOutputBindings(ExpectedOutputs);
		{
			Tests::FTestEditorTransactorOwner Transactions;
			FMaterialEditingSession Session; std::string Error;
			ASSERT_TRUE(Session.Initialize(*Source,EMaterialEditCompileMode::Manual,Error,Transactions.Get())) << Error;
			auto* Draft=Session.GetWorkingMaterial(); ASSERT_TRUE(Draft);
			EXPECT_EQ(Draft->GetDomain(),OriginalDomain); EXPECT_EQ(Draft->GetStaticProperties(),OriginalProperties); EXPECT_FALSE(Session.HasUnappliedChanges());
			ASSERT_TRUE(MaterialEditing::SetConfiguration(*Draft, Configuration)); EXPECT_TRUE(Session.HasUnappliedChanges());
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
		EXPECT_EQ(Source->GetDomain(),Configuration.Domain); EXPECT_EQ(Source->GetStaticProperties(),Configuration.Properties); EXPECT_EQ(CaptureMaterialOutputBindings(*Source), ExpectedBindings);
	}
	ASSERT_TRUE(UnloadPackage(Path)); CollectGarbage(EObjectFlags::Standalone);
}
