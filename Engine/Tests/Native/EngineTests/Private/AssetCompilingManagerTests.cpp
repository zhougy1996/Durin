#include <gtest/gtest.h>

#include "Asset/AssetCompilingManager.h"
#include "DObject/DObjectGlobals.h"
#include "EngineTestSupport.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
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
		uint32 ConsumedCompletionCount = 0;
		uint32 FinishAllCount = 0;
		uint32 ShutdownCount = 0;
		bool bCanceled = false;
		FAssetCompilerStartResult StartResult;
		std::function<void()> OnFinishAll;
	};

	class FSyntheticManager final : public IAssetCompilingManager
	{
	public:
		FSyntheticManager(std::string InName, std::shared_ptr<FSyntheticState> InState, FName InCompilerName = {})
			: Name(std::move(InName)), State(std::move(InState)), CompilerName(InCompilerName) {}

		auto Start() -> FAssetCompilerStartResult override
		{
			++State->StartCount;
			Record("start");
			return State->StartResult;
		}
		auto StopAdmission() -> void override { Record("stop"); }
		auto GetNumRemainingAssets() const -> uint64 override { return State->Remaining; }
		auto ProcessAsyncTasks(const FAssetCompileProcessParams& Params)
			-> void override
		{
			++State->ProcessCount;
			Record("process");
			const uint32 Count = State->AvailableCompletions;
			State->AvailableCompletions -= Count;
			State->Remaining -= std::min<uint64>(State->Remaining, Count);
			State->ConsumedCompletionCount += Count;
		}
		auto FinishCompilationForObjects(std::span<DObject* const> Objects)
			-> void override
		{
			Record("finish-selected");
			State->LastObjects.assign(Objects.begin(), Objects.end());
			std::vector<FWeakObjectPtr> Assets;
			for (DObject* Object : Objects)
				Assets.emplace_back(Object);
			FAssetCompilingManager::Get().OnAssetPostCompile().Broadcast({CompilerName, std::move(Assets)});
		}
		auto MarkCompilationAsCanceled(std::span<DObject* const> Objects) -> void override
		{
			Record("cancel");
			State->LastObjects.assign(Objects.begin(), Objects.end());
			State->bCanceled = !Objects.empty();
		}
		auto FinishAllCompilation() -> void override
		{
			++State->FinishAllCount;
			Record("finish-all");
			const uint32 Count = State->AvailableCompletions;
			State->AvailableCompletions = 0;
			State->Remaining = 0;
			if (auto Notify = std::move(State->OnFinishAll)) Notify();
			State->ConsumedCompletionCount += Count;
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
		FName CompilerName;
	};
}

TEST(FAssetCompilingManagerTests, RoutesClassesBatchesObjectsAndOwnsCompilerLifecycle)
{
	InitializeDObjectSystem();
	auto& Aggregate = FAssetCompilingManager::Get();
	Aggregate.Start();
	EXPECT_TRUE(Aggregate.IsAcceptingRequests());

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
	auto BaseManager = std::make_shared<FSyntheticManager>("base", BaseState, FName("Durin.Tests.Base"));
	auto DerivedManager = std::make_shared<FSyntheticManager>("derived", DerivedState, FName("Durin.Tests.Derived"));

	auto Base = Aggregate.RegisterCompiler({
		.Name = FName("Durin.Tests.Base"),
		.AssetClasses = {DMaterial::StaticClass(), DTexture::StaticClass()},
		.Manager = BaseManager});
	ASSERT_TRUE(Base);
	auto Derived = Aggregate.RegisterCompiler({
		.Name = FName("Durin.Tests.Derived"),
		.AssetClasses = {DTexture2D::StaticClass()},
		.Manager = DerivedManager});
	ASSERT_TRUE(Derived);
	EXPECT_EQ(BaseState->StartCount, 1u);
	EXPECT_EQ(Aggregate.GetDiagnostics().CompilerCount, 2u);
	Aggregate.Start();
	EXPECT_EQ(Aggregate.GetDiagnostics().CompilerCount, 2u);
	EXPECT_EQ(BaseState->StartCount, 1u);
	EXPECT_EQ(DerivedState->StartCount, 1u);

	auto FailedState = std::make_shared<FSyntheticState>();
	FailedState->StartResult = {EAssetCompilerStartError::StateUnavailable};
	auto FailedManager = std::make_shared<FSyntheticManager>("failed", FailedState);
	const auto StartFailure = FailedManager->Start();
	EXPECT_FALSE(StartFailure);
	EXPECT_EQ(StartFailure.Error, EAssetCompilerStartError::StateUnavailable);
	FailedState->StartResult = {EAssetCompilerStartError::Undrained, 7};
	auto Failed = Aggregate.RegisterCompiler({
		.Name = FName("Durin.Tests.Failed"),
		.AssetClasses = {DMaterialInstance::StaticClass()},
		.Manager = FailedManager});
	EXPECT_FALSE(Failed);
	EXPECT_EQ(Failed.Error.Code, EAssetCompilerRegistrationError::Start);
	EXPECT_EQ(Failed.Error.CompilerName, "Durin.Tests.Failed");
	EXPECT_EQ(Failed.Error.StartCause.Error, EAssetCompilerStartError::Undrained);
	EXPECT_EQ(Failed.Error.StartCause.PendingRequests, 7u);
	EXPECT_EQ(FormatAssetCompilerRegistrationError(Failed.Error), "Asset compiler still owns 7 pending requests.");
	EXPECT_EQ(Aggregate.GetDiagnostics().CompilerCount, 2u);
	EXPECT_TRUE(Aggregate.IsAcceptingRequests());

	const auto DuplicateName = Aggregate.RegisterCompiler({
		.Name = FName("Durin.Tests.Base"),
		.AssetClasses = {DTexture2D::StaticClass()},
		.Manager = std::make_shared<FSyntheticManager>(
			"duplicate-name", std::make_shared<FSyntheticState>())});
	EXPECT_FALSE(DuplicateName);
	EXPECT_EQ(DuplicateName.Error.Code, EAssetCompilerRegistrationError::DuplicateName);
	EXPECT_EQ(DuplicateName.Error.CompilerName, "Durin.Tests.Base");
	const auto DuplicateClass = Aggregate.RegisterCompiler({
		.Name = FName("Durin.Tests.Conflict"),
		.AssetClasses = {DMaterial::StaticClass()},
		.Manager = std::make_shared<FSyntheticManager>(
			"duplicate-class", std::make_shared<FSyntheticState>())});
	EXPECT_FALSE(DuplicateClass);
	EXPECT_EQ(DuplicateClass.Error.Code, EAssetCompilerRegistrationError::DuplicateClass);
	EXPECT_EQ(DuplicateClass.Error.AssetClass, DMaterial::StaticClass()->GetQualifiedName().ToString());

	uint32 EventCount = 0;
	uint32 CompiledAssetCount = 0;
	const FDelegateHandle EventHandle = Aggregate.OnAssetPostCompile().AddLambda(
		[&](const FAssetPostCompileData& Data) {
			++EventCount;
			CompiledAssetCount += static_cast<uint32>(Data.Assets.size());
			EXPECT_TRUE(Data.CompilerName == FName("Durin.Tests.Base")
				|| Data.CompilerName == FName("Durin.Tests.Derived"));
		});
	DObject* Objects[] = {
		FirstMaterial, Texture, TextureCube, Unregistered, SecondMaterial, nullptr};
	Aggregate.FinishCompilationForObjects(Objects);
	EXPECT_EQ(CompiledAssetCount, 4u);
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
	Texture->SetInternalFlags(EObjectInternalFlags::BeginDestroyed);
	DerivedState->bCanceled = false;
	Aggregate.MarkCompilationAsCanceled(*Texture);
	EXPECT_TRUE(DerivedState->bCanceled);
	ASSERT_EQ(DerivedState->LastObjects.size(), 1u);
	EXPECT_EQ(DerivedState->LastObjects.front(), Texture);
	Aggregate.FinishCompilationForObject(*Texture);
	EXPECT_EQ(EventCount, 2u);
	Texture->BeginDestroy();
	// Each compiler advances once; the aggregate does not allocate completion quotas.
	Aggregate.ProcessAsyncTasks({.Deadline = std::chrono::steady_clock::now()});
	EXPECT_EQ(BaseState->ProcessCount, 0u);
	EXPECT_EQ(DerivedState->ProcessCount, 0u);
	Aggregate.ProcessAsyncTasks();
	EXPECT_EQ(BaseState->ProcessCount, 1u);
	EXPECT_EQ(DerivedState->ProcessCount, 1u);
	EXPECT_EQ(Aggregate.GetNumRemainingAssets(), 0u);
	Aggregate.OnAssetPostCompile().Remove(EventHandle);

	Derived.Handle.Reset();
	Base.Handle.Reset();
	EXPECT_EQ(BaseState->FinishAllCount, 1u);
	EXPECT_EQ(BaseState->ShutdownCount, 1u);
	EXPECT_EQ(DerivedState->FinishAllCount, 1u);
	EXPECT_EQ(DerivedState->ShutdownCount, 1u);

	auto RetiredState = std::make_shared<FSyntheticState>();
	auto Retired = Aggregate.RegisterCompiler({
		.Name = FName("Durin.Tests.Retired"),
		.AssetClasses = {DMaterial::StaticClass()},
		.Manager = std::make_shared<FSyntheticManager>("retired", RetiredState)});
	ASSERT_TRUE(Retired);
	Aggregate.ProcessAsyncTasks();
	EXPECT_EQ(RetiredState->ConsumedCompletionCount, 0u);
	EXPECT_GT(RetiredState->ProcessCount, 0u);
	Retired.Handle.Reset();
	EXPECT_EQ(RetiredState->FinishAllCount, 1u);
	EXPECT_EQ(RetiredState->ShutdownCount, 1u);
	// Cross-compiler submissions require another pass, regardless of registry order.
	auto FirstDependency = std::make_shared<FSyntheticState>();
	auto SecondDependency = std::make_shared<FSyntheticState>();
	FirstDependency->Remaining = SecondDependency->Remaining = 1;
	FirstDependency->AvailableCompletions = SecondDependency->AvailableCompletions = 1;
	FirstDependency->OnFinishAll = [SecondDependency] { ++SecondDependency->Remaining; ++SecondDependency->AvailableCompletions; };
	SecondDependency->OnFinishAll = [FirstDependency] { ++FirstDependency->Remaining; ++FirstDependency->AvailableCompletions; };
	auto FirstRegistration = Aggregate.RegisterCompiler({.Name = FName("Durin.Tests.DependencyA"), .AssetClasses = {DMaterial::StaticClass()},
		.Manager = std::make_shared<FSyntheticManager>("dependency-a", FirstDependency)});
	auto SecondRegistration = Aggregate.RegisterCompiler({.Name = FName("Durin.Tests.DependencyB"), .AssetClasses = {DTexture::StaticClass()},
		.Manager = std::make_shared<FSyntheticManager>("dependency-b", SecondDependency)});
	ASSERT_TRUE(FirstRegistration); ASSERT_TRUE(SecondRegistration);
	Aggregate.FinishAllCompilation();
	EXPECT_EQ(FirstDependency->ConsumedCompletionCount + SecondDependency->ConsumedCompletionCount, 4u);
	EXPECT_EQ(FirstDependency->Remaining, 0u); EXPECT_EQ(SecondDependency->Remaining, 0u);
	FirstRegistration.Handle.Reset(); SecondRegistration.Handle.Reset();

	auto ConflictingState = std::make_shared<FSyntheticState>();
	auto Conflicting = Aggregate.RegisterCompiler({
		.Name = FName("Durin.Shader"),
		.AssetClasses = {DMaterial::StaticClass()},
		.Manager = std::make_shared<FSyntheticManager>("conflicting-builtin", ConflictingState)});
	ASSERT_TRUE(Conflicting);
	EXPECT_DEATH_IF_SUPPORTED(InitializeAssetCompilingManager(), "Built-in asset compiler registration failed");
	EXPECT_TRUE(Aggregate.IsAcceptingRequests());
	Conflicting.Handle.Reset();
	EXPECT_EQ(ConflictingState->ShutdownCount, 1u);
	EXPECT_EQ(Aggregate.GetDiagnostics().CompilerCount, 0u);
	InitializeAssetCompilingManager();
	EXPECT_TRUE(Aggregate.IsAcceptingRequests());
	EXPECT_GT(Aggregate.GetDiagnostics().CompilerCount, 0u);
	ShutdownAssetCompilingManager();
	EXPECT_EQ(Aggregate.GetDiagnostics().CompilerCount, 0u);
	EXPECT_FALSE(Aggregate.IsAcceptingRequests());
}
