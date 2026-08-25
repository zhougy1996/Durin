#include "SkeletalBuildFunctionRegistry.h"
#include "Modules/ModuleManager.h"
#include "Skeletal/SkeletalBuildOperations.h"
#include "SkeletalMesh/SkeletalAssetPostLoad.h"

namespace Durin
{
	// Owns skeletal build-function and uncooked-payload provider registration.
	class FSkeletalBuildModule final
		: public IModuleInterface
		, public ISkeletalDerivedDataFeature
	{
		FModularFeatureRegistration SkeletalFeatureRegistration;
		FModuleOwnedCallbackRegistration BuildFunctionCallbackRegistration;

		auto LoadSkeletalMeshPayload(
			std::string_view Key,
			const FSkeletalPayloadSerializationContext& Context,
			FSkeletalMeshPayloadData& OutPayload,
			std::string& OutMessage) -> bool override
		{
			return Asset::Build::LoadSkeletalMeshDerivedData(
				Key, Context, OutPayload, OutMessage);
		}

		auto LoadAnimationClipPayload(
			std::string_view Key,
			const FSkeletalPayloadSerializationContext& Context,
			FAnimationClipPayloadData& OutPayload,
			std::string& OutMessage) -> bool override
		{
			return Asset::Build::LoadAnimationClipDerivedData(
				Key, Context, OutPayload, OutMessage);
		}

		auto StartupModule() -> void override
		{
			std::string Error;
			BuildFunctionCallbackRegistration =
				FModuleStartup::CreateOwnedCallbackRegistration(
					"SkeletalBuild.BuildFunctions");
			checkf(Asset::Build::InitializeSkeletalBuildFunctions(
				BuildFunctionCallbackRegistration.GetGate(), &Error),
				"SkeletalBuild could not register its build functions: {}", Error);
			SkeletalFeatureRegistration =
				FModuleStartup::RegisterFeature<ISkeletalDerivedDataFeature>(*this);
			checkf(SkeletalFeatureRegistration.IsValid(),
				"SkeletalBuild could not register skeletal DDC loading.");
		}

		auto ShutdownModule() -> void override
		{
			Asset::Build::ShutdownSkeletalBuildFunctions();
		}
	};

	IMPLEMENT_MODULE(FSkeletalBuildModule, SkeletalBuild)
}
