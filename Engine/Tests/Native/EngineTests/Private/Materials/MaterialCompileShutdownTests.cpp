#include "MaterialResultFanoutTestSupport.h"
#include "Materials/MaterialFunction.h"

// Asset compiler shutdown is terminal for the process. Keep it in a dedicated
// target so ordinary lifecycle cases can run independently and in any order.
TEST(FMaterialCompileShutdownTests, ShutdownDrainsFunctionOwnersAndDetachedFanout)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FModuleManager::Get().LoadModule("RenderCore");
	ASSERT_TRUE(InitializeAssetCompilingManager());
	TStrongObjectPtr<DMaterialFunction> Function(NewObject<DMaterialFunction>(nullptr, "ShutdownFunction"));
	const auto Output = Function->GetFunctionSignature().Outputs.front();
	std::array<TStrongObjectPtr<DMaterial>, 2> Materials;
	for (auto& Material : Materials)
	{
		Material = TStrongObjectPtr<DMaterial>(NewObject<DMaterial>(nullptr, NAME_None));
		Material->SetEditCompileMode(EMaterialEditCompileMode::Manual);
		TStrongObjectPtr<DMaterialExpressionFunctionCall> Call(NewObject<DMaterialExpressionFunctionCall>(nullptr, NAME_None));
		Call->Id = FGuid::NewGuid();
		Call->Function = Function.Get();
		Call->Outputs = {{Output.Id, Output.Type}};
		const std::array<DMaterialExpression*, 1> Expressions{Call.Get()};
		FMaterialExpressionSurfaceOutputs Outputs;
		Outputs.Surface = {.ExpressionId = Call->Id, .OutputId = Output.Id};
		Outputs.bUseMaterialAttributes = true;
		ASSERT_TRUE(Material->SetMaterialExpressions(Expressions, Outputs));
	}
	FThreadEvent Started, Release;
	std::atomic<uint32> StartedCount = 0;
	std::vector<FTaskHandle> Blockers;
	struct FReleaseWorkers
	{
		FThreadEvent& Release;
		std::vector<FTaskHandle>& Blockers;
		~FReleaseWorkers() { Release.Trigger(); for (const auto& Task : Blockers) WaitTask(Task); }
	} ReleaseWorkers{Release, Blockers};
	const auto WorkerCount = GetTaskSchedulerDiagnostics().WorkerCount;
	for (uint32 Index = 0; Index < WorkerCount; ++Index)
		Blockers.push_back(Tasks::LaunchTask("HoldFunctionShutdown", [&] {
			if (StartedCount.fetch_add(1) + 1 == WorkerCount) Started.Trigger();
			Release.WaitFor(10);
		}));
	ASSERT_TRUE(Started.WaitFor(2));
	for (auto& Material : Materials) ASSERT_TRUE(RequestMaterialRecompile(*Material, true));
	EXPECT_GE(GetMaterialCompilationDiagnostics().OutstandingConsumerCount, 2u);
	Release.Trigger();
	ASSERT_NO_FATAL_FAILURE(Testing::CheckDetachedMaterialResultFanout(true));
	const auto Shutdown = GetMaterialCompilationDiagnostics();
	EXPECT_FALSE(Shutdown.bAcceptingRequests);
	EXPECT_EQ(Shutdown.InFlightCount, 0u);
	EXPECT_EQ(Shutdown.OutstandingConsumerCount, 0u);
	EXPECT_EQ(Shutdown.PendingPublicationCount, 0u);
	EXPECT_EQ(Shutdown.RetainedProgramCount, 0u);
	EXPECT_EQ(Shutdown.RetainedProgramBytes, 0u);
}
