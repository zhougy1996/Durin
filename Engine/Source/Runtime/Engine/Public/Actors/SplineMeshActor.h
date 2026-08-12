#pragma once

#include "Engine/Actor.h"
#include "Spline/SplineTypes.h"

#include "SplineMeshActor.gen.h"

namespace Durin
{
	class DMaterialInterface;
	class DSplineComponent;
	class DStaticMesh;

	DCLASS(DisplayName = "Spline Mesh Actor", DefaultObjectName = "SplineMeshActor")
	class ASplineMeshActor : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit ASplineMeshActor(const FObjectInitializer& ObjectInitializer);
		auto GetSplineComponent() const -> DSplineComponent* { return SplineComponent.Get(); }
		auto GetPathMesh() const -> DStaticMesh* { return PathMesh.Get(); }
		ENGINE_API auto SetPathMesh(DStaticMesh* Mesh) -> void;
		ENGINE_API auto SetPathMaterial(DMaterialInterface* Material) -> void;
		ENGINE_API auto SetPathCollisionEnabled(bool bEnabled) -> void;
		ENGINE_API auto SetPathVisible(bool bVisible) -> void;
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	protected:
		ENGINE_API auto OnNativeConstruct(FActorConstructionContext& Context,
			std::string& OutError) -> bool override;

	private:
		DPROPERTY()
		TObjectPtr<DSplineComponent> SplineComponent;

		DPROPERTY(Edit)
		TObjectPtr<DStaticMesh> PathMesh;

		DPROPERTY(Edit)
		TObjectPtr<DMaterialInterface> PathMaterial;

		DPROPERTY(Edit)
		ESplineMeshAxis ForwardAxis = ESplineMeshAxis::X;

		DPROPERTY(Edit)
		FVector2 PathScale{1.0};

		DPROPERTY(Edit)
		double PathRollRadians = 0.0;

		DPROPERTY(Edit)
		FVector2 PathOffset{0.0};

		DPROPERTY(Edit)
		FVector3 FrameUpDirection{0.0, 0.0, 1.0};

		DPROPERTY(Edit)
		ESplineMeshInterpolation PathInterpolation = ESplineMeshInterpolation::Linear;

		DPROPERTY(Edit)
		bool bEnablePathCollision = false;

		DPROPERTY(Edit)
		bool bPathVisible = true;

		uint64 SplineMutationListenerId = 0;
	};
}
