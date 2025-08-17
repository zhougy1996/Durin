#pragma once

#include "Templates/SmartPointers.h"

class CORE_API IModuleInterface
{
public:
	virtual ~IModuleInterface() = default;

	virtual void StartupModule() {};
	virtual void ShutdownModule() {};
};

class CORE_API FModuleInfo
{
public:
	FStringName ModuleName_;

	FString Filename_;

	void* Handle_ = nullptr;

	TUniquePtr<IModuleInterface> Module_;

	// This flag is used to check if the module's startup function has been called.
	std::atomic<bool> bIsReady = false;
};

using InitializeModuleFunc = IModuleInterface* (*)();

class CORE_API FModuleManager
{
public:
	using FModuleInfoPtr = TSharedPtr<FModuleInfo>;
	using FModuleMap = std::unordered_map<FStringName, FModuleInfoPtr>;

	static auto Get() -> FModuleManager&;

	template<typename TModuleInterface>
	static auto LoadModule(const FStringName& InModuleName) -> TModuleInterface*
	{
		return static_cast<TModuleInterface*>(FModuleManager::Get().LoadModule(InModuleName));
	}

	template<typename TModuleInterface>
	static auto LoadModuleChecked(const FStringName& InModuleName) -> TModuleInterface&
	{
		IModuleInterface& Module = FModuleManager::Get().LoadModuleChecked(InModuleName);
		return static_cast<TModuleInterface&>(Module);
	}

	auto AddModule(const FStringName& InModuleName, const FString& FileName) -> void;

	auto FindModule(const FStringName& InModuleName) -> FModuleInfoPtr;

	auto LoadModule(const FStringName& InModuleName) -> IModuleInterface*;

	auto LoadModuleChecked(const FStringName& InModuleName) -> IModuleInterface&;

	auto IsModuleLoaded(const FStringName& InModuleName) -> bool;

	auto GetModule(const FStringName& InModuleName) -> IModuleInterface*;

	auto UnloadModule(const FStringName& InModuleName) -> void;

private:
	FModuleMap Modules_;
};

/**
 * A default implementation of IModuleInterface that does nothing at startup or shutdown.
 */
class CORE_API FDefaultModuleImpl : public IModuleInterface
{
};


/**
 * This template function is used to load a module from a DLL.
 * class FDefaultModuleImpl is the default implementation of IModuleInterface.
 * This function will be called by FModuleManager::LoadModule.
 * @return The module interface.
 */

// clang-format off
#define IMPLEMENT_MODULE(ModuleImplClass, ModuleName) \
	extern "C" DLLEXPORT IModuleInterface* InitializeModule() \
	{ \
		return new ModuleImplClass(); \
	} \
// clang-format on