#include "AssetBuild/BuildHost.h"
#include "Modules/ModuleManager.h"
#include "Skeletal/SkeletalBuildOperations.h"
#include "SkeletalMesh/SkeletalAssetPostLoad.h"
#include "StaticMesh/StaticMeshAuthoring.h"
#include "StaticMesh/StaticMeshBuildOperations.h"

namespace Durin
{
	class FGeometryBuildModule final : public IModuleInterface
	{
		// Module teardown owns this token explicitly. Keeping the module object trivially
		// destructible avoids cross-DLL static-destruction calls when a test process exits
		// without invoking FModuleManager::UnloadModulesAtShutdown().
		Asset::Build::FBuildServiceRegistration* ServiceRegistration = nullptr;

		static auto BuildStaticMeshCollision(
			const FStaticMeshRenderData& RenderData,
			const FStaticMeshSourceImportData& SourceImportData,
			EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy,
			FStaticMeshCollisionAuthoringProduct& OutProduct,
			std::string& OutError) -> bool
		{
			return Asset::Build::FStaticMeshBuildOperations::BuildCollisionProduct(
				RenderData, SourceImportData, Mode, Policy, OutProduct, OutError);
		}

		auto StartupModule(FModuleContext&) -> void override
		{
			std::string Error;
			auto Registration = Asset::Build::RegisterBuildServiceContribution({
				.Identity = "Durin.GeometryBuild.Recipes",
				.DrainOrder = 50,
				.Start = [] { return true; },
				.StopAdmission = [] {},
				.PumpCompletions = [](uint32) { return 0; },
				.Wait = [](double) { return true; },
				.Drain = [] {},
				.Snapshot = [] { return std::tuple{0u, 0u, 0ull}; }}, &Error);
			checkf(Registration.IsValid(),
				"GeometryBuild could not register its recipe service: {}", Error);
			ServiceRegistration = new Asset::Build::FBuildServiceRegistration(
				std::move(Registration));
			checkf(RegisterStaticMeshCollisionBuildHandler(
				&BuildStaticMeshCollision),
				"GeometryBuild could not register StaticMesh collision building.");
			checkf(RegisterSkeletalAssetUncookedPayloadLoaders(
				Asset::Build::LoadSkeletalMeshDerivedData,
				Asset::Build::LoadAnimationClipDerivedData),
				"GeometryBuild could not register skeletal DDC loading.");
		}

		auto ShutdownModule(FModuleShutdownContext&) -> void override
		{
			UnregisterSkeletalAssetUncookedPayloadLoaders();
			UnregisterStaticMeshCollisionBuildHandler();
			delete std::exchange(ServiceRegistration, nullptr);
		}
	};

	IMPLEMENT_MODULE(FGeometryBuildModule, GeometryBuild)
}
