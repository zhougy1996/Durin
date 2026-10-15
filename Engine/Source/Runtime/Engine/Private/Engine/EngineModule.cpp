#include "Modules/ModuleManager.h"
#if DURIN_WITH_EDITOR
#include "Asset/CookRegistry.h"
#endif
#include "Collision/CollisionDebugSubsystem.h"
#include "Materials/MaterialParameterCollection.h"
#if DURIN_WITH_EDITOR
#include "Physics/PhysicsBuildFunction.h"
#include "Materials/MaterialTranslation.h"
#include "Materials/MaterialProgramCache.h"
#endif

namespace Durin
{
	// Publishes native Engine services before any host initializes a World.
	class FEngineModule : public IModuleInterface
	{
	public:
		auto SupportsDynamicReloading() const -> bool override { return false; }
		auto StartupModule() -> void override
		{
#if DURIN_WITH_EDITOR
			MaterialBuildPrivate::OpenTranslationSessions();
#endif
#if DURIN_WITH_EDITOR
			CookRegistrations = std::make_unique<AssetPrivate::FEngineCookRegistrationScope>();
#endif
			CollisionDebug = std::make_unique<FWorldSubsystemRegistration>(FWorldSubsystemDescriptor{
				.Type = DCollisionDebugSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
			MaterialParameterCollections = std::make_unique<FWorldSubsystemRegistration>(FWorldSubsystemDescriptor{
				.Type = DMaterialParameterCollectionSubsystem::StaticClass(),
				.Provider = FModuleStartup::GetModuleName()});
		}
		auto ShutdownModule() -> void override
		{
#if DURIN_WITH_EDITOR
			PhysicsPrivate::ShutdownBuildSessions();
			MaterialBuildPrivate::CloseTranslationSessions();
			MaterialBuildPrivate::ClearPrograms();
#endif
#if DURIN_WITH_EDITOR
			CookRegistrations.reset();
#endif
			MaterialParameterCollections.reset();
			CollisionDebug.reset();
		}
	private:
#if DURIN_WITH_EDITOR
		std::unique_ptr<AssetPrivate::FEngineCookRegistrationScope> CookRegistrations;
#endif
		std::unique_ptr<FWorldSubsystemRegistration> CollisionDebug;
		std::unique_ptr<FWorldSubsystemRegistration> MaterialParameterCollections;
	};
	IMPLEMENT_MODULE(FEngineModule, Engine)
}
