#include "Modules/ModuleManager.h"
#include "Collision/CollisionDebugSubsystem.h"

namespace Durin
{
	// Publishes native Engine services before any host initializes a World.
	class FEngineModule : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			CollisionDebug = std::make_unique<FWorldSubsystemRegistration>(FWorldSubsystemDescriptor{
				.Type = DCollisionDebugSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
		}
		auto ShutdownModule() -> void override { CollisionDebug.reset(); }
	private:
		std::unique_ptr<FWorldSubsystemRegistration> CollisionDebug;
	};
	IMPLEMENT_MODULE(FEngineModule, Engine)
}
