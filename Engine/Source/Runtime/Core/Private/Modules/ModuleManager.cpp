#include "Modules/ModuleManager.h"

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
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
		FName GroupName,
		FAsyncOperationGroupOptions Options) -> FAsyncOperationGroup
	{
		return Detail::CreateAsyncOperationGroup(
			GetCurrentOwner(), std::move(GroupName), Options);
	}

	auto FModuleStartup::CreateOwnedCallbackRegistration(FName DomainName)
		-> FModuleOwnedCallbackRegistration
	{
		return FModularFeatureRegistry::Get().RegisterOwnedCallback(
			GetCurrentOwner(), std::move(DomainName));
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
		if (ExistingState == EModuleState::StoppedMapped || ExistingState == EModuleState::UnloadBlocked
			|| ExistingState == EModuleState::Retiring || ExistingState == EModuleState::Loading)
		{
			DURIN_ERROR(STR("Module {} cannot load from lifecycle state {}."),
				InModuleName.ToString(), static_cast<uint32>(ExistingState));
			return nullptr;
		}

		ModuleInfo->State = EModuleState::Loading;
		DURIN_TRACE(STR("Try load: {}"), ModuleInfo->FileName);
		FModuleHandle ModuleHandle = FPlatformMisc::LoadLibrary(ModuleInfo->FileName);
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
			DURIN_ERROR(STR("Failed to load module \"{}\"."), InModuleName.ToString());
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
		try
		{
			Detail::FScopedModuleStartup StartupScope(InModuleName, ModuleInfo->ModuleOwner);
			ModuleInfo->Module->StartupModule();
		}
		catch (...)
		{
			const auto Retirement = FModularFeatureRegistry::Get().RetireOwner(
				ModuleInfo->ModuleOwner, FeatureRetirementTimeout);
			Detail::BeginRetireAsyncOperationOwner(ModuleInfo->ModuleOwner);
			const auto AsyncRetirement = Detail::DrainAsyncOperationOwner(
				ModuleInfo->ModuleOwner, FeatureRetirementTimeout);
			if (!Retirement.Succeeded() || !AsyncRetirement.Succeeded())
			{
				ModuleInfo->State = EModuleState::UnloadBlocked;
				DURIN_ERROR(STR("Module {} startup failed and its features could not retire."), InModuleName.ToString());
				return nullptr;
			}
			ModuleInfo->Module.reset();
			FPlatformMisc::FreeLibrary(ModuleHandle);
			ModuleInfo->Handle = nullptr;
			ModuleInfo->ModuleOwner.reset();
			ModuleInfo->State = EModuleState::LoadFailed;
			DURIN_ERROR(STR("Module {} startup callback failed."), InModuleName.ToString());
			return nullptr;
		}
		ModuleInfo->LoadOrder = NextLoadOrder++;
		ModuleInfo->State = EModuleState::Active;
		DURIN_DEBUG(STR("Module loaded: {}"), InModuleName.ToString());
		return ModuleInfo->Module.get();
	}

	auto FModuleManager::MakeShutdownFailure(
		const FModuleInfoPtr& ModuleInfo,
		EModuleOperationStatus Status,
		std::string Message,
		FModularFeatureRetirementSnapshot Snapshot,
		FAsyncOperationOwnerSnapshot AsyncSnapshot
	) -> FModuleShutdownResult
	{
		if (ModuleInfo) ModuleInfo->State = EModuleState::UnloadBlocked;
		return {
			Status,
			ModuleInfo ? ModuleInfo->ModuleName : FName(),
			ModuleInfo ? ModuleInfo->State.load() : EModuleState::Registered,
			std::move(Message),
			std::move(Snapshot),
			std::move(AsyncSnapshot)
		};
	}

	auto FModuleManager::ShutdownModule(const FName& InModuleName) -> FModuleShutdownResult
	{
		const auto ModuleInfo = FindModule(InModuleName);
		if (!ModuleInfo)
		{
			return {EModuleOperationStatus::NotFound, InModuleName, EModuleState::Registered, "Module metadata was not found.", {}};
		}
		if (!IsControlThread())
		{
			return {EModuleOperationStatus::WrongControlThread, InModuleName, ModuleInfo->State.load(),
				"Module shutdown must run on the module-control thread.", {}};
		}

		const EModuleState State = ModuleInfo->State.load();
		if (State == EModuleState::StoppedMapped)
		{
			return {EModuleOperationStatus::AlreadyStopped, InModuleName, State, "Module is already stopped and mapped.",
				FModularFeatureRegistry::Get().SnapshotOwner(ModuleInfo->ModuleOwner)};
		}
		if (State == EModuleState::UnloadBlocked)
		{
			return {EModuleOperationStatus::UnloadBlocked, InModuleName, State, "Module retirement previously failed and is irreversible.",
				FModularFeatureRegistry::Get().SnapshotOwner(ModuleInfo->ModuleOwner)};
		}
		if (State != EModuleState::Active || !ModuleInfo->Module)
		{
			return {EModuleOperationStatus::NotLoaded, InModuleName, State, "Module does not have an active instance.", {}};
		}

		ModuleInfo->State = EModuleState::Retiring;
		auto Retirement = FModularFeatureRegistry::Get().RetireOwner(ModuleInfo->ModuleOwner, FeatureRetirementTimeout);
		if (Retirement.Status == EModularFeatureRetirementStatus::SelfWait)
		{
			return MakeShutdownFailure(ModuleInfo, EModuleOperationStatus::RecursiveOwnedExecution,
				"Module shutdown was requested recursively from its own feature invocation.", Retirement.Snapshot);
		}
		if (Retirement.Status == EModularFeatureRetirementStatus::TimedOut)
		{
			return MakeShutdownFailure(ModuleInfo, EModuleOperationStatus::FeatureInvocationDrainTimeout,
				"Timed out draining admitted synchronous feature invocations.", Retirement.Snapshot);
		}
		Detail::BeginRetireAsyncOperationOwner(ModuleInfo->ModuleOwner);

		bool bReflectedObjectsDrained = true;
		try
		{
			bReflectedObjectsDrained = !PreShutdownModuleCallback || PreShutdownModuleCallback(InModuleName);
		}
		catch (...)
		{
			bReflectedObjectsDrained = false;
		}
		if (!bReflectedObjectsDrained)
		{
			return MakeShutdownFailure(ModuleInfo, EModuleOperationStatus::ReflectedObjectDrainRejected,
				"Reflected objects owned by the module did not drain.", Retirement.Snapshot);
		}

		try
		{
			ModuleInfo->Module->ShutdownModule();
		}
		catch (...)
		{
			return MakeShutdownFailure(ModuleInfo, EModuleOperationStatus::ShutdownCallbackFailure,
				"The module shutdown callback failed; the native library remains mapped.", Retirement.Snapshot,
				Detail::SnapshotAsyncOperationOwner(ModuleInfo->ModuleOwner));
		}

		const auto AsyncDrain = Detail::DrainAsyncOperationOwner(ModuleInfo->ModuleOwner, FeatureRetirementTimeout);
		if (!AsyncDrain.Succeeded())
		{
			EModuleOperationStatus Status = EModuleOperationStatus::OutstandingAsyncOperationAudit;
			if (AsyncDrain.Status == EAsyncOperationDrainStatus::TimedOut) Status = EModuleOperationStatus::AsyncOperationDrainTimeout;
			else if (AsyncDrain.Status == EAsyncOperationDrainStatus::SelfWait) Status = EModuleOperationStatus::AsyncOperationSelfWait;
			else if (AsyncDrain.Status == EAsyncOperationDrainStatus::UnsupportedThread) Status = EModuleOperationStatus::AsyncOperationUnsupportedThread;
			return MakeShutdownFailure(ModuleInfo, Status, AsyncDrain.Message, Retirement.Snapshot, AsyncDrain.Snapshot);
		}
		const auto AsyncAudit = Detail::SnapshotAsyncOperationOwner(ModuleInfo->ModuleOwner);
		if (AsyncAudit.ActiveTaskCount != 0 || AsyncAudit.RetainedResultCount != 0
			|| AsyncAudit.RetainedDeferredCallableCount != 0 || AsyncAudit.GroupsWithWorkerCallables != 0)
		{
			return MakeShutdownFailure(ModuleInfo, EModuleOperationStatus::OutstandingAsyncOperationAudit,
				"Owned asynchronous operations failed the final callable and result audit.", Retirement.Snapshot, AsyncAudit);
		}
		const auto Audit = FModularFeatureRegistry::Get().SnapshotOwner(ModuleInfo->ModuleOwner);
		if (Audit.PublishedCount != 0 || Audit.InFlightInvocationCount != 0
			|| Audit.RetainedResourceCount != 0)
		{
			return MakeShutdownFailure(ModuleInfo, EModuleOperationStatus::OutstandingFeatureAudit,
				"Owned feature registrations failed the final synchronous retirement audit.", Audit);
		}

		ModuleInfo->State = EModuleState::StoppedMapped;
		DURIN_DEBUG(STR("Module shutdown: {}"), InModuleName.ToString());
		return {EModuleOperationStatus::Succeeded, InModuleName, EModuleState::StoppedMapped,
			"Module stopped and remains mapped.", Audit, AsyncAudit};
	}

	auto FModuleManager::UnloadModule(const FName& InModuleName) -> FModuleUnloadResult
	{
		const auto ModuleInfo = FindModule(InModuleName);
		if (!ModuleInfo)
		{
			return {EModuleOperationStatus::NotFound, InModuleName, EModuleState::Registered, "Module metadata was not found.", {}};
		}
		if (!IsControlThread())
		{
			return {EModuleOperationStatus::WrongControlThread, InModuleName, ModuleInfo->State.load(),
				"Module unload must run on the module-control thread.", {}};
		}
		if ((ModuleInfo->State.load() == EModuleState::Unloaded
			|| ModuleInfo->State.load() == EModuleState::Registered
			|| ModuleInfo->State.load() == EModuleState::LoadFailed) && !ModuleInfo->Module)
		{
			return {EModuleOperationStatus::NotLoaded, InModuleName, ModuleInfo->State.load(),
				"Module does not have a mapped instance to unload.", {}};
		}

		if (ModuleInfo->State.load() == EModuleState::Active)
		{
			const auto Shutdown = ShutdownModule(InModuleName);
			if (!Shutdown.Succeeded())
			{
				return {Shutdown.Status, InModuleName, Shutdown.ObservedState, Shutdown.Message,
					Shutdown.RetirementSnapshot, Shutdown.AsyncOperationSnapshot};
			}
		}
		if (ModuleInfo->State.load() != EModuleState::StoppedMapped)
		{
			return {EModuleOperationStatus::UnloadBlocked, InModuleName, ModuleInfo->State.load(),
				"Native unload is allowed only after successful synchronous retirement and shutdown.",
				FModularFeatureRegistry::Get().SnapshotOwner(ModuleInfo->ModuleOwner)};
		}

		const auto Snapshot = FModularFeatureRegistry::Get().SnapshotOwner(ModuleInfo->ModuleOwner);
		const auto AsyncSnapshot = Detail::SnapshotAsyncOperationOwner(ModuleInfo->ModuleOwner);
		ModuleInfo->Module.reset();
		if (ModuleInfo->Handle)
		{
			FPlatformMisc::FreeLibrary(ModuleInfo->Handle);
			ModuleInfo->Handle = nullptr;
		}
		ModuleInfo->ModuleOwner.reset();
		ModuleInfo->State = EModuleState::Unloaded;
		DURIN_DEBUG(STR("Module unloaded: {}"), InModuleName.ToString());
		return {EModuleOperationStatus::Succeeded, InModuleName, EModuleState::Unloaded,
			"Module library unloaded.", Snapshot, AsyncSnapshot};
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

	auto FModuleManager::SetPreShutdownModuleCallback(std::function<bool(FName)> Callback) -> void
	{
		PreShutdownModuleCallback = std::move(Callback);
	}

	auto FModuleManager::UnloadModulesAtShutdown(
		std::span<const FName> DeferredModules) -> void
	{
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
			if (std::ranges::find(DeferredModules, ModuleInfo->ModuleName)
				!= DeferredModules.end()) continue;
			if (ModuleInfo->State.load() != EModuleState::Active) continue;
			const auto Result = ShutdownModule(ModuleInfo->ModuleName);
			if (!Result.Succeeded())
			{
				DURIN_ERROR(STR("Module {} failed process-shutdown retirement: {}"),
					ModuleInfo->ModuleName.ToString(), Result.Message);
			}
		}
	}
}
