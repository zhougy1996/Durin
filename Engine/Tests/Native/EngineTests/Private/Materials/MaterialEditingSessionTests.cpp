#include "Widgets/MaterialEditingSession.h"
#include "MaterialGraphOperations.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Asset/AssetCompilingManager.h"
#include "DObject/Package.h"
#include "DObject/ObjectLifecycle.h"
#include "Materials/MaterialInstance.h"
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
			auto Program = *Material.GetMaterialProgram();
			Program.Outputs.RoughnessDefault.X = Roughness;
			ASSERT_TRUE(Material.SetMaterialProgram(std::move(Program)));
		}
		DPackage* Package = nullptr;
		DMaterial* Source = nullptr;
		Testing::FScopedMountRegistryFixture MountRegistry;
	};
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
	const auto Original = *Source->GetMaterialProgram();
	const auto Generation = Source->GetMaterialCompileStatus().RequestGeneration;
	auto* Child = NewObject<DMaterialInstance>(nullptr, "ApplyChild");
	TStrongObjectPtr<DMaterialInstance> ChildRoot(Child);
	ASSERT_TRUE(Child->SetParent(Source));
	const auto ChildRevision = Child->GetMaterialCompileStatus().AuthoredRevision;
	Edit(*Draft, 0.37f);
	Draft->CompileEdits();
	FAssetCompilingManager::Get().FinishCompilationForObject(*Draft);
	EXPECT_EQ(*Source->GetMaterialProgram(), Original);
	EXPECT_EQ(Source->GetMaterialCompileStatus().RequestGeneration, Generation);
	EXPECT_EQ(Child->GetMaterialCompileStatus().AuthoredRevision, ChildRevision);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Session.HasUnappliedChanges());
	ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
	EXPECT_EQ(Child->GetParent(), Source);
	EXPECT_EQ(*Source->GetMaterialProgram(), *Draft->GetMaterialProgram());
	EXPECT_GT(Child->GetMaterialCompileStatus().AuthoredRevision, ChildRevision);
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_FALSE(Session.HasUnappliedChanges());
	const auto AppliedGeneration = Source->GetMaterialCompileStatus().RequestGeneration;
	ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
	EXPECT_EQ(Source->GetMaterialCompileStatus().RequestGeneration, AppliedGeneration);
	Edit(*Draft, 0.62f);
	EXPECT_TRUE(Session.HasUnappliedChanges());
	EXPECT_FLOAT_EQ(Source->GetMaterialProgram()->Outputs.RoughnessDefault.X, 0.37f);
	MarkAsGarbage(Child);
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
	Definition.Value.ScalarValue = 0.23f;
	ASSERT_TRUE(Draft->CreateParameterDefinition(Definition));
	auto Properties = OriginalProperties;
	Properties.bTwoSided = !Properties.bTwoSided;
	ASSERT_TRUE(Draft->SetStaticProperties(Properties));
	auto Presentation = OriginalPresentation;
	Presentation.MaterialOutputX += 100;
	ASSERT_TRUE(Draft->SetMaterialGraphPresentation(Presentation));
	EXPECT_EQ(Source->GetStaticProperties(), OriginalProperties);
	EXPECT_EQ(Source->GetMaterialGraphPresentation(), OriginalPresentation);
	EXPECT_EQ(Source->FindParameterDefinition(Definition.Id), nullptr);
	ASSERT_TRUE(Session.FinishAndApply(Error)) << Error;
	EXPECT_EQ(Source->GetStaticProperties(), Properties);
	EXPECT_EQ(Source->GetMaterialGraphPresentation(), Presentation);
	ASSERT_NE(Source->FindParameterDefinition(Definition.Id), nullptr);
	EXPECT_FLOAT_EQ(Source->FindParameterDefinition(Definition.Id)->Value.ScalarValue, 0.23f);
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
	EXPECT_FLOAT_EQ(Source->GetMaterialProgram()->Outputs.RoughnessDefault.X, 0.72f);
	EXPECT_FLOAT_EQ(Session.GetWorkingMaterial()->GetMaterialProgram()->Outputs.RoughnessDefault.X, 0.31f);
	EXPECT_TRUE(Session.HasUnappliedChanges());
}

TEST_F(FMaterialEditingSessionTests, DiscardRetiresDraftAndItsHistoryWithoutChangingSource)
{
	Tests::FTestTransactorOwner Transactions;
	TObjectPtr<DMaterial> RetiredDraft;
	const auto Original = *Source->GetMaterialProgram();
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
		EXPECT_NE(*Source->GetMaterialProgram(), Original);
	}
	EXPECT_FALSE(RetiredDraft.IsValid());
	EXPECT_FALSE(Transactions->CanUndo());
	EXPECT_FALSE(Transactions->CanRedo());
	EXPECT_FLOAT_EQ(Source->GetMaterialProgram()->Outputs.RoughnessDefault.X, 0.21f);
}

TEST_F(FMaterialEditingSessionTests, DiscardWithoutApplyLeavesSourceClean)
{
	const auto Original = *Source->GetMaterialProgram();
	{
		FMaterialEditingSession Session;
		std::string Error;
		ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Automatic, Error)) << Error;
		Edit(*Session.GetWorkingMaterial(), 0.19f);
	}
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_EQ(*Source->GetMaterialProgram(), Original);
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
	EXPECT_FLOAT_EQ(Source->GetMaterialProgram()->Outputs.RoughnessDefault.X, 0.46f);
}

// Runs inside the existing async-manager fixture, which owns the process-wide
// compiler startup/shutdown and cold-cache qualification ordering.
auto QualifyMaterialEditingSessionAsync() -> void
{
	Testing::FScopedMountRegistryFixture MountRegistry;
	Testing::RegisterMountPointForTests("/ApplyAsync/",
		Testing::GetTestWorkDirectory().generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/ApplyAsync/Material", Path));
	auto* Package = CreatePackage(Path);
	ASSERT_TRUE(Package);
	auto* Source = NewObject<DMaterial>(Package, "Material");
	struct FCleanup
	{
		DMaterial* Source;
		DPackage* Package;
		~FCleanup()
		{
			MarkAsGarbage(Source);
			Package->SetStandaloneResidency(false);
			MarkAsGarbage(Package);
		}
	} Cleanup{Source, Package};
	FMaterialEditingSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Initialize(*Source, EMaterialEditCompileMode::Manual, Error)) << Error;
	auto* Draft = Session.GetWorkingMaterial();
	FAssetCompilingManager::Get().FinishCompilationForObject(*Draft);
	const auto Original = *Source->GetMaterialProgram();
	auto Program = Original;
	Program.Outputs.RoughnessDefault.X = 0.132711f;
	ASSERT_TRUE(Draft->SetMaterialProgram(Program));
	ASSERT_TRUE(Session.RequestApply(Error)) << Error;
	ASSERT_TRUE(Session.IsApplyPending());
	EXPECT_EQ(*Source->GetMaterialProgram(), Original);
	const auto Pending = Draft->GetMaterialCompileStatus();
	FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Draft);
	FMaterialCompileResult Failed{
		.Owner = MakeObjectHandle(Draft),
		.AuthoredRevision = Pending.AuthoredRevision,
		.Generation = Pending.RequestGeneration,
		.DependencyRevision = Pending.DependencyRevision,
		.ProgramIdentity = Pending.RequestedIdentity,
		.Target = Pending.Target,
		.State = EMaterialCompileState::Failed,
		.Category = EMaterialCompileResultCategory::Compile,
	};
	EXPECT_FALSE(Private::FMaterialCompilationLifecycle::Admit(*Draft, std::move(Failed)));
	Session.Tick(Error);
	EXPECT_FALSE(Error.empty());
	EXPECT_FALSE(Session.IsApplyPending());
	EXPECT_EQ(*Source->GetMaterialProgram(), Original);
	EXPECT_FALSE(Package->IsDirty());

	Error.clear();
	Program.Outputs.RoughnessDefault.X = 0.242713f;
	ASSERT_TRUE(Draft->SetMaterialProgram(Program));
	ASSERT_TRUE(Session.RequestApply(Error)) << Error;
	ASSERT_TRUE(Session.IsApplyPending());
	Program.Outputs.RoughnessDefault.X = 0.352717f;
	ASSERT_TRUE(Draft->SetMaterialProgram(Program));
	Session.Tick(Error);
	EXPECT_FALSE(Error.empty());
	EXPECT_FALSE(Session.IsApplyPending());
	EXPECT_EQ(*Source->GetMaterialProgram(), Original);

	Error.clear();
	ASSERT_TRUE(Session.RequestApply(Error)) << Error;
	FAssetCompilingManager::Get().FinishCompilationForObject(*Draft);
	Session.Tick(Error);
	EXPECT_TRUE(Error.empty()) << Error;
	EXPECT_FALSE(Session.IsApplyPending());
	EXPECT_EQ(*Source->GetMaterialProgram(), Program);
	EXPECT_FALSE(Session.HasUnappliedChanges());

	// Automatic deadlines must be serviced for transient drafts even when their
	// document is hidden; the source must remain unchanged after completion.
	Draft->SetEditCompileMode(EMaterialEditCompileMode::Automatic);
	Program.Outputs.RoughnessDefault.X = 0.452719f;
	ASSERT_TRUE(Draft->SetMaterialProgram(Program));
	std::this_thread::sleep_for(std::chrono::milliseconds(450));
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_NE(Draft->GetMaterialCompileStatus().State, EMaterialCompileState::Scheduled);
	FAssetCompilingManager::Get().FinishCompilationForObject(*Draft);
	EXPECT_TRUE(Draft->GetMaterialCompileStatus().IsCurrent());
	EXPECT_NE(*Source->GetMaterialProgram(), Program);
}
