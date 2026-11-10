#include "Editor/EditorEngineTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Editor/EditorSubsystem.h"
#include "Editor/IMainFrameModule.h"
#include "Tools/EditorToolSubsystem.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/ToolkitHost.h"
#include "AssetDefinition/AssetDefinition.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "Documents/EditorDocumentManager.h"
#include "DObject/Package.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "DObject/Class.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/ObjectLifecycle.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include <gtest/gtest.h>

namespace Durin
{
	class FDynamicEditorA : public DEditorSubsystem
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FDynamicEditorA, DEditorSubsystem, FDynamicEditorA::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FDynamicEditorA)
	public:
		explicit FDynamicEditorA(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) {}
		auto Initialize(FSubsystemCollection& Collection) -> void override
		{
			for (auto* Type : Dependencies[GetClass()])
				if (!Collection.InitializeDependency(Type))
				{
					if (GetEditor()->IsShutdownRequested()) return;
					requiref(false, "Required editor dependency is unavailable.");
				}
			if (Init) Init(*this);
		}
		inline static std::map<DClass*, std::vector<DClass*>> Dependencies;
		auto Deinitialize() noexcept -> void override { if (Deinit) Deinit(*this); }
		auto BeginDestroy() -> void override { if (Destroy) Destroy(*this); Super::BeginDestroy(); }
		auto ShouldCreateSubsystem(DObject* Outer) const -> bool override { return !Create || Create(*this, Outer); }
		inline static std::function<bool(const FDynamicEditorA&, DObject*)> Create;
		inline static std::function<void(FDynamicEditorA&)> Init;
		inline static std::function<void(FDynamicEditorA&)> Deinit;
		inline static std::function<void(FDynamicEditorA&)> Destroy;
	};
	class FDynamicEditorB : public FDynamicEditorA
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FDynamicEditorB, FDynamicEditorA, FDynamicEditorB::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FDynamicEditorB)
	public:
		explicit FDynamicEditorB(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) {}
	};
	class FDynamicEditorC : public FDynamicEditorA
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FDynamicEditorC, FDynamicEditorA, FDynamicEditorC::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FDynamicEditorC)
	public:
		explicit FDynamicEditorC(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) {}
	};
	auto FDynamicEditorA::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class)
			GetPrivateStaticClassBody("", "FDynamicEditorA", Class, nullptr, sizeof(FDynamicEditorA), alignof(FDynamicEditorA),
				EClassFlags::None, InternalConstructor<FDynamicEditorA>, &DEditorSubsystem::StaticClass);
		return Class;
	}
	auto FDynamicEditorB::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class)
			GetPrivateStaticClassBody("", "FDynamicEditorB", Class, nullptr, sizeof(FDynamicEditorB), alignof(FDynamicEditorB),
				EClassFlags::None, InternalConstructor<FDynamicEditorB>, &FDynamicEditorA::StaticClass);
		return Class;
	}
	auto FDynamicEditorC::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class)
			GetPrivateStaticClassBody("", "FDynamicEditorC", Class, nullptr, sizeof(FDynamicEditorC), alignof(FDynamicEditorC),
				EClassFlags::None, InternalConstructor<FDynamicEditorC>, &FDynamicEditorA::StaticClass);
		return Class;
	}

	class FFailingAssetEditorSubsystem : public DAssetEditorSubsystem
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FFailingAssetEditorSubsystem, DAssetEditorSubsystem, FFailingAssetEditorSubsystem::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FFailingAssetEditorSubsystem)
	public:
		explicit FFailingAssetEditorSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto Initialize(FSubsystemCollection&) -> void override { throw std::runtime_error("failure before toolkit creation"); }
		auto Deinitialize() noexcept -> void override { ++CleanupCalls; Super::Deinitialize(); }
		inline static int CleanupCalls = 0;
	};
	auto FFailingAssetEditorSubsystem::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class)
			GetPrivateStaticClassBody("", "FFailingAssetEditorSubsystem", Class, nullptr,
				sizeof(FFailingAssetEditorSubsystem), alignof(FFailingAssetEditorSubsystem),
				EClassFlags::None, InternalConstructor<FFailingAssetEditorSubsystem>, &DAssetEditorSubsystem::StaticClass);
		return Class;
	}

	class FDynamicOpenDefinition : public DAssetDefinition
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FDynamicOpenDefinition, DAssetDefinition, FDynamicOpenDefinition::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FDynamicOpenDefinition)
	public:
		explicit FDynamicOpenDefinition(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto GetAssetClassPath() const -> std::string override { return ClassPath; }
		std::string ClassPath = "Durin::DObject";
		auto CanRegisterStatically() const -> bool override { return false; }
		auto GetAssetOpenSupport(const FAssetOpenSupportArgs& Args) const -> FAssetOpenSupport override
		{ return {Args.OpenMethod, Args.OpenMethod == EAssetOpenMethod::Edit || (Args.OpenMethod == EAssetOpenMethod::View && bSupportsView), RequiredMode}; }
		auto OpenAssets(const FAssetOpenArgs& Args) const -> FAssetOpenResult override { return Open(Args); }
		std::function<FAssetOpenResult(const FAssetOpenArgs&)> Open;
		bool bSupportsView = false;
		std::optional<EToolkitMode> RequiredMode;
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FDynamicOpenDefinition)
}

namespace
{
	using namespace Durin;
	class FEditorSubsystemTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			Testing::InitializeDObjectSystemForTests();
			const std::array Classes{FDynamicEditorA::StaticClass(), FDynamicEditorB::StaticClass(), FDynamicEditorC::StaticClass(), FFailingAssetEditorSubsystem::StaticClass()};
			for (auto* Class : Classes) DObjectForceRegistration(Class);
			ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
			FFailingAssetEditorSubsystem::CleanupCalls = 0;
		}
		auto TearDown() -> void override
		{
			FDynamicEditorA::Init = {}; FDynamicEditorA::Deinit = {}; FDynamicEditorA::Destroy = {}; FDynamicEditorA::Create = {}; FDynamicEditorA::Dependencies.clear();
			for (auto& Host : Hosts) { Host->PreExit(); MarkObjectHierarchyAsGarbage(Host.Get()); }
			Hosts.clear(); CollectGarbage();
		}
		auto NewHost() -> DEditorEngine*
		{
			auto* Host = NewObject<DEditorEngine>(nullptr, {}); Hosts.emplace_back(Host);
			EXPECT_NO_THROW(FEditorEngineTestAccess::StartEditor(*Host)); return Host;
		}
		auto RegisterA() -> std::unique_ptr<FEditorSubsystemRegistration>
		{ return std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = FDynamicEditorA::StaticClass()}); }
		std::vector<TStrongObjectPtr<DEditorEngine>> Hosts;
	};

	auto DefinitionTarget(std::string_view Path = "/AssetEditorTests/Package.Asset") -> FAssetOpenTarget
	{
		FSoftObjectPath ObjectPath;
		require(FSoftObjectPath::TryCreate(Path, ObjectPath));
		return {ObjectPath, "Durin::DObject"};
	}
	struct FDefinitionTestMount
	{
		Testing::FScopedMountRegistryFixture Mounts;
		FDefinitionTestMount()
		{
			Testing::RegisterMountPointForTests("/AssetEditorTests/", Testing::GetTestWorkDirectory().generic_string() + "/");
		}
	};

	TEST_F(FEditorSubsystemTests, DefinitionDispatchPreservesMixedResultsAndDeduplicatesInputs)
	{
		FDefinitionTestMount Mount;
		auto* Service = NewHost()->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Definition = NewObject<FDynamicOpenDefinition>(nullptr, {});
		int Opens = 0;
		Definition->Open = [&](const FAssetOpenArgs& Args) {
			++Opens;
			return FAssetOpenResult{{{.Target = Args.Assets.front(), .Status = EAssetOpenStatus::Completed}}};
		};
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		ASSERT_TRUE(Registration);
		const auto Target = DefinitionTarget();
		const auto Result = Service->OpenAssets({.AssetEditors = Service, .Assets = {Target, {}, Target}});
		ASSERT_EQ(3u, Result.Items.size());
		EXPECT_EQ(EAssetOpenStatus::Completed, Result.Items[0].Status);
		EXPECT_EQ(EAssetOpenStatus::Rejected, Result.Items[1].Status);
		EXPECT_EQ(EAssetOpenStatus::Completed, Result.Items[2].Status);
		EXPECT_FALSE(Result.IsAccepted());
		EXPECT_EQ(1, Opens);
	}

	TEST_F(FEditorSubsystemTests, DefinitionReceivesOneUniqueOrderedBatchAndRejectsMetadataConflicts)
	{
		FDefinitionTestMount Mount;
		auto* Service = NewHost()->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Definition = NewObject<FDynamicOpenDefinition>(nullptr, {});
		const auto A = DefinitionTarget(), B = DefinitionTarget("/AssetEditorTests/Other.Asset");
		auto Conflict = A; Conflict.AssetClassName = "Durin::DPackage";
		int Calls = 0;
		Definition->Open = [&](const FAssetOpenArgs& Args) {
			++Calls;
			EXPECT_EQ(2u, Args.Assets.size());
			EXPECT_EQ(A.AssetPath, Args.Assets[0].AssetPath);
			EXPECT_EQ(B.AssetPath, Args.Assets[1].AssetPath);
			// Every member is reserved before the provider executes.
			const auto Recursive = Service->OpenAssets(Args);
			for (const auto& Item : Recursive.Items) EXPECT_EQ(EAssetOpenStatus::Rejected, Item.Status);
			FAssetOpenResult Result;
			for (const auto& Target : Args.Assets) Result.Items.push_back({.Target = Target, .Status = EAssetOpenStatus::Completed});
			return Result;
		};
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		const auto Result = Service->OpenAssets({.AssetEditors = Service, .Assets = {A, B, A, Conflict}});
		ASSERT_EQ(4u, Result.Items.size());
		EXPECT_EQ(1, Calls);
		for (size_t I = 0; I < 3; ++I) EXPECT_EQ(EAssetOpenStatus::Completed, Result.Items[I].Status);
		EXPECT_EQ(EAssetOpenStatus::Rejected, Result.Items[3].Status);
	}

	TEST_F(FEditorSubsystemTests, PendingViewBatchRetainsMethodAndProviderForEveryItem)
	{
		FDefinitionTestMount Mount;
		auto* Service = NewHost()->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Definition = NewObject<FDynamicOpenDefinition>(nullptr, {});
		Definition->bSupportsView = true;
		Definition->Open = [&](const FAssetOpenArgs& Args) {
			FAssetOpenResult Result;
			for (const auto& Target : Args.Assets) Result.Items.push_back({.Target = Target, .Status = EAssetOpenStatus::Pending,
				.RequestId = Service->BeginOpenRequest(Target, [] { return true; })});
			return Result;
		};
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		std::vector<EAssetOpenMethod> Methods;
		auto Subscription = Service->Subscribe([&](const FAssetEditorEvent& Event) {
			if (Event.Type == EAssetEditorEvent::RequestFinished) Methods.push_back(Event.Method);
		});
		const auto Result = Service->OpenAssets({.AssetEditors = Service,
			.Assets = {DefinitionTarget(), DefinitionTarget("/AssetEditorTests/Other.Asset")}, .OpenMethod = EAssetOpenMethod::View});
		ASSERT_TRUE(Result.IsAccepted());
		Registration.Reset(); CollectGarbage();
		for (const auto& Item : Result.Items) EXPECT_TRUE(Service->CompleteOpenRequest(Item.RequestId, EAssetOpenStatus::Completed));
		ASSERT_EQ(2u, Methods.size());
		EXPECT_EQ(EAssetOpenMethod::View, Methods[0]); EXPECT_EQ(EAssetOpenMethod::View, Methods[1]);
	}

	TEST_F(FEditorSubsystemTests, SupportNormalizesRequiredModeAndRejectsExplicitHostBeforeExecution)
	{
		FDefinitionTestMount Mount;
		auto* Service = NewHost()->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Definition = NewObject<FDynamicOpenDefinition>(nullptr, {});
		Definition->RequiredMode = EToolkitMode::WorldCentric;
		int Calls = 0;
		Definition->Open = [&](const FAssetOpenArgs& Args) {
			++Calls; EXPECT_EQ(EToolkitMode::WorldCentric, Args.ToolkitMode);
			EXPECT_EQ("Restore", Args.Origin); EXPECT_FALSE(Args.bIsRestore);
			return FAssetOpenResult{{{.Target = Args.Assets.front(), .Status = EAssetOpenStatus::Completed}}};
		};
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		EXPECT_TRUE(Service->CanOpenAsset(DefinitionTarget()));
		FAssetOpenArgs Args{.AssetEditors = Service, .Assets = {DefinitionTarget()}, .Origin = "Restore"};
		EXPECT_TRUE(Service->OpenAssets(Args).IsAccepted());
		Args.Host = std::make_shared<FNonWindowToolkitHost>();
		EXPECT_EQ(EAssetOpenStatus::Rejected, Service->OpenAssets(Args).Items.front().Status);
		EXPECT_EQ(1, Calls);
		Args.Host = std::make_shared<FNonWindowToolkitHost>(EToolkitMode::WorldCentric);
		EXPECT_TRUE(Service->OpenAssets(Args).IsAccepted()); EXPECT_EQ(2, Calls);
		Definition->RequiredMode.reset();
		// An explicit host determines the effective mode even when the preference differs.
		EXPECT_TRUE(Service->OpenAssets(Args).IsAccepted()); EXPECT_EQ(3, Calls);
	}

	TEST_F(FEditorSubsystemTests, IncompatibleRequiredModesRejectAnEntireGroupBeforeExecution)
	{
		FDefinitionTestMount Mount;
		auto* Service = NewHost()->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* First = NewObject<FDynamicOpenDefinition>(nullptr, {});
		auto* Second = NewObject<FDynamicOpenDefinition>(nullptr, {});
		First->RequiredMode = EToolkitMode::WorldCentric;
		Second->RequiredMode = EToolkitMode::Standalone; Second->ClassPath = "ModeTest::DAsset";
		First->Open = Second->Open = [](const FAssetOpenArgs&) { ADD_FAILURE() << "Incompatible group must not execute"; return FAssetOpenResult{}; };
		auto A = FAssetDefinitionRegistry::Get().RegisterDefinition(*First);
		auto B = FAssetDefinitionRegistry::Get().RegisterDefinition(*Second);
		auto Other = DefinitionTarget("/AssetEditorTests/Other.Asset"); Other.AssetClassName = Second->ClassPath;
		const auto Result = Service->OpenAssets({.AssetEditors = Service, .Assets = {DefinitionTarget(), Other}, .bCreateSingleSession = true});
		ASSERT_EQ(2u, Result.Items.size());
		for (const auto& Item : Result.Items) EXPECT_EQ(EAssetOpenStatus::Rejected, Item.Status);
	}

	TEST_F(FEditorSubsystemTests, PendingDefinitionRequestRetainsProviderUntilCancellationAndRejectsLateCompletion)
	{
		FDefinitionTestMount Mount;
		auto* Service = NewHost()->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Definition = NewObject<FDynamicOpenDefinition>(nullptr, {});
		const FObjectKey Provider(Definition);
		int Cancellations = 0;
		Definition->Open = [&](const FAssetOpenArgs& Args) {
			const auto Id = Args.AssetEditors->BeginOpenRequest(Args.Assets.front(), [&] { ++Cancellations; return true; });
			return FAssetOpenResult{{{.Target = Args.Assets.front(), .Status = EAssetOpenStatus::Pending, .RequestId = Id}}};
		};
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		const auto Opened = Service->OpenAssets({.AssetEditors = Service, .Assets = {DefinitionTarget()}});
		ASSERT_EQ(1u, Opened.Items.size());
		const auto Id = Opened.Items.front().RequestId;
		ASSERT_TRUE(Id.IsValid());
		EXPECT_EQ(EAssetOpenStatus::Pending, Opened.Items.front().Status);
		EXPECT_TRUE(Service->GetAllOpenEditors().empty());
		Registration.Reset();
		CollectGarbage();
		EXPECT_EQ(Definition, Provider.ResolveObjectPtr());
		EXPECT_TRUE(Service->CancelOpenRequest(Id));
		EXPECT_EQ(1, Cancellations);
		ASSERT_TRUE(Service->GetOpenRequest(Id));
		EXPECT_EQ(EAssetOpenStatus::Cancelled, Service->GetOpenRequest(Id)->Status);
		EXPECT_FALSE(Service->CompleteOpenRequest(Id, EAssetOpenStatus::Completed));
		CollectGarbage();
		EXPECT_EQ(nullptr, Provider.ResolveObjectPtr());
	}

	TEST_F(FEditorSubsystemTests, TerminalHistoryIsBoundedWithoutEvictingPendingWork)
	{
		FDefinitionTestMount Mount;
		auto* Service = NewHost()->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Definition = NewObject<FDynamicOpenDefinition>(nullptr, {});
		std::vector<FAssetOpenRequestId> Ids;
		Definition->Open = [&](const FAssetOpenArgs& Args) {
			for (int Index = 0; Index != 300; ++Index)
			{
				const auto Id = Service->BeginOpenRequest(Args.Assets.front(), [] { return true; });
				Ids.push_back(Id);
				if (Index) EXPECT_TRUE(Service->CompleteOpenRequest(Id, EAssetOpenStatus::Completed));
			}
			return FAssetOpenResult{{{.Target = Args.Assets.front(), .Status = EAssetOpenStatus::Pending, .RequestId = Ids.front()}}};
		};
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		EXPECT_EQ(EAssetOpenStatus::Pending, Service->OpenAssets({.AssetEditors = Service, .Assets = {DefinitionTarget()}}).Items.front().Status);
		ASSERT_TRUE(Service->GetOpenRequest(Ids.front()));
		EXPECT_EQ(EAssetOpenStatus::Pending, Service->GetOpenRequest(Ids.front())->Status);
		EXPECT_FALSE(Service->GetOpenRequest(Ids[1]));
		EXPECT_EQ(EAssetOpenStatus::Completed, Service->GetOpenRequest(Ids.back())->Status);
		EXPECT_TRUE(Service->CancelOpenRequest(Ids.front()));
		EXPECT_EQ(EAssetOpenStatus::Cancelled, Service->GetOpenRequest(Ids.front())->Status);
	}

	TEST_F(FEditorSubsystemTests, RefusedCancellationLeavesRequestPendingAndProviderRetained)
	{
		FDefinitionTestMount Mount;
		auto* Service = NewHost()->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Definition = NewObject<FDynamicOpenDefinition>(nullptr, {});
		Definition->Open = [&](const FAssetOpenArgs& Args) {
			const auto Id = Service->BeginOpenRequest(Args.Assets.front(), [] { return false; });
			return FAssetOpenResult{{{.Target = Args.Assets.front(), .Status = EAssetOpenStatus::Pending, .RequestId = Id}}};
		};
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		const auto Result = Service->OpenAssets({.AssetEditors = Service, .Assets = {DefinitionTarget()}});
		const auto Id = Result.Items.front().RequestId;
		ASSERT_TRUE(Id.IsValid());
		EXPECT_FALSE(Service->CancelOpenRequest(Id));
		EXPECT_EQ(EAssetOpenStatus::Pending, Service->GetOpenRequest(Id)->Status);
		EXPECT_TRUE(Service->CompleteOpenRequest(Id, EAssetOpenStatus::Failed, "Provider failed."));
		EXPECT_EQ("Provider failed.", Service->GetOpenRequest(Id)->Message);
	}

	TEST_F(FEditorSubsystemTests, DefinitionDispatchRejectsRecursionAndUnsupportedViewBeforeExecution)
	{
		FDefinitionTestMount Mount;
		auto* Service = NewHost()->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Definition = NewObject<FDynamicOpenDefinition>(nullptr, {});
		int Opens = 0;
		Definition->Open = [&](const FAssetOpenArgs& Args) {
			++Opens;
			const auto Recursive = Service->OpenAssets(Args);
			EXPECT_EQ(EAssetOpenStatus::Rejected, Recursive.Items.front().Status);
			return FAssetOpenResult{{{.Target = Args.Assets.front(), .Status = EAssetOpenStatus::Completed}}};
		};
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		FAssetOpenArgs Args{.AssetEditors = Service, .Assets = {DefinitionTarget()}, .OpenMethod = EAssetOpenMethod::View};
		EXPECT_FALSE(Service->CanOpenAsset(Args.Assets.front(), EAssetOpenMethod::View));
		EXPECT_EQ(EAssetOpenStatus::Rejected, Service->OpenAssets(Args).Items.front().Status);
		EXPECT_EQ(0, Opens);
		Args.OpenMethod = EAssetOpenMethod::Edit;
		EXPECT_TRUE(Service->OpenAssets(Args).IsAccepted());
		EXPECT_EQ(1, Opens);
	}

	TEST_F(FEditorSubsystemTests, InlinePendingCompletionPreservesItsTerminalOutcome)
	{
		FDefinitionTestMount Mount;
		auto* Service = NewHost()->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Definition = NewObject<FDynamicOpenDefinition>(nullptr, {});
		Definition->Open = [&](const FAssetOpenArgs& Args) {
			const auto Id = Service->BeginOpenRequest(Args.Assets.front(), [] { return true; });
			EXPECT_TRUE(Service->CompleteOpenRequest(Id, EAssetOpenStatus::Completed));
			return FAssetOpenResult{{{.Target = Args.Assets.front(), .Status = EAssetOpenStatus::Pending, .RequestId = Id}}};
		};
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		const auto Result = Service->OpenAssets({.AssetEditors = Service, .Assets = {DefinitionTarget()}});
		ASSERT_EQ(1u, Result.Items.size());
		EXPECT_EQ(EAssetOpenStatus::Completed, Result.Items.front().Status);
		EXPECT_TRUE(Result.IsAccepted());
	}

	TEST_F(FEditorSubsystemTests, AddsAndRemovesInstancesWithoutRetiringTheActiveEditor)
	{
		auto* First = NewHost();
		FEditorEngineTestAccess::SetStartupPhase(*First, Editor::Startup::EBootstrapState::Ready);
		ASSERT_TRUE(First->GetCapabilities().bTick);
		auto* Tools = First->GetEditorSubsystem<DEditorToolSubsystem>();
		auto Registration = RegisterA();
		First->Tick(0.0f, false);
		EXPECT_EQ(nullptr, First->GetEditorSubsystem<FDynamicEditorA>());
		ASSERT_NO_THROW(First->AddRegisteredEditorSubsystems());
		auto* A = First->GetEditorSubsystem<FDynamicEditorA>(); ASSERT_NE(nullptr, A);
		EXPECT_EQ(A->GetEditor(), First);
		Registration.reset();
		First->Tick(0.0f, false);
		EXPECT_EQ(A, First->GetEditorSubsystem<FDynamicEditorA>());

		ASSERT_NO_THROW(First->RemoveEditorSubsystem(FDynamicEditorA::StaticClass()));
		FDynamicEditorA::Destroy = [&](auto&) {
			EXPECT_TRUE(IsGarbageCollecting());
			EXPECT_DEATH(First->AddRegisteredEditorSubsystems(), "outside GC and feature dispatch");
		};
		CollectGarbage();
		FDynamicEditorA::Destroy = {};
		EXPECT_TRUE(First->AreEditorSubsystemsInitialized());
		EXPECT_EQ(Tools, First->GetEditorSubsystem<DEditorToolSubsystem>());
		EXPECT_EQ(nullptr, First->GetEditorSubsystem<FDynamicEditorA>());
	}

	TEST_F(FEditorSubsystemTests, RemovalReleasesCollectionReferencesWithoutInvalidatingRetainedObjects)
	{
		auto* Host = NewHost(); auto Registration = RegisterA();
		ASSERT_NO_THROW(Host->AddRegisteredEditorSubsystems());
		TStrongObjectPtr<FDynamicEditorA> Retained(Host->GetEditorSubsystem<FDynamicEditorA>());
		TWeakObjectPtr<FDynamicEditorA> Weak(Retained.Get());
		int Cleanups = 0, Destructions = 0;
		FDynamicEditorA::Deinit = [&](auto&) { ++Cleanups; };
		FDynamicEditorA::Destroy = [&](auto&) { ++Destructions; };
		Registration.reset(); ASSERT_NO_THROW(Host->RemoveEditorSubsystem(FDynamicEditorA::StaticClass()));
		EXPECT_EQ(1, Cleanups); EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_TRUE(IsValid(Retained.Get())); EXPECT_FALSE(Retained->IsGarbage());

		CollectGarbage();
		EXPECT_TRUE(Weak.IsValid()); EXPECT_EQ(0, Destructions);
		Retained.Reset(); CollectGarbage();
		EXPECT_FALSE(Weak.IsValid()); EXPECT_EQ(1, Destructions); EXPECT_EQ(1, Cleanups);
		EXPECT_TRUE(Host->AreEditorSubsystemsInitialized());
		FDynamicEditorA::Deinit = {}; FDynamicEditorA::Destroy = {};
	}

	TEST_F(FEditorSubsystemTests, FailedAdditionDeinitializesRetainedObjectWithoutForcingGarbage)
	{
		auto* Host = NewHost(); auto Registration = RegisterA();
		TStrongObjectPtr<FDynamicEditorA> Retained;
		int Cleanups = 0;
		FDynamicEditorA::Init = [&](auto& Object) {
			Retained = &Object;
			throw std::runtime_error("fixture");
		};
		FDynamicEditorA::Deinit = [&](auto&) { ++Cleanups; };
		EXPECT_THROW(Host->AddRegisteredEditorSubsystems(), std::exception);
		ASSERT_NE(nullptr, Retained.Get());
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_EQ(1, Cleanups); EXPECT_FALSE(Retained->IsGarbage());

		CollectGarbage(); EXPECT_TRUE(IsValid(Retained.Get()));
		Retained.Reset(); CollectGarbage();
		EXPECT_TRUE(Host->AreEditorSubsystemsInitialized());
		EXPECT_EQ(1, Cleanups);
		FDynamicEditorA::Init = {}; FDynamicEditorA::Deinit = {};
	}

	TEST_F(FEditorSubsystemTests, InitializationRequiresExplicitActivationAndNeverPublishesEditor)
	{
		auto* First = NewObject<DEditorEngine>(nullptr, "ExplicitFirstEditor"); Hosts.emplace_back(First);
		auto* Second = NewObject<DEditorEngine>(nullptr, "ExplicitSecondEditor"); Hosts.emplace_back(Second);
		EXPECT_EQ(GEditor, nullptr);
		EXPECT_FALSE(First->Init({}));
		EXPECT_DEATH(FEditorEngineTestAccess::InitializeServices(*First), "active editor");
		EXPECT_EQ(GEditor, nullptr);
		ASSERT_TRUE(First->ActivateEditor());
		EXPECT_FALSE(First->ActivateEditor());
		EXPECT_FALSE(First->AreEditorSubsystemsInitialized());
		EXPECT_DEATH(FEditorEngineTestAccess::InitializeServices(*Second), "active editor");
		EXPECT_FALSE(Second->Init({}));
		EXPECT_EQ(GEditor, First);
		ASSERT_NO_THROW(FEditorEngineTestAccess::InitializeServices(*First));
		First->PreExit();
		EXPECT_EQ(GEditor, nullptr);
		EXPECT_FALSE(First->ActivateEditor());
		ASSERT_TRUE(Second->ActivateEditor());
		ASSERT_NO_THROW(FEditorEngineTestAccess::InitializeServices(*Second));
		EXPECT_EQ(GEditor, Second);
		Second->PreExit();
		EXPECT_EQ(GEditor, nullptr);
	}

	TEST_F(FEditorSubsystemTests, ConstructionCannotReplaceActiveEditorAndActivationIsExclusive)
	{
		auto* First = NewHost();
		TStrongObjectPtr<DEditorEngine> Second(NewObject<DEditorEngine>(nullptr, "InactiveEditor"));
		EXPECT_EQ(GEditor, First);
		EXPECT_DEATH(FEditorEngineTestAccess::StartEditor(*Second), "Editor activation failed");
		EXPECT_EQ(GEditor, First);
		First->PreExit();
		EXPECT_EQ(GEditor, nullptr);
		ASSERT_NO_THROW(FEditorEngineTestAccess::StartEditor(*Second));
		EXPECT_EQ(GEditor, Second.Get());
		Second->PreExit();
		EXPECT_EQ(GEditor, nullptr);
		EXPECT_FALSE(First->ActivateEditor());
	}

	TEST_F(FEditorSubsystemTests, RetirementPreservesRetainedObjectsAndHostOwnsReentrantCleanup)
	{
		auto* Host = NewHost(); auto Registration = RegisterA();
		Host->AddRegisteredEditorSubsystems();
		auto* Service = Host->GetEditorSubsystem<FDynamicEditorA>();
		ASSERT_NE(nullptr, Service);
		TStrongObjectPtr<FDynamicEditorA> Retained(Service);
		bool bInvalidated = false;
		FObjectReferencePurgeRegistration Observer([&](DObject& Object) {
			if (&Object == Service) bInvalidated = true;
		});
		int Cleanups = 0;
		FDynamicEditorA::Deinit = [&](auto&) { ++Cleanups; Host->RequestShutdown(); };
		Host->RemoveEditorSubsystem(FDynamicEditorA::StaticClass());
		EXPECT_TRUE(Host->AreEditorSubsystemsInitialized());
		Host->PreExit();
		EXPECT_EQ(1, Cleanups);
		EXPECT_FALSE(Host->AreEditorSubsystemsInitialized());
		EXPECT_FALSE(bInvalidated);
		CollectGarbage();
		EXPECT_TRUE(Retained.IsValid());
		EXPECT_FALSE(bInvalidated);
		Retained.Reset(); CollectGarbage();
		EXPECT_TRUE(bInvalidated);
		FDynamicEditorA::Deinit = {};
	}

	TEST_F(FEditorSubsystemTests, CollectionCleanupDuringInitializationIsAContractViolation)
	{
		auto* Host = NewHost();
		FSubsystemCollection Collection(*Host, DEditorSubsystem::StaticClass());
		FDynamicEditorA::Init = [&](auto&) { Collection.Deinitialize(); };
		EXPECT_DEATH(Collection.Initialize({{.Type = FDynamicEditorA::StaticClass()}}), "idle collection");
		FDynamicEditorA::Init = {};
		Collection.Deinitialize();
	}

	TEST_F(FEditorSubsystemTests, RuntimeAndTypedLookupAgreeDuringInitializationAndCleanup)
	{
	FDynamicEditorA::Dependencies[FDynamicEditorB::StaticClass()] = {FDynamicEditorA::StaticClass()};
		auto A = RegisterA();
		auto B = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
			.Type = FDynamicEditorB::StaticClass()});
		auto* Host = NewObject<DEditorEngine>(nullptr, {}); Hosts.emplace_back(Host);
		int DependenciesObserved = 0;
		auto CheckDependency = [&](auto& Object) {
			if (Object.GetClass() != FDynamicEditorB::StaticClass()) return;
			auto* Dependency = Host->GetEditorSubsystem<FDynamicEditorA>();
			ASSERT_NE(nullptr, Dependency);
			EXPECT_EQ(Dependency, Host->GetEditorSubsystem(FDynamicEditorA::StaticClass()));
			++DependenciesObserved;
		};
		FDynamicEditorA::Init = [&](auto& Object) {
			CheckDependency(Object);
			EXPECT_EQ(nullptr, Host->GetEditorSubsystem(Object.GetClass()));
			return;
		};
		FDynamicEditorA::Deinit = CheckDependency;
		ASSERT_NO_THROW(FEditorEngineTestAccess::StartEditor(*Host));
		EXPECT_EQ(1, DependenciesObserved);
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem(nullptr));
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem(DObject::StaticClass()));
		ASSERT_TRUE(Host->AddEditorSubsystem(FDynamicEditorB::StaticClass()));
		EXPECT_EQ(1, DependenciesObserved);
		EXPECT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorB>());
		// The caller stops the consumer before removing its dependency.
		ASSERT_NO_THROW(Host->RemoveEditorSubsystem(FDynamicEditorB::StaticClass()));
		B.reset();
		ASSERT_NO_THROW(Host->RemoveEditorSubsystem(FDynamicEditorA::StaticClass())); A.reset();
		EXPECT_EQ(2, DependenciesObserved);
		A = RegisterA();
		B = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
			.Type = FDynamicEditorB::StaticClass()});
		ASSERT_NO_THROW(Host->AddRegisteredEditorSubsystems());
		EXPECT_EQ(3, DependenciesObserved);
		Host->PreExit();
		EXPECT_EQ(4, DependenciesObserved);
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem(FDynamicEditorA::StaticClass()));
		FDynamicEditorA::Init = {}; FDynamicEditorA::Deinit = {};
	}















	TEST_F(FEditorSubsystemTests, RepeatedAdditionReusesTheInstanceUntilExplicitRemoval)
	{
		auto* Host = NewHost(); auto A = RegisterA(); int Initializations = 0, Cleanups = 0;
		FDynamicEditorA::Init = [&](auto&) { ++Initializations; return; };
		FDynamicEditorA::Deinit = [&](auto&) { ++Cleanups; };
		ASSERT_TRUE(Host->AddEditorSubsystem(FDynamicEditorA::StaticClass()));
		TStrongObjectPtr<FDynamicEditorA> Old(Host->GetEditorSubsystem<FDynamicEditorA>());
		A.reset();
		EXPECT_EQ(nullptr, Host->AddEditorSubsystem(FDynamicEditorA::StaticClass()));
		EXPECT_EQ(Old.Get(), Host->GetEditorSubsystem<FDynamicEditorA>());
		A = RegisterA();
		ASSERT_TRUE(Host->AddEditorSubsystem(FDynamicEditorA::StaticClass()));
		EXPECT_EQ(Old.Get(), Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_EQ(1, Initializations); EXPECT_EQ(0, Cleanups);
		ASSERT_NO_THROW(Host->RemoveEditorSubsystem(FDynamicEditorA::StaticClass()));
		ASSERT_TRUE(Host->AddEditorSubsystem(FDynamicEditorA::StaticClass()));
		EXPECT_NE(Old.Get(), Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_EQ(2, Initializations); EXPECT_EQ(1, Cleanups);
		FDynamicEditorA::Init = {}; FDynamicEditorA::Deinit = {};
	}

	TEST_F(FEditorSubsystemTests, FailedAdditionKeepsCompletedServicesAndCleansOnlyTheFailedObject)
	{
		auto* Host = NewHost(); auto A = RegisterA();
		FEditorSubsystemRegistration B({.Type = FDynamicEditorB::StaticClass()});
		FDynamicEditorA::Dependencies[FDynamicEditorB::StaticClass()] = {FDynamicEditorA::StaticClass()};
		std::vector<DClass*> Cleaned;
		FDynamicEditorA::Init = [](auto& Object) {
			if (Object.GetClass() == FDynamicEditorB::StaticClass()) throw std::runtime_error("expected");
		};
		FDynamicEditorA::Deinit = [&](auto& Object) { Cleaned.push_back(Object.GetClass()); };
		EXPECT_THROW(Host->AddEditorSubsystem(FDynamicEditorB::StaticClass()), std::exception);
		EXPECT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorB>());
		EXPECT_EQ((std::vector<DClass*>{FDynamicEditorB::StaticClass()}), Cleaned);
		FDynamicEditorA::Init = {}; ASSERT_TRUE(Host->AddEditorSubsystem(FDynamicEditorB::StaticClass()));
		FDynamicEditorA::Deinit = {};
	}

	TEST_F(FEditorSubsystemTests, RecursiveDependencyIsAContractViolation)
	{
		auto* Host = NewHost(); FEditorSubsystemRegistration C({.Type = FDynamicEditorC::StaticClass()});
		ASSERT_TRUE(Host->AddEditorSubsystem(FDynamicEditorC::StaticClass()));
		auto* Existing = Host->GetEditorSubsystem<FDynamicEditorC>();
		auto A = RegisterA(); FEditorSubsystemRegistration B({.Type = FDynamicEditorB::StaticClass()});
		FDynamicEditorA::Dependencies[FDynamicEditorA::StaticClass()] = {FDynamicEditorB::StaticClass()};
		FDynamicEditorA::Dependencies[FDynamicEditorB::StaticClass()] = {FDynamicEditorA::StaticClass()};
		EXPECT_DEATH(Host->AddEditorSubsystem(FDynamicEditorA::StaticClass()), "Recursive subsystem dependency");
		EXPECT_EQ(Existing, Host->GetEditorSubsystem<FDynamicEditorC>());
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorB>());
		FDynamicEditorA::Dependencies.clear();
	}

	TEST_F(FEditorSubsystemTests, DefersChangesFromCallbacksAndInitialization)
	{
		auto* Host = NewHost(); std::unique_ptr<FEditorSubsystemRegistration> B;
		auto A = RegisterA();
		FDynamicEditorA::Init = [&](auto& Object) {
			if (Object.GetClass() == FDynamicEditorA::StaticClass())
			{
				B = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = FDynamicEditorB::StaticClass()});
				EXPECT_DEATH(Host->AddRegisteredEditorSubsystems(), "idle, initialized collection");
				EXPECT_DEATH(Host->AddEditorSubsystem(FDynamicEditorC::StaticClass()), "idle, initialized collection");
				EXPECT_FALSE(Host->GetEditorSubsystem<DEditorToolSubsystem>()->CanCallNow());
			}
			return;
		};
		ASSERT_NO_THROW(Host->AddRegisteredEditorSubsystems()); EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorB>());
		ASSERT_NO_THROW(Host->AddRegisteredEditorSubsystems()); ASSERT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorB>());
		Host->DispatchSubsystemCallback([&] {
			// Callback callers defer membership changes to their owning safe point.
			A.reset();
			EXPECT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		});
		ASSERT_NO_THROW(Host->RemoveEditorSubsystem(FDynamicEditorA::StaticClass())); EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		FDynamicEditorA::Init = {};
	}
	TEST_F(FEditorSubsystemTests, ExceptionsCleanFailedInstancesAndHostShutdownWaitsForTheBatch)
	{
		auto* Host = NewHost(); auto A = RegisterA();
		FDynamicEditorA::Init = [](auto&) -> void { throw std::runtime_error("expected"); };
		EXPECT_THROW(Host->AddRegisteredEditorSubsystems(), std::exception);
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		FEditorSubsystemRegistration B({.Type = FDynamicEditorB::StaticClass()});
		std::vector<DClass*> Initialized, Cleaned;
		FDynamicEditorA::Init = [&](auto& Object) {
			Initialized.push_back(Object.GetClass());
			if (Object.GetClass() == FDynamicEditorA::StaticClass()) Host->RequestShutdown();
			EXPECT_TRUE(Cleaned.empty());
		};
		FDynamicEditorA::Deinit = [&](auto& Object) { Cleaned.push_back(Object.GetClass()); };
		EXPECT_NO_THROW(Host->AddRegisteredEditorSubsystems());
		EXPECT_EQ(Initialized, (std::vector<DClass*>{FDynamicEditorA::StaticClass(), FDynamicEditorB::StaticClass()}));
		EXPECT_TRUE(Cleaned.empty());
		Host->PreExit();
		EXPECT_EQ(Cleaned, (std::vector<DClass*>{FDynamicEditorB::StaticClass(), FDynamicEditorA::StaticClass()}));
		EXPECT_FALSE(Host->AreEditorSubsystemsInitialized());
		FDynamicEditorA::Init = {}; FDynamicEditorA::Deinit = {};
	}
	TEST_F(FEditorSubsystemTests, OwnerExecutesRetirementAndGcAfterExternalCallbacks)
	{
		auto* Host = NewHost(); auto A = RegisterA(); ASSERT_NO_THROW(Host->AddRegisteredEditorSubsystems());
		auto* Service = Host->GetEditorSubsystem<FDynamicEditorA>(); ASSERT_NE(nullptr, Service);

		int Cleanups = 0, Destructions = 0;
		FDynamicEditorA::Deinit = [&](auto&) { ++Cleanups; };
		FDynamicEditorA::Destroy = [&](auto&) { ++Destructions; };
		Host->DispatchSubsystemCallback([&] {
			Host->RequestShutdown();
			EXPECT_TRUE(Host->IsShutdownRequested());
			EXPECT_TRUE(Host->IsReadyForFinishDestroy());
			// Callback lifetime is an owner contract, not finish-destroy readiness.
			EXPECT_TRUE(Service->IsReadyForFinishDestroy());
			EXPECT_FALSE(IsGarbageCollectionDeferred());
			RequestGarbageCollection();
			EXPECT_TRUE(IsGarbageCollectionRequested());
			EXPECT_EQ(0, Cleanups); EXPECT_EQ(0, Destructions);
		});
		EXPECT_FALSE(IsGarbageCollectionDeferred());
		EXPECT_EQ(0, Cleanups); EXPECT_EQ(0, Destructions);
		Host->PreExit();
		EXPECT_EQ(1, Cleanups); EXPECT_EQ(0, Destructions);
		EXPECT_FALSE(Host->AreEditorSubsystemsInitialized());
		EXPECT_TRUE(Host->IsReadyForFinishDestroy());
		EXPECT_TRUE(Service->IsReadyForFinishDestroy());
		// Forced garbage invalidates the fixture's strong handle before physical collection.
		Hosts.clear(); CollectGarbage();
		EXPECT_EQ(1, Cleanups); EXPECT_EQ(1, Destructions);

	}

	class FDynamicProvider : public IModuleInterface
	{
	public:
		explicit FDynamicProvider(std::shared_ptr<int> InStopped) : Stopped(std::move(InStopped)) {}
		auto SupportsDynamicReloading() const -> bool override { return true; }
		auto StartupModule() -> void override
		{
			Provider = FModuleStartup::GetModuleName();
			Registration = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = FDynamicEditorA::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
			if (DuringStartup) DuringStartup();
		}
		auto CanShutdownModule() const -> bool override
		{ return !IsGarbageCollecting() && (!GEditor || (!GEditor->IsShutdownRequested() && !GEditor->AreEditorSubsystemsUpdating())); }
		auto ShutdownModule() -> void override
		{
			if (GEditor) GEditor->RemoveEditorSubsystems(Provider);
			Registration.reset();
			++*Stopped;
		}
		std::unique_ptr<FEditorSubsystemRegistration> Registration;
		FName Provider;
		std::shared_ptr<int> Stopped;
		std::function<void()> DuringStartup;
	};

	TEST_F(FEditorSubsystemTests, ModuleShutdownUnregistersServicesBeforeUnload)
	{
		auto* Host = NewHost(); auto Stopped = std::make_shared<int>(0);
		const FName Name("DynamicEditorShutdownTest");
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(Name, std::make_unique<FDynamicProvider>(Stopped)));
		ASSERT_NO_THROW(Host->AddRegisteredEditorSubsystems());
		TStrongObjectPtr<FDynamicEditorA> Object(Host->GetEditorSubsystem<FDynamicEditorA>());

		bool bUnloadRequested = false;
		Host->DispatchSubsystemCallback([&] {
			bUnloadRequested = true;
			EXPECT_EQ(0, *Stopped);
			EXPECT_EQ(EModuleState::Active, FModuleManager::Get().FindModule(Name)->State.load());
		});
		ASSERT_TRUE(bUnloadRequested);
		// The owner unloads only after provider callbacks have returned.
		EXPECT_TRUE(FModuleManager::Get().UnloadModule(Name));
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>()); EXPECT_EQ(1, *Stopped);
		EXPECT_EQ(EModuleState::Unloaded, FModuleManager::Get().FindModule(Name)->State.load());
		// Fixture object code is resident in this test executable; reflected DLLs do not opt into unload.
		Object.Reset(); CollectGarbage();
		EXPECT_EQ(1, *Stopped);
		EXPECT_TRUE(Host->AreEditorSubsystemsInitialized());
	}





	TEST_F(FEditorSubsystemTests, FailedModuleStartupCanReleaseUninstantiatedRegistrationsDuringHostCallback)
	{
		auto* Host = NewHost();
		auto Stopped = std::make_shared<int>(0);
		FDynamicProvider Provider(Stopped);
		FModuleTestHarness Module("EditorModuleStartupRollback");
		Provider.DuringStartup = [] { throw std::runtime_error("Module startup exception"); };
		Host->DispatchSubsystemCallback([&] {
			EXPECT_THROW(Module.Start(Provider), std::runtime_error);
			EXPECT_NO_THROW(Provider.ShutdownModule());
		});
		EXPECT_EQ(1, *Stopped);
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_TRUE(Host->AreEditorSubsystemsInitialized());
	}

	TEST_F(FEditorSubsystemTests, ModuleShutdownWaitsForRequestedHostShutdownToFinish)
	{
		auto* Host = NewHost();
		const FName Name("EditorModuleDuringHostRetirement");
		auto Stopped = std::make_shared<int>(0);
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(Name, std::make_unique<FDynamicProvider>(Stopped)));
		Host->AddRegisteredEditorSubsystems();
		Host->DispatchSubsystemCallback([&] { Host->RequestShutdown(); });
		ASSERT_TRUE(Host->IsShutdownRequested());
		EXPECT_EQ(GEditor, Host);
		EXPECT_FALSE(FModuleManager::Get().UnloadModule(Name));
		EXPECT_EQ(0, *Stopped);
		Host->PreExit();
		EXPECT_EQ(GEditor, nullptr);
		EXPECT_TRUE(FModuleManager::Get().UnloadModule(Name));
		EXPECT_EQ(1, *Stopped);
	}

	TEST_F(FEditorSubsystemTests, ModuleLifecycleDoesNotDependOnUnrelatedTransactionRecovery)
	{
		auto* Host = NewHost();
		auto& History = *static_cast<DTransBuffer*>(Host->GetTransactor());
		FTransBufferTestAccess::SetState(History, Editor::ETransactorState::RecoveryRequired);
		ASSERT_FALSE(History.CanTransact());
		const FName Name("EditorModuleDuringTransactionRecovery");
		auto Stopped = std::make_shared<int>(0);
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(Name, std::make_unique<FDynamicProvider>(Stopped)));
		ASSERT_NO_THROW(Host->AddRegisteredEditorSubsystems());
		ASSERT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_TRUE(FModuleManager::Get().UnloadModule(Name));
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_EQ(1, *Stopped);
		EXPECT_FALSE(History.CanTransact());
		FTransBufferTestAccess::SetState(History, Editor::ETransactorState::Idle);
	}

	TEST_F(FEditorSubsystemTests, ProviderMustFinishStartupBeforeItsServiceCanBeAdded)
	{
		auto* Host = NewHost(); const FName Name("EditorExplicitAddStartupTest");
		auto Provider = std::make_unique<FDynamicProvider>(std::make_shared<int>(0));
		Provider->DuringStartup = [&] {
			EXPECT_DEATH(Host->AddEditorSubsystem(FDynamicEditorA::StaticClass()), "Subsystem provider must be active");
			EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		};
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(Name, std::move(Provider)));
		ASSERT_TRUE(Host->AddEditorSubsystem(FDynamicEditorA::StaticClass()));
		EXPECT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_TRUE(FModuleManager::Get().UnloadModule(Name));
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
	}

	class FWorkspaceStartupFrame final : public IMainFrameModule
	{
	public:
		std::function<bool()> LoadModules;
		std::function<void()> CreateWorkspace;
		auto CreateDefaultMainFrame(DEditorEngine&, std::shared_ptr<MWindow>) -> void override {}
		auto DestroyMainFrame() -> void override {}
		auto IsShellPresented() const -> bool override { return true; }
		auto LoadWorkspaceModules() -> bool override { return LoadModules(); }
		auto InitializeWorkspaces(std::string&) -> bool override { CreateWorkspace(); return true; }
		auto TickMainFrame(float) -> void override {}
	};

	TEST_F(FEditorSubsystemTests, WorkspaceStartupInitializesLateModuleServicesBeforeCreatingConsumers)
	{
		auto* Host = NewHost();
		const FName Name("WorkspaceStartupProviderTest");
		std::vector<std::string> Events;
		FDynamicEditorA::Init = [&](auto&) { Events.emplace_back("service"); return; };
		FWorkspaceStartupFrame Frame;
		Frame.LoadModules = [&]() {
			auto Provider = std::make_unique<FDynamicProvider>(std::make_shared<int>(0));
			Provider->DuringStartup = [&] {
				Events.emplace_back("module");
				EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
			};
			return FModuleTestHarness::InstallStartedModule(Name, std::move(Provider)) != nullptr;
		};
		Frame.CreateWorkspace = [&] {
			Events.emplace_back("workspace");
			EXPECT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		};
		auto* Previous = FEditorEngineTestAccess::BindMainFrameModule(*Host, &Frame);
		FEditorEngineTestAccess::SetStartupPhase(*Host, Editor::Startup::EBootstrapState::LoadingWorkspace);
		Host->AdvanceStartup();
		EXPECT_EQ((std::vector<std::string>{"module", "service", "workspace"}), Events);
		EXPECT_EQ(Editor::Startup::EBootstrapState::LoadingDefaultDocument, Host->GetStartupPhase());
		FEditorEngineTestAccess::BindMainFrameModule(*Host, Previous);
		EXPECT_TRUE(FModuleManager::Get().UnloadModule(Name));
	}

	TEST_F(FEditorSubsystemTests, WorkspaceStartupPropagatesModuleAndServiceFailuresBeforeCreatingConsumers)
	{
		auto* Host = NewHost();
		FWorkspaceStartupFrame Frame;
		Frame.CreateWorkspace = [] { ADD_FAILURE() << "Failed providers must not expose workspace consumers."; };
		Frame.LoadModules = [] { return false; };
		auto* Previous = FEditorEngineTestAccess::BindMainFrameModule(*Host, &Frame);
		FEditorEngineTestAccess::SetStartupPhase(*Host, Editor::Startup::EBootstrapState::LoadingWorkspace);
		Host->AdvanceStartup();
		EXPECT_EQ(EHostStartupState::Failed, Host->GetStartupState());
		EXPECT_EQ("Could not load the editor workspace modules.", Host->GetStartupError());

		auto Registration = RegisterA();
		FDynamicEditorA::Init = [](auto&) { throw std::runtime_error("workspace service failed"); };
		Frame.LoadModules = [] { return true; };
		FEditorEngineTestAccess::SetStartupPhase(*Host, Editor::Startup::EBootstrapState::LoadingWorkspace);
		EXPECT_THROW(Host->AdvanceStartup(), std::runtime_error);
		EXPECT_EQ(Editor::Startup::EBootstrapState::LoadingWorkspace, Host->GetStartupPhase());
		EXPECT_TRUE(Host->GetStartupError().empty());
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		FEditorEngineTestAccess::BindMainFrameModule(*Host, Previous);
	}

	class FTestAssetEditorInstance final : public IAssetEditorInstance
	{
	public:
		auto GetEditorName() const -> FName override { return "TestAssetEditor"; }
		auto IsPrimaryEditor() const -> bool override { return bPrimary; }
		auto FocusWindow(DObject*) -> void override { ++FocusCount; if (Focus) Focus(); }
		auto GetLastActivationTime() -> double override { return 0.0; }
		auto CloseWindow(EAssetEditorCloseReason Reason) -> bool override
		{ LastCloseReason = Reason; ++CloseCount; return Close ? Close() : false; }
		EAssetEditorCloseReason LastCloseReason = EAssetEditorCloseReason::AssetEditorHostClosed;
		bool bPrimary = true;
		int FocusCount = 0;
		int CloseCount = 0;
		std::function<void()> Focus;
		std::function<bool()> Close;
	};

	TEST_F(FEditorSubsystemTests, AssetEditorsTrackManyToManyAssociationsAndFocusThePrimary)
	{
		auto* Host = NewHost();
		auto* Service = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
		ASSERT_NE(nullptr, Service);
		auto* First = NewObject<DObject>(Host, "FirstAsset");
		auto* Second = NewObject<DObject>(Host, "SecondAsset");
		FTestAssetEditorInstance Primary, Secondary;
		Secondary.bPrimary = false;
		Service->NotifyAssetOpened(First, &Secondary);
		Service->NotifyAssetsOpened({First, Second, First}, &Primary);
		EXPECT_EQ(2u, Service->FindEditorsForAsset(First).size());
		EXPECT_EQ(2u, Service->GetAllEditedAssets().size());
		EXPECT_EQ(2u, Service->GetAllOpenEditors().size());
		EXPECT_EQ(&Primary, Service->FindEditorForAsset(First, true));
		EXPECT_EQ(1, Primary.FocusCount);
		EXPECT_EQ(0, Secondary.FocusCount);
		EXPECT_TRUE(Service->OpenEditorForAsset(First));
		EXPECT_EQ(2, Primary.FocusCount);
		Primary.Focus = [&] { EXPECT_FALSE(Service->OpenEditorForAsset(First)); };
		EXPECT_TRUE(Service->OpenEditorForAsset(First));
		Service->NotifyAssetClosed(First, &Primary);
		EXPECT_EQ(nullptr, Service->FindEditorForAsset(First, false));
		EXPECT_EQ(&Primary, Service->FindEditorForAsset(Second, false));
		Service->NotifyEditorClosed(&Primary);
		Service->NotifyEditorClosed(&Secondary);
		EXPECT_TRUE(Service->GetAllEditedAssets().empty());
		EXPECT_TRUE(Service->GetAllOpenEditors().empty());
	}

	TEST_F(FEditorSubsystemTests, AssetEditorCallsRejectWorkersAndCloseOtherEditorsPreservesTheException)
	{
		auto* Host = NewHost();
		auto* Service = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Asset = NewObject<DObject>(Host, "Asset");
		FTestAssetEditorInstance Kept, Closed;
		Service->NotifyAssetOpened(Asset, &Kept);
		Service->NotifyAssetOpened(Asset, &Closed);
		std::thread Worker([&] {
			EXPECT_FALSE(Service->OpenEditorForAsset(Asset));
			EXPECT_TRUE(Service->FindEditorsForAsset(Asset).empty());
			EXPECT_FALSE(Service->CloseAllAssetEditors());
			Service->NotifyEditorClosed(&Kept);
		});
		Worker.join();
		Closed.Close = [&] { Service->NotifyEditorClosed(&Closed); return true; };
		Service->CloseOtherEditors(Asset, &Kept);
		EXPECT_EQ(0, Kept.CloseCount);
		EXPECT_EQ(1, Closed.CloseCount);
		EXPECT_EQ(EAssetEditorCloseReason::CloseOtherEditors, Closed.LastCloseReason);
		EXPECT_EQ(&Kept, Service->FindEditorForAsset(Asset, false));
		Service->NotifyEditorClosed(&Kept);
	}

	TEST_F(FEditorSubsystemTests, AssetEditorCloseHonorsVetoAndToleratesRegistryMutation)
	{
		auto* Host = NewHost();
		auto* Service = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Asset = NewObject<DObject>(Host, "Asset");
		FTestAssetEditorInstance First, Second;
		Service->NotifyAssetsOpened({Asset}, &First);
		Service->NotifyAssetsOpened({Asset}, &Second);
		EXPECT_FALSE(Service->CloseAllAssetEditors());
		EXPECT_EQ(EAssetEditorCloseReason::CloseAllAssetEditors, First.LastCloseReason);
		EXPECT_EQ(0, Service->CloseAllEditorsForAsset(Asset));
		EXPECT_EQ(EAssetEditorCloseReason::CloseAllEditorsForAsset, First.LastCloseReason);
		First.Close = [&] {
			Service->NotifyEditorClosed(&First);
			Service->NotifyEditorClosed(&Second);
			return true;
		};
		const int PreviousSecondCloses = Second.CloseCount;
		EXPECT_EQ(1, Service->CloseAllEditorsForAsset(Asset));
		EXPECT_EQ(PreviousSecondCloses, Second.CloseCount);
		EXPECT_TRUE(Service->GetAllOpenEditors().empty());
	}

	TEST_F(FEditorSubsystemTests, AssetEditorCallbacksKeepHostAliveUntilRetirementBoundary)
	{
		auto* Host = NewHost();
		auto* Service = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Asset = NewObject<DObject>(Host, "Asset");
		FTestAssetEditorInstance Instance;
		Service->NotifyAssetOpened(Asset, &Instance);
		Instance.Focus = [&] {
			Host->RequestShutdown();
			EXPECT_TRUE(Host->AreEditorSubsystemsInitialized());
			EXPECT_TRUE(Host->IsShutdownRequested());
		};
		EXPECT_EQ(nullptr, Service->FindEditorForAsset(Asset, true));
		EXPECT_FALSE(Service->OpenEditorForAsset(Asset));
		EXPECT_TRUE(Service->GetAllOpenEditors().empty());
		Service->NotifyEditorClosed(&Instance);
	}


	TEST_F(FEditorSubsystemTests, RejectingDefinitionDoesNotFallBackAndMissingDefinitionsAreUnhandled)
	{
		FDefinitionTestMount Mount;
		auto* Service = NewHost()->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Definition = NewObject<FDynamicOpenDefinition>(nullptr, {});
		Definition->Open = [](const FAssetOpenArgs& Args) {
			return FAssetOpenResult{{{.Target = Args.Assets.front(), .Status = EAssetOpenStatus::Rejected}}};
		};
		auto Registration = FAssetDefinitionRegistry::Get().RegisterDefinition(*Definition);
		const FAssetOpenArgs Args{.AssetEditors = Service, .Assets = {DefinitionTarget()}};
		EXPECT_EQ(EAssetOpenStatus::Rejected, Service->OpenAssets(Args).Items.front().Status);
		Registration.Reset();
		EXPECT_EQ(EAssetOpenStatus::Unhandled, Service->OpenAssets(Args).Items.front().Status);
		EXPECT_TRUE(Service->GetAllOpenEditors().empty());
	}


}

TEST_F(FEditorSubsystemTests, AssetEditorCleanupToleratesMissingToolkitAndRepeatedCalls)
{
	TStrongObjectPtr<DAssetEditorSubsystem> Service(NewObject<DAssetEditorSubsystem>(nullptr, "UninitializedAssetEditor"));
	ASSERT_TRUE(Service);
	Service->Deinitialize();
	Service->Deinitialize();
	MarkObjectHierarchyAsGarbage(Service.Get());
}

TEST_F(FEditorSubsystemTests, FailedAssetEditorInitializationCleansUpBeforeToolkitCreation)
{
	FEditorSubsystemRegistration Token({.Type = FFailingAssetEditorSubsystem::StaticClass()});
	auto* Host = NewObject<DEditorEngine>(nullptr, {});
	Hosts.emplace_back(Host);
	EXPECT_THROW(FEditorEngineTestAccess::StartEditor(*Host), std::exception);
	EXPECT_EQ(FFailingAssetEditorSubsystem::CleanupCalls, 1);
	EXPECT_EQ(Host->GetEditorSubsystem<FFailingAssetEditorSubsystem>(), nullptr);
	Host->PreExit();
	EXPECT_EQ(FFailingAssetEditorSubsystem::CleanupCalls, 1);
}

TEST_F(FEditorSubsystemTests, FailedLateAssetEditorInitializationRetainsExistingServices)
{
	auto A = RegisterA();
	auto* Host = NewHost();
	auto* Existing = Host->GetEditorSubsystem<FDynamicEditorA>();
	ASSERT_NE(Existing, nullptr);
	FEditorSubsystemRegistration Token({.Type = FFailingAssetEditorSubsystem::StaticClass()});
	EXPECT_THROW(Host->AddRegisteredEditorSubsystems(), std::exception);
	EXPECT_EQ(FFailingAssetEditorSubsystem::CleanupCalls, 1);
	EXPECT_EQ(Host->GetEditorSubsystem<FFailingAssetEditorSubsystem>(), nullptr);
	EXPECT_EQ(Host->GetEditorSubsystem<FDynamicEditorA>(), Existing);
}
