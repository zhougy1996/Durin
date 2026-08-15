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
	class FModuleTestHarness;

	// Creates state attributed to the exact module load generation currently starting.
	class FModuleStartup final
	{
	public:
		// These operations require the manager-controlled startup scope and reject unattributed use.
		template<CModularFeature T>
		static auto RegisterFeature(T& Implementation) -> FModularFeatureRegistration
		{
			return FModularFeatureRegistry::Get().Register(
				GetCurrentOwner(),
				{FName(std::string_view(T::FeatureName)), static_cast<uint32>(T::FeatureVersion)},
				Implementation);
		}

		CORE_API static auto CreateAsyncOperationGroup(
			FName GroupName,
			FAsyncOperationGroupOptions Options = {}
		) -> FAsyncOperationGroup;

		CORE_API static auto CreateOwnedCallbackRegistration(FName DomainName)
			-> FModuleOwnedCallbackRegistration;
		[[nodiscard]] CORE_API static auto GetModuleName() -> FName;

	private:
		CORE_API static auto GetCurrentOwner() -> std::shared_ptr<Detail::FModuleOwnerState>;
	};

	// Defines parameterless lifecycle hooks for one native module instance.
	class IModuleInterface
	{
	public:
		virtual ~IModuleInterface() = default;
		virtual auto StartupModule() -> void {}
		virtual auto ShutdownModule() -> void {}
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
		std::shared_ptr<Detail::FModuleOwnerState> ModuleOwner;
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
		CORE_API auto UnloadModulesAtShutdown(
			std::span<const FName> DeferredModules = {}) -> void;

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

		friend class FModuleTestHarness;
	};
}

// clang-format off
#define IMPLEMENT_MODULE(ModuleImplClass, ModuleName) \
	extern "C" DLLEXPORT IModuleInterface* InitializeModule() \
	{ \
		return new ModuleImplClass(); \
	} \
// clang-format on
