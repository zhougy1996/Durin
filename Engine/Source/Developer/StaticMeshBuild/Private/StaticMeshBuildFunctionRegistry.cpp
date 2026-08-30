#include "StaticMeshBuildFunctionRegistry.h"

#include "StaticMesh/StaticMeshBuildFunctions.h"

namespace Durin::Asset
{
	namespace
	{
		std::mutex GStaticMeshBuildFunctionMutex;
		FBuildFunctionRegistration GStaticMeshRegistration;
		FBuildFunctionRegistration GStaticMeshCollisionRegistration;
	}

	auto EnsureStaticMeshBuildFunctions(
		std::string* OutError, FModuleOwnedCallbackGate Gate) -> bool
	{
		std::lock_guard Lock(GStaticMeshBuildFunctionMutex);
		if (GStaticMeshRegistration.IsValid()
			&& GStaticMeshCollisionRegistration.IsValid()) return true;

		const bool bAcquiredRender = !GStaticMeshRegistration.IsValid();
		const bool bAcquiredCollision = !GStaticMeshCollisionRegistration.IsValid();
		auto RollBack = [&] {
			if (bAcquiredCollision) GStaticMeshCollisionRegistration.Reset();
			if (bAcquiredRender) GStaticMeshRegistration.Reset();
		};
		if (bAcquiredRender)
		{
			GStaticMeshRegistration = RegisterBuildFunction(
				Private::StaticMeshFunctionName,
				Private::CreateStaticMeshBuildFunction(), Gate, OutError);
			if (!GStaticMeshRegistration.IsValid()) return false;
		}
		if (bAcquiredCollision)
		{
			GStaticMeshCollisionRegistration = RegisterBuildFunction(
				Private::StaticMeshCollisionFunctionName,
				Private::CreateStaticMeshCollisionBuildFunction(), Gate, OutError);
			if (!GStaticMeshCollisionRegistration.IsValid())
			{
				RollBack();
				return false;
			}
		}
		if (OutError) OutError->clear();
		return true;
	}

	auto InitializeStaticMeshBuildFunctions(
		FModuleOwnedCallbackGate Gate, std::string* OutError) -> bool
	{
		return EnsureStaticMeshBuildFunctions(OutError, std::move(Gate));
	}

	auto ShutdownStaticMeshBuildFunctions() -> void
	{
		std::lock_guard Lock(GStaticMeshBuildFunctionMutex);
		GStaticMeshCollisionRegistration.Reset();
		GStaticMeshRegistration.Reset();
	}
}
