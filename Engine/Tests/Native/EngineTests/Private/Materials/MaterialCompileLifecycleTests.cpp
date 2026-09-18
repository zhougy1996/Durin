#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "FunctionPortTestFixture.h"
#include "ExplicitMaterialProgramTestFixture.h"
#include "MaterialVariantTestFixture.h"
#include "Threading/TaskComposition.h"
#include "MaterialTestSupport.h"

#include "Asset/AssetCompilingManager.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Materials/MaterialCompileRetryQueue.h"
#include "Materials/MaterialCookedProgram.h"
#include "Materials/MaterialFunction.h"
#include "Modules/ModuleManager.h"
#include "Threading/Task.h"
#include "Threading/ThreadEvent.h"

#include <iostream>

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
			if (State != Durin::EMaterialCompileState::Pending
				&& State != Durin::EMaterialCompileState::Running
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
		Outputs.RoughnessDefault += Delta;
		std::vector<Durin::DMaterialExpression*> Expressions;
		for (const auto& Expression : Material.GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
		return Material.SetMaterialExpressions(Expressions, Outputs);
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
	// Keep this fixture's retained result separate from the default-material
	// identity used by the subsequent cold single-flight assertions.
	Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
	ASSERT_TRUE(EditRoughnessDefault(*Material, 0.173125f));
	for (const bool bExplicitCancel : {false, true})
	{
		SCOPED_TRACE(bExplicitCancel ? "cancel then compile" : "compile twice");
		{
			const auto WorkerCount = GetTaskSchedulerDiagnostics().WorkerCount;
			FThreadEvent Started, Release;
			std::atomic<uint32> StartedCount = 0;
			std::vector<FTaskHandle> Blockers;
			struct FReleaseWorkers
			{
				FThreadEvent& Release;
				std::vector<FTaskHandle>& Blockers;
				~FReleaseWorkers() { Release.Trigger(); for (const auto& Task : Blockers) WaitTask(Task); }
			} ReleaseWorkers{Release, Blockers};
			for (uint32 Index = 0; Index < WorkerCount; ++Index)
				Blockers.push_back(Tasks::LaunchTask("HoldCanceledMaterialFlight", [&] {
					if (StartedCount.fetch_add(1) + 1 == WorkerCount) Started.Trigger();
					Release.WaitFor(10.0);
				}).GetCompletion().GetTaskHandle());
			ASSERT_TRUE(Started.WaitFor(2.0));
			// Force compilation so the second iteration cannot use a retained hit.
			ASSERT_TRUE(RequestMaterialRecompile(*Material, true));
			const auto Generation = Material->GetMaterialCompileStatus().RequestGeneration;
			if (bExplicitCancel)
				FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Material);
			ASSERT_TRUE(RequestMaterialRecompile(*Material, true));
			EXPECT_GT(Material->GetMaterialCompileStatus().RequestGeneration, Generation);
			EXPECT_EQ(Material->GetMaterialCompileStatus().State, EMaterialCompileState::Deferred);
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
	Invalid.Roughness.ExpressionId = FGuid::NewGuid();
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
	EXPECT_EQ(Root->GetMaterialCompileStatus().RequestGeneration, Generation + 1);
	ASSERT_TRUE(WaitForMaterialCompile(*Root));
	ASSERT_TRUE(WaitForMaterialCompile(*Child));
	EXPECT_EQ(Root->GetMaterialCompileStatus().RequestGeneration, Generation + 2);

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

TEST(FMaterialCompileLifecycleTests,
	LatestGenerationSingleFlightFailureFallbackAndShutdownAreBounded)
{
	InitializeDObjectSystem();
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	const bool bOwnsScheduler = !Durin::IsTaskSchedulerRunning();
	if (bOwnsScheduler) ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	QualifyCanceledFlightResubmission();
	Durin::Testing::CheckInstanceVariantsForTest(false);
	QualifyInstanceCompilationOwners();

	auto* First = Durin::NewObject<Durin::DMaterial>(
		nullptr, "AsyncCompileFirst");
	auto* Second = Durin::NewObject<Durin::DMaterial>(
		nullptr, "AsyncCompileSecond");
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
		std::vector<Durin::FTaskHandle> Blockers;
		struct FReleaseWorkers
		{
			Durin::FThreadEvent& Event;
			std::vector<Durin::FTaskHandle>& Tasks;
			~FReleaseWorkers() { Event.Trigger(); for (const auto& Task : Tasks) Durin::WaitTask(Task); }
		} ReleaseWorkers{Release, Blockers};
		for (uint32 Index = 0; Index < WorkerCount; ++Index)
			Blockers.push_back(Durin::Tasks::LaunchTask("HoldMaterialSingleFlight", [&] {
				if (StartedCount.fetch_add(1) + 1 == WorkerCount) Started.Trigger();
				Release.WaitFor(2.0);
			}).GetCompletion().GetTaskHandle());
		ASSERT_TRUE(Started.WaitFor(1.0));
		ASSERT_TRUE(Durin::RequestMaterialRecompile(*First));
		ASSERT_TRUE(Durin::RequestMaterialRecompile(*Second));
		ASSERT_EQ(First->GetMaterialCompileStatus().State,
			Durin::EMaterialCompileState::Running);
		ASSERT_EQ(Second->GetMaterialCompileStatus().State,
			Durin::EMaterialCompileState::Pending);
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
		First->GetMaterialRenderProxy();
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
		<< " category=" << static_cast<uint32>(First->GetMaterialCompileStatus().ResultCategory)
		<< " diagnostic=" << (First->GetMaterialCompileDiagnostics().empty()
			? std::string("<none>")
			: Durin::FormatMaterialError(First->GetMaterialCompileDiagnostics().front().Source.Error));
	ASSERT_TRUE(First->GetAcceptedCompiledProgram());
	EXPECT_NE(First->GetAcceptedCompiledProgram()->Identity,
		InitialProgram->Identity);
	EXPECT_TRUE(First->GetMaterialCompileStatus().IsCurrent());

	auto ParameterValidation = Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*First);

	ASSERT_TRUE(ParameterValidation);
	auto* PendingInstance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "PendingParameterEdit");
	ASSERT_TRUE(PendingInstance->SetParent(First));
	EXPECT_TRUE(First->GetAcceptedCompiledProgram()->ActiveParameters.empty());
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
	Durin::FMaterialCompileResult Failed{
		.Owner = Durin::FWeakObjectPtr(First),
		.AuthoredRevision = Pending.AuthoredRevision,
		.Generation = Pending.RequestGeneration,
		.DependencyRevision = Pending.DependencyRevision,
		.ProgramIdentity = Pending.RequestedIdentity,
		.Target = Pending.Target,
		.State = Durin::EMaterialCompileState::Failed,
		.Category = Durin::EMaterialCompileResultCategory::Compile,
	};
	// An obsolete failure cannot retire a newer request's visible generation.
	auto StaleFailure = Failed;
	--StaleFailure.Generation;
	EXPECT_FALSE(Durin::Private::FMaterialCompilationLifecycle::Admit(
		*First, std::move(StaleFailure)));
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), LastKnownGood);
	EXPECT_FALSE(Durin::Private::FMaterialCompilationLifecycle::Admit(
		*First, std::move(Failed)));
	EXPECT_FALSE(First->GetAcceptedCompiledProgram());
	EXPECT_EQ(First->GetMaterialCompileStatus().State,
		Durin::EMaterialCompileState::Failed);
	EXPECT_FALSE(First->GetMaterialCompileStatus().IsCurrent());
	ExpectColorNear(GetMaterialBinding(First->GetRenderData()).BaseColor,
		GetMaterialBinding(Durin::GetErrorMaterialRenderData()).BaseColor);

	Durin::FMaterialCompileResult Stale{
		.Owner = Durin::FWeakObjectPtr(First),
		.AuthoredRevision = Pending.AuthoredRevision,
		.Generation = Pending.RequestGeneration - 1,
		.DependencyRevision = Pending.DependencyRevision,
		.ProgramIdentity = LastKnownGood->Identity,
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
		.ProgramIdentity = LastKnownGood->Identity,
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
		.ProgramIdentity = LastKnownGood->Identity,
		.Target = Pending.Target,
		.State = Durin::EMaterialCompileState::Ready,
		.CompiledProgram = LastKnownGood,
	};
	EXPECT_FALSE(Durin::Private::FMaterialCompilationLifecycle::Admit(
		*First, std::move(WrongDependency)));
	EXPECT_FALSE(First->GetAcceptedCompiledProgram());

	// Recovery replaces the error terminal, including after stale results were rejected.
	ASSERT_TRUE(Durin::RequestMaterialRecompile(*First));
	ASSERT_TRUE(WaitForMaterialCompile(*First));
	EXPECT_TRUE(First->GetAcceptedCompiledProgram());
	EXPECT_TRUE(First->GetMaterialCompileStatus().IsCurrent());

	// Pending replacements retain deleted declarations; failed owners retire them.
	{
		auto* Root = Durin::NewObject<Durin::DMaterial>(nullptr, "RetainedDeclarationRoot");
		ASSERT_TRUE(Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Root));
		ASSERT_TRUE(WaitForMaterialCompile(*Root));
		ASSERT_NE(Root, nullptr);
		ASSERT_NE(Root->GetAcceptedCompiledProgram(), nullptr);
		auto* Instance = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "RetainedDeclarationInstance");
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
		std::vector<Durin::FTaskHandle> Blockers;
		const auto WorkerCount = Durin::GetTaskSchedulerDiagnostics().WorkerCount;
		struct FReleaseWorkers
		{
			Durin::FThreadEvent& Release;
			std::vector<Durin::FTaskHandle>& Tasks;
			~FReleaseWorkers() { Release.Trigger(); for (const auto& Task : Tasks) Durin::WaitTask(Task); }
		};
		{
			FReleaseWorkers ReleaseWorkers{Release, Blockers};
			for (uint32 Index = 0; Index < WorkerCount; ++Index)
				Blockers.push_back(Durin::Tasks::LaunchTask("HoldRetainedSchemaCompile", [&] {
					if (StartedCount.fetch_add(1) + 1 == WorkerCount) Started.Trigger();
					Release.WaitFor(10.0);
				}).GetCompletion().GetTaskHandle());
			ASSERT_TRUE(Started.WaitFor(2.0));
			ASSERT_TRUE(Graph.Apply(*Root));
			EXPECT_FLOAT_EQ(GetMaterialBinding(Root->GetRenderData()).Metallic, 0.65f);
			EXPECT_FLOAT_EQ(GetMaterialBinding(Instance->GetRenderData()).Metallic, 0.9f);
			const auto Pending = Root->GetMaterialCompileStatus();
			Durin::FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Root);
			Durin::FMaterialCompileResult Failed{
				.Owner = Durin::FWeakObjectPtr(Root),
				.AuthoredRevision = Pending.AuthoredRevision,
				.Generation = Pending.RequestGeneration,
				.DependencyRevision = Pending.DependencyRevision,
				.ProgramIdentity = Pending.RequestedIdentity,
				.Target = Pending.Target,
				.State = Durin::EMaterialCompileState::Failed,
				.Category = Durin::EMaterialCompileResultCategory::Compile};
			EXPECT_FALSE(Durin::Private::FMaterialCompilationLifecycle::Admit(*Root, std::move(Failed)));
		}
		EXPECT_FALSE(Root->GetAcceptedCompiledProgram());
		ExpectColorNear(GetMaterialBinding(Root->GetRenderData()).BaseColor,
			GetMaterialBinding(Durin::GetErrorMaterialRenderData()).BaseColor);
		// Each variant owns its result: the child's successful compilation can still publish.
		ASSERT_TRUE(WaitForMaterialCompile(*Instance));
		EXPECT_TRUE(Instance->GetAcceptedCompiledProgram());
		ASSERT_TRUE(Durin::RequestMaterialRecompile(*Root));
		ASSERT_TRUE(WaitForMaterialCompile(*Root));
		EXPECT_EQ(Root->GetAcceptedCompiledProgram()->Identity,
			Instance->GetAcceptedCompiledProgram()->Identity);
		Durin::MarkAsGarbage(Instance);
		Durin::MarkAsGarbage(Root);
	}

	QualifyEditScheduling();
	QualifyMaterialEditingSessionAsync();
	QualifyMaterialFunctionCompilationAsync();

	Durin::MarkAsGarbage(PendingInstance);
	Durin::MarkAsGarbage(Second);
	Durin::MarkAsGarbage(First);
	Durin::CollectGarbage();
	Durin::ShutdownAssetCompilingManager();
	const Durin::FMaterialCompilationDiagnostics Shutdown =
		Durin::GetMaterialCompilationDiagnostics();
	EXPECT_FALSE(Shutdown.bAcceptingRequests);
	EXPECT_EQ(Shutdown.InFlightCount, 0u);
	EXPECT_EQ(Shutdown.OutstandingConsumerCount, 0u);
	EXPECT_EQ(Shutdown.PendingPublicationCount, 0u);
	EXPECT_EQ(Shutdown.RetainedProgramCount, 0u);
	EXPECT_EQ(Shutdown.RetainedProgramBytes, 0u);
	if (bOwnsScheduler)
		Durin::ShutdownTaskSystem(Durin::ETaskShutdownMode::Drain);
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
		std::vector<FTaskHandle> Tasks;
		auto Hold() -> bool
		{
			const auto Count = GetTaskSchedulerDiagnostics().WorkerCount;
			for (uint32 Index = 0; Index < Count; ++Index)
				Tasks.push_back(Durin::Tasks::LaunchTask("HoldFunctionCompile", [this, Count] {
					if (StartedCount.fetch_add(1) + 1 == Count) Started.Trigger();
					Release.WaitFor(10.0);
				}).GetCompletion().GetTaskHandle());
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
		Body.emplace_back(DuplicateObject(Expression.Get(), nullptr, NAME_None).Object);
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
		EXPECT_EQ(Second->GetMaterialCompileStatus().State, EMaterialCompileState::Pending);
		FAssetCompilingManager::Get().MarkCompilationAsCanceled(*First);
		EXPECT_EQ(First->GetMaterialCompileStatus().State, EMaterialCompileState::Canceled);
	}
	ASSERT_TRUE(WaitForMaterialCompile(*Second));
	EXPECT_EQ(Second->GetMaterialCompileStatus().CacheOutcome, EMaterialCompileCacheOutcome::SingleFlight);
	EXPECT_EQ(First->GetMaterialCompileStatus().State, EMaterialCompileState::Canceled);
	ASSERT_TRUE(First->CompileEdits());
	ASSERT_TRUE(WaitForMaterialCompile(*First));
	const auto Accepted = First->GetAcceptedCompiledProgram();
	EXPECT_EQ(Accepted, Second->GetAcceptedCompiledProgram());
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
		EXPECT_EQ(Material->GetAcceptedCompiledProgram(), Accepted);
		ASSERT_TRUE(Material->CompileEdits());
		ASSERT_TRUE(WaitForMaterialCompile(*Material));
		EXPECT_NE(Material->GetAcceptedCompiledProgram()->Identity, Accepted->Identity);
	}
	EXPECT_EQ(First->GetAcceptedCompiledProgram(), Second->GetAcceptedCompiledProgram());
	{
		FHoldWorkers Hold;
		ASSERT_TRUE(Hold.Hold());
		ASSERT_TRUE(RequestMaterialRecompile(*First, true));
		ASSERT_TRUE(RequestMaterialRecompile(*Second, true));
		EXPECT_GE(GetMaterialCompilationDiagnostics().OutstandingConsumerCount, 2u);
		Hold.Release.Trigger();
		ShutdownAssetCompilingManager();
	}
	const auto Shutdown = GetMaterialCompilationDiagnostics();
	EXPECT_FALSE(Shutdown.bAcceptingRequests);
	EXPECT_EQ(Shutdown.InFlightCount, 0u);
	EXPECT_EQ(Shutdown.OutstandingConsumerCount, 0u);
	EXPECT_EQ(Shutdown.PendingPublicationCount, 0u);
	EXPECT_EQ(Shutdown.RetainedProgramCount, 0u);
}

TEST(FMaterialCompileLifecycleTests,
	CookedProgramRoundTripIsDeterministicBoundedAndTargetQualified)
{
	InitializeDObjectSystem();
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	auto* Material = Durin::NewObject<Durin::DMaterial>(
		nullptr, "CookedProgramRoundTrip");
	auto Validation = Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*Material);
	ASSERT_TRUE(Validation);
	ASSERT_TRUE(Material->GetAcceptedCompiledProgram());

	Durin::FByteBuffer FirstBytes;
	Durin::FByteBuffer SecondBytes;
	Durin::FMaterialOperationResult Error;
	ASSERT_TRUE((Error = Durin::EncodeMaterialCookedProgram(
		*Material->GetAcceptedCompiledProgram(), Material->GetStaticProperties(),
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game, FirstBytes))) << Durin::FormatMaterialError(Error.Error);
	ASSERT_TRUE((Error = Durin::EncodeMaterialCookedProgram(
		*Material->GetAcceptedCompiledProgram(), Material->GetStaticProperties(),
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game, SecondBytes))) << Durin::FormatMaterialError(Error.Error);
	EXPECT_EQ(FirstBytes, SecondBytes);
	EXPECT_LE(FirstBytes.size(), Durin::MaterialCookedProgramMaxPayloadBytes);

	Durin::FMaterialStaticProperties DecodedProperties;
	std::shared_ptr<const Durin::FMaterialCompilerResult> DecodedProgram;
	ASSERT_TRUE((Error = Durin::DecodeMaterialCookedProgram(
		FirstBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		DecodedProperties, DecodedProgram))) << Durin::FormatMaterialError(Error.Error);
	ASSERT_TRUE(DecodedProgram);
	EXPECT_EQ(DecodedProgram->Identity,
		Material->GetAcceptedCompiledProgram()->Identity);
	EXPECT_EQ(DecodedProperties, Material->GetStaticProperties());
	EXPECT_EQ(DecodedProgram->ActiveParameters,
		Material->GetAcceptedCompiledProgram()->ActiveParameters);
	EXPECT_EQ(DecodedProgram->ActiveParameters.size(), 36u);
	ASSERT_EQ(DecodedProgram->CompiledShaders.size(),
		Material->GetAcceptedCompiledProgram()->CompiledShaders.size());
	for (size_t Index = 0; Index < DecodedProgram->CompiledShaders.size(); ++Index)
	{
		const Durin::FCompiledShader& Decoded =
			DecodedProgram->CompiledShaders[Index];
		const Durin::FCompiledShader& Source =
			Material->GetAcceptedCompiledProgram()->CompiledShaders[Index];
		EXPECT_EQ(Decoded.Frequency, Source.Frequency);
		EXPECT_EQ(Decoded.SourceEntryPoint, Source.SourceEntryPoint);
		EXPECT_EQ(Decoded.BinaryEntryPoint, Source.BinaryEntryPoint);
		EXPECT_EQ(Decoded.Hash, Source.Hash);
		ASSERT_TRUE(Decoded.Code);
		ASSERT_TRUE(Source.Code);
		EXPECT_EQ(*Decoded.Code, *Source.Code);
	}
	EXPECT_TRUE(DecodedProgram->IR.Nodes.empty());
	EXPECT_TRUE(DecodedProgram->GeneratedSource.empty());
	ASSERT_FALSE(DecodedProgram->ActiveParameters.empty());
	for (int Corruption = 0; Corruption < 3; ++Corruption)
	{
		auto Invalid = *DecodedProgram;
		if (Corruption == 0) Invalid.ActiveParameters.push_back(Invalid.ActiveParameters.front());
		if (Corruption == 1) Invalid.ActiveParameters.front().Id = {};
		if (Corruption == 2) Invalid.ActiveParameters.front().Type =
			static_cast<Durin::EMaterialParameterType>(255);
		Durin::FByteBuffer InvalidBytes;
		EXPECT_FALSE((Error = Durin::EncodeMaterialCookedProgram(Invalid, DecodedProperties,
			Durin::ECookTargetPlatform::Win64, Durin::ECookTargetProfile::Game,
			InvalidBytes)));
	}

	EXPECT_FALSE((Error = Durin::DecodeMaterialCookedProgram(
		FirstBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::EditorValidation,
		DecodedProperties, DecodedProgram)));
	Durin::FMaterialCompilerResult WrongEnvironment =
		*Material->GetAcceptedCompiledProgram();
	WrongEnvironment.CompilerIdentity = "incompatible-compiler";
	Durin::FByteBuffer WrongEnvironmentBytes;
	ASSERT_TRUE((Error = Durin::EncodeMaterialCookedProgram(
		WrongEnvironment, Material->GetStaticProperties(),
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		WrongEnvironmentBytes))) << Durin::FormatMaterialError(Error.Error);
	EXPECT_FALSE((Error = Durin::DecodeMaterialCookedProgram(
		WrongEnvironmentBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		DecodedProperties, DecodedProgram)));
	WrongEnvironment = *Material->GetAcceptedCompiledProgram();
	WrongEnvironment.Target = "wrong-target";
	ASSERT_TRUE((Error = Durin::EncodeMaterialCookedProgram(
		WrongEnvironment, Material->GetStaticProperties(),
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		WrongEnvironmentBytes))) << Durin::FormatMaterialError(Error.Error);
	EXPECT_FALSE((Error = Durin::DecodeMaterialCookedProgram(
		WrongEnvironmentBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		DecodedProperties, DecodedProgram)));
	Durin::FByteBuffer OldSchemaBytes = FirstBytes;
	OldSchemaBytes[4] = std::byte{2};
	EXPECT_FALSE((Error = Durin::DecodeMaterialCookedProgram(
		OldSchemaBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		DecodedProperties, DecodedProgram)));
	Durin::FByteBuffer TrailingBytes = FirstBytes;
	TrailingBytes.push_back(std::byte{0});
	EXPECT_FALSE((Error = Durin::DecodeMaterialCookedProgram(
		TrailingBytes, Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		DecodedProperties, DecodedProgram)));

	Durin::MarkAsGarbage(Material);
	Durin::CollectGarbage();
}
