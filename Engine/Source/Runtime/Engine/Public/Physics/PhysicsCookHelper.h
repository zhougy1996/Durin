#pragma once

#include "CoreMinimal.h"

#include "Physics/CookBodySetupInfo.h"
#include "Collision/CollisionGeometry.h"
#include "Misc/TargetIdentity.h"

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
		auto ShouldPersist() const -> bool { return bPersist; }
		auto GetTargetPlatform() const -> EAssetPayloadTargetPlatform { return TargetPlatform; }
	private:
		friend class FPhysicsCookHelper;
		FSharedByteBuffer Positions, Indices;
		FXxHash128 Identity;
		EBodySetupCollisionSourceMode Mode = EBodySetupCollisionSourceMode::None;
		bool bPersist = true;
		EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Win64;
	};

	// Synchronous work runs to completion. Admission and result acceptance belong to the caller.
	class FPhysicsCookHelper
	{
	public:
		ENGINE_API static auto Capture(FCookBodySetupInfo Info, uint64 MaximumWorkingSetBytes = UINT64_MAX,
			EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Win64)
			-> std::expected<FPhysicsCookInput, std::string>;
		ENGINE_API static auto CookCaptured(const FPhysicsCookInput& Input, uint64 MaximumWorkingSetBytes = UINT64_MAX)
			-> std::expected<FPhysicsCookResult, std::string>;
		ENGINE_API static auto Cook(const FCookBodySetupInfo& Info, uint64 MaximumWorkingSetBytes = UINT64_MAX)
			-> std::expected<FPhysicsCookResult, std::string>;
	};
}
