#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "Physics/PhysicsTypes.h"

#include "CollisionTypes.gen.h"

namespace Durin
{
	class AActor;
	class DPrimitiveComponent;

	// Selects whether a component publishes query and future simulation state.
	DENUM()
	enum class ECollisionEnabled : uint8
	{
		NoCollision,
		QueryOnly,
		QueryAndPhysics
	};

	// Stable built-in collision channels used by Engine and gameplay queries.
	DENUM()
	enum class ECollisionChannel : uint8
	{
		WorldStatic,
		WorldDynamic,
		Pawn,
		Visibility,
		Camera
	};

	// Resolves two-sided query participation for one channel pair.
	DENUM()
	enum class ECollisionResponse : uint8
	{
		Ignore,
		Overlap,
		Block
	};

	// Stores one response for every low-level physics channel.
	struct FCollisionResponseContainer
	{
		std::array<ECollisionResponse, MaximumPhysicsChannels> Responses{};

		ENGINE_API FCollisionResponseContainer();
		ENGINE_API explicit FCollisionResponseContainer(ECollisionResponse DefaultResponse);
		ENGINE_API auto SetResponse(ECollisionChannel Channel, ECollisionResponse Response) -> void;
		auto GetResponse(ECollisionChannel Channel) const -> ECollisionResponse
		{
			return Responses[static_cast<uint8>(Channel)];
		}
	};

	// Selects actors and components that must be excluded from one synchronous query.
	struct FCollisionQueryParams
	{
		std::vector<const AActor*> IgnoredActors;
		std::vector<const DPrimitiveComponent*> IgnoredComponents;

		ENGINE_API auto AddIgnoredActor(const AActor* Actor) -> void;
		ENGINE_API auto AddIgnoredComponent(const DPrimitiveComponent* Component) -> void;
	};

	// Supplies the querying side of two-sided channel response resolution.
	struct FCollisionResponseParams
	{
		FCollisionResponseContainer CollisionResponse;
	};

	// Selects body object channels for overlap-style query variants.
	struct FCollisionObjectQueryParams
	{
		std::array<bool, MaximumPhysicsChannels> ObjectChannels{};

		ENGINE_API auto AddObjectType(ECollisionChannel Channel) -> void;
	};

	// Gameplay-facing blocking hit mapped from one opaque Aether result.
	struct FHitResult
	{
		bool bBlockingHit = false;
		bool bStartPenetrating = false;
		double Time = 1.0;
		double Distance = 0.0;
		FVector3 Location{0.0};
		FVector3 ImpactPoint{0.0};
		FVector3 ImpactNormal{0.0};
		double PenetrationDepth = 0.0;
		AActor* Actor = nullptr;
		DPrimitiveComponent* Component = nullptr;

		auto Reset() -> void { *this = {}; }
	};

	// Gameplay-facing overlap mapped to its owning Actor and primitive component.
	struct FOverlapResult
	{
		AActor* Actor = nullptr;
		DPrimitiveComponent* Component = nullptr;
		bool bBlockingHit = false;
	};

	// Inspectable collision debug data produced only while World collision debugging is enabled.
	struct FCollisionDebugBody
	{
		FPhysicsActorHandle Handle;
		FCollisionShape Shape;
		bool bHasPrimitiveShape = false;
		ECollisionGeometryKind GeometryKind = ECollisionGeometryKind::Primitive;
		uint64 ResourceIdentity = 0;
		uint64 RetainedBytes = 0;
		FTransform Transform;
		FVector3 LocalBoundsMinimum{0.0};
		FVector3 LocalBoundsMaximum{0.0};
		uint32 TotalTriangles = 0;
		uint32 HeightFieldWidth = 0;
		uint32 HeightFieldHeight = 0;
		uint32 HeightFieldNodes = 0;
		uint32 HeightFieldRegions = 0;
		std::vector<std::array<FVector3, 2>> HeightFieldNodeBoundsSample;
		std::vector<std::array<FVector3, 3>> TriangleSample;
		ECollisionChannel ObjectChannel = ECollisionChannel::WorldDynamic;
		AActor* Actor = nullptr;
		DPrimitiveComponent* Component = nullptr;
	};

	struct FCollisionDebugSnapshot
	{
		std::vector<FCollisionDebugBody> Bodies;
		std::optional<FHitResult> LastBlockingHit;
	};

	namespace CollisionProfile
	{
		inline const FName NoCollision{"NoCollision"};
		inline const FName BlockAll{"BlockAll"};
		inline const FName WorldStatic{"WorldStatic"};
		inline const FName Pawn{"Pawn"};
		inline const FName Trigger{"Trigger"};

		struct FProfile
		{
			ECollisionEnabled Enabled = ECollisionEnabled::NoCollision;
			ECollisionChannel ObjectChannel = ECollisionChannel::WorldDynamic;
			FCollisionResponseContainer Responses;
		};

		ENGINE_API auto Resolve(FName ProfileName, FProfile& OutProfile) -> bool;
	}

	ENGINE_API auto ToPhysicsResponse(ECollisionResponse Response) -> EPhysicsQueryResponse;
}
