#include "RoadNet/RoadNetActor.h"

#include "Components/SplineMeshComponent.h"
#include "Asset/AssetCompilingManager.h"
#include "DObject/Property.h"
#include "StaticMesh/StaticMesh.h"
#include "Math/Operations.h"

namespace Durin::RoadNet
{
	auto DRoadSceneRoot::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (Proposal.MemberProperty && Proposal.MemberProperty->NamePrivate == FName("RelativeTransform")
			&& Proposal.DraftRootProperty == Proposal.MemberProperty && Proposal.DraftRootContainer)
			return ValidateRoadPlacement(*Proposal.DraftRootProperty->ContainerPtrToValuePtr<FTransform>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex), OutError);
		return true;
	}

	auto DRoadSceneRoot::OnUpdateTransform() -> void
	{
		Super::OnUpdateTransform();
		if (auto* Actor = Cast<ARoadNetActor>(GetOwner()); Actor && !Actor->IsBeingDestroyed())
			Actor->RequestNativeReconstruction();
	}

	ARoadNetActor::ARoadNetActor(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
	{
		SetRootComponent(CreateDefaultComponent<DRoadSceneRoot>("RoadRoot"));
	}

	auto ARoadNetActor::BindAsset() -> void
	{
		if (BoundAsset.Get() == RoadNet.Get()) return;
		if (BoundAsset.Get()) BoundAsset.Get()->RemoveMutationListener(ListenerId);
		BoundAsset = RoadNet.Get();
		ListenerId = RoadNet ? RoadNet->AddMutationListener([Weak = TWeakObjectPtr<ARoadNetActor>(this)] {
			if (auto* Actor = Weak.Get(); Actor && !Actor->IsBeingDestroyed()) Actor->RequestNativeReconstruction();
		}) : 0;
	}

	auto ARoadNetActor::SetRoadNet(DRoadNet* Asset) -> void
	{
		RoadNet = Asset;
		BindAsset();
		MarkPackageDirty();
		RequestNativeReconstruction();
	}

	auto ARoadNetActor::SetPreviewMesh(DStaticMesh* Mesh) -> void
	{
		PreviewMesh = Mesh;
		MarkPackageDirty();
		RequestNativeReconstruction();
	}

	auto ARoadNetActor::BeginDestroy() -> void
	{
		if (BoundAsset.Get()) BoundAsset.Get()->RemoveMutationListener(ListenerId);
		BoundAsset = nullptr;
		ListenerId = 0;
		Alignments.clear();
		Super::BeginDestroy();
	}

	auto ARoadNetActor::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		BindAsset();
		RequestNativeReconstruction();
	}

	auto ARoadNetActor::OnNativeConstruct(FActorConstructionContext& Context, std::string& OutError) -> bool
	{
		BindAsset();
		auto Fail = [&](std::string Message) {
			// Construction rollback only retires new candidates; explicitly discard reused output too.
			for (auto* Component : FindComponentsByClass<DSplineMeshComponent>())
				if (Component->GetCreationMethod() == EComponentCreationMethod::Generated)
					Component->DestroyComponent();
			Alignments.clear();
			Diagnostic = std::move(Message);
			GenerationState = "Error";
			OutError = Diagnostic;
			return false;
		};
		if (!Super::OnNativeConstruct(Context, OutError)) return Fail(OutError);
		if (!RoadNet)
		{
			Alignments.clear();
			GenerationState = "Empty";
			Diagnostic.clear();
			OutError.clear();
			return true;
		}
		if (RoadNet->GetSchemaVersion() != RoadNetSchemaVersion) return Fail("Road asset has an unsupported schema.");
		if (!ValidateRoadPlacement(GetActorTransform(), OutError)) return Fail(OutError);
		if (!PreviewMesh) return Fail("Assign a preview StaticMesh with nonzero X and Y extent.");
		// Package loading submits mesh compilation asynchronously; construction needs its published CPU geometry.
		FAssetCompilingManager::Get().FinishCompilationForObject(*PreviewMesh);
		const auto Loaded = PreviewMesh->EnsureRenderDataLoadedBlocking();
		if (!Loaded.Status.HasCpuData()) return Fail(Loaded.Message);
		const auto Bounds = PreviewMesh->GetLOD0LocalBounds();
		if (!Bounds || Bounds->Max.x - Bounds->Min.x <= 1.e-6 || Bounds->Max.y - Bounds->Min.y <= 1.e-6)
			return Fail("Preview mesh CPU data is unavailable or its X/Y extent is degenerate.");
		std::vector<std::shared_ptr<const FRoadAlignment>> Candidates;
		struct FSpec { FActorGeneratedComponentKey Key; FSplineMeshParams Params; };
		std::vector<FSpec> Specs;
		for (const auto& Road : RoadNet->GetRoads())
		{
			std::shared_ptr<const FRoadAlignment> Alignment;
			if (!FRoadAlignment::Build(Road, RoadNet->GetDefinition().Planet, Alignment, OutError)) return Fail(OutError);
			FRoadSample Start;
			if (!Alignment->Sample(0, Start, OutError)) return Fail(OutError);
			double Left = 0, Right = 0;
			for (const auto& Extent : Start.Lanes)
			{
				Left = std::min(Left, Extent.MinimumMeters);
				Right = std::max(Right, Extent.MaximumMeters);
			}
			for (const auto& Section : Road.LaneSections)
			{
				double SectionLeft = 0, SectionRight = 0;
				for (const auto& Lane : Section.Lanes)
					if (Lane.Index > 0) SectionRight += Lane.WidthMeters; else SectionLeft -= Lane.WidthMeters;
				if (std::abs(SectionLeft - Left) > RoadEndpointTolerance || std::abs(SectionRight - Right) > RoadEndpointTolerance)
					return Fail("P0 preview requires constant left and right road widths across all sections.");
			}
			for (const auto& Interval : Alignment->GetIntervals())
			{
				auto Params = Interval.Params;
				Params.SourceForwardMin = Bounds->Min.x;
				Params.SourceForwardMax = Bounds->Max.x;
				Params.StartScale.x = Params.EndScale.x = (Right - Left) / (Bounds->Max.y - Bounds->Min.y);
				Params.StartOffset.x = Params.EndOffset.x = (Right + Left) * 0.5
					- (Bounds->Max.y + Bounds->Min.y) * 0.5 * Params.StartScale.x;
				FSplineMeshParams Normalized;
				if (!FSplineMeshDeformer::Normalize(Params, Normalized, &OutError)) return Fail(OutError);
				Specs.push_back({{FName(std::format("Road-{}-{}", Road.Id.ToString(), Interval.SubdivisionKey)),
					Interval.SourcePointId}, Normalized});
			}
			Candidates.push_back(std::move(Alignment));
		}
		std::vector<DSplineMeshComponent*> Components;
		for (const auto& Spec : Specs)
		{
			auto* Component = Cast<DSplineMeshComponent>(Context.AcquireGeneratedComponent(Spec.Key,
				DSplineMeshComponent::StaticClass(), FName("RoadInterval")));
			if (!Component) return Fail(Context.GetError());
			Components.push_back(Component);
		}
		for (size_t Index = 0; Index < Specs.size(); ++Index)
		{
			auto* Component = Components[Index];
			Component->SetStaticMesh(PreviewMesh.Get(), false);
			Component->SetSplineMeshParams(Specs[Index].Params, false);
			Component->UpdateMesh();
			const auto State = Component->GetDerivedState();
			if (!State || !State->IsValid())
				return Fail("Road preview generation failed: a spline mesh is unavailable.");
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetVisible(true);
		}
		Alignments = std::move(Candidates);
		GenerationState = "Ready";
		Diagnostic.clear();
		OutError.clear();
		return true;
	}
}
