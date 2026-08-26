#include <gtest/gtest.h>

#include "Asset/AssetCompilingManager.h"
#include "DObject/DObjectGlobals.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Modules/ModuleTestSupport.h"

namespace
{
	using namespace Durin;

	struct FSyntheticState
	{
		std::vector<std::string>* Calls = nullptr;
		DObject* OwnedObject = nullptr;
		uint64 Remaining = 0;
		uint32 AvailableCompletions = 0;
		bool bCanceled = false;
		bool bShutdown = false;
	};

	class FSyntheticManager final : public IAssetCompilingManager
	{
	public:
		FSyntheticManager(std::string InName, std::vector<FName> InDependencies,
			std::shared_ptr<FSyntheticState> InState)
			: Name(std::move(InName)), Dependencies(std::move(InDependencies)),
			  State(std::move(InState)) {}

		auto GetDomainName() const -> FName override { return FName(Name); }
		auto GetDependencies() const -> std::vector<FName> override
		{
			return Dependencies;
		}
		auto Start(std::string*) -> bool override
		{
			Record("start");
			return true;
		}
		auto StopAdmission() -> void override { Record("stop"); }
		auto GetNumRemainingAssets() const -> uint64 override { return State->Remaining; }
		auto ProcessAsyncTasks(const FAssetCompileProcessParams& Params)
			-> FAssetCompileProcessResult override
		{
			Record("process");
			const uint32 Count = std::min(Params.MaximumCompletions,
				State->AvailableCompletions);
			State->AvailableCompletions -= Count;
			State->Remaining -= std::min<uint64>(State->Remaining, Count);
			FAssetCompileProcessResult Result{.ProcessedCompletionCount = Count};
			if (Count != 0 && State->OwnedObject)
				Result.SuccessfullyCompiledAssets.emplace_back(State->OwnedObject);
			return Result;
		}
		auto FinishCompilationForObjects(std::span<DObject* const> Objects)
			-> FAssetCompileProcessResult override
		{
			Record("finish-selected");
			if (!State->OwnedObject || std::ranges::find(Objects, State->OwnedObject) == Objects.end())
				return {};
			const uint32 Count = State->AvailableCompletions;
			State->AvailableCompletions = 0;
			State->Remaining = 0;
			FAssetCompileProcessResult Result{.ProcessedCompletionCount = Count};
			if (Count != 0) Result.SuccessfullyCompiledAssets.emplace_back(State->OwnedObject);
			return Result;
		}
		auto MarkCompilationAsCanceled(std::span<DObject* const> Objects) -> void override
		{
			Record("cancel");
			State->bCanceled = State->OwnedObject
				&& std::ranges::find(Objects, State->OwnedObject) != Objects.end();
		}
		auto FinishAllCompilation() -> FAssetCompileProcessResult override
		{
			Record("finish-all");
			const uint32 Count = State->AvailableCompletions;
			State->AvailableCompletions = 0;
			State->Remaining = 0;
			return {.ProcessedCompletionCount = Count};
		}
		auto Shutdown() -> void override
		{
			Record("shutdown");
			State->bShutdown = true;
		}

	private:
		auto Record(std::string_view Operation) const -> void
		{
			if (State->Calls) State->Calls->push_back(std::format("{}:{}", Operation, Name));
		}

		std::string Name;
		std::vector<FName> Dependencies;
		std::shared_ptr<FSyntheticState> State;
	};
}

TEST(FAssetCompilingManagerTests, AggregatesDomainsObjectsEventsAndModuleLifetime)
{
	InitializeDObjectSystem();
	auto& Aggregate = FAssetCompilingManager::Get();
	std::string Error;
	ASSERT_TRUE(Aggregate.Start(&Error)) << Error;
	FModuleTestOwner Owner("AssetCompilingManagerTests.Provider");
	auto GateRegistration = Owner.CreateOwnedCallbackRegistration(
		"Engine.AssetCompilingManager.Tests");
	std::vector<std::string> Calls;
	DMaterial* Material = NewObject<DMaterial>(nullptr, "AssetCompileAggregateMaterial");
	auto PrerequisiteState = std::make_shared<FSyntheticState>();
	PrerequisiteState->Calls = &Calls;
	PrerequisiteState->Remaining = 2;
	PrerequisiteState->AvailableCompletions = 2;
	auto DependentState = std::make_shared<FSyntheticState>();
	DependentState->Calls = &Calls;
	DependentState->OwnedObject = Material;
	DependentState->Remaining = 2;
	DependentState->AvailableCompletions = 2;
	auto Prerequisite = Aggregate.RegisterManager(
		std::make_shared<FSyntheticManager>("Durin.Tests.Prerequisite",
			std::vector<FName>{}, PrerequisiteState),
		GateRegistration.GetGate(), &Error);
	ASSERT_TRUE(Prerequisite.IsValid()) << Error;
	auto Dependent = Aggregate.RegisterManager(
		std::make_shared<FSyntheticManager>("Durin.Tests.Dependent",
			std::vector<FName>{FName("Durin.Tests.Prerequisite")}, DependentState),
		GateRegistration.GetGate(), &Error);
	ASSERT_TRUE(Dependent.IsValid()) << Error;
	EXPECT_FALSE(Aggregate.RegisterManager(
		std::make_shared<FSyntheticManager>("Durin.Tests.Dependent",
			std::vector<FName>{}, std::make_shared<FSyntheticState>()),
		GateRegistration.GetGate(), &Error).IsValid());

	uint32 EventCount = 0;
	const FDelegateHandle EventHandle = Aggregate.OnAssetPostCompile().AddLambda(
		[&](const FAssetPostCompileData& Data) {
			++EventCount;
			EXPECT_EQ(Data.DomainName, FName("Durin.Tests.Dependent"));
			EXPECT_EQ(Data.Assets.size(), 1u);
			EXPECT_EQ(Aggregate.GetDiagnostics().ManagerCount, 2u);
		});
	Calls.clear();
	const FAssetCompileProcessResult Frame = Aggregate.ProcessAsyncTasks(
		{.MaximumCompletions = 2});
	EXPECT_EQ(Frame.ProcessedCompletionCount, 2u);
	ASSERT_GE(Calls.size(), 2u);
	EXPECT_EQ(Calls[0], "process:Durin.Tests.Prerequisite");
	EXPECT_EQ(Calls[1], "process:Durin.Tests.Dependent");
	EXPECT_EQ(EventCount, 1u);
	EXPECT_EQ(Aggregate.GetNumRemainingAssets(), 2u);

	DObject* Selected = Material;
	Aggregate.MarkCompilationAsCanceled(std::span<DObject* const>(&Selected, 1));
	EXPECT_TRUE(DependentState->bCanceled);
	EXPECT_GT(DependentState->Remaining, 0u);
	const auto Finished = Aggregate.FinishCompilationForObjects(
		std::span<DObject* const>(&Selected, 1));
	EXPECT_EQ(Finished.SuccessfullyCompiledAssets.size(), 1u);
	EXPECT_EQ(DependentState->Remaining, 0u);
	EXPECT_EQ(EventCount, 2u);
	Aggregate.OnAssetPostCompile().Remove(EventHandle);

	Dependent.Reset();
	Prerequisite.Reset();
	EXPECT_TRUE(DependentState->bShutdown);
	EXPECT_TRUE(PrerequisiteState->bShutdown);

	auto FirstCycle = Aggregate.RegisterManager(
		std::make_shared<FSyntheticManager>("Durin.Tests.CycleA",
			std::vector<FName>{FName("Durin.Tests.CycleB")},
			std::make_shared<FSyntheticState>()),
		GateRegistration.GetGate(), &Error);
	ASSERT_TRUE(FirstCycle.IsValid()) << Error;
	EXPECT_FALSE(Aggregate.RegisterManager(
		std::make_shared<FSyntheticManager>("Durin.Tests.CycleB",
			std::vector<FName>{FName("Durin.Tests.CycleA")},
			std::make_shared<FSyntheticState>()),
		GateRegistration.GetGate(), &Error).IsValid());
	EXPECT_EQ(Aggregate.GetDiagnostics().ManagerCount, 1u);
	FirstCycle.Reset();

	auto RetiredState = std::make_shared<FSyntheticState>();
	RetiredState->AvailableCompletions = 1;
	auto Retired = Aggregate.RegisterManager(
		std::make_shared<FSyntheticManager>("Durin.Tests.Retired",
			std::vector<FName>{}, RetiredState),
		GateRegistration.GetGate(), &Error);
	ASSERT_TRUE(Retired.IsValid()) << Error;
	GateRegistration.Retire();
	EXPECT_EQ(Aggregate.ProcessAsyncTasks().ProcessedCompletionCount, 0u);
	Retired.Reset();
	EXPECT_TRUE(GateRegistration.Reset().Succeeded());
	Aggregate.Shutdown();
}
