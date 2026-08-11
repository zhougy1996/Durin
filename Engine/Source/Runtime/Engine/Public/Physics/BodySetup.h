#pragma once

#include "EngineAPI.h"
#include "CookedAsset.h"
#include "DObject/CoreDObject.h"
#include "Physics/PhysicsTypes.h"

#include "BodySetup.gen.h"

namespace Durin
{
	// Identifies the authored simple geometry published by a body setup.
	DENUM()
	enum class EBodySetupShapeType : uint8
	{
		None,
		Box,
		Sphere,
		Capsule
	};

	DENUM()
	enum class EBodySetupCollisionSourceMode : uint8
	{
		None = 0,
		ConvexHullFromLOD0 = 1,
		TriangleMeshFromLOD0 = 2
	};

	DENUM()
	enum class EBodySetupCollisionQueryPolicy : uint8
	{
		SimpleOnly,
		ComplexOnly,
		SimpleAndComplex
	};

	DENUM()
	enum class EBodySetupCollisionBuildStatus : uint8
	{
		None,
		Ready,
		CacheHit,
		Rebuilt,
		SourceUnavailable,
		Failed,
		CookedLoaded
	};

	// Owns reusable asset collision independently from render data and component transforms.
	DCLASS()
	class DBodySetup : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DBodySetup(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto SetBox(const FVector3& HalfExtent, const FVector3& Center = FVector3(0.0)) -> bool;
		ENGINE_API auto SetSphere(double Radius, const FVector3& Center = FVector3(0.0)) -> bool;
		ENGINE_API auto SetCapsule(double Radius, double HalfHeight, const FVector3& Center = FVector3(0.0)) -> bool;
		ENGINE_API auto BuildShape(FCollisionShape& OutShape, FTransform& OutLocalTransform) const -> bool;
		ENGINE_API auto BuildGeometry(FCollisionGeometryRef& OutGeometry, FTransform& OutLocalTransform) const -> bool;
		ENGINE_API auto BuildSimpleGeometry(FCollisionGeometryRef& OutGeometry) const -> bool;
		ENGINE_API auto BuildComplexGeometry(FCollisionGeometryRef& OutGeometry) const -> bool;
		ENGINE_API auto SetCollisionSourceMode(EBodySetupCollisionSourceMode Mode) -> bool;
		ENGINE_API auto SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy Policy) -> bool;
		ENGINE_API auto PublishCollisionGeometry(
			const FCollisionGeometryRef& Simple,
			const FCollisionGeometryRef& Complex,
			EBodySetupCollisionBuildStatus Status,
			std::string DerivedDataKey,
			std::string Diagnostic,
			uint64 PayloadBytes = 0) -> bool;
		ENGINE_API auto ClearCollisionGeometry(
			EBodySetupCollisionBuildStatus Status,
			std::string Diagnostic) -> void;
		ENGINE_API auto ExchangeCollisionState(DBodySetup& Other) noexcept -> void;
		ENGINE_API auto SetCookedCollisionPayloadDescriptor(
			const Asset::FCookedPayloadDescriptor& Descriptor) -> void;
		ENGINE_API auto IsValid(std::string* OutDiagnostic = nullptr) const -> bool;
		auto GetRevision() const -> uint64 { return Revision; }
		auto GetShapeType() const -> EBodySetupShapeType { return ShapeType; }
		auto GetCollisionSourceMode() const -> EBodySetupCollisionSourceMode { return CollisionSourceMode; }
		auto GetCollisionQueryPolicy() const -> EBodySetupCollisionQueryPolicy { return CollisionQueryPolicy; }
		auto GetCollisionBuildStatus() const -> EBodySetupCollisionBuildStatus { return CollisionBuildStatus; }
		auto GetCollisionBuildRevision() const -> uint64 { return CollisionBuildRevision; }
		auto GetCollisionDerivedDataKey() const -> const std::string& { return CollisionDerivedDataKey; }
		auto GetCollisionDiagnostic() const -> const std::string& { return CollisionDiagnostic; }
		auto GetCollisionPayloadBytes() const -> uint64 { return CollisionPayloadBytes; }
		auto GetCookedCollisionPayloadDescriptor() const
			-> const Asset::FCookedPayloadDescriptor& { return CookedCollisionPayload; }

	private:
		DPROPERTY()
		EBodySetupShapeType ShapeType = EBodySetupShapeType::None;

		// Box uses XYZ half extents; Sphere uses X radius; Capsule uses X radius and Z half height.
		DPROPERTY(Edit)
		FVector3 Dimensions{0.0};

		DPROPERTY(Edit)
		FVector3 Center{0.0};

		DPROPERTY()
		uint64 Revision = 1;

		DPROPERTY(Edit)
		EBodySetupCollisionSourceMode CollisionSourceMode = EBodySetupCollisionSourceMode::None;

		DPROPERTY(Edit)
		EBodySetupCollisionQueryPolicy CollisionQueryPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;

		DPROPERTY()
		uint64 CollisionBuildRevision = 0;

		DPROPERTY()
		EBodySetupCollisionBuildStatus CollisionBuildStatus = EBodySetupCollisionBuildStatus::None;

		DPROPERTY()
		Asset::FCookedPayloadDescriptor CookedCollisionPayload;

		mutable FCollisionGeometryRef CachedGeometry;
		mutable uint64 CachedGeometryRevision = 0;
		FCollisionGeometryRef CachedSimpleCollision;
		FCollisionGeometryRef CachedComplexCollision;
		std::string CollisionDerivedDataKey;
		std::string CollisionDiagnostic;
		uint64 CollisionPayloadBytes = 0;
	};
}
