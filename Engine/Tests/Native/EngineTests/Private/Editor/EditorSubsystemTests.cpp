#include "Editor/EditorEngineTestSupport.h"
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
		auto Initialize() -> FSubsystemResult override { return Init ? Init(*this) : FSubsystemResult{}; }
		auto Deinitialize() noexcept -> void override { if (Deinit) Deinit(*this); }
		auto BeginDestroy() -> void override { if (Destroy) Destroy(*this); Super::BeginDestroy(); }
		auto ShouldCreateSubsystem(DObject* Outer) const -> bool override { return !Create || Create(*this, Outer); }
		inline static std::function<bool(const FDynamicEditorA&, DObject*)> Create;
		inline static std::function<FSubsystemResult(FDynamicEditorA&)> Init;
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

	class FFailingAssetEditorSubsystem : public DAssetEditorSubsystem
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FFailingAssetEditorSubsystem, DAssetEditorSubsystem, FFailingAssetEditorSubsystem::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FFailingAssetEditorSubsystem)
	public:
		explicit FFailingAssetEditorSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto Initialize() -> FSubsystemResult override { throw std::runtime_error("failure before toolkit creation"); }
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
			const std::array Classes{FDynamicEditorA::StaticClass(), FDynamicEditorB::StaticClass(), FFailingAssetEditorSubsystem::StaticClass()};
			for (auto* Class : Classes) DObjectForceRegistration(Class);
			ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
			FFailingAssetEditorSubsystem::CleanupCalls = 0;
		}
		auto TearDown() -> void override
		{
			FDynamicEditorA::Init = {}; FDynamicEditorA::Deinit = {}; FDynamicEditorA::Destroy = {}; FDynamicEditorA::Create = {};
			for (auto& Host : Hosts) { Host->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Host.Get()); }
			Hosts.clear(); CollectGarbage();
		}
		auto NewHost() -> DEditorEngine*
		{
			auto* Host = NewObject<DEditorEngine>(nullptr, {}); Hosts.emplace_back(Host);
			EXPECT_TRUE(FEditorEngineTestAccess::StartEditor(*Host)); return Host;
		}
		auto RegisterA() -> std::unique_ptr<FEditorSubsystemRegistration>
		{ return std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = FDynamicEditorA::StaticClass()}); }
		std::vector<TStrongObjectPtr<DEditorEngine>> Hosts;
	};

	auto DefinitionTarget(std::string_view Path = "/AssetEditorTests/Package.Asset") -> FAssetOpenTarget
	{
		FObjectPath ObjectPath;
		require(FObjectPath::TryCreate(Path, ObjectPath));
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
		auto* Tools = First->GetEditorSubsystem<DEditorToolSubsystem>();
		auto Registration = RegisterA();
		EXPECT_EQ(nullptr, First->GetEditorSubsystem<FDynamicEditorA>());
		ASSERT_TRUE(First->RefreshEditorSubsystems());
		auto* A = First->GetEditorSubsystem<FDynamicEditorA>(); ASSERT_NE(nullptr, A);
		EXPECT_EQ(A->GetEditor(), First);
		auto Gate = A->GetWorkGate(); Registration.reset();
		EXPECT_TRUE(Gate->IsOpen());
		ASSERT_TRUE(First->RefreshEditorSubsystems()); EXPECT_FALSE(Gate->IsOpen());
		FDynamicEditorA::Destroy = [&](auto&) {
			EXPECT_TRUE(IsGarbageCollecting());
			EXPECT_EQ(ESubsystemError::InvalidState, First->RefreshEditorSubsystems().Error);
		};
		CollectGarbage();
		FDynamicEditorA::Destroy = {};
		EXPECT_EQ(ESubsystemState::Ready, First->GetEditorSubsystemState());
		EXPECT_EQ(Tools, First->GetEditorSubsystem<DEditorToolSubsystem>());
		EXPECT_EQ(nullptr, First->GetEditorSubsystem<FDynamicEditorA>());
	}

	TEST_F(FEditorSubsystemTests, RemovalReleasesCollectionReferencesWithoutInvalidatingRetainedObjects)
	{
		auto* Host = NewHost(); auto Registration = RegisterA();
		ASSERT_TRUE(Host->RefreshEditorSubsystems());
		TStrongObjectPtr<FDynamicEditorA> Retained(Host->GetEditorSubsystem<FDynamicEditorA>());
		TWeakObjectPtr<FDynamicEditorA> Weak(Retained.Get());
		int Cleanups = 0, Destructions = 0;
		FDynamicEditorA::Deinit = [&](auto&) { ++Cleanups; };
		FDynamicEditorA::Destroy = [&](auto&) { ++Destructions; };
		Registration.reset(); ASSERT_TRUE(Host->RefreshEditorSubsystems());
		EXPECT_EQ(1, Cleanups); EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_TRUE(IsValid(Retained.Get())); EXPECT_FALSE(Retained->IsGarbage());
		EXPECT_FALSE(Retained->GetWorkGate()->IsOpen());
		CollectGarbage();
		EXPECT_TRUE(Weak.IsValid()); EXPECT_EQ(0, Destructions);
		Retained.Reset(); CollectGarbage();
		EXPECT_FALSE(Weak.IsValid()); EXPECT_EQ(1, Destructions); EXPECT_EQ(1, Cleanups);
		EXPECT_EQ(ESubsystemState::Ready, Host->GetEditorSubsystemState());
		FDynamicEditorA::Deinit = {}; FDynamicEditorA::Destroy = {};
	}

	TEST_F(FEditorSubsystemTests, FailedAdditionDeinitializesRetainedObjectWithoutForcingGarbage)
	{
		auto* Host = NewHost(); auto Registration = RegisterA();
		TStrongObjectPtr<FDynamicEditorA> Retained;
		int Cleanups = 0;
		FDynamicEditorA::Init = [&](auto& Object) {
			Retained = &Object;
			return FSubsystemResult{ESubsystemError::InitializationFailed, "fixture"};
		};
		FDynamicEditorA::Deinit = [&](auto&) { ++Cleanups; };
		EXPECT_EQ(ESubsystemError::InitializationFailed, Host->RefreshEditorSubsystems().Error);
		ASSERT_NE(nullptr, Retained.Get());
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_EQ(1, Cleanups); EXPECT_FALSE(Retained->IsGarbage());
		EXPECT_FALSE(Retained->GetWorkGate()->IsOpen());
		CollectGarbage(); EXPECT_TRUE(IsValid(Retained.Get()));
		Retained.Reset(); CollectGarbage();
		EXPECT_EQ(ESubsystemState::Ready, Host->GetEditorSubsystemState());
		EXPECT_EQ(1, Cleanups);
		FDynamicEditorA::Init = {}; FDynamicEditorA::Deinit = {};
	}

	TEST_F(FEditorSubsystemTests, InitializationRequiresExplicitActivationAndNeverPublishesEditor)
	{
		auto* First = NewObject<DEditorEngine>(nullptr, "ExplicitFirstEditor"); Hosts.emplace_back(First);
		auto* Second = NewObject<DEditorEngine>(nullptr, "ExplicitSecondEditor"); Hosts.emplace_back(Second);
		EXPECT_EQ(GEditor, nullptr);
		EXPECT_FALSE(First->Init({}));
		EXPECT_EQ(FEditorEngineTestAccess::InitializeServices(*First).Error, ESubsystemError::InvalidState);
		EXPECT_EQ(GEditor, nullptr);
		ASSERT_TRUE(First->ActivateEditor());
		EXPECT_FALSE(First->ActivateEditor());
		EXPECT_EQ(First->GetEditorSubsystemState(), ESubsystemState::Uninitialized);
		EXPECT_EQ(FEditorEngineTestAccess::InitializeServices(*Second).Error, ESubsystemError::InvalidState);
		EXPECT_FALSE(Second->Init({}));
		EXPECT_EQ(GEditor, First);
		ASSERT_TRUE(FEditorEngineTestAccess::InitializeServices(*First));
		First->PrepareForShutdown();
		EXPECT_EQ(GEditor, nullptr);
		EXPECT_FALSE(First->ActivateEditor());
		ASSERT_TRUE(Second->ActivateEditor());
		ASSERT_TRUE(FEditorEngineTestAccess::InitializeServices(*Second));
		EXPECT_EQ(GEditor, Second);
		Second->PrepareForShutdown();
		EXPECT_EQ(GEditor, nullptr);
	}

	TEST_F(FEditorSubsystemTests, ConstructionCannotReplaceActiveEditorAndActivationIsExclusive)
	{
		auto* First = NewHost();
		TStrongObjectPtr<DEditorEngine> Second(NewObject<DEditorEngine>(nullptr, "InactiveEditor"));
		EXPECT_EQ(GEditor, First);
		EXPECT_EQ(FEditorEngineTestAccess::StartEditor(*Second).Error, ESubsystemError::InvalidState);
		EXPECT_EQ(GEditor, First);
		First->PrepareForShutdown();
		EXPECT_EQ(GEditor, nullptr);
		ASSERT_TRUE(FEditorEngineTestAccess::StartEditor(*Second));
		EXPECT_EQ(GEditor, Second.Get());
		Second->PrepareForShutdown();
		EXPECT_EQ(GEditor, nullptr);
		EXPECT_FALSE(First->ActivateEditor());
	}

	TEST_F(FEditorSubsystemTests, CloseObserversCannotReenterCollectionCleanup)
	{
		auto* Host = NewHost();
		for (bool bDeinitialize : {false, true})
		{
			FSubsystemCollection Collection(*Host, DEditorSubsystem::StaticClass());
			ASSERT_TRUE(Collection.Initialize({{.Type = FDynamicEditorA::StaticClass()},
				{.Type = FDynamicEditorB::StaticClass(), .Dependencies = {FDynamicEditorA::StaticClass()}}}));
			auto* A = Collection.Find(FDynamicEditorA::StaticClass());
			auto* B = Collection.Find(FDynamicEditorB::StaticClass());
			ASSERT_NE(nullptr, A); ASSERT_NE(nullptr, B);
			auto GateA = A->GetWorkGate(); auto GateB = B->GetWorkGate();
			bool bInObserver = false, bObserved = false;
			std::vector<DClass*> Cleanup;
			FDynamicEditorA::Deinit = [&](auto& Object) {
				EXPECT_FALSE(bInObserver);
				EXPECT_FALSE(GateA->IsOpen()); EXPECT_FALSE(GateB->IsOpen());
				Cleanup.push_back(Object.GetClass());
				Collection.CloseWork(); Collection.Deinitialize();
			};
			FObjectReferencePurgeRegistration Observer([&](DObject& Object) {
				if (&Object != A || bObserved) return;
				bObserved = true; bInObserver = true;
				Collection.CloseWork(); Collection.Deinitialize(); CollectGarbage();
				EXPECT_TRUE(Cleanup.empty());
				EXPECT_EQ(A, Collection.Find(FDynamicEditorA::StaticClass()));
				EXPECT_EQ(B, Collection.Find(FDynamicEditorB::StaticClass()));
				bInObserver = false;
			});
			if (bDeinitialize) Collection.Deinitialize(); else Collection.CloseWork();
			EXPECT_TRUE(bObserved);
			EXPECT_EQ((std::vector<DClass*>{FDynamicEditorB::StaticClass(), FDynamicEditorA::StaticClass()}), Cleanup);
			EXPECT_EQ(ESubsystemState::Shutdown, Collection.GetState());
			EXPECT_EQ(nullptr, Collection.Find(FDynamicEditorA::StaticClass()));
			EXPECT_EQ(nullptr, Collection.Find(FDynamicEditorB::StaticClass()));
			FDynamicEditorA::Deinit = {};
		}
	}

	TEST_F(FEditorSubsystemTests, RuntimeAndTypedLookupAgreeDuringInitializationAndCleanup)
	{
		auto A = RegisterA();
		FEditorSubsystemRegistration B({.Type = FDynamicEditorB::StaticClass(), .Dependencies = {FDynamicEditorA::StaticClass()}});
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
			return FSubsystemResult{};
		};
		FDynamicEditorA::Deinit = CheckDependency;
		ASSERT_TRUE(FEditorEngineTestAccess::StartEditor(*Host));
		EXPECT_EQ(1, DependenciesObserved);
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem(nullptr));
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem(DObject::StaticClass()));
		A.reset(); EXPECT_EQ(ESubsystemError::MissingDependency, Host->RefreshEditorSubsystems().Error);
		EXPECT_EQ(2, DependenciesObserved);
		A = RegisterA(); ASSERT_TRUE(Host->RefreshEditorSubsystems());
		EXPECT_EQ(3, DependenciesObserved);
		Host->PrepareForShutdown();
		EXPECT_EQ(4, DependenciesObserved);
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem(FDynamicEditorA::StaticClass()));
		FDynamicEditorA::Init = {}; FDynamicEditorA::Deinit = {};
	}

	TEST_F(FEditorSubsystemTests, ResolvesLateDependenciesAndRetiresDependentsFirst)
	{
		auto* Host = NewHost(); std::vector<std::string> Events;
		FDynamicEditorA* BoundDependency = nullptr;
		FDynamicEditorA::Init = [&](auto& Object) {
			EXPECT_TRUE(Host->AreEditorSubsystemsUpdating());
			if (Object.GetClass() == FDynamicEditorB::StaticClass())
			{
				BoundDependency = Host->GetEditorSubsystem<FDynamicEditorA>();
				EXPECT_NE(nullptr, BoundDependency);
			}
			Events.push_back(Object.GetClass() == FDynamicEditorA::StaticClass() ? "A+" : "B+"); return FSubsystemResult{};
		};
		FDynamicEditorA::Deinit = [&](auto& Object) {
			Events.push_back(Object.GetClass() == FDynamicEditorA::StaticClass() ? "A-" : "B-");
			if (Object.GetClass() == FDynamicEditorB::StaticClass()) EXPECT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		};
		auto B = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
			.Type = FDynamicEditorB::StaticClass(), .Dependencies = {FDynamicEditorA::StaticClass()}});
		EXPECT_EQ(ESubsystemError::MissingDependency, Host->RefreshEditorSubsystems().Error);
		auto A = RegisterA(); ASSERT_TRUE(Host->RefreshEditorSubsystems());
		EXPECT_EQ((std::vector<std::string>{"A+", "B+"}), Events);
		A.reset(); EXPECT_EQ(ESubsystemError::MissingDependency, Host->RefreshEditorSubsystems().Error);
		EXPECT_EQ((std::vector<std::string>{"A+", "B+", "B-", "A-"}), Events);
		A = RegisterA(); ASSERT_TRUE(Host->RefreshEditorSubsystems());
		ASSERT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorB>());
		TStrongObjectPtr<FDynamicEditorA> OldA(Host->GetEditorSubsystem<FDynamicEditorA>());
		TStrongObjectPtr<FDynamicEditorB> OldB(Host->GetEditorSubsystem<FDynamicEditorB>());
		const auto BeforeReplacement = Events;
		A.reset(); A = RegisterA();
		auto Result = Host->RefreshEditorSubsystems();
		EXPECT_EQ(ESubsystemError::InvalidState, Result.Error);
		EXPECT_NE(std::string::npos, Result.Message.find("FDynamicEditorB"));
		EXPECT_NE(std::string::npos, Result.Message.find("FDynamicEditorA"));
		EXPECT_EQ(BeforeReplacement, Events);
		EXPECT_EQ(OldA.Get(), Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_EQ(OldB.Get(), Host->GetEditorSubsystem<FDynamicEditorB>());
		EXPECT_EQ(OldA.Get(), BoundDependency);
		EXPECT_TRUE(OldA->GetWorkGate()->IsOpen()); EXPECT_TRUE(OldB->GetWorkGate()->IsOpen());

		// Only an explicit consumer registration change authorizes recreating B.
		B.reset();
		B = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
			.Type = FDynamicEditorB::StaticClass(), .Dependencies = {FDynamicEditorA::StaticClass()}});
		ASSERT_TRUE(Host->RefreshEditorSubsystems());
		EXPECT_FALSE(OldA->GetWorkGate()->IsOpen()); EXPECT_FALSE(OldB->GetWorkGate()->IsOpen());
		EXPECT_NE(OldA.Get(), Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_NE(OldB.Get(), Host->GetEditorSubsystem<FDynamicEditorB>());
		EXPECT_EQ(Host->GetEditorSubsystem<FDynamicEditorA>(), BoundDependency);
		EXPECT_NE(OldA.Get(), BoundDependency);
		auto Expected = BeforeReplacement;
		Expected.insert(Expected.end(), {"B-", "A-", "A+", "B+"});
		EXPECT_EQ(Expected, Events);
		FDynamicEditorA::Init = {}; FDynamicEditorA::Deinit = {};
	}

	TEST_F(FEditorSubsystemTests, RejectsDuplicateTypesAndCyclesWithoutLosingUnrelatedServices)
	{
		auto* Host = NewHost(); auto* Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>();
		auto A = RegisterA(); ASSERT_TRUE(Host->RefreshEditorSubsystems());
		auto Duplicate = RegisterA(); EXPECT_EQ(ESubsystemError::DuplicateType, Host->RefreshEditorSubsystems().Error);
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		Duplicate.reset(); ASSERT_TRUE(Host->RefreshEditorSubsystems()); A.reset();
		FEditorSubsystemRegistration CycleA({.Type = FDynamicEditorA::StaticClass(), .Dependencies = {FDynamicEditorB::StaticClass()}});
		FEditorSubsystemRegistration CycleB({.Type = FDynamicEditorB::StaticClass(), .Dependencies = {FDynamicEditorA::StaticClass()}});
		EXPECT_EQ(ESubsystemError::DependencyCycle, Host->RefreshEditorSubsystems().Error);
		EXPECT_EQ(Tools, Host->GetEditorSubsystem<DEditorToolSubsystem>());
	}

	TEST_F(FEditorSubsystemTests, SharedDependencyValidationPreservesInitialAndDynamicFailurePolicies)
	{
		class FCollection : public FSubsystemCollection
		{
		public:
			using FSubsystemCollection::FSubsystemCollection;
			using FSubsystemCollection::Reconcile;
		};
		auto* Host = NewHost();
		const FSubsystemDescriptor Independent{.Type = FDynamicEditorB::StaticClass()};
		const FSubsystemDescriptor Invalid{.Type = FDynamicEditorA::StaticClass(), .Dependencies = {DObject::StaticClass()}};
		FCollection Initial(*Host, DEditorSubsystem::StaticClass());
		auto Result = Initial.Initialize({Independent, Invalid});
		EXPECT_EQ(ESubsystemError::InvalidDescriptor, Result.Error);
		EXPECT_NE(std::string::npos, Result.Message.find("wrong-scope dependency"));
		EXPECT_EQ(ESubsystemState::Failed, Initial.GetState());
		EXPECT_FALSE(Initial.IsPopulating());
		EXPECT_EQ(nullptr, Initial.Find(Independent.Type));

		FCollection Dynamic(*Host, DEditorSubsystem::StaticClass());
		ASSERT_TRUE(Dynamic.Initialize({Independent}));
		auto* Existing = Dynamic.Find(Independent.Type);
		ASSERT_NE(nullptr, Existing);
		Result = Dynamic.Reconcile({Independent, Invalid});
		EXPECT_EQ(ESubsystemError::InvalidDescriptor, Result.Error);
		EXPECT_NE(std::string::npos, Result.Message.find("wrong-scope dependency"));
		EXPECT_EQ(ESubsystemState::Ready, Dynamic.GetState());
		EXPECT_EQ(Existing, Dynamic.Find(Independent.Type));
		EXPECT_EQ(nullptr, Dynamic.Find(Invalid.Type));

		// Invalid types must be removed before CDO selection and dependency ordering.
		for (auto* Type : std::array<DClass*, 3>{nullptr, DEditorSubsystem::StaticClass(), DObject::StaticClass()})
		{
			Result = Dynamic.Reconcile({Independent, {.Type = Type},
				{.Type = FDynamicEditorA::StaticClass(), .Dependencies = {Type}}});
			EXPECT_EQ(ESubsystemError::InvalidDescriptor, Result.Error);
			EXPECT_EQ(ESubsystemState::Ready, Dynamic.GetState());
			EXPECT_EQ(Existing, Dynamic.Find(Independent.Type));
			EXPECT_EQ(nullptr, Dynamic.Find(FDynamicEditorA::StaticClass()));
		}
		Dynamic.Deinitialize();
		EXPECT_EQ(nullptr, Dynamic.Find(Independent.Type));
		EXPECT_EQ(ESubsystemState::Shutdown, Dynamic.GetState());
		EXPECT_FALSE(Dynamic.IsPopulating());
		Dynamic.Deinitialize();
		Initial.Deinitialize();
	}

	TEST_F(FEditorSubsystemTests, RollsBackFailedAdditionAndRetriesOnlyWhenRequested)
	{
		auto* Host = NewHost(); int Initializations = 0, Cleanups = 0;
		auto A = RegisterA(); FEditorSubsystemRegistration B({.Type = FDynamicEditorB::StaticClass(), .Dependencies = {FDynamicEditorA::StaticClass()}});
		FDynamicEditorA::Init = [&](auto& Object) {
			++Initializations;
			return Object.GetClass() == FDynamicEditorB::StaticClass()
				? FSubsystemResult{ESubsystemError::InitializationFailed, "expected failure"} : FSubsystemResult{};
		};
		FDynamicEditorA::Deinit = [&](auto& Object) {
			++Cleanups;
			EXPECT_FALSE(Object.GetWorkGate()->IsOpen());
			if (Object.GetClass() == FDynamicEditorB::StaticClass())
			{
				auto* Dependency = Host->GetEditorSubsystem<FDynamicEditorA>();
				ASSERT_NE(nullptr, Dependency);
				EXPECT_FALSE(Dependency->GetWorkGate()->IsOpen());
			}
		};
		EXPECT_EQ(ESubsystemError::InitializationFailed, Host->RefreshEditorSubsystems().Error);
		EXPECT_EQ(2, Cleanups); EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		(void)Host->RefreshEditorSubsystems(); EXPECT_EQ(2, Initializations);
		FDynamicEditorA::Init = {}; ASSERT_TRUE(Host->RefreshEditorSubsystems(true));
		FDynamicEditorA::Deinit = {};
	}

	TEST_F(FEditorSubsystemTests, DefersChangesFromCallbacksAndInitialization)
	{
		auto* Host = NewHost(); std::unique_ptr<FEditorSubsystemRegistration> B;
		auto A = RegisterA();
		FDynamicEditorA::Init = [&](auto& Object) {
			if (Object.GetClass() == FDynamicEditorA::StaticClass())
			{
				B = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = FDynamicEditorB::StaticClass()});
				EXPECT_EQ(ESubsystemError::InvalidState, Host->RefreshEditorSubsystems().Error);
				EXPECT_FALSE(Host->GetEditorSubsystem<DEditorToolSubsystem>()->CanCallNow());
			}
			return FSubsystemResult{};
		};
		ASSERT_TRUE(Host->RefreshEditorSubsystems()); EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorB>());
		ASSERT_TRUE(Host->RefreshEditorSubsystems()); ASSERT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorB>());
		Host->DispatchSubsystemCallback([&] {
			A.reset(); EXPECT_EQ(ESubsystemError::InvalidState, Host->RefreshEditorSubsystems().Error);
			EXPECT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		});
		ASSERT_TRUE(Host->RefreshEditorSubsystems()); EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		FDynamicEditorA::Init = {};
	}
	TEST_F(FEditorSubsystemTests, ExceptionsAndHostRetirementRollBackNewInstances)
	{
		auto* Host = NewHost(); auto A = RegisterA();
		FDynamicEditorA::Init = [](auto&) -> FSubsystemResult { throw std::runtime_error("expected"); };
		EXPECT_EQ(ESubsystemError::InitializationFailed, Host->RefreshEditorSubsystems().Error);
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		FDynamicEditorA::Init = [&](auto&) { Host->PrepareForShutdown(); return FSubsystemResult{}; };
		EXPECT_EQ(ESubsystemError::Aborted, Host->RefreshEditorSubsystems(true).Error);
		EXPECT_EQ(ESubsystemState::Shutdown, Host->GetEditorSubsystemState());
		FDynamicEditorA::Init = {};
	}
	TEST_F(FEditorSubsystemTests, ClosingAGateDoesNotBypassUnfinishedHostRetirement)
	{
		auto* Host = NewHost(); auto A = RegisterA(); ASSERT_TRUE(Host->RefreshEditorSubsystems());
		auto* Service = Host->GetEditorSubsystem<FDynamicEditorA>(); ASSERT_NE(nullptr, Service);
		Host->DispatchSubsystemCallback([&] {
			Host->PrepareForShutdown();
			EXPECT_FALSE(Service->GetWorkGate()->IsOpen());
			EXPECT_FALSE(Service->IsReadyForFinishDestroy());
		});
		Host->PrepareForShutdown();
		EXPECT_TRUE(Service->IsReadyForFinishDestroy());
	}

	class FDynamicProvider : public IModuleInterface
	{
	public:
		explicit FDynamicProvider(std::shared_ptr<int> InStopped) : Stopped(std::move(InStopped)) {}
		auto SupportsDynamicReloading() const -> bool override { return true; }
		auto StartupModule() -> void override
		{
			Registration = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = FDynamicEditorA::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
			if (DuringStartup) DuringStartup();
		}
		auto CanShutdownModule() const -> bool override { return FEditorSubsystemCollection::CanSynchronizeActiveEditor(); }
		auto ShutdownModule() -> void override
		{
			require(CanShutdownModule());
			Registration.reset();
			FEditorSubsystemCollection::SynchronizeActiveEditor();
			++*Stopped;
		}
		std::unique_ptr<FEditorSubsystemRegistration> Registration;
		std::shared_ptr<int> Stopped;
		std::function<void()> DuringStartup;
	};

	TEST_F(FEditorSubsystemTests, ModuleShutdownUnregistersServicesBeforeUnload)
	{
		auto* Host = NewHost(); auto Stopped = std::make_shared<int>(0);
		const FName Name("DynamicEditorShutdownTest");
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(Name, std::make_unique<FDynamicProvider>(Stopped)));
		ASSERT_TRUE(Host->RefreshEditorSubsystems());
		TStrongObjectPtr<FDynamicEditorA> Object(Host->GetEditorSubsystem<FDynamicEditorA>());
		auto Gate = Object->GetWorkGate();
		Host->DispatchSubsystemCallback([&] {
			EXPECT_FALSE(FModuleManager::Get().UnloadModule(Name)); EXPECT_TRUE(Gate->IsOpen());
			EXPECT_EQ(0, *Stopped);
			EXPECT_EQ(EModuleState::Active, FModuleManager::Get().FindModule(Name)->State.load());
		});
		EXPECT_TRUE(FModuleManager::Get().UnloadModule(Name)); EXPECT_FALSE(Gate->IsOpen());
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>()); EXPECT_EQ(1, *Stopped);
		EXPECT_EQ(EModuleState::Unloaded, FModuleManager::Get().FindModule(Name)->State.load());
		// Fixture object code is resident in this test executable; reflected DLLs do not opt into unload.
		Object.Reset(); CollectGarbage();
		EXPECT_FALSE(Gate->IsOpen()); EXPECT_EQ(1, *Stopped);
		EXPECT_EQ(ESubsystemState::Ready, Host->GetEditorSubsystemState());
	}
	TEST_F(FEditorSubsystemTests, ExplicitModuleShutdownRemovesActiveEditorDependenciesBeforeReturning)
	{
		auto* First = NewHost();
		const FName Name("DynamicEditorExplicitShutdownTest"); auto Stopped = std::make_shared<int>(0);
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(Name, std::make_unique<FDynamicProvider>(Stopped)));
		FEditorSubsystemRegistration B({.Type = FDynamicEditorB::StaticClass(), .Dependencies = {FDynamicEditorA::StaticClass()}});
		ASSERT_TRUE(First->RefreshEditorSubsystems());
		auto FirstGate = First->GetEditorSubsystem<FDynamicEditorB>()->GetWorkGate();
		First->DispatchSubsystemCallback([&] {
			EXPECT_FALSE(FModuleManager::Get().UnloadModule(Name));
			EXPECT_TRUE(FirstGate->IsOpen());
		});
		FModuleManager::Get().ShutdownModule(Name);
		EXPECT_EQ(1, *Stopped); EXPECT_FALSE(FirstGate->IsOpen());
		EXPECT_EQ(nullptr, First->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_EQ(nullptr, First->GetEditorSubsystem<FDynamicEditorB>());
		CollectGarbage(); EXPECT_TRUE(FModuleManager::Get().UnloadModule(Name));
	}
	TEST_F(FEditorSubsystemTests, DoesNotInstantiateAProviderUntilItsModuleStartupCompletes)
	{
		auto* Host = NewHost(); const FName Name("DynamicEditorStartupTest");
		auto Provider = std::make_unique<FDynamicProvider>(std::make_shared<int>(0));
		Provider->DuringStartup = [&] {
			EXPECT_TRUE(Host->RefreshEditorSubsystems());
			EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		};
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(Name, std::move(Provider)));
		ASSERT_TRUE(Host->RefreshEditorSubsystems()); ASSERT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_TRUE(FModuleManager::Get().UnloadModule(Name)); CollectGarbage();
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
		FDynamicEditorA::Init = [&](auto&) { Events.emplace_back("service"); return FSubsystemResult{}; };
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
		FDynamicEditorA::Init = [](auto&) { return FSubsystemResult{ESubsystemError::InitializationFailed, "workspace service failed"}; };
		Frame.LoadModules = [] { return true; };
		FEditorEngineTestAccess::SetStartupPhase(*Host, Editor::Startup::EBootstrapState::LoadingWorkspace);
		Host->AdvanceStartup();
		EXPECT_EQ(EHostStartupState::Failed, Host->GetStartupState());
		EXPECT_NE(std::string::npos, Host->GetStartupError().find("workspace service failed"));
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
			Host->PrepareForShutdown();
			EXPECT_EQ(ESubsystemState::Ready, Host->GetEditorSubsystemState());
			EXPECT_FALSE(Service->GetWorkGate()->IsOpen());
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
	EXPECT_EQ(FEditorEngineTestAccess::StartEditor(*Host).Error, ESubsystemError::InitializationFailed);
	EXPECT_EQ(FFailingAssetEditorSubsystem::CleanupCalls, 1);
	EXPECT_EQ(Host->GetEditorSubsystem<FFailingAssetEditorSubsystem>(), nullptr);
	Host->PrepareForShutdown();
	EXPECT_EQ(FFailingAssetEditorSubsystem::CleanupCalls, 1);
}

TEST_F(FEditorSubsystemTests, FailedLateAssetEditorInitializationRetainsExistingServices)
{
	auto A = RegisterA();
	auto* Host = NewHost();
	auto* Existing = Host->GetEditorSubsystem<FDynamicEditorA>();
	ASSERT_NE(Existing, nullptr);
	FEditorSubsystemRegistration Token({.Type = FFailingAssetEditorSubsystem::StaticClass()});
	EXPECT_EQ(Host->RefreshEditorSubsystems().Error, ESubsystemError::InitializationFailed);
	EXPECT_EQ(FFailingAssetEditorSubsystem::CleanupCalls, 1);
	EXPECT_EQ(Host->GetEditorSubsystem<FFailingAssetEditorSubsystem>(), nullptr);
	EXPECT_EQ(Host->GetEditorSubsystem<FDynamicEditorA>(), Existing);
	EXPECT_TRUE(Existing->GetWorkGate()->IsOpen());
}

TEST_F(FEditorSubsystemTests, CreationEligibilityDoesNotReevaluateExistingInstances)
{
	int Selections = 0;
	bool bAllowA = true;
	FDynamicEditorA::Create = [&](const auto& Default, DObject*) {
		EXPECT_TRUE(Default.IsClassDefaultObject());
		++Selections;
		return bAllowA || Default.GetClass() != FDynamicEditorA::StaticClass();
	};
	auto A = RegisterA();
	auto B = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
		.Type = FDynamicEditorB::StaticClass(), .Dependencies = {FDynamicEditorA::StaticClass()}});
	auto* Host = NewHost();
	TStrongObjectPtr<FDynamicEditorA> ExistingA(Host->GetEditorSubsystem<FDynamicEditorA>());
	TStrongObjectPtr<FDynamicEditorB> ExistingB(Host->GetEditorSubsystem<FDynamicEditorB>());
	ASSERT_NE(nullptr, ExistingA.Get()); ASSERT_NE(nullptr, ExistingB.Get());
	EXPECT_EQ(2, Selections);
	bAllowA = false;
	ASSERT_TRUE(Host->RefreshEditorSubsystems(true));
	EXPECT_EQ(2, Selections);
	EXPECT_EQ(ExistingA.Get(), Host->GetEditorSubsystem<FDynamicEditorA>());
	EXPECT_EQ(ExistingB.Get(), Host->GetEditorSubsystem<FDynamicEditorB>());

	// Refusing implicit replacement does not run creation callbacks or disturb live instances.
	A.reset(); A = RegisterA();
	EXPECT_EQ(ESubsystemError::InvalidState, Host->RefreshEditorSubsystems().Error);
	EXPECT_EQ(2, Selections);
	EXPECT_EQ(ExistingA.Get(), Host->GetEditorSubsystem<FDynamicEditorA>());
	EXPECT_EQ(ExistingB.Get(), Host->GetEditorSubsystem<FDynamicEditorB>());
	B.reset();
	B = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
		.Type = FDynamicEditorB::StaticClass(), .Dependencies = {FDynamicEditorA::StaticClass()}});
	EXPECT_EQ(ESubsystemError::MissingDependency, Host->RefreshEditorSubsystems().Error);
	EXPECT_EQ(4, Selections);
	EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
	EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorB>());
	EXPECT_TRUE(IsValid(ExistingA.Get())); EXPECT_TRUE(IsValid(ExistingB.Get()));
	EXPECT_FALSE(ExistingA->GetWorkGate()->IsOpen()); EXPECT_FALSE(ExistingB->GetWorkGate()->IsOpen());
	bAllowA = true;
	ASSERT_TRUE(Host->RefreshEditorSubsystems(true));
	EXPECT_EQ(6, Selections);
	EXPECT_NE(ExistingA.Get(), Host->GetEditorSubsystem<FDynamicEditorA>());
	EXPECT_NE(ExistingB.Get(), Host->GetEditorSubsystem<FDynamicEditorB>());
	FDynamicEditorA::Create = {};
}
