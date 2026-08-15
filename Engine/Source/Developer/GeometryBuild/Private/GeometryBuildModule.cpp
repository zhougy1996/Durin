#include "GeometryBuildFunctionRegistry.h"
#include "Modules/ModuleManager.h"
#include "Skeletal/SkeletalBuildOperations.h"
#include "SkeletalMesh/SkeletalAssetPostLoad.h"
#include "StaticMesh/StaticMeshAuthoring.h"
#include "StaticMesh/StaticMeshBuildOperations.h"

namespace Durin
{
	class FGeometryBuildModule final
		: public IModuleInterface
		, public IStaticMeshCollisionBuildFeature
		, public ISkeletalDerivedDataFeature
	{
		// Module teardown owns this token explicitly. Keeping the module object trivially
		// destructible avoids cross-DLL static-destruction calls when a test process exits
		// without invoking FModuleManager::UnloadModulesAtShutdown().
		FModularFeatureRegistration CollisionFeatureRegistration;
		FModularFeatureRegistration SkeletalFeatureRegistration;
		FModuleOwnedCallbackRegistration BuildFunctionCallbackRegistration;

		auto BuildCollisionProduct(
			const FStaticMeshRenderData& RenderData,
			const FStaticMeshSourceImportData& SourceImportData,
			EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy,
			FStaticMeshCollisionAuthoringProduct& OutProduct,
			std::string& OutError) -> bool override
		{
			return Asset::Build::FStaticMeshBuildOperations::BuildCollisionProduct(
				RenderData, SourceImportData, Mode, Policy, OutProduct, OutError);
		}

		auto LoadSkeletalMeshPayload(
			std::string_view Key,
			const FSkeletalPayloadSerializationContext& Context,
			FSkeletalMeshPayloadData& OutPayload,
			std::string& OutMessage) -> bool override
		{
			return Asset::Build::LoadSkeletalMeshDerivedData(Key, Context, OutPayload, OutMessage);
		}

		auto LoadAnimationClipPayload(
			std::string_view Key,
			const FSkeletalPayloadSerializationContext& Context,
			FAnimationClipPayloadData& OutPayload,
			std::string& OutMessage) -> bool override
		{
			return Asset::Build::LoadAnimationClipDerivedData(Key, Context, OutPayload, OutMessage);
		}

		auto StartupModule() -> void override
		{
			std::string Error;
			BuildFunctionCallbackRegistration =
				FModuleStartup::CreateOwnedCallbackRegistration("GeometryBuild.BuildFunctions");
			checkf(Asset::Build::InitializeGeometryBuildFunctions(
				BuildFunctionCallbackRegistration.GetGate(), &Error),
				"GeometryBuild could not register its build functions: {}", Error);
			CollisionFeatureRegistration = FModuleStartup::RegisterFeature<IStaticMeshCollisionBuildFeature>(*this);
			SkeletalFeatureRegistration = FModuleStartup::RegisterFeature<ISkeletalDerivedDataFeature>(*this);
			checkf(CollisionFeatureRegistration.IsValid(),
				"GeometryBuild could not register StaticMesh collision building.");
			checkf(SkeletalFeatureRegistration.IsValid(),
				"GeometryBuild could not register skeletal DDC loading.");
		}

		auto ShutdownModule() -> void override
		{
			Asset::Build::ShutdownGeometryBuildFunctions();
		}
	};

	IMPLEMENT_MODULE(FGeometryBuildModule, GeometryBuild)
}
