#pragma once

#include "CoreAPI.h"

#include "Templates/SmartPointers.h"

namespace Durin
{
	// Defines the startup and shutdown hooks implemented by dynamically loaded modules.
	class IModuleInterface
	{
	public:
		virtual ~IModuleInterface() = default;

		virtual void StartupModule() {};
		virtual void ShutdownModule() {};
	};

	// Owns one loaded module instance and the native handle backing it.
	class FModuleInfo
	{
	public:
		FName ModuleName;

		std::string FileName;

		FModuleHandle Handle = nullptr;

		std::unique_ptr<IModuleInterface> Module;

		// This flag is used to check if the module's startup function has been called.
		std::atomic<bool> bIsReady = false;

		// This is used to record the order in which the modules are loaded, so that we can call their shutdown functions in the reverse order of loading.
		uint32 LoadOrder = 0;
	};

	using InitializeModuleFunc = IModuleInterface* (*)();

	// Loads modules on demand and shuts initialized modules down in reverse load order.
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
			IModuleInterface& Module = Get().LoadModuleChecked(InModuleName);
			return static_cast<TModuleInterface&>(Module);
		}

		CORE_API auto AddModule(const FName& InModuleName, const std::string& FileName) -> void;

		CORE_API auto FindModule(const FName& InModuleName) -> FModuleInfoPtr;

		CORE_API auto LoadModule(const FName& InModuleName) -> IModuleInterface*;

		CORE_API auto LoadModuleChecked(const FName& InModuleName) -> IModuleInterface&;

		CORE_API auto IsModuleLoaded(const FName& InModuleName) -> bool;

		CORE_API auto GetModule(const FName& InModuleName) -> IModuleInterface*;

		// Runs a module's shutdown callback without releasing its instance or native library.
		CORE_API auto ShutdownModule(const FName& InModuleName) -> void;

		CORE_API auto UnloadModule(const FName& InModuleName) -> void;

		CORE_API auto StartProcessingNewlyLoadedObjects() -> void;

		CORE_API auto SetProcessLoadedObjectsCallback(std::function<void()> Callback) -> void;
		CORE_API auto SetPreShutdownModuleCallback(
			std::function<bool(FName)> Callback
		) -> void;

		// Does not actually unload the modules, but calls their shutdown functions in the reverse order of loading. The modules will be unloaded when the process exits.
		CORE_API auto UnloadModulesAtShutdown() -> void;

	private:
		FModuleMap Modules;

		bool bCanProcessNewlyLoadedObjects = false;

		std::function<void()> ProcessLoadedObjectsCallback;

		std::function<bool(FName)> PreShutdownModuleCallback;
	};

	/**
	 * A default implementation of IModuleInterface that does nothing at startup or shutdown.
	 */
	class FDefaultModuleImpl : public IModuleInterface
	{
	};
}
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
