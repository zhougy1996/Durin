#pragma once

#include "Engine/Actor.h"
#include "DObject/WeakObjectPtr.h"
#include "RoadNet/RoadSurface.h"
#include "RoadNetActor.gen.h"

namespace Durin { class DStaticMesh; }

namespace Durin::RoadNet
{
	// Validates reflected placement and signals changes through the native construction path.
	DCLASS()
	class DRoadSceneRoot : public DSceneComponent
	{
		GENERATED_BODY()
	public:
		ROADWEAVER_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
	protected:
		ROADWEAVER_API auto OnUpdateTransform() -> void override;
	};

	// Owns transient constant-width previews of one asset; source graph editing remains on DRoadNet.
	DCLASS(DisplayName = "Road Net Preview", DefaultObjectName = "RoadNetPreview")
	class ARoadNetActor : public AActor
	{
		GENERATED_BODY()
	public:
		ROADWEAVER_API explicit ARoadNetActor(const FObjectInitializer& ObjectInitializer);
		ROADWEAVER_API auto SetRoadNet(DRoadNet* Asset) -> void;
		ROADWEAVER_API auto SetPreviewMesh(DStaticMesh* Mesh) -> void;
		ROADWEAVER_API auto SetSurface(const FRoadSurface& Value) -> void;
		ROADWEAVER_API auto BeginDestroy() -> void override;
		ROADWEAVER_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		auto GetGenerationState() const -> const std::string& { return GenerationState; }
		auto GetDiagnostic() const -> const std::string& { return Diagnostic; }
		auto GetAlignments() const -> const std::vector<std::shared_ptr<const FRoadAlignment>>& { return Alignments; }

	protected:
		ROADWEAVER_API auto OnNativeConstruct(FActorConstructionContext& Context, std::string& OutError) -> bool override;

	private:
		auto BindAsset() -> void;
		DPROPERTY(Edit)
		TObjectPtr<DRoadNet> RoadNet;

		DPROPERTY(Edit)
		TObjectPtr<DStaticMesh> PreviewMesh;

		DPROPERTY(Edit)
		FRoadSurface Surface;
		FRoadSurface ObservedSurface;

		DPROPERTY(Edit, ReadOnly, Transient)
		std::string GenerationState = "Empty";

		DPROPERTY(Edit, ReadOnly, Transient)
		std::string Diagnostic;

		TWeakObjectPtr<DRoadNet> BoundAsset;
		uint64 ListenerId = 0;
		std::vector<std::shared_ptr<const FRoadAlignment>> Alignments;
	};
}
