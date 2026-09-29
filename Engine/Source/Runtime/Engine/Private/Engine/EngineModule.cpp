#include "Modules/ModuleManager.h"
#include "Collision/CollisionDebugSubsystem.h"
#include "Materials/MaterialParameterCollection.h"

namespace Durin
{
	// Publishes native Engine services before any host initializes a World.
	class FEngineModule : public IModuleInterface
	{
	public:
		auto SupportsDynamicReloading() const -> bool override { return false; }
		auto StartupModule() -> void override
		{
			CollisionDebug = std::make_unique<FWorldSubsystemRegistration>(FWorldSubsystemDescriptor{
				.Type = DCollisionDebugSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
			MaterialParameterCollections = std::make_unique<FWorldSubsystemRegistration>(FWorldSubsystemDescriptor{
				.Type = DMaterialParameterCollectionSubsystem::StaticClass(),
				.Provider = FModuleStartup::GetModuleName()});
		}
		auto ShutdownModule() -> void override
		{
			MaterialParameterCollections.reset();
			CollisionDebug.reset();
		}
	private:
		std::unique_ptr<FWorldSubsystemRegistration> CollisionDebug;
		std::unique_ptr<FWorldSubsystemRegistration> MaterialParameterCollections;
	};
	IMPLEMENT_MODULE(FEngineModule, Engine)
}
