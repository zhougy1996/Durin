#include "Modules/ModuleManager.h"
#include "Shader/ShaderCompilingManager.h"
#include "Shader/GlobalShader.h"
#include "RHIGlobals.h"
#include "Collision/CollisionDebugSubsystem.h"
#include "Materials/MaterialParameterCollection.h"
#if DURIN_WITH_EDITOR
#include "Physics/PhysicsBuildFunction.h"
#include "Materials/MaterialTranslation.h"
#include "Materials/MaterialShaderMap.h"
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
			RHIReleaseResourcesHandle = GetRHIReleaseResourcesDelegate().AddLambda([] { GetGlobalShaderMap().ReleaseDeviceResources_RenderThread(); });
#if DURIN_WITH_EDITOR
			MaterialBuildPrivate::OpenTranslationSessions();
#endif
			CollisionDebug = std::make_unique<FWorldSubsystemRegistration>(FWorldSubsystemDescriptor{
				.Type = DCollisionDebugSubsystem::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
			MaterialParameterCollections = std::make_unique<FWorldSubsystemRegistration>(FWorldSubsystemDescriptor{
				.Type = DMaterialParameterCollectionSubsystem::StaticClass(),
				.Provider = FModuleStartup::GetModuleName()});
		}
		auto ShutdownModule() -> void override
		{
			FShaderCompilingManager::Get().Shutdown();
			GetRHIReleaseResourcesDelegate().Remove(RHIReleaseResourcesHandle);
			GetGlobalShaderMap().Shutdown_RenderThread();
#if DURIN_WITH_EDITOR
			PhysicsPrivate::ShutdownBuildSessions();
			MaterialBuildPrivate::CloseTranslationSessions();
			FMaterialShaderMap::ClearCache();
#endif
			MaterialParameterCollections.reset();
			CollisionDebug.reset();
		}
	private:
		FDelegateHandle RHIReleaseResourcesHandle;
		std::unique_ptr<FWorldSubsystemRegistration> CollisionDebug;
		std::unique_ptr<FWorldSubsystemRegistration> MaterialParameterCollections;
	};
	IMPLEMENT_MODULE(FEngineModule, Engine)
}
