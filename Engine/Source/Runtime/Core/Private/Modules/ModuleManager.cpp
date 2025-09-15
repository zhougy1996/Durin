#include "Modules/ModuleManager.h"

#include "DObject/Object.h"

auto FModuleManager::Get() -> FModuleManager&
{
	static FModuleManager Instance;
	return Instance;
}

auto FModuleManager::AddModule(const FStringName& InModuleName, const FString& FileName) -> void
{
	auto ModuleInfoPtr = std::make_shared<FModuleInfo>();
	ModuleInfoPtr->ModuleName = InModuleName;
	ModuleInfoPtr->FileName = FileName;
	Modules.emplace(InModuleName, ModuleInfoPtr);
}

auto FModuleManager::FindModule(const FStringName& InModuleName) -> FModuleInfoPtr
{
	auto Iter = Modules.find(InModuleName);
	if (Iter == Modules.end())
	{
		return nullptr;
	}

	return Iter->second;
}

auto FModuleManager::IsModuleLoaded(const FStringName& InModuleName) -> bool
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

auto FModuleManager::LoadModuleChecked(const FStringName& InModuleName) -> IModuleInterface&
{
	auto Module = LoadModule(InModuleName);

	// TODO: assert if module is null
	if (Module == nullptr)
	{
		DOGE_ERROR(STR("Failed to load module: {}"), InModuleName);
	}

	return *Module;
}

auto FModuleManager::GetModule(const FStringName& InModuleName) -> IModuleInterface*
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
	DOGE_ERROR(STR("Module {} is not ready when trying to get it."), InModuleName);

	return nullptr;
}

static constexpr auto GetDogeModuleFileName(const FStringName& InModuleName) -> FString
{
	return FString(STR("DogeEditor-")) + InModuleName + STR(".dll");
}

auto FModuleManager::LoadModule(const FStringName& InModuleName) -> IModuleInterface*
{
	IModuleInterface* LoadedModule = nullptr;
	FModuleInfoPtr FoundModuleInfo = FindModule(InModuleName);

	if (FoundModuleInfo)
	{
		LoadedModule = FoundModuleInfo->Module.get();
		if (LoadedModule)
		{
			DOGE_DEBUG(STR("Module {} is already loaded."), InModuleName);
			return LoadedModule;
		}
	}

	if (FoundModuleInfo == nullptr)
	{
		AddModule(InModuleName, GetDogeModuleFileName(InModuleName));
		FoundModuleInfo = FindModule(InModuleName);
	}

	const FString& FileName = FoundModuleInfo->FileName;
	std::wstring DLLPath(FileName.begin(), FileName.end());
	HMODULE ModuleHandle = LoadLibrary(DLLPath.c_str());
	if (!ModuleHandle)
	{
		DOGE_ERROR(STR("Failed to load module."));
		return nullptr;
	}
	IModuleInterface* Result = nullptr;

	// InitializeModule is defined by the macro IMPLEMENT_MODULE in the module's source file.
	InitializeModuleFunc InitializeModuleFunctionPtr = (InitializeModuleFunc)GetProcAddress(ModuleHandle, "InitializeModule");

	if (!InitializeModuleFunctionPtr)
	{
		DOGE_ERROR(STR("Failed to get module interface from module."));
		FreeLibrary(ModuleHandle);
		return nullptr;
	}

	FoundModuleInfo->Handle = ModuleHandle;
	Result = InitializeModuleFunctionPtr();
	FoundModuleInfo->Module = TUniquePtr<IModuleInterface>(Result);
	DOGE_INFO(STR("Module loaded: {}"), InModuleName);

	ProcessNewlyLoadedDObjects();

	// Call the module's startup function.
	Result->StartupModule();
	FoundModuleInfo->bIsReady = true;

	return Result;
}

auto FModuleManager::UnloadModule(const FStringName& InModuleName) -> void
{
	DOGE_INFO(STR("Module Unloaded: {}"), InModuleName);
}
