#include "SkeletalBuildFunctionRegistry.h"
#include "Modules/ModuleManager.h"
#include "Skeletal/SkeletalBuildOperations.h"
#include "SkeletalMesh/SkeletalAssetPostLoad.h"

namespace Durin
{
	// Owns skeletal build-function and canonical authored-data rebuild registration.
	class FSkeletalBuildModule final
		: public IModuleInterface
		, public ISkeletalDerivedDataFeature
	{
		FModularFeatureRegistration SkeletalFeatureRegistration;
		FModuleOwnedCallbackRegistration BuildFunctionCallbackRegistration;

		auto PostLoadUncooked(
			DSkeletalMesh& Mesh,
			std::string& OutMessage) -> bool override
		{
			return RebuildSkeletalMeshFromImportedData(Mesh, OutMessage);
		}

		auto PostLoadUncooked(
			DAnimationClip& Clip,
			std::string& OutMessage) -> bool override
		{
			return RebuildAnimationClipFromImportedData(Clip, OutMessage);
		}

		auto StartupModule() -> void override
		{
			std::string Error;
			BuildFunctionCallbackRegistration =
				FModuleStartup::CreateOwnedCallbackRegistration(
					"SkeletalBuild.BuildFunctions");
			checkf(InitializeSkeletalBuildFunctions(
				BuildFunctionCallbackRegistration.GetGate(), &Error),
				"SkeletalBuild could not register its build functions: {}", Error);
			SkeletalFeatureRegistration =
				FModuleStartup::RegisterFeature<ISkeletalDerivedDataFeature>(*this);
			checkf(SkeletalFeatureRegistration.IsValid(),
				"SkeletalBuild could not register skeletal DDC loading.");
		}

		auto ShutdownModule() -> void override
		{
			ShutdownSkeletalBuildFunctions();
		}
	};

	IMPLEMENT_MODULE(FSkeletalBuildModule, SkeletalBuild)
}
