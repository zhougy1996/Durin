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


using namespace Durin;
using namespace Durin::Editor::Material;

// Runs inside the existing async-manager fixture, which owns the process-wide
// compiler startup/shutdown ordering.
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
	const auto Original = Source->GetExpressionOutputs();
	auto Outputs = Original;
	Outputs.RoughnessDefault = 0.132711f;
	ASSERT_TRUE(Draft->SetMaterialExpressions({}, Outputs));
	ASSERT_TRUE(Session.RequestApply(Error)) << Error;
	ASSERT_TRUE(Session.IsApplyPending());
	EXPECT_EQ(Source->GetExpressionOutputs(), Original);
	const auto Pending = Draft->GetMaterialCompileStatus();
	FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Draft);
	FMaterialCompileResult Failed{
		.Owner = FWeakObjectPtr(Draft),
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
	EXPECT_EQ(Source->GetExpressionOutputs(), Original);
	EXPECT_FALSE(Package->IsDirty());

	Error.clear();
	Outputs.RoughnessDefault = 0.242713f;
	ASSERT_TRUE(Draft->SetMaterialExpressions({}, Outputs));
	ASSERT_TRUE(Session.RequestApply(Error)) << Error;
	ASSERT_TRUE(Session.IsApplyPending());
	Outputs.RoughnessDefault = 0.352717f;
	ASSERT_TRUE(Draft->SetMaterialExpressions({}, Outputs));
	Session.Tick(Error);
	EXPECT_FALSE(Error.empty());
	EXPECT_FALSE(Session.IsApplyPending());
	EXPECT_EQ(Source->GetExpressionOutputs(), Original);

	Error.clear();
	ASSERT_TRUE(Session.RequestApply(Error)) << Error;
	FAssetCompilingManager::Get().FinishCompilationForObject(*Draft);
	Session.Tick(Error);
	EXPECT_TRUE(Error.empty()) << Error;
	EXPECT_FALSE(Session.IsApplyPending());
	EXPECT_EQ(Source->GetExpressionOutputs(), Outputs);
	EXPECT_FALSE(Session.HasUnappliedChanges());

	// Automatic deadlines must be serviced for transient drafts even when their
	// document is hidden; the source must remain unchanged after completion.
	Draft->SetEditCompileMode(EMaterialEditCompileMode::Automatic);
	Outputs.RoughnessDefault = 0.452719f;
	ASSERT_TRUE(Draft->SetMaterialExpressions({}, Outputs));
	std::this_thread::sleep_for(std::chrono::milliseconds(450));
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_NE(Draft->GetMaterialCompileStatus().State, EMaterialCompileState::Scheduled);
	FAssetCompilingManager::Get().FinishCompilationForObject(*Draft);
	EXPECT_TRUE(Draft->GetMaterialCompileStatus().IsCurrent());
	EXPECT_NE(Source->GetExpressionOutputs(), Outputs);
}
