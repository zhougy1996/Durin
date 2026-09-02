#include "Modules/ModuleManager.h"
#include "SkeletalMesh/SkeletalBuildProvider.h"

namespace Durin
{
	// Owns pure skeletal recipe registration and drains admitted calls on retirement.
	class FSkeletalBuildModule final
		: public IModuleInterface
		, public ISkeletalBuildProvider
	{
		FModularFeatureRegistration ProviderRegistration;

		auto GetDescriptor() const -> FSkeletalBuildProviderDescriptor override
		{
			return {
				.SkeletalMeshProducerIdentity = "CanonicalSkeletalMesh",
				.SkeletalMeshProducerVersion = SkeletalMeshImportedDataSchemaVersion,
				.AnimationClipProducerIdentity = "CanonicalAnimationClip",
				.AnimationClipProducerVersion = AnimationClipImportedDataSchemaVersion};
		}

		auto BuildSkeletalMesh(
			const FSkeletalMeshRecipeRequest& Request,
			FSkeletalMeshRecipeProduct& OutProduct,
			std::string& OutError) -> bool override
		{
			OutProduct = {};
			if (Request.ShouldCancel && Request.ShouldCancel())
			{
				OutError = "SkeletalMesh build was canceled.";
				return false;
			}
			if (!Request.Payload || !ValidateSkeletalMeshPayload(*Request.Payload,
				Request.Context.SkeletonBoneCount,
				Request.Context.MaterialSlotCount, OutError)) return false;
			OutProduct.Payload = Request.Payload;
			OutError.clear();
			return true;
		}

		auto BuildAnimationClip(
			const FAnimationClipRecipeRequest& Request,
			FAnimationClipRecipeProduct& OutProduct,
			std::string& OutError) -> bool override
		{
			OutProduct = {};
			if (Request.ShouldCancel && Request.ShouldCancel())
			{
				OutError = "AnimationClip build was canceled.";
				return false;
			}
			if (!Request.Payload || !ValidateAnimationClipPayload(*Request.Payload,
				Request.Context.SkeletonBoneCount, OutError)) return false;
			OutProduct.Payload = Request.Payload;
			OutError.clear();
			return true;
		}

		auto StartupModule() -> void override
		{
			ProviderRegistration =
				FModuleStartup::RegisterFeature<ISkeletalBuildProvider>(*this);
			checkf(ProviderRegistration.IsValid(),
				"SkeletalBuild could not register its typed recipe provider.");
		}

		auto ShutdownModule() -> void override
		{
			ProviderRegistration.Reset();
		}
	};

	IMPLEMENT_MODULE(FSkeletalBuildModule, SkeletalBuild)
}
