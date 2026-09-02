#include <gtest/gtest.h>

#include "Asset/AssetCompilingManager.h"
#include "DObject/DObjectGlobals.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Modules/ModuleTestSupport.h"
#include "Texture/Texture.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"

namespace
{
	using namespace Durin;

	struct FSyntheticState
	{
		std::vector<std::string>* Calls = nullptr;
		std::vector<DObject*> LastObjects;
		uint64 Remaining = 0;
		uint32 AvailableCompletions = 0;
		uint32 StartCount = 0;
		uint32 ProcessCount = 0;
		uint32 FinishAllCount = 0;
		uint32 ShutdownCount = 0;
		bool bCanceled = false;
	};

	class FSyntheticManager final : public IAssetCompilingManager
	{
	public:
		FSyntheticManager(std::string InName, std::shared_ptr<FSyntheticState> InState)
			: Name(std::move(InName)), State(std::move(InState)) {}

		auto Start(std::string*) -> bool override
		{
			++State->StartCount;
			Record("start");
			return true;
		}
		auto StopAdmission() -> void override { Record("stop"); }
		auto GetNumRemainingAssets() const -> uint64 override { return State->Remaining; }
		auto ProcessAsyncTasks(const FAssetCompileProcessParams& Params)
			-> FAssetCompileProcessResult override
		{
			++State->ProcessCount;
			Record("process");
			const uint32 Count = std::min(Params.MaximumCompletions,
				State->AvailableCompletions);
			State->AvailableCompletions -= Count;
			State->Remaining -= std::min<uint64>(State->Remaining, Count);
			return {.ProcessedCompletionCount = Count};
		}
		auto FinishCompilationForObjects(std::span<DObject* const> Objects)
			-> FAssetCompileProcessResult override
		{
			Record("finish-selected");
			State->LastObjects.assign(Objects.begin(), Objects.end());
			FAssetCompileProcessResult Result;
			for (DObject* Object : Objects)
				Result.SuccessfullyCompiledAssets.emplace_back(Object);
			return Result;
		}
		auto MarkCompilationAsCanceled(std::span<DObject* const> Objects) -> void override
		{
			Record("cancel");
			State->LastObjects.assign(Objects.begin(), Objects.end());
			State->bCanceled = !Objects.empty();
		}
		auto FinishAllCompilation() -> FAssetCompileProcessResult override
		{
			++State->FinishAllCount;
			Record("finish-all");
			const uint32 Count = State->AvailableCompletions;
			State->AvailableCompletions = 0;
			State->Remaining = 0;
			return {.ProcessedCompletionCount = Count};
		}
		auto Shutdown() -> void override
		{
			++State->ShutdownCount;
			Record("shutdown");
		}

	private:
		auto Record(std::string_view Operation) const -> void
		{
			if (State->Calls) State->Calls->push_back(std::format("{}:{}", Operation, Name));
		}

		std::string Name;
		std::shared_ptr<FSyntheticState> State;
	};
}

TEST(FAssetCompilingManagerTests, RoutesClassesBatchesObjectsAndOwnsCompilerLifecycle)
{
	InitializeDObjectSystem();
	auto& Aggregate = FAssetCompilingManager::Get();
	std::string Error;
	ASSERT_TRUE(Aggregate.Start(&Error)) << Error;
	FModuleTestOwner Owner("AssetCompilingManagerTests.Provider");
	auto GateRegistration = Owner.CreateOwnedCallbackRegistration(
		"Engine.AssetCompilingManager.Tests");
	std::vector<std::string> Calls;
	DMaterial* FirstMaterial = NewObject<DMaterial>(nullptr, "FirstRoutedMaterial");
	DMaterial* SecondMaterial = NewObject<DMaterial>(nullptr, "SecondRoutedMaterial");
	DTexture2D* Texture = NewObject<DTexture2D>(nullptr, "DerivedRoutedTexture");
	DTextureCube* TextureCube = NewObject<DTextureCube>(nullptr, "FallbackRoutedTexture");
	DMaterialInstance* Unregistered =
		NewObject<DMaterialInstance>(nullptr, "UnregisteredMaterialInstance");

	auto BaseState = std::make_shared<FSyntheticState>();
	BaseState->Calls = &Calls;
	BaseState->Remaining = 2;
	BaseState->AvailableCompletions = 2;
	auto DerivedState = std::make_shared<FSyntheticState>();
	DerivedState->Calls = &Calls;
	DerivedState->Remaining = 2;
	DerivedState->AvailableCompletions = 2;
	auto BaseManager = std::make_shared<FSyntheticManager>("base", BaseState);
	auto DerivedManager = std::make_shared<FSyntheticManager>("derived", DerivedState);

	auto Base = Aggregate.RegisterCompiler({
		.Name = FName("Durin.Tests.Base"),
		.AssetClasses = {DMaterial::StaticClass(), DTexture::StaticClass()},
		.Manager = BaseManager}, GateRegistration.GetGate(), &Error);
	ASSERT_TRUE(Base.IsValid()) << Error;
	auto Derived = Aggregate.RegisterCompiler({
		.Name = FName("Durin.Tests.Derived"),
		.AssetClasses = {DTexture2D::StaticClass()},
		.Manager = DerivedManager}, GateRegistration.GetGate(), &Error);
	ASSERT_TRUE(Derived.IsValid()) << Error;
	EXPECT_EQ(BaseState->StartCount, 1u);
	EXPECT_EQ(Aggregate.GetDiagnostics().CompilerCount, 2u);

	EXPECT_FALSE(Aggregate.RegisterCompiler({
		.Name = FName("Durin.Tests.Base"),
		.AssetClasses = {DTexture2D::StaticClass()},
		.Manager = std::make_shared<FSyntheticManager>(
			"duplicate-name", std::make_shared<FSyntheticState>())},
		GateRegistration.GetGate(), &Error).IsValid());
	EXPECT_FALSE(Aggregate.RegisterCompiler({
		.Name = FName("Durin.Tests.Conflict"),
		.AssetClasses = {DMaterial::StaticClass()},
		.Manager = std::make_shared<FSyntheticManager>(
			"duplicate-class", std::make_shared<FSyntheticState>())},
		GateRegistration.GetGate(), &Error).IsValid());

	uint32 EventCount = 0;
	const FDelegateHandle EventHandle = Aggregate.OnAssetPostCompile().AddLambda(
		[&](const FAssetPostCompileData& Data) {
			++EventCount;
			EXPECT_TRUE(Data.CompilerName == FName("Durin.Tests.Base")
				|| Data.CompilerName == FName("Durin.Tests.Derived"));
		});
	DObject* Objects[] = {
		FirstMaterial, Texture, TextureCube, Unregistered, SecondMaterial, nullptr};
	const auto Finished = Aggregate.FinishCompilationForObjects(Objects);
	EXPECT_EQ(Finished.SuccessfullyCompiledAssets.size(), 4u);
	ASSERT_EQ(BaseState->LastObjects.size(), 3u);
	EXPECT_EQ(BaseState->LastObjects[0], FirstMaterial);
	EXPECT_EQ(BaseState->LastObjects[1], TextureCube);
	EXPECT_EQ(BaseState->LastObjects[2], SecondMaterial);
	ASSERT_EQ(DerivedState->LastObjects.size(), 1u);
	EXPECT_EQ(DerivedState->LastObjects[0], Texture);
	EXPECT_EQ(EventCount, 2u);

	Aggregate.MarkCompilationAsCanceled(Objects);
	EXPECT_TRUE(BaseState->bCanceled);
	EXPECT_TRUE(DerivedState->bCanceled);
	const auto Frame = Aggregate.ProcessAsyncTasks({.MaximumCompletions = 2});
	EXPECT_EQ(Frame.ProcessedCompletionCount, 2u);
	EXPECT_GE(BaseState->ProcessCount, 1u);
	EXPECT_GE(DerivedState->ProcessCount, 1u);
	Aggregate.OnAssetPostCompile().Remove(EventHandle);

	Derived.Reset();
	Base.Reset();
	EXPECT_EQ(BaseState->FinishAllCount, 1u);
	EXPECT_EQ(BaseState->ShutdownCount, 1u);
	EXPECT_EQ(DerivedState->FinishAllCount, 1u);
	EXPECT_EQ(DerivedState->ShutdownCount, 1u);

	auto RetiredState = std::make_shared<FSyntheticState>();
	auto Retired = Aggregate.RegisterCompiler({
		.Name = FName("Durin.Tests.Retired"),
		.AssetClasses = {DMaterial::StaticClass()},
		.Manager = std::make_shared<FSyntheticManager>("retired", RetiredState)},
		GateRegistration.GetGate(), &Error);
	ASSERT_TRUE(Retired.IsValid()) << Error;
	GateRegistration.Retire();
	EXPECT_EQ(Aggregate.ProcessAsyncTasks().ProcessedCompletionCount, 0u);
	Retired.Reset();
	EXPECT_TRUE(GateRegistration.Reset().Succeeded());
	Aggregate.Shutdown();
}
