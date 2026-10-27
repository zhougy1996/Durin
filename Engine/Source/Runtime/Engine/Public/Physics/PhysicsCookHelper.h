#pragma once

#include "CoreMinimal.h"

#include "Asset/AssetBuildTaskContext.h"
#include "Physics/CookBodySetupInfo.h"
#include "Collision/CollisionGeometry.h"
#include "Misc/TargetIdentity.h"

namespace Durin
{
	enum class EPhysicsCookError : uint8 { Failed, Cancelled };
	struct FPhysicsCookError
	{
		EPhysicsCookError Code = EPhysicsCookError::Failed;
		std::string Message;
	};

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

	// Cancellation is an explicit failure outcome; a successful empty product means no collision was requested.
	class FPhysicsCookHelper
	{
	public:
		ENGINE_API static auto Capture(FCookBodySetupInfo Info, const FAssetBuildTaskContext& Context = {},
			EAssetPayloadTargetPlatform TargetPlatform = EAssetPayloadTargetPlatform::Win64)
			-> std::expected<FPhysicsCookInput, FPhysicsCookError>;
		ENGINE_API static auto CookCaptured(const FPhysicsCookInput& Input, const FAssetBuildTaskContext& Context = {})
			-> std::expected<FPhysicsCookResult, FPhysicsCookError>;
		ENGINE_API static auto Cook(const FCookBodySetupInfo& Info, const FAssetBuildTaskContext& Context = {})
			-> std::expected<FPhysicsCookResult, FPhysicsCookError>;
	};
}
