#include "Modules/ModuleManager.h"

auto FModuleManager::Get() -> FModuleManager&
{
	static FModuleManager Instance;
	return Instance;
}

auto FModuleManager::AddModule(const FName& InModuleName, const FString& FileName) -> void
{
	auto ModuleInfoPtr = std::make_shared<FModuleInfo>();
	ModuleInfoPtr->ModuleName_ = InModuleName;
	ModuleInfoPtr->Filename_ = FileName;
	Modules_.emplace(InModuleName, ModuleInfoPtr);
}

auto FModuleManager::FindModule(const FName& InModuleName) -> FModuleInfoPtr
{
	auto Iter = Modules_.find(InModuleName);
	if (Iter == Modules_.end())
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
	if (ModuleInfo->Module_ != nullptr)
	{
		return true;
	}

	return false;
}

auto FModuleManager::LoadModuleChecked(const FName& InModuleName) -> IModuleInterface&
{
	auto Module = LoadModule(InModuleName);

	// TODO: assert if module is null
	if (Module == nullptr)
	{
		DOGE_ERROR("Failed to load module: {}", InModuleName);
	}

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
		return ModuleInfo->Module_.get();
	}

	// If the module is not ready, it is not loaded.
	DOGE_ERROR("Module {} is not ready when trying to get it.", InModuleName);

	return nullptr;
}

auto FModuleManager::LoadModule(const FName& InModuleName) -> IModuleInterface*
{
	IModuleInterface* LoadedModule = nullptr;
	FModuleInfoPtr FoundModuleInfo = FindModule(InModuleName);

	if (FoundModuleInfo)
	{
		LoadedModule = FoundModuleInfo->Module_.get();
		if (LoadedModule)
		{
			DOGE_DEBUG("Module {} is already loaded.", InModuleName);
			return LoadedModule;
		}
	}

	if (FoundModuleInfo == nullptr)
	{
		AddModule(InModuleName, InModuleName + ".dll");
		FoundModuleInfo = FindModule(InModuleName);
	}

	const auto& Filename = FoundModuleInfo->Filename_;
	std::wstring DllPath(Filename.begin(), Filename.end());
	HMODULE ModuleHandle = LoadLibrary(DllPath.c_str());
	if (!ModuleHandle)
	{
		DOGE_ERROR("Failed to load module.");
		return nullptr;
	}
	IModuleInterface* Result = nullptr;

	// InitializeModule is defined by the macro IMPLEMENT_MODULE in the module's source file.
	InitializeModuleFunc InitializeModuleFunctionPtr = (InitializeModuleFunc)GetProcAddress(ModuleHandle, "InitializeModule");

	if (!InitializeModuleFunctionPtr)
	{
		DOGE_ERROR("Failed to get module interface from module.");
		FreeLibrary(ModuleHandle);
		return nullptr;
	}

	FoundModuleInfo->Handle_ = ModuleHandle;
	Result = InitializeModuleFunctionPtr();
	FoundModuleInfo->Module_ = TUniquePtr<IModuleInterface>(Result);
	DOGE_INFO("Module loaded: {}", InModuleName);

	// Call the module's startup function.
	Result->StartupModule();
	FoundModuleInfo->bIsReady = true;

	return Result;
}

auto FModuleManager::UnloadModule(const FName& InModuleName) -> void
{
	DOGE_INFO("Module Unloaded: {}", InModuleName);
}
