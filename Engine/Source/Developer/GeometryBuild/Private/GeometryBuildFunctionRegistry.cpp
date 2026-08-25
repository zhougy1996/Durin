#include "GeometryBuildFunctionRegistry.h"

#include "Skeletal/SkeletalBuildFunctions.h"
#include "StaticMesh/StaticMeshBuildFunctions.h"
#include "Terrain/TerrainHeightmapBuildFunctions.h"
#include "Terrain/TerrainWorldBuildFunctions.h"

namespace Durin::Asset::Build
{
	namespace
	{
		std::mutex GGeometryBuildFunctionMutex;
		FBuildFunctionRegistration GStaticMeshRegistration;
		FBuildFunctionRegistration GStaticMeshCollisionRegistration;
		FBuildFunctionRegistration GSkeletalMeshRegistration;
		FBuildFunctionRegistration GAnimationClipRegistration;
		FBuildFunctionRegistration GTerrainHeightmapRegistration;
		std::array<FBuildFunctionRegistration, 5> GTerrainWorldRegistrations;
	}

	auto EnsureGeometryBuildFunctions(
		std::string* OutError, FModuleOwnedCallbackGate Gate) -> bool
	{
		std::lock_guard Lock(GGeometryBuildFunctionMutex);
		if (GStaticMeshRegistration.IsValid()
			&& GStaticMeshCollisionRegistration.IsValid()
			&& GSkeletalMeshRegistration.IsValid()
			&& GAnimationClipRegistration.IsValid()
			&& GTerrainHeightmapRegistration.IsValid()
			&& std::ranges::all_of(GTerrainWorldRegistrations,
				[](const FBuildFunctionRegistration& Registration) { return Registration.IsValid(); })) return true;

		const bool bAcquiredRender = !GStaticMeshRegistration.IsValid();
		const bool bAcquiredCollision = !GStaticMeshCollisionRegistration.IsValid();
		const bool bAcquiredSkeletalMesh = !GSkeletalMeshRegistration.IsValid();
		const bool bAcquiredAnimationClip = !GAnimationClipRegistration.IsValid();
		const bool bAcquiredTerrain = !GTerrainHeightmapRegistration.IsValid();
		std::array<bool, 5> AcquiredTerrainWorld{};
		for (size_t Index = 0; Index < AcquiredTerrainWorld.size(); ++Index)
			AcquiredTerrainWorld[Index] = !GTerrainWorldRegistrations[Index].IsValid();
		auto RollBack = [&] {
			for (size_t Index = 0; Index < GTerrainWorldRegistrations.size(); ++Index)
				if (AcquiredTerrainWorld[Index]) GTerrainWorldRegistrations[Index].Reset();
			if (bAcquiredTerrain) GTerrainHeightmapRegistration.Reset();
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

		if (bAcquiredTerrain)
		{
			GTerrainHeightmapRegistration = RegisterBuildFunction(
				Private::TerrainHeightmapFunctionIdentity,
				Private::CreateTerrainHeightmapBuildFunction(), Gate, OutError);
			if (!GTerrainHeightmapRegistration.IsValid())
			{
				RollBack();
				return false;
			}
		}
		for (uint8 Value = 1; Value <= 5; ++Value)
		{
			const size_t Index = Value - 1;
			if (!AcquiredTerrainWorld[Index]) continue;
			const auto ProductClass = static_cast<ETerrainTileProductClass>(Value);
			GTerrainWorldRegistrations[Index] = RegisterBuildFunction(
				Private::GetTerrainWorldBuildFunctionIdentity(ProductClass),
				Private::CreateTerrainWorldBuildFunction(ProductClass), Gate, OutError);
			if (!GTerrainWorldRegistrations[Index].IsValid())
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
		for (auto& Registration : GTerrainWorldRegistrations) Registration.Reset();
		GTerrainHeightmapRegistration.Reset();
		GAnimationClipRegistration.Reset();
		GSkeletalMeshRegistration.Reset();
		GStaticMeshCollisionRegistration.Reset();
		GStaticMeshRegistration.Reset();
	}
}
