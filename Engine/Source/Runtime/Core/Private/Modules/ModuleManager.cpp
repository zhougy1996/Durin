#include "Modules/ModuleManager.h"

namespace Durin
{
	namespace
	{
		constexpr std::string_view GDefaultRuntimeVariant = "DurinEditor";

		constexpr auto GetConfiguredRuntimeVariant() -> std::string_view
		{
#ifdef DURIN_RUNTIME_VARIANT
			return DURIN_RUNTIME_VARIANT;
#else
			return GDefaultRuntimeVariant;
#endif
		}
	}

	auto FModuleManager::Get() -> FModuleManager&
	{
		static FModuleManager Instance;
		return Instance;
	}

	auto FModuleManager::AddModule(const FName& InModuleName, const std::string& FileName) -> void
	{
		static uint32 ModuleIndex = 0;

		auto ModuleInfoPtr = std::make_shared<FModuleInfo>();
		ModuleInfoPtr->ModuleName = InModuleName;
		ModuleInfoPtr->FileName = FileName;
		ModuleInfoPtr->LoadOrder = ModuleIndex++;
		Modules.emplace(InModuleName, ModuleInfoPtr);
	}

	auto FModuleManager::FindModule(const FName& InModuleName) -> FModuleInfoPtr
	{
		auto Iter = Modules.find(InModuleName);
		if (Iter == Modules.end())
		{
			return nullptr;
		}

		return Iter->second;
	}

	auto FModuleManager::IsModuleLoaded(const FName& InModuleName) -> bool
	{
		auto ModuleInfo = FindModule(InModuleName);

		// If the module is not found, it is not loaded.
		if (ModuleInfo == nullptr) { return false; }

		// Only if the module is loaded and the module interface is not null, the module is considered loaded.
		if (ModuleInfo->Module != nullptr)
		{
			return true;
		}

		return false;
	}

	auto FModuleManager::LoadModuleChecked(const FName& InModuleName) -> IModuleInterface&
	{
		const auto Module = LoadModule(InModuleName);
		check(Module != nullptr);
		return *Module;
	}

	auto FModuleManager::GetModule(const FName& InModuleName) -> IModuleInterface*
	{
		auto ModuleInfo = FindModule(InModuleName);
		if (ModuleInfo == nullptr)
		{
			return nullptr;
		}

		if (ModuleInfo->bIsReady)
		{
			return ModuleInfo->Module.get();
		}

		// If the module is not ready, it is not loaded.
		DURIN_ERROR(STR("Module {} is not ready when trying to get it."), InModuleName.ToString());

		return nullptr;
	}

	static constexpr auto GetDurinModuleFileName(const FName& InModuleName) -> std::string
	{
		return std::string(FPlatformMisc::FLibraryPrefix) + std::string(GetConfiguredRuntimeVariant()) + "-" + InModuleName.ToString() + FPlatformMisc::FLibraryExtension;
	}

	auto FModuleManager::LoadModule(const FName& InModuleName) -> IModuleInterface*
	{
		IModuleInterface* LoadedModule = nullptr;
		FModuleInfoPtr FoundModuleInfo = FindModule(InModuleName);

		if (FoundModuleInfo)
		{
			LoadedModule = FoundModuleInfo->Module.get();
			if (LoadedModule)
			{
				DURIN_DEBUG(STR("Module {} is already loaded."), InModuleName.ToString());
				return LoadedModule;
			}
		}

		if (FoundModuleInfo == nullptr)
		{
			AddModule(InModuleName, GetDurinModuleFileName(InModuleName));
			FoundModuleInfo = FindModule(InModuleName);
		}
		DURIN_TRACE(STR("Try load: {}"), FoundModuleInfo->FileName);
		FModuleHandle ModuleHandle = FPlatformMisc::LoadLibrary(FoundModuleInfo->FileName);
		if (!ModuleHandle)
		{
			DURIN_ERROR(STR("Failed to load module \"{}\"."), InModuleName.ToString());
			return nullptr;
		}
		IModuleInterface* Result = nullptr;

		// InitializeModule is defined by the macro IMPLEMENT_MODULE in the module's source file.
		InitializeModuleFunc InitializeModuleFunctionPtr = (InitializeModuleFunc)FPlatformMisc::GetProcAddress(ModuleHandle, "InitializeModule");

		if (!InitializeModuleFunctionPtr)
		{
			DURIN_ERROR(STR("Failed to get module interface from module."));
			FPlatformMisc::FreeLibrary(ModuleHandle);
			return nullptr;
		}

		FoundModuleInfo->Handle = ModuleHandle;
		Result = InitializeModuleFunctionPtr();
		FoundModuleInfo->Module = std::unique_ptr<IModuleInterface>(Result);
		DURIN_DEBUG(STR("Module loaded: {}"), InModuleName.ToString());

		if (bCanProcessNewlyLoadedObjects)
		{
			ProcessLoadedObjectsCallback();
		}

		// Call the module's startup function.
		Result->StartupModule();
		FoundModuleInfo->bIsReady = true;

		return Result;
	}

	auto FModuleManager::ShutdownModule(const FName& InModuleName) -> void
	{
		const FModuleInfoPtr ModuleInfo = FindModule(InModuleName);
		if (!ModuleInfo || !ModuleInfo->Module || !ModuleInfo->bIsReady.exchange(false))
		{
			return;
		}

		ModuleInfo->Module->ShutdownModule();
		DURIN_DEBUG(STR("Module shutdown: {}"), InModuleName.ToString());
	}

	auto FModuleManager::UnloadModule(const FName& InModuleName) -> void
	{
		auto ModuleIt = Modules.find(InModuleName);
		if (ModuleIt == Modules.end())
		{
			return;
		}

		FModuleInfoPtr ModuleInfo = ModuleIt->second;
		ShutdownModule(InModuleName);

		ModuleInfo->Module.reset();
		if (ModuleInfo->Handle)
		{
			FPlatformMisc::FreeLibrary(ModuleInfo->Handle);
			ModuleInfo->Handle = nullptr;
		}

		Modules.erase(ModuleIt);
		DURIN_DEBUG(STR("Module unloaded: {}"), InModuleName.ToString());
	}

	auto FModuleManager::StartProcessingNewlyLoadedObjects() -> void
	{
		// Make sure only called once
		check(bCanProcessNewlyLoadedObjects == false);
		bCanProcessNewlyLoadedObjects = true;
	}

	auto FModuleManager::SetProcessLoadedObjectsCallback(std::function<void()> Callback) -> void
	{
		ProcessLoadedObjectsCallback = std::move(Callback);
	}

	auto FModuleManager::UnloadModulesAtShutdown() -> void
	{
		std::vector<FModuleInfoPtr> ModulesToUnload;

		ModulesToUnload.reserve(Modules.size());
		for (const auto& ModuleInfo : Modules | std::views::values)
		{
			ModulesToUnload.push_back(ModuleInfo);
		}

		std::ranges::sort(ModulesToUnload, [](const FModuleInfoPtr& A, const FModuleInfoPtr& B) {
			return A->LoadOrder > B->LoadOrder;
		});

		for (const auto& ModuleInfo : ModulesToUnload)
		{
			ShutdownModule(ModuleInfo->ModuleName);
		}

		ModulesToUnload.clear();
		Modules.clear();
	}
} // namespace Durin
