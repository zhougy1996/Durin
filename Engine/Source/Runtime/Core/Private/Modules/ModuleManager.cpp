#include "Modules/ModuleManager.h"

#include "DObject/Object.h"

auto FModuleManager::Get() -> FModuleManager&
{
	static FModuleManager Instance;
	return Instance;
}

auto FModuleManager::AddModule(const FName& InModuleName, const FString& FileName) -> void
{
	auto ModuleInfoPtr = std::make_shared<FModuleInfo>();
	ModuleInfoPtr->ModuleName = InModuleName;
	ModuleInfoPtr->FileName = FileName;
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
	DOGE_ERROR(STR("Module {} is not ready when trying to get it."), InModuleName.ToString());

	return nullptr;
}

static constexpr auto GetDogeModuleFileName(const FName& InModuleName) -> FString
{
	return FString(FPlatformMisc::FLibraryPrefix) + FString(STR("DogeEditor-")) + InModuleName.ToString() + FPlatformMisc::FLibraryExtension;
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
			DOGE_DEBUG(STR("Module {} is already loaded."), InModuleName.ToString());
			return LoadedModule;
		}
	}

	if (FoundModuleInfo == nullptr)
	{
		AddModule(InModuleName, GetDogeModuleFileName(InModuleName));
		FoundModuleInfo = FindModule(InModuleName);
	}
	DOGE_DEBUG(STR("ModuleFileName: {}"), FoundModuleInfo->FileName);
	FModuleHandle ModuleHandle = FPlatformMisc::LoadLibrary(FoundModuleInfo->FileName);
	if (!ModuleHandle)
	{
		DOGE_ERROR(STR("Failed to load module \"{}\"."), InModuleName.ToString());
		return nullptr;
	}
	IModuleInterface* Result = nullptr;

	// InitializeModule is defined by the macro IMPLEMENT_MODULE in the module's source file.
	InitializeModuleFunc InitializeModuleFunctionPtr = (InitializeModuleFunc)FPlatformMisc::GetProcAddress(ModuleHandle, "InitializeModule");

	if (!InitializeModuleFunctionPtr)
	{
		DOGE_ERROR(STR("Failed to get module interface from module."));
		FPlatformMisc::FreeLibrary(ModuleHandle);
		return nullptr;
	}

	FoundModuleInfo->Handle = ModuleHandle;
	Result = InitializeModuleFunctionPtr();
	FoundModuleInfo->Module = TUniquePtr<IModuleInterface>(Result);
	DOGE_INFO(STR("Module loaded: {}"), InModuleName.ToString());

	ProcessNewlyLoadedDObjects();

	// Call the module's startup function.
	Result->StartupModule();
	FoundModuleInfo->bIsReady = true;

	return Result;
}

auto FModuleManager::UnloadModule(const FName& InModuleName) -> void
{
	DOGE_INFO(STR("Module Unloaded: {}"), InModuleName.ToString());
}
