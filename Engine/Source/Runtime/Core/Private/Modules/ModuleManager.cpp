#include "Modules/ModuleManager.h"

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Build.h"
#include "Misc/Paths.h"
#include "Modules/ModuleStartupScope.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view GDefaultRuntimeVariant = "DurinEditor";

		struct FCurrentModuleStartup
		{
			FName ModuleName;
			std::shared_ptr<Detail::FModuleOwnerState> ModuleOwner;
		};

		thread_local std::vector<FCurrentModuleStartup> GCurrentModuleStartupStack;

		constexpr auto GetConfiguredRuntimeVariant() -> std::string_view
		{
#ifdef DURIN_RUNTIME_VARIANT
			return DURIN_RUNTIME_VARIANT;
#else
			return GDefaultRuntimeVariant;
#endif
		}

		auto GetDurinModuleFileName(const FName& InModuleName) -> std::string
		{
			return std::string(FPlatformMisc::FLibraryPrefix) + std::string(GetConfiguredRuntimeVariant())
				+ "-" + InModuleName.ToString() + FPlatformMisc::FLibraryExtension;
		}
	}

	namespace Detail
	{
		FScopedModuleStartup::FScopedModuleStartup(
			FName ModuleName,
			std::shared_ptr<FModuleOwnerState> InModuleOwner)
			: ModuleOwner(std::move(InModuleOwner))
		{
			require(ModuleOwner != nullptr);
			GCurrentModuleStartupStack.push_back({std::move(ModuleName), ModuleOwner});
		}

		FScopedModuleStartup::~FScopedModuleStartup()
		{
			require(!GCurrentModuleStartupStack.empty());
			require(GCurrentModuleStartupStack.back().ModuleOwner == ModuleOwner);
			GCurrentModuleStartupStack.pop_back();
		}
	}

	FModuleManager::FModuleManager()
		: ControlThreadId(FPlatformLTS::GetCurrentThreadId())
	{
	}

	auto FModuleManager::Get() -> FModuleManager&
	{
		// Construct the registry first so retained module instances may safely
		// release registrations when the manager is destroyed at process exit.
		(void)FModularFeatureRegistry::Get();
		static FModuleManager Instance;
		return Instance;
	}

	auto FModuleStartup::GetCurrentOwner() -> std::shared_ptr<Detail::FModuleOwnerState>
	{
		require(!GCurrentModuleStartupStack.empty());
		return GCurrentModuleStartupStack.back().ModuleOwner;
	}

	auto FModuleStartup::CreateAsyncOperationGroup(
		FName GroupName) -> FAsyncOperationGroup
	{
		return Detail::CreateAsyncOperationGroup(
			GetCurrentOwner(), std::move(GroupName));
	}

	auto FModuleStartup::GetModuleName() -> FName
	{
		require(!GCurrentModuleStartupStack.empty());
		return GCurrentModuleStartupStack.back().ModuleName;
	}

	auto FModuleManager::IsControlThread() const -> bool
	{
		const uint32 ExpectedThread = GIsGameThreadIdInitialized ? GGameThreadId : ControlThreadId;
		return FPlatformLTS::GetCurrentThreadId() == ExpectedThread;
	}

	auto FModuleManager::AddModule(const FName& InModuleName, const std::string& FileName) -> void
	{
		std::lock_guard Lock(ModuleMapMutex);
		if (Modules.contains(InModuleName)) return;
		auto ModuleInfo = std::make_shared<FModuleInfo>();
		ModuleInfo->ModuleName = InModuleName;
		ModuleInfo->FileName = FileName;
		Modules.emplace(InModuleName, std::move(ModuleInfo));
	}

	auto FModuleManager::FindModule(const FName& InModuleName) -> FModuleInfoPtr
	{
		std::lock_guard Lock(ModuleMapMutex);
		const auto Iter = Modules.find(InModuleName);
		return Iter == Modules.end() ? nullptr : Iter->second;
	}

	auto FModuleManager::IsModuleLoaded(const FName& InModuleName) -> bool
	{
		const auto ModuleInfo = FindModule(InModuleName);
		return ModuleInfo && ModuleInfo->Module && ModuleInfo->State.load() == EModuleState::Active;
	}

	auto FModuleManager::LoadModuleChecked(const FName& InModuleName) -> IModuleInterface&
	{
		const auto Module = LoadModule(InModuleName);
		check(Module != nullptr);
		return *Module;
	}

	auto FModuleManager::GetModule(const FName& InModuleName) -> IModuleInterface*
	{
		const auto ModuleInfo = FindModule(InModuleName);
		if (ModuleInfo && ModuleInfo->State.load() == EModuleState::Active) return ModuleInfo->Module.get();
		DURIN_ERROR(STR("Module {} is not active when trying to get it."), InModuleName.ToString());
		return nullptr;
	}

	auto FModuleManager::LoadModule(const FName& InModuleName) -> IModuleInterface*
	{
		if (!IsControlThread())
		{
			DURIN_ERROR(STR("Module {} load was requested from the wrong control thread."), InModuleName.ToString());
			return nullptr;
		}

		auto ModuleInfo = FindModule(InModuleName);
		if (!ModuleInfo)
		{
			AddModule(InModuleName, GetDurinModuleFileName(InModuleName));
			ModuleInfo = FindModule(InModuleName);
		}

		const EModuleState ExistingState = ModuleInfo->State.load();
		if (ExistingState == EModuleState::Active) return ModuleInfo->Module.get();
		if (ExistingState == EModuleState::StoppedMapped
			|| ExistingState == EModuleState::ShuttingDown || ExistingState == EModuleState::Loading)
		{
			DURIN_ERROR(STR("Module {} cannot load from lifecycle state {}."),
				InModuleName.ToString(), static_cast<uint32>(ExistingState));
			return nullptr;
		}

		ModuleInfo->State = EModuleState::Loading;
		DURIN_TRACE(STR("Try load: {}"), ModuleInfo->FileName);
		FModuleHandle ModuleHandle = FPlatformMisc::LoadLibrary(ModuleInfo->FileName);
		if (!ModuleHandle)
		{
			const std::filesystem::path ExecutableModule =
				std::filesystem::path(FPlatformProcess::ExecutablePath()).parent_path()
				/ ModuleInfo->FileName;
			DURIN_TRACE(STR("Try executable-adjacent module: {}"),
				ExecutableModule.generic_string());
			ModuleHandle = FPlatformMisc::LoadLibrary(ExecutableModule.generic_string());
			if (ModuleHandle) ModuleInfo->FileName = ExecutableModule.generic_string();
		}
		if (!ModuleHandle && !FPaths::ProjectDir().empty())
		{
			const std::filesystem::path ProjectModule = std::filesystem::path(FPaths::ProjectDir())
				/ "Binaries" / DURIN_BUILD_PLATFORM_STRING / DURIN_BUILD_TYPE_STRING
				/ "Runtime" / GetConfiguredRuntimeVariant() / ModuleInfo->FileName;
			DURIN_TRACE(STR("Try active-project module: {}"), ProjectModule.generic_string());
			ModuleHandle = FPlatformMisc::LoadLibrary(ProjectModule.generic_string());
			if (ModuleHandle) ModuleInfo->FileName = ProjectModule.generic_string();
		}
		if (!ModuleHandle)
		{
			ModuleInfo->State = EModuleState::LoadFailed;
			DURIN_ERROR(STR("Failed to load module \"{}\" from \"{}\": {}."),
				InModuleName.ToString(), ModuleInfo->FileName,
				FPlatformMisc::GetLastLibraryError());
			return nullptr;
		}

		const auto InitializeModuleFunction = reinterpret_cast<InitializeModuleFunc>(
			FPlatformMisc::GetProcAddress(ModuleHandle, "InitializeModule"));
		if (!InitializeModuleFunction)
		{
			FPlatformMisc::FreeLibrary(ModuleHandle);
			ModuleInfo->State = EModuleState::LoadFailed;
			DURIN_ERROR("Failed to get module interface from module.");
			return nullptr;
		}

		ModuleInfo->Handle = ModuleHandle;
		ModuleInfo->Module.reset(InitializeModuleFunction());
		if (!ModuleInfo->Module)
		{
			FPlatformMisc::FreeLibrary(ModuleHandle);
			ModuleInfo->Handle = nullptr;
			ModuleInfo->State = EModuleState::LoadFailed;
			return nullptr;
		}

		ModuleInfo->OwnerGeneration = NextOwnerGeneration++;
		ModuleInfo->ModuleOwner = FModularFeatureRegistry::Get().CreateOwner(InModuleName, ModuleInfo->OwnerGeneration);
		if (bCanProcessNewlyLoadedObjects && ProcessLoadedObjectsCallback) ProcessLoadedObjectsCallback();
		bool bStartupFailed = false;
		try
		{
			Detail::FScopedModuleStartup StartupScope(InModuleName, ModuleInfo->ModuleOwner);
			ModuleInfo->Module->StartupModule();
		}
		catch (...)
		{
			bStartupFailed = true;
		}
		if (bStartupFailed)
		{
			// Destroy the startup exception before any code library can be released.
			// A partially started module owns its rollback as well. If cleanup throws,
			// leave the instance and code mapped and propagate the contract failure.
			ModuleInfo->State = EModuleState::ShuttingDown;
			ModuleInfo->Module->ShutdownModule();
			ModuleInfo->State = EModuleState::StoppedMapped;
			// Failed startup is not permission to unload process-resident code (for
			// example, reflected types registered before StartupModule was entered).
			if (ModuleInfo->Module->SupportsDynamicReloading())
			{
				ModuleInfo->State = EModuleState::ShuttingDown;
				ModuleInfo->Module.reset();
				FPlatformMisc::FreeLibrary(ModuleHandle);
				ModuleInfo->Handle = nullptr;
				ModuleInfo->ModuleOwner.reset();
				ModuleInfo->State = EModuleState::LoadFailed;
			}
			DURIN_ERROR(STR("Module {} startup callback failed."), InModuleName.ToString());
			return nullptr;
		}
		ModuleInfo->LoadOrder = NextLoadOrder++;
		ModuleInfo->State = EModuleState::Active;
		DURIN_DEBUG(STR("Module loaded: {}"), InModuleName.ToString());
		return ModuleInfo->Module.get();
	}

	auto FModuleManager::AcquireCodeLease(FName ModuleName) -> std::shared_ptr<void>
	{
		if (!IsControlThread()) return {};
		auto Info = FindModule(ModuleName);
		if (!Info || Info->State.load() != EModuleState::Active) return {};
		++Info->CodeLeaseCount;
		return std::shared_ptr<void>(Info.get(), [Info](void*) { --Info->CodeLeaseCount; });
	}

	auto FModuleManager::ShutdownModule(const FName& InModuleName) -> void
	{
		requiref(IsControlThread(), "Module shutdown must run on the module-control thread.");
		const auto Info = FindModule(InModuleName);
		if (!Info || !Info->Module || Info->State.load() == EModuleState::StoppedMapped) return;
		requiref(Info->State.load() == EModuleState::Active,
			"Module {} cannot shut down recursively or after failed cleanup.", InModuleName.ToString());
		requiref(Info->CodeLeaseCount.load() == 0,
			"Module {} still has live consumers at shutdown.", InModuleName.ToString());
		Info->State = EModuleState::ShuttingDown;
		try
		{
			Info->Module->ShutdownModule();
		}
		catch (...)
		{
			DURIN_ERROR(STR("Module {} shutdown failed; cleanup cannot continue."), InModuleName.ToString());
			throw;
		}
		Info->State = EModuleState::StoppedMapped;
		DURIN_DEBUG(STR("Module shutdown: {}"), InModuleName.ToString());
	}

	auto FModuleManager::UnloadModule(const FName& InModuleName) -> bool
	{
		if (!IsControlThread())
		{
			DURIN_ERROR("Module unload must run on the module-control thread.");
			return false;
		}
		const auto Info = FindModule(InModuleName);
		if (!Info || !Info->Module) return false;
		const auto State = Info->State.load();
		if ((State != EModuleState::Active && State != EModuleState::StoppedMapped)
			|| !Info->Module->SupportsDynamicReloading() || Info->CodeLeaseCount.load() != 0)
		{
			DURIN_ERROR(STR("Module {} does not permit runtime unload in its current state."), InModuleName.ToString());
			return false;
		}
		ShutdownModule(InModuleName);
		// Keep reentrant loads/unloads out while the module destructor runs.
		Info->State = EModuleState::ShuttingDown;
		Info->Module.reset();
		if (Info->Handle)
		{
			FPlatformMisc::FreeLibrary(Info->Handle);
			Info->Handle = nullptr;
		}
		Info->ModuleOwner.reset();
		Info->State = EModuleState::Unloaded;
		DURIN_DEBUG(STR("Module unloaded: {}"), InModuleName.ToString());
		return true;
	}

	auto FModuleManager::StartProcessingNewlyLoadedObjects() -> void
	{
		check(!bCanProcessNewlyLoadedObjects);
		bCanProcessNewlyLoadedObjects = true;
	}

	auto FModuleManager::SetProcessLoadedObjectsCallback(std::function<void()> Callback) -> void
	{
		ProcessLoadedObjectsCallback = std::move(Callback);
	}

	auto FModuleManager::ShutdownModulesAtExit(
		std::span<const FName> DeferredModules) -> void
	{
		require(IsControlThread());
		std::vector<FModuleInfoPtr> ModulesToStop;
		{
			std::lock_guard Lock(ModuleMapMutex);
			ModulesToStop.reserve(Modules.size());
			for (const auto& ModuleInfo : Modules | std::views::values) ModulesToStop.push_back(ModuleInfo);
		}
		std::ranges::sort(ModulesToStop, [](const FModuleInfoPtr& A, const FModuleInfoPtr& B) {
			return A->LoadOrder > B->LoadOrder;
		});
		for (const auto& ModuleInfo : ModulesToStop)
		{
			if (std::ranges::contains(DeferredModules, ModuleInfo->ModuleName)) continue;
			if (ModuleInfo->State.load() != EModuleState::Active) continue;
			ShutdownModule(ModuleInfo->ModuleName);
		}
	}
}
