#include "SkeletalBuildFunctionRegistry.h"

#include "Skeletal/SkeletalBuildFunctions.h"

namespace Durin
{
	namespace
	{
		std::mutex GSkeletalBuildFunctionMutex;
		FBuildFunctionRegistration GSkeletalMeshRegistration;
		FBuildFunctionRegistration GAnimationClipRegistration;
	}

	auto EnsureSkeletalBuildFunctions(
		std::string* OutError, FModuleOwnedCallbackGate Gate) -> bool
	{
		std::lock_guard Lock(GSkeletalBuildFunctionMutex);
		if (GSkeletalMeshRegistration.IsValid()
			&& GAnimationClipRegistration.IsValid()) return true;

		const bool bAcquiredSkeletalMesh = !GSkeletalMeshRegistration.IsValid();
		const bool bAcquiredAnimationClip = !GAnimationClipRegistration.IsValid();
		auto RollBack = [&] {
			if (bAcquiredAnimationClip) GAnimationClipRegistration.Reset();
			if (bAcquiredSkeletalMesh) GSkeletalMeshRegistration.Reset();
		};
		if (bAcquiredSkeletalMesh)
		{
			GSkeletalMeshRegistration = RegisterBuildFunction(
				AssetPrivate::SkeletalMeshFunctionName,
				AssetPrivate::CreateSkeletalMeshBuildFunction(), Gate, OutError);
			if (!GSkeletalMeshRegistration.IsValid()) return false;
		}
		if (bAcquiredAnimationClip)
		{
			GAnimationClipRegistration = RegisterBuildFunction(
				AssetPrivate::AnimationClipFunctionName,
				AssetPrivate::CreateAnimationClipBuildFunction(), Gate, OutError);
			if (!GAnimationClipRegistration.IsValid())
			{
				RollBack();
				return false;
			}
		}
		if (OutError) OutError->clear();
		return true;
	}

	auto InitializeSkeletalBuildFunctions(
		FModuleOwnedCallbackGate Gate, std::string* OutError) -> bool
	{
		return EnsureSkeletalBuildFunctions(OutError, std::move(Gate));
	}

	auto ShutdownSkeletalBuildFunctions() -> void
	{
		std::lock_guard Lock(GSkeletalBuildFunctionMutex);
		GAnimationClipRegistration.Reset();
		GSkeletalMeshRegistration.Reset();
	}
}
