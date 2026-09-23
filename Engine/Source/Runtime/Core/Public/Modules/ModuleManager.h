#pragma once

#include "CoreAPI.h"
#include "Modules/AsyncOperationGroup.h"
#include "Modules/ModularFeature.h"
#include "Templates/SmartPointers.h"

#include <atomic>
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
			FName GroupName
		) -> FAsyncOperationGroup;

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
		// Runtime DLL release is opt-in; process shutdown does not consult this flag.
		virtual auto SupportsDynamicReloading() const -> bool { return false; }
		// Stop external entry points, drain work, and release every externally stored
		// callback/object before returning. Cleanup failure is a lifecycle contract error.
		// Also cleans partially initialized state if StartupModule throws.
		virtual auto ShutdownModule() -> void {}
	};

	// Describes loading, explicit shutdown, and native-library release.
	enum class EModuleState : uint8
	{
		Registered,
		Loading,
		Active,
		ShuttingDown,
		StoppedMapped,
		LoadFailed,
		Unloaded,
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
		std::atomic<uint64> CodeLeaseCount = 0;
		std::shared_ptr<Detail::FModuleOwnerState> ModuleOwner;
	};

	using InitializeModuleFunc = IModuleInterface* (*)();

	// Orders lifecycle callbacks; modules own all resource and task cleanup.
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
		// Pins an active load generation until every copy is released. Control-thread admission only.
		CORE_API auto AcquireCodeLease(FName ModuleName) -> std::shared_ptr<void>;
		CORE_API auto IsModuleLoaded(const FName& InModuleName) -> bool;
		CORE_API auto GetModule(const FName& InModuleName) -> IModuleInterface*;
		// Control-thread safe point only: dependent consumers and external dispatch are stopped.
		// Calls ShutdownModule once and retains the instance and DLL until unload/process exit.
		// Independent of SupportsDynamicReloading. Cleanup failures must not be ignored.
		CORE_API auto ShutdownModule(const FName& InModuleName) -> void;
		// Runtime DLL release requires explicit dynamic-reload support and no code leases.
		// Returns false for rejected requests. Shutdown contract failures propagate.
		CORE_API auto UnloadModule(const FName& InModuleName) -> bool;
		CORE_API auto StartProcessingNewlyLoadedObjects() -> void;
		CORE_API auto SetProcessLoadedObjectsCallback(std::function<void()> Callback) -> void;
		CORE_API auto ShutdownModulesAtExit(
			std::span<const FName> DeferredModules = {}) -> void;

	private:
		FModuleManager();
		auto IsControlThread() const -> bool;

		mutable std::mutex ModuleMapMutex;
		uint32 ControlThreadId = 0;
		uint32 NextLoadOrder = 0;
		uint64 NextOwnerGeneration = 1;
		bool bCanProcessNewlyLoadedObjects = false;
		std::function<void()> ProcessLoadedObjectsCallback;
		// Keep the map last so module instances are destroyed before the mutex and
		// callbacks they may consult during process-exit teardown.
		FModuleMap Modules;

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
