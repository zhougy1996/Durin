#include "GeometryBuildFunctionRegistry.h"

#include "Skeletal/SkeletalBuildFunctions.h"
#include "StaticMesh/StaticMeshBuildFunctions.h"

namespace Durin::Asset::Build
{
	namespace
	{
		std::mutex GGeometryBuildFunctionMutex;
		FBuildFunctionRegistration GStaticMeshRegistration;
		FBuildFunctionRegistration GStaticMeshCollisionRegistration;
		FBuildFunctionRegistration GSkeletalMeshRegistration;
		FBuildFunctionRegistration GAnimationClipRegistration;
	}

	auto EnsureGeometryBuildFunctions(
		std::string* OutError, FModuleOwnedCallbackGate Gate) -> bool
	{
		std::lock_guard Lock(GGeometryBuildFunctionMutex);
		if (GStaticMeshRegistration.IsValid()
			&& GStaticMeshCollisionRegistration.IsValid()
			&& GSkeletalMeshRegistration.IsValid()
			&& GAnimationClipRegistration.IsValid()) return true;

		const bool bAcquiredRender = !GStaticMeshRegistration.IsValid();
		const bool bAcquiredCollision = !GStaticMeshCollisionRegistration.IsValid();
		const bool bAcquiredSkeletalMesh = !GSkeletalMeshRegistration.IsValid();
		const bool bAcquiredAnimationClip = !GAnimationClipRegistration.IsValid();
		auto RollBack = [&] {
			if (bAcquiredAnimationClip) GAnimationClipRegistration.Reset();
			if (bAcquiredSkeletalMesh) GSkeletalMeshRegistration.Reset();
			if (bAcquiredCollision) GStaticMeshCollisionRegistration.Reset();
			if (bAcquiredRender) GStaticMeshRegistration.Reset();
		};
		if (bAcquiredRender)
		{
			GStaticMeshRegistration = RegisterBuildFunction(
				Private::StaticMeshFunctionIdentity,
				Private::CreateStaticMeshBuildFunction(), Gate, OutError);
			if (!GStaticMeshRegistration.IsValid()) return false;
		}

		if (bAcquiredCollision)
		{
			GStaticMeshCollisionRegistration = RegisterBuildFunction(
				Private::StaticMeshCollisionFunctionIdentity,
				Private::CreateStaticMeshCollisionBuildFunction(), Gate, OutError);
			if (!GStaticMeshCollisionRegistration.IsValid())
			{
				RollBack();
				return false;
			}
		}

		if (bAcquiredSkeletalMesh)
		{
			GSkeletalMeshRegistration = RegisterBuildFunction(
				Private::SkeletalMeshFunctionIdentity,
				Private::CreateSkeletalMeshBuildFunction(), Gate, OutError);
			if (!GSkeletalMeshRegistration.IsValid())
			{
				RollBack();
				return false;
			}
		}

		if (bAcquiredAnimationClip)
		{
			GAnimationClipRegistration = RegisterBuildFunction(
				Private::AnimationClipFunctionIdentity,
				Private::CreateAnimationClipBuildFunction(), Gate, OutError);
			if (!GAnimationClipRegistration.IsValid())
			{
				RollBack();
				return false;
			}
		}
		if (OutError) OutError->clear();
		return true;
	}

	auto InitializeGeometryBuildFunctions(
		FModuleOwnedCallbackGate Gate, std::string* OutError) -> bool
	{
		return EnsureGeometryBuildFunctions(OutError, std::move(Gate));
	}

	auto ShutdownGeometryBuildFunctions() -> void
	{
		std::lock_guard Lock(GGeometryBuildFunctionMutex);
		GAnimationClipRegistration.Reset();
		GSkeletalMeshRegistration.Reset();
		GStaticMeshCollisionRegistration.Reset();
		GStaticMeshRegistration.Reset();
	}
}
