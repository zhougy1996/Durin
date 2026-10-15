#pragma once

#include "CoreMinimal.h"

#include "Asset/AssetBuildTaskContext.h"
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
		auto GetPolicy() const -> EBodySetupCollisionQueryPolicy { return Policy; }
		auto ShouldPersist() const -> bool { return bPersist; }
		auto GetTargetPlatform() const -> EAssetPayloadTargetPlatform { return TargetPlatform; }
	private:
		friend class FPhysicsCookHelper;
		FSharedByteBuffer Positions, Indices;
		FXxHash128 Identity;
		EBodySetupCollisionSourceMode Mode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy Policy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		bool bPersist = true;
		EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Win64;
	};

	// Check Context.Cancellation before using results. Cancelled calls return empty
	// inputs/products; only genuine failures populate the error channel.
	class FPhysicsCookHelper
	{
	public:
		ENGINE_API static auto Capture(FCookBodySetupInfo Info, const FAssetBuildTaskContext& Context = {},
			EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Win64)
			-> std::expected<FPhysicsCookInput, std::string>;
		ENGINE_API static auto CookCaptured(const FPhysicsCookInput& Input, const FAssetBuildTaskContext& Context = {})
			-> std::expected<FPhysicsCookResult, std::string>;
		ENGINE_API static auto Cook(const FCookBodySetupInfo& Info, const FAssetBuildTaskContext& Context = {})
			-> std::expected<FPhysicsCookResult, std::string>;
	};
}
