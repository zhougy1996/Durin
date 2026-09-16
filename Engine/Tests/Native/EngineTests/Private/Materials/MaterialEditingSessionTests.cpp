#include "Graph/MaterialGraphEditSession.h"
#include "FunctionPortTestFixture.h"
#include "TypedMaterialGraphTestFixture.h"
#include "Widgets/MaterialEditingSession.h"
#include "MaterialGraphOperations.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "DObject/Package.h"
#include "DObject/ObjectLifecycle.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialFunction.h"
#include "Modules/ModuleManager.h"
#include "NativeDObjectTestSupport.h"
#include "EngineTestSupport.h"
#include "Misc/MountPathTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::Editor::Material;

	class FMaterialEditingSessionTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			FModuleManager::Get().LoadModule("RenderCore");
			Testing::RegisterMountPointForTests("/SessionTests/",
				Testing::GetTestWorkDirectory().generic_string() + "/");
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate(std::format("/SessionTests/{}",
				FGuid::NewGuid().ToString()), Path));
			Package = CreatePackage(Path);
			ASSERT_TRUE(Package);
			Source = NewObject<DMaterial>(Package, "Material");
			ASSERT_TRUE(Source);
			Package->ClearDirty();
		}
		auto TearDown() -> void override
		{
			MarkAsGarbage(Source);
			if (Package) Package->SetStandaloneResidency(false);
			MarkAsGarbage(Package);
			CollectGarbage();
		}
		auto Edit(DMaterial& Material, float Roughness) -> void
		{
			auto Outputs = Material.GetExpressionOutputs();
			Outputs.RoughnessDefault = Roughness;
			std::vector<DMaterialExpression*> Expressions;
			for (const auto& Expression : Material.GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
			ASSERT_TRUE(Material.SetMaterialExpressions(Expressions, Outputs));
		}
		DPackage* Package = nullptr;
		DMaterial* Source = nullptr;
		Testing::FScopedMountRegistryFixture MountRegistry;
	};
}

TEST_F(FMaterialEditingSessionTests, TypedExpressionCopiesStayIndependentAcrossApplyDiscardAndCollection)
{
	TStrongObjectPtr<DMaterialExpressionScalarParameter> Parameter(
		NewObject<DMaterialExpressionScalarParameter>(nullptr, "SessionParameter"));
	Parameter->Id = FGuid::NewGuid();
	Parameter->Metadata.Id = FGuid::NewGuid();
	Parameter->Metadata.Name = FName("SessionValue");
	Parameter->DefaultValue = 0.23f;
	DMaterialExpression* Inputs[] = {Parameter.Get()};
	auto Outputs = Source->GetExpressionOutputs();
	Outputs.Roughness.ExpressionId = Parameter->Id;
	ASSERT_TRUE(Source->SetMaterialExpressions(Inputs, Outputs));
	Package->ClearDirty();
	TObjectPtr<DMaterialExpression> RetiredDraftExpression;
	{
		FMaterialEditingSession Session;
		std::string Error;
		ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error)) << Error;
		auto* Draft = Session.GetWorkingMaterial();
		auto* Original = Cast<DMaterialExpressionScalarParameter>(Source->GetExpressionCollection().Expressions[0].Get());
		auto* Edited = Cast<DMaterialExpressionScalarParameter>(Draft->GetExpressionCollection().Expressions[0].Get());
		ASSERT_NE(Original, nullptr);
		ASSERT_NE(Edited, nullptr);
		EXPECT_NE(Original, Edited);
		EXPECT_EQ(Original->GetOuter(), Source);
		EXPECT_EQ(Edited->GetOuter(), Draft);
		EXPECT_EQ(Original->Id, Edited->Id);
		EXPECT_FALSE(Session.HasUnappliedChanges());
		Edited->DefaultValue = 0.64f;
		Edited->Metadata.DisplayName = "Edited parameter";
		Edited->MarkPackageDirty();
		Edited->PostEditChangeProperty({});
		EXPECT_TRUE(Session.HasUnappliedChanges());
		EXPECT_FLOAT_EQ(Original->DefaultValue, 0.23f);
		CollectGarbage();
		ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
		auto* Applied = Cast<DMaterialExpressionScalarParameter>(Source->GetExpressionCollection().Expressions[0].Get());
		ASSERT_NE(Applied, nullptr);
		EXPECT_NE(Applied, Edited);
		EXPECT_EQ(Applied->GetOuter(), Source);
		EXPECT_FLOAT_EQ(Applied->DefaultValue, 0.64f);
		EXPECT_EQ(Applied->Metadata.DisplayName, "Edited parameter");
		EXPECT_FALSE(Session.HasUnappliedChanges());
		Edited->DefaultValue = 0.91f;
		Edited->MarkPackageDirty();
		Edited->PostEditChangeProperty({});
		EXPECT_TRUE(Session.HasUnappliedChanges());
		Edited->DefaultValue = 0.64f;
		Edited->MarkPackageDirty();
		Edited->PostEditChangeProperty({});
		EXPECT_FALSE(Session.HasUnappliedChanges());
		RetiredDraftExpression = Edited;
	}
	CollectGarbage();
	EXPECT_FALSE(RetiredDraftExpression.IsValid());
	auto* Applied = Cast<DMaterialExpressionScalarParameter>(Source->GetExpressionCollection().Expressions[0].Get());
	ASSERT_NE(Applied, nullptr);
	EXPECT_FLOAT_EQ(Applied->DefaultValue, 0.64f);
}

TEST_F(FMaterialEditingSessionTests, PreviewCompilationIsIsolatedAndApplyPreservesSourceIdentity)
{
	FMaterialEditingSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error)) << Error;
	auto* Draft = Session.GetWorkingMaterial();
	ASSERT_NE(Draft, Source);
	EXPECT_NE(Draft->GetPackage(), Package);
	EXPECT_TRUE(Draft->GetPackage()->GetTopLevelAssets().empty());
	EXPECT_FALSE(Session.HasUnappliedChanges());
	const auto Original = Source->GetExpressionOutputs();
	const auto Generation = Source->GetMaterialCompileStatus().RequestGeneration;
	auto* Child = NewObject<DMaterialInstance>(nullptr, "ApplyChild");
	TStrongObjectPtr<DMaterialInstance> ChildRoot(Child);
	ASSERT_TRUE(Child->SetParent(Source));
	const auto ChildRevision = Child->GetMaterialCompileStatus().AuthoredRevision;
	Edit(*Draft, 0.37f);
	Draft->CompileEdits();
	FAssetCompilingManager::Get().FinishCompilationForObject(*Draft);
	EXPECT_EQ(Source->GetExpressionOutputs(), Original);
	EXPECT_EQ(Source->GetMaterialCompileStatus().RequestGeneration, Generation);
	EXPECT_EQ(Child->GetMaterialCompileStatus().AuthoredRevision, ChildRevision);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Session.HasUnappliedChanges());
	ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
	EXPECT_EQ(Child->GetParent(), Source);
	EXPECT_EQ(Source->GetExpressionOutputs(), Draft->GetExpressionOutputs());
	EXPECT_GT(Child->GetMaterialCompileStatus().AuthoredRevision, ChildRevision);
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_FALSE(Session.HasUnappliedChanges());
	const auto AppliedGeneration = Source->GetMaterialCompileStatus().RequestGeneration;
	ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
	EXPECT_EQ(Source->GetMaterialCompileStatus().RequestGeneration, AppliedGeneration);
	Edit(*Draft, 0.62f);
	EXPECT_TRUE(Session.HasUnappliedChanges());
	EXPECT_FLOAT_EQ(Source->GetExpressionOutputs().RoughnessDefault, 0.37f);
	MarkAsGarbage(Child);
}

TEST_F(FMaterialEditingSessionTests, FunctionCallDraftIsCompleteAndAppliesBindingsAtomically)
{
	auto* First = NewObject<DMaterialFunction>(Package, "FirstFunction");
	auto* Second = NewObject<DMaterialFunction>(Package, "SecondFunction");
	auto Signature = Second->GetFunctionSignature();
	Signature.Inputs[0].Default.Surface.RoughnessDefault.X = 0.23f;
	std::vector<DMaterialExpression*> Body;
	for (const auto& Expression : Second->GetExpressionCollection().Expressions) Body.push_back(Expression.Get());
	ASSERT_TRUE(Second->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Signature, Body)));
	Source->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	const auto Output = First->GetFunctionSignature().Outputs[0];
	const FGuid CallId{72, 1, 1, 1};
	TStrongObjectPtr<DMaterialExpressionFunctionCall> Call(NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None));
	Call->Id = CallId; Call->Function = First; Call->Outputs = {{Output.Id, Output.Type}};
	const std::array<DMaterialExpression*, 1> Expressions{Call.Get()};
	FMaterialExpressionSurfaceOutputs Outputs;
	Outputs.Surface = {.ExpressionId = CallId, .OutputId = Output.Id};
	ASSERT_TRUE(Source->SetMaterialExpressions(Expressions, Outputs));
	const auto GetCall = [](DMaterial& Material) {
		return Cast<DMaterialExpressionFunctionCall>(Material.GetExpressionCollection().Expressions.at(0).Get());
	};
	Tests::FTestTransactorOwner Transactions;
	FMaterialEditingSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error, Transactions.Get())) << Error;
	auto* Draft = Session.GetWorkingMaterial();
	ASSERT_EQ(Draft->GetExpressionCollection().Expressions.size(), 2u);
	EXPECT_EQ(GetCall(*Draft)->Function.Get(), First);
	EXPECT_FALSE(Session.HasUnappliedChanges());
	{
		GraphEditInternals::FGraphEditSession Renamed(*Draft);
		const auto Position = std::ranges::find(Renamed.Presentation.Nodes, CallId, &FMaterialGraphNodePresentation::NodeId);
		if (Position == Renamed.Presentation.Nodes.end()) Renamed.Presentation.Nodes.push_back({.NodeId = CallId, .DisplayName = "Reusable Surface"});
		else Position->DisplayName = "Reusable Surface";
		ASSERT_TRUE(Renamed.Commit("Rename Call", Transactions.Get()));
	}
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Draft->GetExpressionOutputs(), Outputs);
	ASSERT_TRUE(Transactions.Get()->Redo());
	const auto Label = std::ranges::find(Draft->GetMaterialGraphPresentation().Nodes, CallId, &FMaterialGraphNodePresentation::NodeId);
	ASSERT_NE(Label, Draft->GetMaterialGraphPresentation().Nodes.end());
	EXPECT_EQ(Label->DisplayName, "Reusable Surface");
	EXPECT_EQ(GetCall(*Draft)->Function.Get(), First);
	Call->Function = Second;
	ASSERT_TRUE(Draft->SetMaterialExpressions(Expressions, Outputs));
	EXPECT_TRUE(Session.HasUnappliedChanges());
	EXPECT_EQ(GetCall(*Source)->Function.Get(), First);
	ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
	EXPECT_EQ(GetCall(*Source)->Function.Get(), Second);
	EXPECT_FALSE(Session.HasUnappliedChanges());
	const auto Revision = Source->GetMaterialCompileStatus().AuthoredRevision;
	Call->Id = FGuid::NewGuid();
	EXPECT_FALSE(Source->SetMaterialExpressions(Expressions, Outputs));
	EXPECT_EQ(Source->GetMaterialCompileStatus().AuthoredRevision, Revision);
	EXPECT_EQ(GetCall(*Source)->Id, CallId);
}

TEST_F(FMaterialEditingSessionTests, DefaultsStaticPropertiesAndPresentationStayInTheDraft)
{
	FMaterialEditingSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error)) << Error;
	auto* Draft = Session.GetWorkingMaterial();
	const auto OriginalProperties = Source->GetStaticProperties();
	const auto OriginalPresentation = Source->GetMaterialGraphPresentation();
	FMaterialParameterDefinition Definition;
	Definition.Id = FGuid::NewGuid();
	Definition.Name = FName("DraftValue");
	Definition.Type = EMaterialParameterType::Scalar;
	Definition.Value.GetScalar() = 0.23f;
	TStrongObjectPtr<DMaterialExpressionScalarParameter> Parameter(NewObject<DMaterialExpressionScalarParameter>(nullptr, NAME_None));
	Parameter->Id = FGuid::NewGuid(); Parameter->Metadata = {.Id = Definition.Id, .Name = Definition.Name};
	Parameter->DefaultValue = Definition.Value.GetScalar();
	const std::array<DMaterialExpression*, 1> Expressions{Parameter.Get()};
	ASSERT_TRUE(Draft->SetMaterialExpressions(Expressions, Draft->GetExpressionOutputs()));
	auto Properties = OriginalProperties;
	Properties.bTwoSided = !Properties.bTwoSided;
	ASSERT_TRUE(Draft->SetStaticProperties(Properties));
	auto Presentation = OriginalPresentation;
	Testing::OutputPosition(*Draft, Presentation).X += 100;
	ASSERT_TRUE(Draft->SetMaterialGraphPresentation(Presentation) != Durin::EMaterialGraphPresentationResult::Rejected);
	EXPECT_EQ(Source->GetStaticProperties(), OriginalProperties);
	EXPECT_EQ(Source->GetMaterialGraphPresentation(), OriginalPresentation);
	EXPECT_EQ(Source->FindParameterDefinition(Definition.Id), nullptr);
	ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
	EXPECT_EQ(Source->GetStaticProperties(), Properties);
	EXPECT_EQ(Source->GetMaterialGraphPresentation(), Presentation);
	ASSERT_NE(Source->FindParameterDefinition(Definition.Id), nullptr);
	EXPECT_FLOAT_EQ(Source->FindParameterDefinition(Definition.Id)->Value.GetScalar(), 0.23f);
}

TEST_F(FMaterialEditingSessionTests, ExternalSourceChangeRejectsApplyWithoutOverwritingEitherSide)
{
	FMaterialEditingSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error)) << Error;
	Edit(*Session.GetWorkingMaterial(), 0.31f);
	Edit(*Source, 0.72f);
	EXPECT_FALSE(Session.FinishAndApply(Error));
	EXPECT_NE(Error.find("outside"), std::string::npos);
	EXPECT_FLOAT_EQ(Source->GetExpressionOutputs().RoughnessDefault, 0.72f);
	EXPECT_FLOAT_EQ(Session.GetWorkingMaterial()->GetExpressionOutputs().RoughnessDefault, 0.31f);
	EXPECT_TRUE(Session.HasUnappliedChanges());
}

TEST_F(FMaterialEditingSessionTests, DiscardRetiresDraftAndItsHistoryWithoutChangingSource)
{
	Tests::FTestTransactorOwner Transactions;
	TObjectPtr<DMaterial> RetiredDraft;
	const auto Original = Source->GetExpressionOutputs();
	{
		FMaterialEditingSession Session;
		std::string Error;
		ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error,
			Transactions.Get())) << Error;
		auto* Draft = Session.GetWorkingMaterial();
		RetiredDraft = Draft;
		FMaterialGraphSurfaceDefaultRequest Request;
		Request.Output = EMaterialSurfaceOutput::Roughness;
		Request.Value.X = 0.21f;
		ASSERT_TRUE(FMaterialGraphOperations::SetSurfaceDefault(*Draft, Request, Transactions.Get()));
		EXPECT_TRUE(Session.HasUnappliedChanges());
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_FALSE(Session.HasUnappliedChanges());
		ASSERT_TRUE(Transactions->Redo());
		EXPECT_TRUE(Session.HasUnappliedChanges());
		ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
		Session.MarkSaved();
		ASSERT_TRUE(Transactions->Undo());
		EXPECT_TRUE(Session.HasUnappliedChanges());
		EXPECT_NE(Source->GetExpressionOutputs(), Original);
	}
	EXPECT_FALSE(RetiredDraft.IsValid());
	EXPECT_FALSE(Transactions->CanUndo());
	EXPECT_FALSE(Transactions->CanRedo());
	EXPECT_FLOAT_EQ(Source->GetExpressionOutputs().RoughnessDefault, 0.21f);
}

TEST_F(FMaterialEditingSessionTests, DiscardWithoutApplyLeavesSourceClean)
{
	const auto Original = Source->GetExpressionOutputs();
	{
		FMaterialEditingSession Session;
		std::string Error;
		ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Automatic, Error)) << Error;
		Edit(*Session.GetWorkingMaterial(), 0.19f);
	}
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_EQ(Source->GetExpressionOutputs(), Original);
}

TEST_F(FMaterialEditingSessionTests, RelocationDoesNotInvalidateAnUnchangedSource)
{
	FMaterialEditingSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error)) << Error;
	Edit(*Session.GetWorkingMaterial(), 0.46f);
	FPackagePath Destination;
	ASSERT_TRUE(FPackagePath::TryCreate(std::format("/SessionTests/Moved{}",
		FGuid::NewGuid().ToString()), Destination));
	ASSERT_TRUE(Package->RelocateAssetPackage(Destination));
	ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
	EXPECT_FLOAT_EQ(Source->GetExpressionOutputs().RoughnessDefault, 0.46f);
}
