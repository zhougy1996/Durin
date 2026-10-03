#pragma once

#include "CoreMinimal.h"

#include "Asset/AssetBuildTaskContext.h"
#include "Physics/PhysicsCookVersion.h"
#include "Physics/CookBodySetupInfo.h"
#include "Collision/CollisionGeometry.h"

namespace Durin
{
	struct FPhysicsCookResult
	{
		FCollisionGeometryRef Simple;
		FCollisionGeometryRef Complex;
	};

	// Immutable source capture. Identity is computed once while taking ownership, never during action formation.
	class FPhysicsCookInput
	{
	public:
		auto GetPositions() const -> const FSharedByteBuffer& { return Positions; }
		auto GetIndices() const -> const FSharedByteBuffer& { return Indices; }
		auto GetIdentity() const -> FXxHash128 { return Identity; }
		auto GetMode() const -> EBodySetupCollisionSourceMode { return Mode; }
		auto GetPolicy() const -> EBodySetupCollisionQueryPolicy { return Policy; }
		auto ShouldPersist() const -> bool { return bPersist; }
	private:
		friend class FPhysicsCookHelper;
		FSharedByteBuffer Positions, Indices;
		FXxHash128 Identity;
		EBodySetupCollisionSourceMode Mode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy Policy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		bool bPersist = true;
	};

	class FPhysicsCookHelper
	{
	public:
		ENGINE_API static auto Capture(FCookBodySetupInfo Info, const FAssetBuildTaskContext& Context = {})
			-> std::expected<FPhysicsCookInput, FPhysicsCookFailure>;
		ENGINE_API static auto CookCaptured(const FPhysicsCookInput& Input, const FAssetBuildTaskContext& Context = {})
			-> std::expected<FPhysicsCookResult, FPhysicsCookFailure>;
		ENGINE_API static auto Cook(const FCookBodySetupInfo& Info, const FAssetBuildTaskContext& Context = {})
			-> std::expected<FPhysicsCookResult, FPhysicsCookFailure>;
	};
}
