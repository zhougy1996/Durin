#include "MaterialResourceTestSupport.h"
#include "MetalShaderFormat.h"
#include "Materials/MaterialShaderMap.h"
#include "ShaderBuildTestSupport.h"
#include "Shader/IShaderBuildModule.h"
#include "ShaderBuild/ShaderPaths.h"
#include "Materials/MaterialExpressionBuild.h"
#include "NativeRHIBackendTestSupport.h"
#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "FunctionPortTestFixture.h"
#include "ExplicitMaterialProgramTestFixture.h"
#include "MaterialCookedProgramTestSupport.h"
#include "MaterialVariantTestFixture.h"
#include "MaterialResultFanoutTestSupport.h"
#include "Threading/TaskComposition.h"
#include "MaterialTestSupport.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/OfflinePreparation.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialCompileRetryQueue.h"
#include "Materials/MaterialCompileTestSupport.h"
#include "Shader/ShaderCompilingManager.h"
#include "Materials/MaterialCookedProgram.h"
#include "Materials/MaterialFunction.h"
#include "Modules/ModuleManager.h"
#include "Threading/Task.h"
#include "Threading/ThreadEvent.h"


auto QualifyMaterialEditingSessionAsync() -> void;
auto QualifyMaterialFunctionCompilationAsync() -> void;

namespace
{
	auto QualifyInstanceCompilationOwners() -> void;

	auto WaitForMaterialCompile(
		Durin::DMaterialInterface& Material,
		std::chrono::milliseconds Timeout = std::chrono::seconds(10)) -> bool
	{
		const auto Deadline = std::chrono::steady_clock::now() + Timeout;
		while (std::chrono::steady_clock::now() < Deadline)
		{
			Durin::FAssetCompilingManager::Get().ProcessAsyncTasks();
			const Durin::EMaterialCompileState State =
				Material.GetMaterialCompileStatus().State;
			if (State != Durin::EMaterialCompileState::Running
				&& State != Durin::EMaterialCompileState::Deferred
				&& State != Durin::EMaterialCompileState::Scheduled)
				return State == Durin::EMaterialCompileState::Ready;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}

	auto EditRoughnessDefault(Durin::DMaterial& Material, float Delta) -> Durin::FMaterialProgramValidationResult
	{
		auto Outputs = Material.GetExpressionOutputs();
		Outputs.Roughness.SetConstant({ReadMaterialOutputDefault(Outputs, Durin::EMaterialOutputPin::Roughness)[0] + Delta});
		std::vector<Durin::DMaterialExpression*> Expressions;
		for (const auto& Expression : Material.GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
		return Material.SetMaterialExpressions(Expressions, Outputs);
	}

}

namespace
{
	auto QualifyDistinctTranslationsConvergingToOneProgram() -> void
	{
		using namespace Durin;
		struct FGate final : DerivedData::ICache {
			mutable FThreadEvent Entered, Release;
			mutable std::atomic_bool Blocked = false;
			auto Get(const DerivedData::FCacheGetRequest& Request) const -> DerivedData::FCacheGetResult override { if (!Blocked.exchange(true)) { Entered.Trigger(); Release.WaitFor(10); } return DerivedData::GetCache().Get(Request); }
			auto Put(const DerivedData::FCachePutRequest& Request) const -> DerivedData::FCachePutResult override { if (!Blocked.exchange(true)) { Entered.Trigger(); Release.WaitFor(10); } return DerivedData::GetCache().Put(Request); }
		};
		auto Cache = std::make_shared<FGate>(); ShaderBuildPrivate::ResetServiceForTests({.Cache = Cache});
		auto* A = NewObject<DMaterial>(nullptr, "SemanticConvergenceA"); auto* B = NewObject<DMaterial>(nullptr, "SemanticConvergenceB");
		struct FRestore { std::shared_ptr<FGate> Cache; DMaterial* A; DMaterial* B; ~FRestore() { Cache->Release.Trigger(); FAssetCompilingManager::Get().FinishAllCompilation(); ShaderBuildPrivate::ResetServiceForTests(); MarkAsGarbage(A); MarkAsGarbage(B); CollectGarbage(); } } Restore{Cache, A, B};
		A->SetEditCompileMode(EMaterialEditCompileMode::Manual); B->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		ASSERT_TRUE(EditRoughnessDefault(*A, .141971f)); ASSERT_TRUE(EditRoughnessDefault(*B, .141971f));
		FMaterialCompilerEnvironment Environment; ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Environment));
		auto Capture = SnapshotMaterialCompilerInput(*A, Environment); ASSERT_TRUE(Capture);
		auto Input = Capture.Snapshot->Input; auto Other = Input;
		Other.IR.Nodes.push_back({.Opcode = EMaterialProgramOpcode::Constant, .ResultType = EMaterialProgramValueType::Float, .Payload = FMaterialProgramLiteral{.739f}});
		ASSERT_EQ(MIR::Normalize(Input).Identity, MIR::Normalize(Other).Identity);
		const auto Before = GetMaterialCompilationDiagnostics();
		const auto Joins = MaterialBuildPrivate::GetMaterialProgramCompileStatsForTests().Joins;
		ASSERT_TRUE(Private::FMaterialCompilationLifecycle::Submit(*A, Input, true)); ASSERT_TRUE(Cache->Entered.WaitFor(5));
		ASSERT_TRUE(Private::FMaterialCompilationLifecycle::Submit(*B, Other, true));
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (MaterialBuildPrivate::GetMaterialProgramCompileStatsForTests().Joins == Joins && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
		ASSERT_GT(MaterialBuildPrivate::GetMaterialProgramCompileStatsForTests().Joins, Joins);
		Cache->Release.Trigger(); ASSERT_TRUE(WaitForMaterialCompile(*A)); ASSERT_TRUE(WaitForMaterialCompile(*B));
		EXPECT_GT(GetMaterialCompilationDiagnostics().SingleFlightConsumers, Before.SingleFlightConsumers);
		EXPECT_EQ(A->GetAcceptedCompiledProgram(), B->GetAcceptedCompiledProgram()); EXPECT_EQ(GetShaderBuildStats().Compilations, MaterialCompiledEntryPoints.size());
	}
}

namespace
{
auto QualifyCanceledFlightResubmission() -> void
{
	using namespace Durin;
	auto* Material = NewObject<DMaterial>(nullptr, "CanceledFlightResubmission");
	struct FObjectCleanup
	{
		DMaterial* Material;
		~FObjectCleanup() { MarkAsGarbage(Material); CollectGarbage(); }
	} Cleanup{Material};
	// Use a distinct graph for the cancellation and resubmission assertions.
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(EditRoughnessDefault(*Material, 0.173125f));
	for (const bool bExplicitCancel : {false, true})
	{
		SCOPED_TRACE(bExplicitCancel ? "cancel then compile" : "compile twice");
		{
			const auto WorkerCount = GetTaskSchedulerDiagnostics().WorkerCount;
			FThreadEvent Started, Release;
			std::atomic<uint32> StartedCount = 0;
			std::vector<Tasks::FTask> Blockers;
			struct FReleaseWorkers
			{
				FThreadEvent& Release;
				std::vector<Tasks::FTask>& Blockers;
				~FReleaseWorkers() { Release.Trigger(); for (const auto& Task : Blockers) WaitTask(Task); }
			} ReleaseWorkers{Release, Blockers};
			for (uint32 Index = 0; Index < WorkerCount; ++Index)
				Blockers.push_back(Durin::Tasks::FTask(Tasks::LaunchTask("HoldCanceledMaterialFlight", [&] {
					if (StartedCount.fetch_add(1) + 1 == WorkerCount) Started.Trigger();
					Release.WaitFor(10.0);
				})));
			ASSERT_TRUE(Started.WaitFor(2.0));
			// Force compilation so the second iteration cannot use a retained hit.
			ASSERT_TRUE(RequestMaterialRecompile(*Material, true));
			const auto Generation = Material->GetMaterialCompileStatus().RequestGeneration;
			if (bExplicitCancel)
				FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Material);
			ASSERT_TRUE(RequestMaterialRecompile(*Material, true));
			EXPECT_GT(Material->GetMaterialCompileStatus().RequestGeneration, Generation);
			EXPECT_TRUE(Material->GetMaterialCompileStatus().State == EMaterialCompileState::Deferred || Material->GetMaterialCompileStatus().State == EMaterialCompileState::Running);
		}
		ASSERT_TRUE(WaitForMaterialCompile(*Material));
		EXPECT_TRUE(Material->GetMaterialCompileStatus().IsCurrent());
		EXPECT_EQ(Material->GetMaterialCompileStatus().CacheOutcome, EMaterialCompileCacheOutcome::Forced);
	}
}

auto QualifyEditScheduling() -> void
{
	using namespace Durin;
	auto* Root = NewObject<DMaterial>(nullptr, "ScheduledRoot");
	auto* Child = NewObject<DMaterialInstance>(nullptr, "ScheduledChild");
	struct FObjects
	{
		DMaterial* Root;
		DMaterialInstance* Child;
		~FObjects() { MarkAsGarbage(Child); MarkAsGarbage(Root); CollectGarbage(); }
	} Objects{Root, Child};
	ASSERT_TRUE(Child->SetParent(Root));
	ASSERT_TRUE(Root->CompileEdits());
	ASSERT_TRUE(WaitForMaterialCompile(*Root));
	ASSERT_TRUE(WaitForMaterialCompile(*Child));
	const auto Previous = Root->GetAcceptedCompiledProgram();
	const auto Generation = Root->GetMaterialCompileStatus().RequestGeneration;
	const auto ChildGeneration = Child->GetMaterialCompileStatus().RequestGeneration;

	Root->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(EditRoughnessDefault(*Root, 0.1f));
	ASSERT_TRUE(EditRoughnessDefault(*Root, 0.1f));
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_EQ(Root->GetMaterialCompileStatus().State, EMaterialCompileState::NeedsCompile);
	EXPECT_EQ(Child->GetMaterialCompileStatus().State, EMaterialCompileState::NeedsCompile);
	EXPECT_EQ(Root->GetMaterialCompileStatus().RequestGeneration, Generation);
	EXPECT_EQ(Child->GetMaterialCompileStatus().RequestGeneration, ChildGeneration);
	EXPECT_EQ(Root->GetAcceptedCompiledProgram(), Previous);
	EXPECT_FALSE(Root->GetMaterialCompileStatus().IsCurrent());
	FMaterialParameterDefinition Extra;
	Extra.Id = FGuid::NewGuid();
	Extra.Name = FName("PendingScalar");
	Extra.Type = EMaterialParameterType::Scalar;
	auto Parameter = TStrongObjectPtr<DMaterialExpressionScalarParameter>(NewObject<DMaterialExpressionScalarParameter>(nullptr, NAME_None));
	Parameter->Id = FGuid::NewGuid(); Parameter->Metadata = {.Id = Extra.Id, .Name = Extra.Name};
	std::vector<DMaterialExpression*> Expressions;
	for (const auto& Expression : Root->GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
	Expressions.push_back(Parameter.Get());
	ASSERT_TRUE(Root->SetMaterialExpressions(Expressions, Root->GetExpressionOutputs()));
	const auto Revision = Root->GetMaterialCompileStatus().AuthoredRevision;
	ASSERT_TRUE(Root->SetScalarParameterValue(Extra.Name, 0.25f));
	Root->PostEditChangeProperty({
		.MemberProperty = Root->GetClass()->FindPropertyByName("ExpressionCollection")});
	EXPECT_EQ(Root->GetMaterialCompileStatus().AuthoredRevision, Revision);
	auto Invalid = Root->GetExpressionOutputs();
	Invalid.Roughness.Connection.ExpressionId = FGuid::NewGuid();
	Expressions.clear();
	for (const auto& Expression : Root->GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
	EXPECT_FALSE(Root->SetMaterialExpressions(Expressions, Invalid));
	EXPECT_EQ(Root->GetMaterialCompileStatus().AuthoredRevision, Revision);
	ASSERT_TRUE(Root->CompileEdits());
	ASSERT_TRUE(WaitForMaterialCompile(*Root));
	ASSERT_TRUE(WaitForMaterialCompile(*Child));
	EXPECT_TRUE(Root->GetMaterialCompileStatus().IsCurrent());
	EXPECT_TRUE(Child->GetMaterialCompileStatus().IsCurrent());
	EXPECT_EQ(Root->GetMaterialCompileStatus().RequestGeneration, Generation + 1);

	auto& Manager = FAssetCompilingManager::Get();
	const auto RemainingBeforeSchedule = Manager.GetNumRemainingAssets();
	Root->SetEditCompileMode(EMaterialEditCompileMode::Automatic);
	ASSERT_TRUE(EditRoughnessDefault(*Root, 0.05f));
	EXPECT_EQ(Manager.GetNumRemainingAssets(), RemainingBeforeSchedule + 2);
	std::this_thread::sleep_for(std::chrono::milliseconds(250));
	ASSERT_TRUE(EditRoughnessDefault(*Root, 0.05f));
	EXPECT_EQ(Manager.GetNumRemainingAssets(), RemainingBeforeSchedule + 2);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_EQ(Root->GetMaterialCompileStatus().State, EMaterialCompileState::Scheduled);
	EXPECT_TRUE(Root->IsCompiling());
	EXPECT_FALSE(Root->IsComplete());
	// Pending edits retain the accepted generation rather than an error material.
	EXPECT_TRUE(Root->GetMaterialResource().HasValidGameThreadShaderMap());
	EXPECT_EQ(Root->GetMaterialCompileStatus().RequestGeneration, Generation + 1);
	ASSERT_TRUE(WaitForMaterialCompile(*Root));
	ASSERT_TRUE(WaitForMaterialCompile(*Child));
	EXPECT_EQ(Root->GetMaterialCompileStatus().RequestGeneration, Generation + 2);
	EXPECT_FALSE(Root->IsCompiling());
	EXPECT_TRUE(Root->IsComplete());

	ASSERT_TRUE(EditRoughnessDefault(*Root, 0.01f));
	Root->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	std::this_thread::sleep_for(std::chrono::milliseconds(450));
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_EQ(Root->GetMaterialCompileStatus().State, EMaterialCompileState::NeedsCompile);
	EXPECT_EQ(Root->GetMaterialCompileStatus().RequestGeneration, Generation + 2);
	Root->SetEditCompileMode(EMaterialEditCompileMode::Automatic);
	ASSERT_TRUE(WaitForMaterialCompile(*Root));
	ASSERT_TRUE(WaitForMaterialCompile(*Child));
	EXPECT_EQ(Root->GetMaterialCompileStatus().RequestGeneration, Generation + 3);
	ASSERT_TRUE(EditRoughnessDefault(*Root, 0.01f));
	const auto RemainingBeforeCancel = Manager.GetNumRemainingAssets();
	FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Root);
	EXPECT_EQ(Manager.GetNumRemainingAssets(), RemainingBeforeCancel - 1);
	EXPECT_EQ(Root->GetMaterialCompileStatus().State, EMaterialCompileState::Canceled);
	EXPECT_EQ(Root->GetMaterialCompileStatus().RequestGeneration, Generation + 3);
}

}

TEST(FMaterialCompileLifecycleTests, ShaderBuildStartupIsVisibleToLinkedCalls)
{
	using namespace Durin;
	InitializeDObjectSystem();
	ASSERT_NE(FModuleManager::Get().LoadModule("ShaderBuild"), nullptr);
	// A second dylib image can start successfully while linked calls still see
	// an empty mount registry and an uninitialized compiler service.
	ASSERT_TRUE(std::filesystem::is_regular_file(FShaderPaths::SourcePath("/Engine/MaterialCompilerEnvironment")));
	FMaterialCompilerEnvironment Environment;
	ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Environment));
	FAssetCompilingManager::Get().FinishAllCompilation();
	ShaderBuildPrivate::ResetServiceForTests();
}

TEST(FMaterialCompileLifecycleTests, RetryQueueBoundsChecksAndRotatesWithoutDuplicates)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Private::FMaterialCompileRetryQueue Queue;
	constexpr uint32 Budget = Private::MaterialCompileMaxRetryChecks;
	std::vector<FWeakObjectPtr> Visited;
	std::vector<FWeakObjectPtr> Owners;
	for (uint32 I = 0; I < Budget + 3; ++I) Owners.emplace_back(NewObject<DObject>(nullptr, NAME_None));
	const auto Keep = [&](FWeakObjectPtr Owner) { Visited.push_back(Owner); return true; };
	Queue.Process(Budget, Keep);
	EXPECT_TRUE(Visited.empty());
	for (uint32 Index = 0; Index < Budget + 3; ++Index)
	{
		Queue.Add(Owners[Index]);
		Queue.Add(Owners[Index]);
	}
	EXPECT_EQ(Queue.Num(), Budget + 3);
	Queue.Process(Budget, Keep);
	ASSERT_EQ(Visited.size(), Budget);
	EXPECT_EQ(Visited.front(), Owners[0]);
	EXPECT_EQ(Visited.back(), Owners[Budget - 1]);
	Visited.clear();
	Queue.Process(Budget, Keep);
	ASSERT_EQ(Visited.size(), Budget);
	EXPECT_EQ(Visited.front(), Owners[Budget]);
	EXPECT_EQ(Visited[3], Owners[0]);
	EXPECT_EQ(Queue.Num(), Budget + 3);

	// Discarded/stale entries consume the same budget as retries.
	Visited.clear();
	Queue.Process(Budget, [&](FWeakObjectPtr Owner) { Visited.push_back(Owner); return false; });
	EXPECT_EQ(Visited.size(), Budget);
	EXPECT_EQ(Queue.Num(), 3u);
	Visited.clear();
	Queue.Process(Budget, Keep);
	EXPECT_EQ(Visited.size(), 3u); // Requeued owners are not revisited this pump.
	for (const auto Owner : Owners) MarkAsGarbage(Owner.Get());
	CollectGarbage();
}

TEST(FMaterialCompileLifecycleTests, RetryQueueRemovesOwnersAndDistinguishesSlotGenerations)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Private::FMaterialCompileRetryQueue Queue;
	Queue.Add({});
	EXPECT_EQ(Queue.Num(), 0u);
	auto* Old = NewObject<DObject>(nullptr, NAME_None);
	const FWeakObjectPtr First(Old);
	Queue.Add(First);
	MarkAsGarbage(Old);
	CollectGarbage();
	const FWeakObjectPtr Second(NewObject<DObject>(nullptr, NAME_None));
	const FWeakObjectPtr Third(NewObject<DObject>(nullptr, NAME_None));
	Queue.Add(Second);
	Queue.Add(Third);
	Queue.Remove(First);
	Queue.Remove(First);
	EXPECT_EQ(Queue.Num(), 2u);
	std::vector<FWeakObjectPtr> Visited;
	Queue.Process(256, [&](FWeakObjectPtr Owner) {
		Visited.push_back(Owner);
		// Submission may register the popped owner itself. Requeue must deduplicate.
		Queue.Add(Owner);
		return true;
	});
	ASSERT_EQ(Visited.size(), 2u);
	EXPECT_EQ(Visited[0], Second);
	EXPECT_EQ(Visited[1], Third);
	EXPECT_EQ(Queue.Num(), 2u);
	Queue.Remove(Second);
	Queue.Remove(Third);
	MarkAsGarbage(Second.Get());
	MarkAsGarbage(Third.Get());
	CollectGarbage();
	EXPECT_EQ(Queue.Num(), 0u);
}


namespace
{
	class FMaterialAsyncCompileTests : public testing::Test
	{
		std::vector<Durin::DObject*> Objects;
	protected:
		static auto SetUpTestSuite() -> void
		{
			InitializeDObjectSystem();
			Durin::FModuleManager::Get().LoadModule("RenderCore");
			ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
		}

		auto TearDown() -> void override
		{
			for (auto* Object : Objects) Durin::MarkAsGarbage(Object);
			Durin::CollectGarbage();
			Durin::FAssetCompilingManager::Get().FinishAllCompilation();
			const auto Drained = Durin::GetMaterialCompilationDiagnostics();
			EXPECT_EQ(Drained.InFlightCount, 0u);
			EXPECT_EQ(Drained.OutstandingConsumerCount, 0u);
			EXPECT_EQ(Drained.PendingPublicationCount, 0u);
		}

		template<class T> auto MakeObject(const char* Name) -> T*
		{
			auto* Object = Durin::NewObject<T>(nullptr, Name);
			Objects.push_back(Object);
			return Object;
		}
	};
}

TEST_F(FMaterialAsyncCompileTests, InitialCompilationQueriesDoNotSelectAnInvalidMaterial)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* Material = MakeObject<DMaterial>("InitialReadiness");
	struct FGate final : DerivedData::ICache {
		mutable FThreadEvent Entered, Release;
		auto Get(const DerivedData::FCacheGetRequest& Request) const -> DerivedData::FCacheGetResult override { return DerivedData::GetCache().Get(Request); }
		auto Put(const DerivedData::FCachePutRequest& Request) const -> DerivedData::FCachePutResult override { Entered.Trigger(); Release.WaitFor(10); return DerivedData::GetCache().Put(Request); }
	};
	auto Cache = std::make_shared<FGate>();
	ShaderBuildPrivate::ResetServiceForTests({.Cache = Cache});
	struct FRestore { std::shared_ptr<FGate> Cache; ~FRestore() { Cache->Release.Trigger(); FAssetCompilingManager::Get().FinishAllCompilation(); ShaderBuildPrivate::ResetServiceForTests(); } } Restore{Cache};
	FMaterialCompilerEnvironment Environment; ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Environment));
	auto Snapshot = SnapshotMaterialCompilerInput(*Material, Environment); ASSERT_TRUE(Snapshot);
	ASSERT_TRUE(Private::FMaterialCompilationLifecycle::Submit(*Material, Snapshot.Snapshot->Input, true));
	ASSERT_TRUE(Cache->Entered.WaitFor(5));
	const auto Before = GetMaterialFallbackDiagnosticsSnapshot();
	EXPECT_TRUE(Material->IsCompiling());
	EXPECT_FALSE(Material->IsComplete());
	const auto Resource = Material->GetMaterialResource();
	EXPECT_FALSE(Resource.HasValidGameThreadShaderMap());
	EXPECT_EQ(GetMaterialFallbackDiagnosticsSnapshot().Get(EMaterialFallbackReason::MaterialDataInvalid),
		Before.Get(EMaterialFallbackReason::MaterialDataInvalid));
	Cache->Release.Trigger();
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_FALSE(Material->IsCompiling());
	EXPECT_TRUE(Material->IsComplete());
	EXPECT_TRUE(Material->GetMaterialResource().HasValidGameThreadShaderMap());
}

TEST_F(FMaterialAsyncCompileTests, SynchronousAndAsynchronousResultsAgreeOnSharedCompilation)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* A = MakeObject<DMaterial>("SharedAsync"); auto* B = MakeObject<DMaterial>("SharedSync");
	A->SetEditCompileMode(EMaterialEditCompileMode::Manual); B->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FAssetCompilingManager::Get().FinishAllCompilation();
	struct FGate final : DerivedData::ICache {
		mutable FThreadEvent Entered, Release;
		auto Get(const DerivedData::FCacheGetRequest& Request) const -> DerivedData::FCacheGetResult override { return DerivedData::GetCache().Get(Request); }
		auto Put(const DerivedData::FCachePutRequest& Request) const -> DerivedData::FCachePutResult override { Entered.Trigger(); Release.WaitFor(10); return DerivedData::GetCache().Put(Request); }
	};
	auto Cache = std::make_shared<FGate>();
	ShaderBuildPrivate::ResetServiceForTests({.Cache = Cache});
	struct FRestore { std::shared_ptr<FGate> Cache; ~FRestore() { Cache->Release.Trigger(); FAssetCompilingManager::Get().FinishAllCompilation(); ShaderBuildPrivate::ResetServiceForTests(); } } Restore{Cache};
	FMaterialCompilerEnvironment Environment; ASSERT_TRUE(BuildDefaultMaterialCompilerEnvironment(Environment));
	auto Snapshot = SnapshotMaterialCompilerInput(*A, Environment); ASSERT_TRUE(Snapshot);
	ASSERT_TRUE(Private::FMaterialCompilationLifecycle::Submit(*A, Snapshot.Snapshot->Input, true));
	ASSERT_TRUE(Cache->Entered.WaitFor(5));
	const auto Joins = MaterialBuildPrivate::GetMaterialProgramCompileStatsForTests().Joins;
	std::jthread Releaser([&] {
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (MaterialBuildPrivate::GetMaterialProgramCompileStatsForTests().Joins == Joins && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
		Cache->Release.Trigger();
	});
	{
		FScopedOfflinePreparation Offline;
		EXPECT_TRUE(Private::FMaterialCompilationLifecycle::Submit(*B, Snapshot.Snapshot->Input, true));
	}
	Releaser.join();
	EXPECT_GT(MaterialBuildPrivate::GetMaterialProgramCompileStatsForTests().Joins, Joins);
	EXPECT_EQ(B->GetMaterialCompileStatus().CacheOutcome, EMaterialCompileCacheOutcome::SingleFlight);
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_EQ(A->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId, B->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId);
}

TEST_F(FMaterialAsyncCompileTests, PublicationDeadlineLeavesQueuedResultsForTheNextPump)
{
	using namespace Durin;
	auto* A = MakeObject<DMaterial>("BudgetA"); auto* B = MakeObject<DMaterial>("BudgetB");
	A->SetEditCompileMode(EMaterialEditCompileMode::Manual); B->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FAssetCompilingManager::Get().FinishAllCompilation();
	ASSERT_TRUE(RequestMaterialRecompile(*A, true)); ASSERT_TRUE(RequestMaterialRecompile(*B, true));
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (GetMaterialCompilationDiagnostics().PendingPublicationCount < 2 && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
	ASSERT_EQ(GetMaterialCompilationDiagnostics().PendingPublicationCount, 2u);
	auto& Manager = FShaderCompilingManager::Get();
	EXPECT_EQ(Manager.ProcessAsyncResults({.Deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1)}).ProcessedCompletionCount, 0u);
	EXPECT_EQ(GetMaterialCompilationDiagnostics().PendingPublicationCount, 2u);
	EXPECT_EQ(Manager.ProcessAsyncResults({.MaximumCompletions = 1}).ProcessedCompletionCount, 1u);
	EXPECT_EQ(GetMaterialCompilationDiagnostics().PendingPublicationCount, 1u);
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_TRUE(A->GetMaterialCompileStatus().IsCurrent()); EXPECT_TRUE(B->GetMaterialCompileStatus().IsCurrent());
}

TEST_F(FMaterialAsyncCompileTests, DistinctTranslationsShareOneProgram)
{
	ASSERT_NO_FATAL_FAILURE(QualifyDistinctTranslationsConvergingToOneProgram());
}

TEST_F(FMaterialAsyncCompileTests, CanceledFlightsCanBeResubmitted)
{
	ASSERT_NO_FATAL_FAILURE(QualifyCanceledFlightResubmission());
}

TEST_F(FMaterialAsyncCompileTests, InstanceVariantsShareOnlyCompatiblePrograms)
{
	ASSERT_NO_FATAL_FAILURE(Durin::Testing::CheckInstanceVariantsForTest(false));
}

TEST_F(FMaterialAsyncCompileTests, InstanceOwnersRetryDeferredCompilation)
{
	ASSERT_NO_FATAL_FAILURE(QualifyInstanceCompilationOwners());
}

TEST_F(FMaterialAsyncCompileTests, SingleFlightRetainedHitsAndDemandedReload)
{
	auto* First = MakeObject<Durin::DMaterial>("AsyncCompileFirst");
	auto* Second = MakeObject<Durin::DMaterial>("AsyncCompileSecond");
	ASSERT_EQ(First->GetMaterialCompileStatus().State,
		Durin::EMaterialCompileState::NeverRequested);
	ASSERT_EQ(Second->GetMaterialCompileStatus().State,
		Durin::EMaterialCompileState::NeverRequested);
	{
		// Single-flight requires overlapping requests. A warm compiler can finish
		// before the second submission unless the fixture holds worker entry.
		const uint32 WorkerCount = Durin::GetTaskSchedulerDiagnostics().WorkerCount;
		Durin::FThreadEvent Started, Release;
		std::atomic<uint32> StartedCount = 0;
		std::vector<Durin::Tasks::FTask> Blockers;
		struct FReleaseWorkers
		{
			Durin::FThreadEvent& Event;
			std::vector<Durin::Tasks::FTask>& Tasks;
			~FReleaseWorkers() { Event.Trigger(); for (const auto& Task : Tasks) Durin::WaitTask(Task); }
		} ReleaseWorkers{Release, Blockers};
		for (uint32 Index = 0; Index < WorkerCount; ++Index)
			Blockers.push_back(Durin::Tasks::FTask(Durin::Tasks::LaunchTask("HoldMaterialSingleFlight", [&] {
				if (StartedCount.fetch_add(1) + 1 == WorkerCount) Started.Trigger();
				Release.WaitFor(2.0);
			})));
		ASSERT_TRUE(Started.WaitFor(1.0));
		ASSERT_TRUE(Durin::RequestMaterialRecompile(*First, true));
		ASSERT_TRUE(Durin::RequestMaterialRecompile(*Second, true));
		ASSERT_EQ(First->GetMaterialCompileStatus().State,
			Durin::EMaterialCompileState::Running);
		ASSERT_EQ(Second->GetMaterialCompileStatus().State,
			Durin::EMaterialCompileState::Running);
		EXPECT_GE(Durin::GetMaterialCompilationDiagnostics()
			.SingleFlightConsumers, 1u);
	}
	ASSERT_TRUE(WaitForMaterialCompile(*First));
	ASSERT_TRUE(WaitForMaterialCompile(*Second));
	const auto InitialProgram = First->GetAcceptedCompiledProgram();
	ASSERT_TRUE(InitialProgram);
	EXPECT_EQ(InitialProgram, Second->GetAcceptedCompiledProgram());
	EXPECT_EQ(Second->GetMaterialCompileStatus().CacheOutcome,
		Durin::EMaterialCompileCacheOutcome::SingleFlight);
	EXPECT_TRUE(First->GetMaterialCompileStatus().IsCurrent());
	ASSERT_TRUE(Durin::RequestMaterialRecompile(*First));
	ASSERT_TRUE(WaitForMaterialCompile(*First));
	EXPECT_EQ(First->GetMaterialCompileStatus().CacheOutcome,
		Durin::EMaterialCompileCacheOutcome::RetainedHit);
	const Durin::FMaterialRenderProxyRef DemandedProxy =
		First->GetRenderProxy();
	const uint64 DemandedGeneration =
		First->GetMaterialCompileStatus().RequestGeneration;
	const uint64 UnusedGeneration =
		Second->GetMaterialCompileStatus().RequestGeneration;
	Durin::NotifyMaterialShaderReload(false);
	Durin::FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_EQ(First->GetMaterialCompileStatus().RequestGeneration,
		DemandedGeneration + 1);
	EXPECT_EQ(Second->GetMaterialCompileStatus().RequestGeneration,
		UnusedGeneration);
	ASSERT_TRUE(WaitForMaterialCompile(*First));
}

TEST_F(FMaterialAsyncCompileTests, LatestGenerationReplacesTheAcceptedProgram)
{
	auto* First = MakeObject<Durin::DMaterial>("LatestGeneration");
	ASSERT_TRUE(Durin::RequestMaterialRecompile(*First));
	ASSERT_TRUE(WaitForMaterialCompile(*First));
	const auto InitialProgram = First->GetAcceptedCompiledProgram();
	ASSERT_TRUE(InitialProgram);
	const uint64 InitialGeneration =
		First->GetMaterialCompileStatus().RequestGeneration;
	auto Validation = EditRoughnessDefault(*First, 0.03125f);
	ASSERT_TRUE(Validation);
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), InitialProgram);
	EXPECT_FALSE(First->GetMaterialCompileStatus().IsCurrent());
	ASSERT_TRUE((Validation = EditRoughnessDefault(*First, 0.0625f)));
	EXPECT_EQ(First->GetMaterialCompileStatus().RequestGeneration,
		InitialGeneration + 2);
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), InitialProgram);
	ASSERT_TRUE(WaitForMaterialCompile(*First))
		<< "state=" << static_cast<uint32>(First->GetMaterialCompileStatus().State)
		<< " diagnostic=" << (First->GetMaterialCompileDiagnostics().empty()
			? std::string("<none>")
			: First->GetMaterialCompileDiagnostics().front().Message);
	ASSERT_TRUE(First->GetAcceptedCompiledProgram());
	EXPECT_NE(First->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId,
		InitialProgram->GetShaderMap().ShaderMapId);
	EXPECT_TRUE(First->GetMaterialCompileStatus().IsCurrent());
}

TEST_F(FMaterialAsyncCompileTests, FailedAndStaleResultsPreserveGenerationBoundaries)
{
	auto* First = MakeObject<Durin::DMaterial>("FailedGeneration");
	ASSERT_TRUE(Durin::RequestMaterialRecompile(*First));
	ASSERT_TRUE(WaitForMaterialCompile(*First));
	const auto InitialProgram = First->GetAcceptedCompiledProgram();
	ASSERT_TRUE(InitialProgram);
	auto ParameterValidation = Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*First);

	ASSERT_TRUE(ParameterValidation);
	auto* PendingInstance = MakeObject<Durin::DMaterialInstance>("PendingParameterEdit");
	ASSERT_TRUE(PendingInstance->SetParent(First));
	EXPECT_TRUE(First->GetAcceptedCompiledProgram()->GetShaderMap().ActiveParameters.empty());
	ASSERT_TRUE(PendingInstance->SetVectorParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.7, 0.2, 0.4)));
	ASSERT_TRUE(WaitForMaterialCompile(*First));
	ASSERT_TRUE(First->SetVectorParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(0.2, 0.6, 0.8)));
	const auto LastKnownGood = First->GetAcceptedCompiledProgram();
	Durin::FMaterialStaticProperties FailedProperties =
		First->GetStaticProperties();
	FailedProperties.BlendMode = Durin::EMaterialBlendMode::Translucent;
	FailedProperties.bTwoSided = !FailedProperties.bTwoSided;
	ASSERT_TRUE(First->SetStaticProperties(FailedProperties));
	ASSERT_TRUE((ParameterValidation = First->SetMaterialExpressions({}, {})));
	const Durin::FMaterialCompileStatus Pending =
		First->GetMaterialCompileStatus();
	Durin::FAssetCompilingManager::Get().MarkCompilationAsCanceled(*First);
	const std::vector<Durin::FMaterialProgramDiagnostic> BeforeDiagnostics(
		First->GetMaterialCompileDiagnostics().begin(), First->GetMaterialCompileDiagnostics().end());
	const Durin::FMaterialProgramDiagnostic FailureDiagnostic{
		.LocationKind = Durin::EMaterialProgramDiagnosticLocationKind::Node,
		.NodeId = Durin::FGuid::NewGuid(), .Message = "Material compile admission was rejected."};
	Durin::FMaterialCompileResult Failed{
		.Owner = Durin::FWeakObjectPtr(First),
		.AuthoredRevision = Pending.AuthoredRevision,
		.Generation = Pending.RequestGeneration,
		.DependencyRevision = Pending.DependencyRevision,
		.ProgramIdentity = Pending.RequestedIdentity,
		.Target = Pending.Target,
		.State = Durin::EMaterialCompileState::Failed,
		.Diagnostics = {FailureDiagnostic},
	};
	// An obsolete failure cannot retire a newer request's visible generation.
	auto StaleFailure = Failed;
	--StaleFailure.Generation;
	EXPECT_FALSE(Durin::Private::FMaterialCompilationLifecycle::Admit(
		*First, std::move(StaleFailure)));
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), LastKnownGood);
	EXPECT_TRUE(std::ranges::equal(First->GetMaterialCompileDiagnostics(), BeforeDiagnostics));
	EXPECT_FALSE(Durin::Private::FMaterialCompilationLifecycle::Admit(
		*First, std::move(Failed)));
	EXPECT_FALSE(First->GetAcceptedCompiledProgram());
	EXPECT_EQ(First->GetMaterialCompileStatus().State,
		Durin::EMaterialCompileState::Failed);
	EXPECT_FALSE(First->GetMaterialCompileStatus().IsCurrent());
	ASSERT_EQ(First->GetMaterialCompileDiagnostics().size(), 1u);
	EXPECT_EQ(First->GetMaterialCompileDiagnostics().front(), FailureDiagnostic);
	ExpectColorNear(GetMaterialBinding(Durin::Testing::CaptureMaterialRenderData(*First)).BaseColor,
		GetMaterialBinding(Durin::GetErrorMaterialRenderData()).BaseColor);

	Durin::FMaterialCompileResult Stale{
		.Owner = Durin::FWeakObjectPtr(First),
		.AuthoredRevision = Pending.AuthoredRevision,
		.Generation = Pending.RequestGeneration - 1,
		.DependencyRevision = Pending.DependencyRevision,
		.ProgramIdentity = LastKnownGood->GetShaderMap().ShaderMapId,
		.Target = Pending.Target,
		.State = Durin::EMaterialCompileState::Ready,
		.CompiledProgram = LastKnownGood,
	};
	EXPECT_FALSE(Durin::Private::FMaterialCompilationLifecycle::Admit(
		*First, std::move(Stale)));
	EXPECT_FALSE(First->GetAcceptedCompiledProgram());
	Durin::FMaterialCompileResult WrongTarget{
		.Owner = Durin::FWeakObjectPtr(First),
		.AuthoredRevision = Pending.AuthoredRevision,
		.Generation = Pending.RequestGeneration,
		.DependencyRevision = Pending.DependencyRevision,
		.ProgramIdentity = LastKnownGood->GetShaderMap().ShaderMapId,
		.Target = "wrong-target",
		.State = Durin::EMaterialCompileState::Ready,
		.CompiledProgram = LastKnownGood,
	};
	EXPECT_FALSE(Durin::Private::FMaterialCompilationLifecycle::Admit(
		*First, std::move(WrongTarget)));
	Durin::FMaterialCompileResult WrongDependency{
		.Owner = Durin::FWeakObjectPtr(First),
		.AuthoredRevision = Pending.AuthoredRevision,
		.Generation = Pending.RequestGeneration,
		.DependencyRevision = Pending.DependencyRevision + 1,
		.ProgramIdentity = LastKnownGood->GetShaderMap().ShaderMapId,
		.Target = Pending.Target,
		.State = Durin::EMaterialCompileState::Ready,
		.CompiledProgram = LastKnownGood,
	};
	EXPECT_FALSE(Durin::Private::FMaterialCompilationLifecycle::Admit(
		*First, std::move(WrongDependency)));
	EXPECT_FALSE(First->GetAcceptedCompiledProgram());

	ASSERT_EQ(First->GetMaterialCompileDiagnostics().size(), 1u);
	EXPECT_EQ(First->GetMaterialCompileDiagnostics().front(), FailureDiagnostic);

	// Recovery replaces the error terminal, including after stale results were rejected.
	ASSERT_TRUE(Durin::RequestMaterialRecompile(*First));
	EXPECT_TRUE(First->GetMaterialCompileDiagnostics().empty());
	ASSERT_TRUE(WaitForMaterialCompile(*First));
	EXPECT_TRUE(First->GetAcceptedCompiledProgram());
	EXPECT_TRUE(First->GetMaterialCompileStatus().IsCurrent());
}

TEST_F(FMaterialAsyncCompileTests, PendingSchemaReplacementRetainsBindingsUntilFailure)
{
	// Pending replacements retain deleted declarations; failed owners retire them.
	auto* Root = MakeObject<Durin::DMaterial>("RetainedDeclarationRoot");
	ASSERT_TRUE(Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Root));
	ASSERT_TRUE(WaitForMaterialCompile(*Root));
	ASSERT_NE(Root, nullptr);
	ASSERT_NE(Root->GetAcceptedCompiledProgram(), nullptr);
	auto* Instance = MakeObject<Durin::DMaterialInstance>("RetainedDeclarationInstance");
	ASSERT_TRUE(Instance->SetParent(Root));
	ASSERT_TRUE(WaitForMaterialCompile(*Instance));
	ASSERT_TRUE(Root->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::MetallicName(), 0.65f));
	ASSERT_TRUE(Instance->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::MetallicName(), 0.9f));
	Durin::FMaterialParameterDefinition Definition;
	Definition.Id = Durin::FGuid::NewGuid();
	Definition.Name = "IndependentAmount";
	Durin::Testing::FTestMaterialExpressionGraph Graph;
	const std::array Definitions{Definition};
	auto& Node = Graph.Add(Durin::EMaterialProgramOpcode::Parameter, Durin::EMaterialProgramValueType::Float,
		{}, Definition.Id, {}, Definitions);
	Graph.Outputs.Roughness = {Node.Id};
	Durin::FThreadEvent Started, Release;
	std::atomic<uint32> StartedCount = 0;
	std::vector<Durin::Tasks::FTask> Blockers;
	const auto WorkerCount = Durin::GetTaskSchedulerDiagnostics().WorkerCount;
	struct FReleaseWorkers
	{
		Durin::FThreadEvent& Release;
		std::vector<Durin::Tasks::FTask>& Tasks;
		~FReleaseWorkers() { Release.Trigger(); for (const auto& Task : Tasks) Durin::WaitTask(Task); }
	};
	{
		FReleaseWorkers ReleaseWorkers{Release, Blockers};
		for (uint32 Index = 0; Index < WorkerCount; ++Index)
			Blockers.push_back(Durin::Tasks::FTask(Durin::Tasks::LaunchTask("HoldRetainedSchemaCompile", [&] {
				if (StartedCount.fetch_add(1) + 1 == WorkerCount) Started.Trigger();
				Release.WaitFor(10.0);
			})));
		ASSERT_TRUE(Started.WaitFor(2.0));
		ASSERT_TRUE(Graph.Apply(*Root));
		EXPECT_FLOAT_EQ(GetMaterialBinding(Durin::Testing::CaptureMaterialRenderData(*Root)).Metallic, 0.65f);
		EXPECT_FLOAT_EQ(GetMaterialBinding(Durin::Testing::CaptureMaterialRenderData(*Instance)).Metallic, 0.9f);
		const auto Pending = Root->GetMaterialCompileStatus();
		Durin::FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Root);
		Durin::FMaterialCompileResult Failed{
			.Owner = Durin::FWeakObjectPtr(Root),
			.AuthoredRevision = Pending.AuthoredRevision,
			.Generation = Pending.RequestGeneration,
			.DependencyRevision = Pending.DependencyRevision,
			.ProgramIdentity = Pending.RequestedIdentity,
			.Target = Pending.Target,
			.State = Durin::EMaterialCompileState::Failed};
		EXPECT_FALSE(Durin::Private::FMaterialCompilationLifecycle::Admit(*Root, std::move(Failed)));
	}
	EXPECT_FALSE(Root->GetAcceptedCompiledProgram());
	ExpectColorNear(GetMaterialBinding(Durin::Testing::CaptureMaterialRenderData(*Root)).BaseColor,
		GetMaterialBinding(Durin::GetErrorMaterialRenderData()).BaseColor);
	// Each variant owns its result: the child's successful compilation can still publish.
	ASSERT_TRUE(WaitForMaterialCompile(*Instance));
	EXPECT_TRUE(Instance->GetAcceptedCompiledProgram());
	ASSERT_TRUE(Durin::RequestMaterialRecompile(*Root));
	ASSERT_TRUE(WaitForMaterialCompile(*Root));
	EXPECT_EQ(Root->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId,
		Instance->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId);
}

TEST_F(FMaterialAsyncCompileTests, DetachedFanoutRetiresCanceledAndFailedConsumers)
{
	ASSERT_NO_FATAL_FAILURE(Durin::Testing::CheckDetachedMaterialResultFanout(false));
}

TEST_F(FMaterialAsyncCompileTests, EditSchedulingHonorsManualAndAutomaticModes)
{
	ASSERT_NO_FATAL_FAILURE(QualifyEditScheduling());
}

TEST_F(FMaterialAsyncCompileTests, EditingSessionAppliesOnlyAcceptedResults)
{
	ASSERT_NO_FATAL_FAILURE(QualifyMaterialEditingSessionAsync());
}

TEST_F(FMaterialAsyncCompileTests, FunctionEditsInvalidateOwnersAndRejectStaleResults)
{
	ASSERT_NO_FATAL_FAILURE(QualifyMaterialFunctionCompilationAsync());
}


namespace
{
auto QualifyInstanceCompilationOwners() -> void
{
	struct FFixtureScope
	{
		std::vector<Durin::DObject*> Objects;
		~FFixtureScope()
		{
			for (auto* Object : Objects) Durin::MarkAsGarbage(Object);
			Durin::CollectGarbage();
			Durin::FAssetCompilingManager::Get().FinishAllCompilation();
		}
	} Scope;
	auto* Root = Durin::NewObject<Durin::DMaterial>(nullptr, "InstanceCompileRoot");
	Scope.Objects.push_back(Root);
	ASSERT_TRUE(Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Root));
	ASSERT_TRUE(WaitForMaterialCompile(*Root));
	auto* Child = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "InstanceCompileChild");
	auto* Grandchild = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "InstanceCompileGrandchild");
	Scope.Objects.push_back(Child);
	Scope.Objects.push_back(Grandchild);
	ASSERT_TRUE(Child->SetParent(Root));
	ASSERT_TRUE(Grandchild->SetParent(Child));
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Grandchild);
	ASSERT_TRUE(Grandchild->GetMaterialCompileStatus().IsCurrent());
	EXPECT_EQ(Grandchild->GetMaterialCompileStatus().CompiledIdentity,
		Root->GetMaterialCompileStatus().CompiledIdentity);
	EXPECT_EQ(Grandchild->GetMaterialCompileStatus().CacheOutcome,
		Durin::EMaterialCompileCacheOutcome::RetainedHit);
	Durin::FMaterialPropertyOverrides Overrides;
	Overrides.bOverrideBlendMode = true;
	Overrides.Values.BlendMode = Durin::EMaterialBlendMode::Masked;
	ASSERT_TRUE(Child->SetPropertyOverrides(Overrides));
	ASSERT_TRUE(WaitForMaterialCompile(*Grandchild));
	ASSERT_TRUE(WaitForMaterialCompile(*Child));
	EXPECT_EQ(Child->GetMaterialCompileStatus().CompiledIdentity,
		Grandchild->GetMaterialCompileStatus().CompiledIdentity);
	EXPECT_NE(Child->GetMaterialCompileStatus().CompiledIdentity,
		Root->GetMaterialCompileStatus().CompiledIdentity);
	// An inactive root cutoff remains active in a masked descendant.
	const auto FirstMaskedIdentity = Child->GetMaterialCompileStatus().CompiledIdentity;
	auto Properties = Root->GetStaticProperties();
	Properties.OpacityMaskThreshold = 0.75f;
	ASSERT_TRUE(Root->SetStaticProperties(Properties));
	ASSERT_TRUE(WaitForMaterialCompile(*Grandchild));
	EXPECT_NE(Grandchild->GetMaterialCompileStatus().CompiledIdentity, FirstMaskedIdentity);
	const auto BeforeDynamic = Durin::GetMaterialCompilationDiagnostics().AcceptedRequests;
	ASSERT_TRUE(Child->SetScalarParameterValue(Durin::AssetForge::Builtins::MaterialParameters::MetallicName(), 0.42f));
	Overrides.bOverrideTwoSided = true;
	Overrides.Values.bTwoSided = true;
	ASSERT_TRUE(Child->SetPropertyOverrides(Overrides));
	EXPECT_EQ(Durin::GetMaterialCompilationDiagnostics().AcceptedRequests, BeforeDynamic);
	const auto ChainRevision = Grandchild->GetMaterialCompileStatus().ParentChainRevision;
	ASSERT_TRUE(Grandchild->SetParent(Root));
	ASSERT_TRUE(WaitForMaterialCompile(*Grandchild));
	EXPECT_GT(Grandchild->GetMaterialCompileStatus().ParentChainRevision, ChainRevision);
	EXPECT_EQ(Grandchild->GetMaterialCompileStatus().CompiledIdentity,
		Root->GetMaterialCompileStatus().CompiledIdentity);
	ASSERT_TRUE(Grandchild->SetParent(nullptr));
	EXPECT_EQ(Grandchild->GetMaterialCompileStatus().State, Durin::EMaterialCompileState::Failed);
	EXPECT_FALSE(Grandchild->GetMaterialCompileDiagnostics().empty());
	// Retained results still consume bounded mailbox slots. Overflow must retry
	// through aggregate finish without retaining detached requests or objects.
	Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	std::vector<Durin::DMaterialInstance*> Fanout;
	for (uint32 Index = 0; Index < Durin::MaterialCompileMaxConsumers + 8; ++Index)
	{
		auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr,
			Durin::FName(std::format("InstanceFanout{}", Index)));
		Scope.Objects.push_back(Instance);
		Fanout.push_back(Instance);
		ASSERT_TRUE(Instance->SetParent(Root));
	}
	EXPECT_EQ(Fanout.back()->GetMaterialCompileStatus().State, Durin::EMaterialCompileState::Deferred);
	Durin::FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Fanout.back());
	EXPECT_EQ(Fanout.back()->GetMaterialCompileStatus().State, Durin::EMaterialCompileState::Canceled);
	Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	for (size_t Index = 0; Index + 1 < Fanout.size(); ++Index)
	{
		EXPECT_TRUE(Fanout[Index]->GetMaterialCompileStatus().IsCurrent());
		EXPECT_EQ(Fanout[Index]->GetMaterialCompileStatus().CompiledIdentity,
			Root->GetMaterialCompileStatus().CompiledIdentity);
	}
	EXPECT_EQ(Durin::GetMaterialCompilationDiagnostics().OutstandingConsumerCount, 0u);
}


}

auto QualifyMaterialFunctionCompilationAsync() -> void
{
	using namespace Durin;
	struct FFixture
	{
		std::vector<DObject*> Objects;
		~FFixture()
		{
			for (auto* Object : Objects) MarkAsGarbage(Object);
			CollectGarbage();
		}
	} Fixture;
	struct FHoldWorkers
	{
		FThreadEvent Started, Release;
		std::atomic<uint32> StartedCount = 0;
		std::vector<Tasks::FTask> Tasks;
		auto Hold() -> bool
		{
			const auto Count = GetTaskSchedulerDiagnostics().WorkerCount;
			for (uint32 Index = 0; Index < Count; ++Index)
				Tasks.push_back(Durin::Tasks::FTask(Durin::Tasks::LaunchTask("HoldFunctionCompile", [this, Count] {
					if (StartedCount.fetch_add(1) + 1 == Count) Started.Trigger();
					Release.WaitFor(10.0);
				})));
			return Started.WaitFor(2.0);
		}
		~FHoldWorkers() { Release.Trigger(); for (const auto& Task : Tasks) WaitTask(Task); }
	};
	auto* Leaf = NewObject<DMaterialFunction>(nullptr, "AsyncFunctionLeaf");
	auto* Wrapper = NewObject<DMaterialFunction>(nullptr, "AsyncFunctionWrapper");
	auto* First = NewObject<DMaterial>(nullptr, "AsyncFunctionFirst");
	auto* Second = NewObject<DMaterial>(nullptr, "AsyncFunctionSecond");
	Fixture.Objects = {First, Second, Wrapper, Leaf};
	const auto LeafOutput = Leaf->GetFunctionSignature().Outputs[0];
	std::vector<TStrongObjectPtr<DMaterialExpression>> Body;
	for (const auto& Expression : Wrapper->GetExpressionCollection().Expressions)
		Body.emplace_back(DuplicateObject(Expression.Get(), nullptr, NAME_None).value());
	const FGuid NestedCall{71, 2, 3, 1};
	TStrongObjectPtr<DMaterialExpressionFunctionCall> Nested(NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None));
	Nested->Id = NestedCall; Nested->Function = Leaf; Nested->Outputs = {{LeafOutput.Id, LeafOutput.Type}};
	for (const auto& Expression : Body)
		if (auto* Terminal = Cast<DMaterialExpressionFunctionOutput>(Expression.Get()))
			Terminal->Source = {.ExpressionId = NestedCall, .OutputId = LeafOutput.Id};
	std::vector<DMaterialExpression*> BodyValues;
	for (const auto& Expression : Body) BodyValues.push_back(Expression.Get());
	BodyValues.push_back(Nested.Get());
	ASSERT_TRUE(Wrapper->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(Wrapper->GetFunctionSignature(), BodyValues)));
	const auto Output = Wrapper->GetFunctionSignature().Outputs[0];
	for (auto* Material : {First, Second})
	{
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		const FGuid Call{71, 2, 3, Material == First ? 2u : 3u};
		TStrongObjectPtr<DMaterialExpressionFunctionCall> Expression(NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None));
		Expression->Id = Call; Expression->Function = Wrapper; Expression->Outputs = {{Output.Id, Output.Type}};
		const std::array<DMaterialExpression*, 1> Expressions{Expression.Get()};
		FMaterialExpressionSurfaceOutputs Outputs;
		Outputs.Surface = {.ExpressionId = Call, .OutputId = Output.Id}; Outputs.bUseMaterialAttributes = true;
		ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
	}
	{
		FHoldWorkers Hold;
		ASSERT_TRUE(Hold.Hold());
		ASSERT_TRUE(RequestMaterialRecompile(*First, true));
		ASSERT_TRUE(RequestMaterialRecompile(*Second, true));
		EXPECT_EQ(Second->GetMaterialCompileStatus().State, EMaterialCompileState::Running);
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*First);
		EXPECT_EQ(First->GetMaterialCompileStatus().State, EMaterialCompileState::Canceled);
	}
	ASSERT_TRUE(WaitForMaterialCompile(*Second));
	EXPECT_EQ(Second->GetMaterialCompileStatus().CacheOutcome, EMaterialCompileCacheOutcome::SingleFlight);
	EXPECT_EQ(First->GetMaterialCompileStatus().State, EMaterialCompileState::Canceled);
	ASSERT_TRUE(First->CompileEdits());
	ASSERT_TRUE(WaitForMaterialCompile(*First));
	const auto Accepted = First->GetAcceptedCompiledProgram();
	const auto SecondAccepted = Second->GetAcceptedCompiledProgram();
	ASSERT_TRUE(Accepted);
	ASSERT_TRUE(SecondAccepted);
	EXPECT_EQ(Accepted->ShaderMap, SecondAccepted->ShaderMap);
	auto LeafSignature = Leaf->GetFunctionSignature();
	const auto ApplyLeafSignature = [&] {
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Leaf->GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
		return Leaf->SetFunctionExpressions(Durin::Testing::WithFunctionPorts(LeafSignature, Expressions));
	};
	{
		FHoldWorkers Hold;
		ASSERT_TRUE(Hold.Hold());
		LeafSignature.Inputs[0].Default.Surface.RoughnessDefault.X = 0.381f;
		ASSERT_TRUE(ApplyLeafSignature());
		ASSERT_TRUE(First->CompileEdits());
		ASSERT_TRUE(Second->CompileEdits());
		LeafSignature.Inputs[0].Default.Surface.RoughnessDefault.X = 0.482f;
		ASSERT_TRUE(ApplyLeafSignature());
		EXPECT_EQ(First->GetAcceptedCompiledProgram(), Accepted);
	}
	FAssetCompilingManager::Get().FinishAllCompilation();
	for (auto* Material : {First, Second})
	{
		EXPECT_EQ(Material->GetMaterialCompileStatus().State, EMaterialCompileState::NeedsCompile);
		EXPECT_EQ(Material->GetAcceptedCompiledProgram(), Material == First ? Accepted : SecondAccepted);
		ASSERT_TRUE(Material->CompileEdits());
		ASSERT_TRUE(WaitForMaterialCompile(*Material));
		EXPECT_NE(Material->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId, Accepted->GetShaderMap().ShaderMapId);
	}
	EXPECT_EQ(First->GetAcceptedCompiledProgram()->ShaderMap, Second->GetAcceptedCompiledProgram()->ShaderMap);
}

TEST(FMaterialCompileLifecycleTests,
	AcceptedProgramStatisticsAndLoadedFamilyQueryAreDetachedAndSideEffectFree)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FModuleManager::Get().LoadModule("RenderCore");
	auto* AbsentOwner = NewObject<DMaterialInstance>(nullptr, "StatisticsAbsent");
	EXPECT_EQ(GetMaterialProgramStatistics(*AbsentOwner).State,
		EMaterialProgramStatisticsState::Absent);
	auto* Root = NewObject<DMaterial>(nullptr, "StatisticsRoot");
	ASSERT_TRUE(Testing::MakePBRMaterialExpressionsForTest().Apply(*Root));
	ASSERT_TRUE(WaitForMaterialCompile(*Root));
	ASSERT_TRUE(Root->GetAcceptedCompiledProgram());
	const auto Program = Root->GetAcceptedCompiledProgram();
	const uint64 CompileGeneration = Root->GetMaterialCompileStatus().RequestGeneration;
	const FMaterialProgramStatistics Current = GetMaterialProgramStatistics(*Root);
	EXPECT_EQ(Current.State, EMaterialProgramStatisticsState::Current);
	EXPECT_EQ(Current.ProgramIdentity, Program->GetShaderMap().ShaderMapId);
	EXPECT_EQ(Current.NormalizedIRNodeCount, Program->GetDebugData().IR.Nodes.size());
	EXPECT_EQ(Current.ActiveParameterCount, Program->GetShaderMap().ActiveParameters.size());
	EXPECT_EQ(Current.UniformPayloadBytes, Program->GetShaderMap().Layout.UniformPayloadSize);
	EXPECT_EQ(Current.GeneratedSourceBytes, Program->GetDebugData().GeneratedSource.size());
	EXPECT_EQ(Current.CompiledShaderCount, Program->GetShaderMap().CompiledShaders.size());
	EXPECT_EQ(Current.DependencyCount, Program->GetShaderMap().Dependencies.size());
	uint64 ExpectedCodeBytes = 0;
	for (const auto& Shader : Program->GetShaderMap().CompiledShaders)
		ExpectedCodeBytes += !Shader.Code.IsEmpty() ? Shader.Code.size() : 0;
	EXPECT_EQ(Current.CompiledCodeBytes, ExpectedCodeBytes);
	EXPECT_FALSE(Current.bSizeOverflow);

	auto* Same = NewObject<DMaterialInstance>(nullptr, "StatisticsSame");
	ASSERT_TRUE(Same->SetParent(Root));
	auto* Variant = NewObject<DMaterialInstance>(nullptr, "StatisticsVariant");
	ASSERT_TRUE(Variant->SetParent(Root));
	FMaterialPropertyOverrides Overrides;
	Overrides.bOverrideBlendMode = true;
	Overrides.Values.BlendMode = EMaterialBlendMode::Masked;
	ASSERT_TRUE(Variant->SetPropertyOverrides(Overrides));
	ASSERT_TRUE(WaitForMaterialCompile(*Same));
	ASSERT_TRUE(WaitForMaterialCompile(*Variant));
	auto* Dynamic = DMaterialInstance::CreateDynamic(Root, nullptr, "StatisticsDynamic");
	ASSERT_TRUE(Dynamic);
	EXPECT_EQ(GetMaterialProgramStatistics(*Dynamic).State,
		EMaterialProgramStatisticsState::Current);
	const FLoadedMaterialFamilyStatistics Family =
		GetLoadedMaterialFamilyStatistics(*Same);
	EXPECT_EQ(Family.LoadedOwnerCount, 3u);
	EXPECT_EQ(Family.AcceptedOwnerCount, 3u);
	EXPECT_EQ(Family.DistinctProgramCount, 2u);
	EXPECT_EQ(Family.DistinctStaticConfigurationCount, 2u);
	EXPECT_EQ(Family.PendingOwnerCount, 0u);
	EXPECT_EQ(Family.FailedOwnerCount, 0u);
	EXPECT_EQ(Root->GetMaterialCompileStatus().RequestGeneration, CompileGeneration);

	Root->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(EditRoughnessDefault(*Root, 0.03125f));
	const FMaterialProgramStatistics Stale = GetMaterialProgramStatistics(*Root);
	EXPECT_EQ(Stale.State, EMaterialProgramStatisticsState::LastKnownGood);
	EXPECT_EQ(Stale.ProgramIdentity, Current.ProgramIdentity);
	EXPECT_EQ(Root->GetMaterialCompileStatus().State, EMaterialCompileState::NeedsCompile);

	MarkAsGarbage(Dynamic);
	MarkAsGarbage(Variant);
	MarkAsGarbage(Same);
	MarkAsGarbage(Root);
	MarkAsGarbage(AbsentOwner);
	CollectGarbage();
}

TEST(FMaterialCompileLifecycleTests,
	CookedProgramRoundTripIsDeterministicBoundedAndTargetQualified)
{
	Durin::Testing::FScopedRHIBackendOverride Backend("vulkan");
	InitializeDObjectSystem();
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	auto* Material = Durin::NewObject<Durin::DMaterial>(
		nullptr, "CookedProgramRoundTrip");
	auto Validation = Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Material);
	ASSERT_TRUE(Validation);
	ASSERT_TRUE(WaitForMaterialCompile(*Material));
	ASSERT_TRUE(Material->GetAcceptedCompiledProgram());

	Durin::FByteBuffer FirstBytes;
	Durin::FByteBuffer SecondBytes;
	std::expected<void, std::string> Error;
	ASSERT_TRUE((Error = Durin::Testing::EncodeMaterialCookedProgramFamilyForTest(
		*Material->GetAcceptedCompiledProgram(), Material->GetStaticProperties(),
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game, FirstBytes))) << Error.error();
	ASSERT_TRUE((Error = Durin::Testing::EncodeMaterialCookedProgramFamilyForTest(
		*Material->GetAcceptedCompiledProgram(), Material->GetStaticProperties(),
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game, SecondBytes))) << Error.error();
	EXPECT_EQ(FirstBytes, SecondBytes);
	EXPECT_LE(FirstBytes.size(), Durin::MaterialCookedProgramMaxPayloadBytes);

	Durin::FMaterialStaticProperties DecodedProperties;
	std::shared_ptr<const Durin::FMaterialCompilerResult> DecodedProgram;
	ASSERT_TRUE((Error = Durin::Testing::DecodeMaterialCookedProgramFamilyForTest(
		FirstBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		*Material->GetAcceptedCompiledProgram(), DecodedProperties,
		DecodedProgram))) << Error.error();
	ASSERT_TRUE(DecodedProgram);
	EXPECT_EQ(DecodedProgram->GetShaderMap().ShaderMapId,
		Material->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId);
	EXPECT_EQ(DecodedProperties, Material->GetStaticProperties());
	EXPECT_EQ(DecodedProgram->GetShaderMap().ActiveParameters,
		Material->GetAcceptedCompiledProgram()->GetShaderMap().ActiveParameters);
	EXPECT_EQ(DecodedProgram->GetShaderMap().ActiveParameters.size(), 36u);
	std::vector<Durin::FMaterialCompilerResult> FamilyPrograms;
	for (const auto Quality : {Durin::EMaterialQualityLevel::Low,
		Durin::EMaterialQualityLevel::High})
		for (const auto Feature : {Durin::ERHIFeatureLevel::ES3_1,
			Durin::ERHIFeatureLevel::SM5, Durin::ERHIFeatureLevel::SM6})
		{
			auto Variant = *Material->GetAcceptedCompiledProgram();
			Variant.Quality = Quality;
			Variant.FeatureLevel = Feature;
			FamilyPrograms.push_back(std::move(Variant));
		}
	std::vector<const Durin::FMaterialCompilerResult*> FamilyPointers;
	for (const auto& Program : FamilyPrograms) FamilyPointers.push_back(&Program);
	Durin::FByteBuffer FamilyBytes;
	ASSERT_TRUE((Error = Durin::EncodeMaterialCookedProgramFamily(FamilyPointers,
		Material->GetStaticProperties(), Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game, FamilyBytes)))
		<< Error.error();
	EXPECT_LT(FamilyBytes.size(), FirstBytes.size() * 2);
	ASSERT_TRUE((Error = Durin::DecodeMaterialCookedProgramFamily(FamilyBytes,
		Durin::ECookTargetPlatform::Win64, Durin::ECookTargetProfile::Game,
		Durin::EMaterialQualityLevel::Low, Durin::ERHIFeatureLevel::SM6, {},
		DecodedProperties, DecodedProgram))) << Error.error();
	EXPECT_EQ(DecodedProgram->Quality, Durin::EMaterialQualityLevel::Low);
	EXPECT_EQ(DecodedProgram->FeatureLevel, Durin::ERHIFeatureLevel::SM6);
	const std::array MissingStatic{
		Durin::FMaterialCompilerEnvironment::FStaticBoolValue{
			Durin::FGuid::NewGuid(), true}};
	EXPECT_FALSE((Error = Durin::DecodeMaterialCookedProgramFamily(FamilyBytes,
		Durin::ECookTargetPlatform::Win64, Durin::ECookTargetProfile::Game,
		Durin::EMaterialQualityLevel::Low, Durin::ERHIFeatureLevel::SM6,
		MissingStatic, DecodedProperties, DecodedProgram)));
	ASSERT_EQ(DecodedProgram->GetShaderMap().CompiledShaders.size(),
		Material->GetAcceptedCompiledProgram()->GetShaderMap().CompiledShaders.size());
	for (size_t Index = 0; Index < DecodedProgram->GetShaderMap().CompiledShaders.size(); ++Index)
	{
		const Durin::FShaderCode& Decoded =
			DecodedProgram->GetShaderMap().CompiledShaders[Index];
		const Durin::FShaderCode& Source =
			Material->GetAcceptedCompiledProgram()->GetShaderMap().CompiledShaders[Index];
		EXPECT_EQ(Decoded.Frequency, Source.Frequency);
		EXPECT_EQ(Decoded.SourceEntryPoint, Source.SourceEntryPoint);
		EXPECT_EQ(Decoded.BinaryEntryPoint, Source.BinaryEntryPoint);
		EXPECT_EQ(Decoded.CodeHash, Source.CodeHash);
		ASSERT_TRUE(!Decoded.Code.IsEmpty());
		ASSERT_TRUE(!Source.Code.IsEmpty());
		EXPECT_TRUE(std::ranges::equal(Decoded.Code.GetBytes(), Source.Code.GetBytes()));
	}
	EXPECT_TRUE(DecodedProgram->GetDebugData().IR.Nodes.empty());
	EXPECT_TRUE(DecodedProgram->GetDebugData().GeneratedSource.empty());
	ASSERT_FALSE(DecodedProgram->GetShaderMap().ActiveParameters.empty());
	for (int Corruption = 0; Corruption < 3; ++Corruption)
	{
		auto Invalid = *DecodedProgram;
		auto InvalidArtifact = std::make_shared<Durin::FMaterialShaderMap>(DecodedProgram->GetShaderMap());
		Invalid.ShaderMap = InvalidArtifact;
		if (Corruption == 0) InvalidArtifact->ActiveParameters.push_back(InvalidArtifact->ActiveParameters.front());
		if (Corruption == 1) InvalidArtifact->ActiveParameters.front().Id = {};
		if (Corruption == 2) InvalidArtifact->ActiveParameters.front().Type =
			static_cast<Durin::EMaterialParameterType>(255);
		Durin::FByteBuffer InvalidBytes;
		EXPECT_FALSE((Error = Durin::Testing::EncodeMaterialCookedProgramFamilyForTest(Invalid, DecodedProperties,
			Durin::ECookTargetPlatform::Win64, Durin::ECookTargetProfile::Game,
			InvalidBytes)));
	}

	EXPECT_FALSE((Error = Durin::Testing::DecodeMaterialCookedProgramFamilyForTest(
		FirstBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::EditorValidation,
		*Material->GetAcceptedCompiledProgram(), DecodedProperties, DecodedProgram)));
	Durin::FMaterialCompilerResult WrongEnvironment =
		*Material->GetAcceptedCompiledProgram();
	Durin::FByteBuffer WrongEnvironmentBytes;
	auto WrongMap = std::make_shared<Durin::FMaterialShaderMap>(WrongEnvironment.GetShaderMap());
	WrongMap->Target = "wrong-target";
	WrongEnvironment.ShaderMap = WrongMap;
	ASSERT_TRUE((Error = Durin::Testing::EncodeMaterialCookedProgramFamilyForTest(
		WrongEnvironment, Material->GetStaticProperties(),
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		WrongEnvironmentBytes))) << Error.error();
	EXPECT_FALSE((Error = Durin::Testing::DecodeMaterialCookedProgramFamilyForTest(
		WrongEnvironmentBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		WrongEnvironment, DecodedProperties, DecodedProgram)));
	Durin::FByteBuffer OldSchemaBytes = FirstBytes;
	ASSERT_TRUE(Durin::Testing::SetMaterialCookedProgramFamilyVersionForTest(
		OldSchemaBytes, 2));
	EXPECT_FALSE((Error = Durin::Testing::DecodeMaterialCookedProgramFamilyForTest(
		OldSchemaBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		*Material->GetAcceptedCompiledProgram(), DecodedProperties, DecodedProgram)));
	Durin::FByteBuffer TrailingBytes = FirstBytes;
	TrailingBytes.push_back(std::byte{0});
	EXPECT_FALSE((Error = Durin::Testing::DecodeMaterialCookedProgramFamilyForTest(
		TrailingBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		*Material->GetAcceptedCompiledProgram(), DecodedProperties, DecodedProgram)));

	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}

TEST(FMaterialCompileLifecycleTests, MetalCookedProgramPreservesStageBindingMaps)
{
	InitializeDObjectSystem();
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	auto* Material = Durin::NewObject<Durin::DMaterial>(nullptr, "MetalCookedProgramRoundTrip");
	ASSERT_TRUE(Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Material));
	ASSERT_TRUE(WaitForMaterialCompile(*Material));
	ASSERT_TRUE(Material->GetAcceptedCompiledProgram());
	auto Program = *Material->GetAcceptedCompiledProgram();
	auto Artifact = std::make_shared<Durin::FMaterialShaderMap>(Program.GetShaderMap());
	Program.ShaderMap = Artifact;
	Artifact->Target = "metal-msl-2.0";
	for (auto& Shader : Artifact->CompiledShaders)
	{
		Shader.Target = Durin::MetalShaderTarget;
		Shader.CodeFormat = Durin::EShaderCodeFormat::Msl20Source;
		Shader.BinaryEntryPoint = "main";
		const std::string Source = "#include <metal_stdlib>\nusing namespace metal;\nfragment float4 main() { return float4(1); }\n";
		Durin::FByteBuffer Bytes;
		for (char Character : Source) Bytes.push_back(std::byte(Character));
		Shader.Code = Durin::FSharedByteBuffer::Take(std::move(Bytes));
		Shader.CodeHash = Durin::FXxHash128::HashBuffer(Shader.Code);
		const auto Map = Durin::ShaderFormat::BuildMetalBindingMap(Shader.Frequency, Durin::ShaderFormat::BuildLogicalBindings(Shader.Reflection), !Shader.Reflection.PushConstantRanges.empty());
		ASSERT_TRUE(Map);
		Shader.PlatformData = Durin::ShaderFormat::EncodeMetalMetadata(Shader.Frequency,
			{std::move(Map->Bindings), Map->PushConstantBufferSlot, Map->Identity}).value();
	}
	const std::array Programs{&Program};
	Durin::FByteBuffer Bytes;
	EXPECT_FALSE(Durin::EncodeMaterialCookedProgramFamily(Programs,
		Material->GetStaticProperties(), Durin::ECookTargetPlatform::MacOS,
		Durin::ECookTargetProfile::Game, Bytes));
	Artifact->Target = "metal-library-1";
	for (auto& Shader : Artifact->CompiledShaders)
	{
		Shader.Target = Durin::MetalLibraryShaderTarget;
		Shader.CodeFormat = Durin::EShaderCodeFormat::MetalLibrary;
		// CPU codec admission checks the library header; native loading is separate.
		Durin::FByteBuffer Library(88);
		Library[0] = std::byte{'M'}; Library[1] = std::byte{'T'};
		Library[2] = std::byte{'L'}; Library[3] = std::byte{'B'};
		Library[16] = std::byte{88};
		Shader.Code = Durin::FSharedByteBuffer::Take(std::move(Library));
		Shader.CodeHash = Durin::FXxHash128::HashBuffer(Shader.Code);
	}
	auto Result = Durin::EncodeMaterialCookedProgramFamily(Programs,
		Material->GetStaticProperties(), Durin::ECookTargetPlatform::MacOS,
		Durin::ECookTargetProfile::Game, Bytes);
	ASSERT_TRUE(Result) << Result.error();
	Durin::FMaterialStaticProperties Properties;
	std::shared_ptr<const Durin::FMaterialCompilerResult> Decoded;
	Result = Durin::DecodeMaterialCookedProgramFamily(Bytes,
		Durin::ECookTargetPlatform::MacOS, Durin::ECookTargetProfile::Game,
		Program.Quality, Program.FeatureLevel, {}, Properties, Decoded);
	ASSERT_TRUE(Result) << Result.error();
	ASSERT_TRUE(Decoded);
	for (size_t Index = 0; Index < Artifact->CompiledShaders.size(); ++Index)
	{
		EXPECT_EQ(Decoded->GetShaderMap().CompiledShaders[Index].Target, Durin::MetalLibraryShaderTarget);
		EXPECT_EQ(Durin::ShaderFormat::DecodeMetalMetadata(Decoded->GetShaderMap().CompiledShaders[Index].Frequency, Decoded->GetShaderMap().CompiledShaders[Index].PlatformData.GetBytes())->Bindings,
			Durin::ShaderFormat::DecodeMetalMetadata(Artifact->CompiledShaders[Index].Frequency, Artifact->CompiledShaders[Index].PlatformData.GetBytes())->Bindings);
		EXPECT_EQ(Durin::ShaderFormat::DecodeMetalMetadata(Decoded->GetShaderMap().CompiledShaders[Index].Frequency, Decoded->GetShaderMap().CompiledShaders[Index].PlatformData.GetBytes())->Identity,
			Durin::ShaderFormat::DecodeMetalMetadata(Artifact->CompiledShaders[Index].Frequency, Artifact->CompiledShaders[Index].PlatformData.GetBytes())->Identity);
	}
	EXPECT_FALSE(Durin::DecodeMaterialCookedProgramFamily(Bytes,
		Durin::ECookTargetPlatform::Win64, Durin::ECookTargetProfile::Game,
		Program.Quality, Program.FeatureLevel, {}, Properties, Decoded));
	auto CorruptedMetadata = Durin::FByteBuffer(Artifact->CompiledShaders.front().PlatformData.begin(), Artifact->CompiledShaders.front().PlatformData.end());
	CorruptedMetadata.back() ^= std::byte{1};
	Artifact->CompiledShaders.front().PlatformData = Durin::FSharedByteBuffer::Take(std::move(CorruptedMetadata));
	EXPECT_FALSE(Durin::EncodeMaterialCookedProgramFamily(Programs,
		Material->GetStaticProperties(), Durin::ECookTargetPlatform::MacOS,
		Durin::ECookTargetProfile::Game, Bytes));
	Durin::MarkAsGarbage(Material);
}

TEST_F(FMaterialAsyncCompileTests, StoppingAdmissionDrainsAcceptedMaterialWithoutCountingItsStagesTwice)
{
	using namespace Durin;
	auto& Compiler = FShaderCompilingManager::Get();
	auto* Material = MakeObject<DMaterial>("StopAdmissionMaterial");
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	FThreadEvent Started, Release;
	std::atomic_uint32_t Entered = 0;
	std::vector<Tasks::FTask> Blockers;
	struct FResume {
		FThreadEvent& Release;
		std::vector<Tasks::FTask>& Blockers;
		~FResume() {
			Release.Trigger();
			for (const auto& Task : Blockers) WaitTask(Task);
			FShaderCompilingManager::Get().Start();
		}
	} Resume{Release, Blockers};
	const auto Workers = GetTaskSchedulerDiagnostics().WorkerCount;
	for (uint32 Index = 0; Index < Workers; ++Index)
		Blockers.push_back(Tasks::LaunchTask("HoldAcceptedMaterialTranslation", [&] {
			if (Entered.fetch_add(1) + 1 == Workers) Started.Trigger();
			Release.WaitFor(10);
		}));
	ASSERT_TRUE(Started.WaitFor(5));
	ASSERT_TRUE(RequestMaterialRecompile(*Material, true));
	EXPECT_EQ(Compiler.GetNumRemainingAssets(), 1u);
	Compiler.StopAdmission();
	Release.Trigger();
	const auto Finished = FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_GT(Finished.ProcessedCompletionCount, 0u);
	EXPECT_EQ(Material->GetMaterialCompileStatus().State, EMaterialCompileState::Ready);
	EXPECT_EQ(Compiler.GetNumRemainingAssets(), 0u);
}

TEST_F(FMaterialAsyncCompileTests, ShaderCompilerRegistersOnceAndStopsBothRequestEntries)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto& Aggregate = FAssetCompilingManager::Get();
	struct FResume { ~FResume() { FShaderCompilingManager::Get().Start(); } } Resume;
	const auto Diagnostics = Aggregate.GetDiagnostics();
	EXPECT_EQ(std::ranges::count_if(Diagnostics.Compilers, [](const auto& Item) { return Item.CompilerName == FName("Durin.Shader"); }), 1);
	EXPECT_EQ(std::ranges::count_if(Diagnostics.Compilers, [](const auto& Item) { return Item.CompilerName == FName("Durin.Material"); }), 0);
	auto& Compiler = FShaderCompilingManager::Get();
	EXPECT_TRUE(IsMaterialCompilationAcceptingRequests());
	Compiler.StopAdmission();
	EXPECT_FALSE(IsMaterialCompilationAcceptingRequests());
	uint32 ShaderCalls = 0;
	auto Shader = Compiler.PrepareJobs("/Closed", {}, {}, [&](auto Output) { EXPECT_EQ(Output.Status, EShaderBuildStatus::Canceled); ++ShaderCalls; });
	EXPECT_EQ(ShaderCalls, 0u);
	Compiler.SubmitJobs(Shader);
	EXPECT_EQ(Shader.Wait(), EShaderBuildWaitResult::Completed);
	EXPECT_EQ(ShaderCalls, 1u);
	auto Material = MaterialBuildPrivate::FMaterialCompilation::Compile(std::make_shared<const MIR::FCompilerInput>(), {},
		[](auto) { ADD_FAILURE() << "Rejected material must not complete."; });
	ASSERT_FALSE(Material);
	EXPECT_EQ(Material.error(), MaterialBuildPrivate::EMaterialCompileAdmissionError::Closed);
	EXPECT_TRUE(Compiler.Start());
	EXPECT_TRUE(IsMaterialCompilationAcceptingRequests());
}
