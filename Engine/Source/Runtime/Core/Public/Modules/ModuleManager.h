#pragma once

#include "CoreAPI.h"
#include "Modules/AsyncOperationGroup.h"
#include "Modules/ModularFeature.h"
#include "Templates/SmartPointers.h"

#include <atomic>
#include <chrono>
#include <mutex>

namespace Durin
{
	class FModuleTestContextFactory;

	// Exposes the current load generation's owner-scoped registration surface during startup.
	class FModuleContext final
	{
	public:
		FModuleContext(FModuleContext&&) noexcept = default;
		auto operator=(FModuleContext&&) noexcept -> FModuleContext& = default;
		FModuleContext(const FModuleContext&) = delete;
		auto operator=(const FModuleContext&) -> FModuleContext& = delete;

		template<CModularFeature T>
		auto RegisterFeature(T& Implementation) -> FModularFeatureRegistration
		{
			return FModularFeatureRegistry::Get().Register(
				Owner,
				{FName(std::string_view(T::FeatureName)), static_cast<uint32>(T::FeatureVersion)},
				Implementation);
		}

		// Creates one owner-bound task admission and shutdown drain boundary.
		auto CreateAsyncOperationGroup(
			FName GroupName,
			FAsyncOperationGroupOptions Options = {}
		) -> FAsyncOperationGroup
		{
			return Detail::CreateAsyncOperationGroup(Owner, GroupName, Options);
		}

		[[nodiscard]] auto GetModuleName() const -> FName { return ModuleName; }

	private:
		FModuleContext(FName InModuleName, std::shared_ptr<Detail::FModularFeatureOwnerState> InOwner)
			: ModuleName(InModuleName), Owner(std::move(InOwner)) {}

		FName ModuleName;
		std::shared_ptr<Detail::FModularFeatureOwnerState> Owner;

		friend class FModuleManager;
		friend class FModuleTestContextFactory;
	};

	// Exposes mapped-library retirement evidence during the shutdown callback.
	class FModuleShutdownContext final
	{
	public:
		FModuleShutdownContext(FModuleShutdownContext&&) noexcept = default;
		auto operator=(FModuleShutdownContext&&) noexcept -> FModuleShutdownContext& = default;
		FModuleShutdownContext(const FModuleShutdownContext&) = delete;
		auto operator=(const FModuleShutdownContext&) -> FModuleShutdownContext& = delete;

		[[nodiscard]] auto GetModuleName() const -> FName { return ModuleName; }
		[[nodiscard]] auto GetFeatureRetirementSnapshot() const -> const FModularFeatureRetirementSnapshot& { return Snapshot; }
		[[nodiscard]] auto GetAsyncOperationSnapshot() const -> FAsyncOperationOwnerSnapshot;
		auto DrainAsyncOperations(std::chrono::milliseconds Timeout = std::chrono::seconds(5)) -> FAsyncOperationDrainResult;

	private:
		FModuleShutdownContext(
			FName InModuleName,
			FModularFeatureRetirementSnapshot InSnapshot,
			std::shared_ptr<Detail::FModularFeatureOwnerState> InOwner)
			: ModuleName(InModuleName), Snapshot(std::move(InSnapshot)), Owner(std::move(InOwner)) {}

		FName ModuleName;
		FModularFeatureRetirementSnapshot Snapshot;
		std::shared_ptr<Detail::FModularFeatureOwnerState> Owner;

		friend class FModuleManager;
		friend class FModuleTestContextFactory;
	};

	// Defines context-bearing lifecycle hooks for one native module instance.
	class IModuleInterface
	{
	public:
		virtual ~IModuleInterface() = default;
		virtual auto StartupModule(FModuleContext& Context) -> void {}
		virtual auto ShutdownModule(FModuleShutdownContext& Context) -> void {}
	};

	// Describes metadata, mapped-instance, retirement, and native-release lifecycle states.
	enum class EModuleState : uint8
	{
		Registered,
		Loading,
		Active,
		Retiring,
		StoppedMapped,
		UnloadBlocked,
		LoadFailed,
		Unloaded,
	};

	// Categorizes shutdown and unload completion or fail-closed rejection.
	enum class EModuleOperationStatus : uint8
	{
		Succeeded,
		NotFound,
		NotLoaded,
		AlreadyStopped,
		WrongControlThread,
		RecursiveOwnedExecution,
		FeatureInvocationDrainTimeout,
		ReflectedObjectDrainRejected,
		ShutdownCallbackFailure,
		AsyncOperationDrainTimeout,
		AsyncOperationSelfWait,
		AsyncOperationUnsupportedThread,
		OutstandingAsyncOperationAudit,
		OutstandingFeatureAudit,
		UnloadBlocked,
	};

	// Reports shutdown state, diagnostic text, and synchronous retirement evidence.
	struct FModuleShutdownResult
	{
		EModuleOperationStatus Status = EModuleOperationStatus::NotFound;
		FName ModuleName;
		EModuleState ObservedState = EModuleState::Registered;
		std::string Message;
		FModularFeatureRetirementSnapshot RetirementSnapshot;
		FAsyncOperationOwnerSnapshot AsyncOperationSnapshot;

		[[nodiscard]] auto Succeeded() const -> bool
		{
			return Status == EModuleOperationStatus::Succeeded || Status == EModuleOperationStatus::AlreadyStopped;
		}
	};

	// Reports physical unload state and the evidence that authorized or rejected it.
	struct FModuleUnloadResult
	{
		EModuleOperationStatus Status = EModuleOperationStatus::NotFound;
		FName ModuleName;
		EModuleState ObservedState = EModuleState::Registered;
		std::string Message;
		FModularFeatureRetirementSnapshot RetirementSnapshot;
		FAsyncOperationOwnerSnapshot AsyncOperationSnapshot;

		[[nodiscard]] auto Succeeded() const -> bool { return Status == EModuleOperationStatus::Succeeded; }
	};

	// Owns one logical module record and its current load generation resources.
	class FModuleInfo
	{
	public:
		FName ModuleName;
		std::string FileName;
		FModuleHandle Handle = nullptr;
		std::unique_ptr<IModuleInterface> Module;
		std::atomic<EModuleState> State = EModuleState::Registered;
		uint32 LoadOrder = 0;
		uint64 OwnerGeneration = 0;
		std::shared_ptr<Detail::FModularFeatureOwnerState> FeatureOwner;
	};

	using InitializeModuleFunc = IModuleInterface* (*)();

	// Serializes native module lifecycle and prevents unload before owner quiescence.
	class FModuleManager
	{
	public:
		using FModuleInfoPtr = std::shared_ptr<FModuleInfo>;
		using FModuleMap = std::unordered_map<FName, FModuleInfoPtr>;

		CORE_API static auto Get() -> FModuleManager&;

		template<typename TModuleInterface>
		static auto LoadModule(const FName& InModuleName) -> TModuleInterface*
		{
			return static_cast<TModuleInterface*>(Get().LoadModule(InModuleName));
		}

		template<typename TModuleInterface>
		static auto LoadModuleChecked(const FName& InModuleName) -> TModuleInterface&
		{
			return static_cast<TModuleInterface&>(Get().LoadModuleChecked(InModuleName));
		}

		CORE_API auto AddModule(const FName& InModuleName, const std::string& FileName) -> void;
		CORE_API auto FindModule(const FName& InModuleName) -> FModuleInfoPtr;
		CORE_API auto LoadModule(const FName& InModuleName) -> IModuleInterface*;
		CORE_API auto LoadModuleChecked(const FName& InModuleName) -> IModuleInterface&;
		CORE_API auto IsModuleLoaded(const FName& InModuleName) -> bool;
		CORE_API auto GetModule(const FName& InModuleName) -> IModuleInterface*;
		CORE_API auto ShutdownModule(const FName& InModuleName) -> FModuleShutdownResult;
		CORE_API auto UnloadModule(const FName& InModuleName) -> FModuleUnloadResult;
		CORE_API auto StartProcessingNewlyLoadedObjects() -> void;
		CORE_API auto SetProcessLoadedObjectsCallback(std::function<void()> Callback) -> void;
		CORE_API auto SetPreShutdownModuleCallback(std::function<bool(FName)> Callback) -> void;
		CORE_API auto UnloadModulesAtShutdown() -> void;

	private:
		FModuleManager();
		auto IsControlThread() const -> bool;
		auto MakeShutdownFailure(
			const FModuleInfoPtr& ModuleInfo,
			EModuleOperationStatus Status,
			std::string Message,
			FModularFeatureRetirementSnapshot Snapshot = {},
			FAsyncOperationOwnerSnapshot AsyncSnapshot = {}
		) -> FModuleShutdownResult;

		FModuleMap Modules;
		mutable std::mutex ModuleMapMutex;
		uint32 ControlThreadId = 0;
		uint32 NextLoadOrder = 0;
		uint64 NextOwnerGeneration = 1;
		std::chrono::milliseconds FeatureRetirementTimeout = std::chrono::seconds(5);
		bool bCanProcessNewlyLoadedObjects = false;
		std::function<void()> ProcessLoadedObjectsCallback;
		std::function<bool(FName)> PreShutdownModuleCallback;

		friend class FModuleTestContextFactory;
	};

	// Supplies no-op context-bearing lifecycle hooks for modules without custom work.
	class FDefaultModuleImpl : public IModuleInterface
	{
	};
}

// clang-format off
#define IMPLEMENT_MODULE(ModuleImplClass, ModuleName) \
	extern "C" DLLEXPORT IModuleInterface* InitializeModule() \
	{ \
		return new ModuleImplClass(); \
	} \
// clang-format on
