#include "Components/TerrainComponent.h"

#include "DObject/DurinPropertyTypes.h"
#include "Engine/TerrainSceneProxy.h"
#include "Engine/Level.h"
#include "Materials/DefaultMaterialService.h"
#include "Materials/MaterialInterface.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 TerrainPatchCellCount = 64;
		constexpr uint32 MaximumTerrainRenderSamples = 1025;

		auto IsValidTerrainProperties(
			double SpacingX, double SpacingY, double HeightScale,
			double HeightOffset) -> bool
		{
			return std::isfinite(SpacingX) && SpacingX > 0.0
				&& std::isfinite(SpacingY) && SpacingY > 0.0
				&& std::isfinite(HeightScale) && std::isfinite(HeightOffset);
		}

		auto HeightFromSample(uint16 Sample, double Scale, double Offset) -> double
		{
			return Offset + (static_cast<double>(Sample) / 65535.0) * Scale;
		}

		auto BuildTerrainPatches(
			const FTerrainHeightmapPayload& Payload, double SpacingX,
			double SpacingY, double HeightScale, double HeightOffset,
			std::vector<FTerrainPatchDescriptor>& OutPatches,
			FBox& OutBounds) -> bool
		{
			OutPatches.clear();
			OutBounds.Reset();
			if (!Payload.IsValid() || Payload.Width < 2 || Payload.Height < 2
				|| Payload.Width > MaximumTerrainRenderSamples
				|| Payload.Height > MaximumTerrainRenderSamples) return false;
			const uint32 CellsX = Payload.Width - 1;
			const uint32 CellsY = Payload.Height - 1;
			const uint64 PatchCountX = (CellsX + TerrainPatchCellCount - 1) / TerrainPatchCellCount;
			const uint64 PatchCountY = (CellsY + TerrainPatchCellCount - 1) / TerrainPatchCellCount;
			OutPatches.reserve(static_cast<size_t>(PatchCountX * PatchCountY));
			for (uint32 OriginY = 0; OriginY < CellsY; OriginY += TerrainPatchCellCount)
			{
				for (uint32 OriginX = 0; OriginX < CellsX; OriginX += TerrainPatchCellCount)
				{
					FTerrainPatchDescriptor Patch;
					Patch.OriginX = OriginX;
					Patch.OriginY = OriginY;
					Patch.CellCountX = std::min(TerrainPatchCellCount, CellsX - OriginX);
					Patch.CellCountY = std::min(TerrainPatchCellCount, CellsY - OriginY);
					uint16 Minimum = 0;
					uint16 Maximum = 0;
					if (!Payload.QueryMinMax(OriginX, OriginY,
						OriginX + Patch.CellCountX + 1,
						OriginY + Patch.CellCountY + 1, Minimum, Maximum)) return false;
					const double Z0 = HeightFromSample(Minimum, HeightScale, HeightOffset);
					const double Z1 = HeightFromSample(Maximum, HeightScale, HeightOffset);
					Patch.LocalBounds = FBox(
						FVector3(OriginX * SpacingX, OriginY * SpacingY, std::min(Z0, Z1)),
						FVector3((OriginX + Patch.CellCountX) * SpacingX,
							(OriginY + Patch.CellCountY) * SpacingY, std::max(Z0, Z1)));
					OutBounds.AddPoint(Patch.LocalBounds.Min);
					OutBounds.AddPoint(Patch.LocalBounds.Max);
					OutPatches.push_back(Patch);
				}
			}
			return OutBounds.bIsValid && !OutPatches.empty();
		}
	}

	DTerrainComponent::DTerrainComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DTerrainComponent::SetHeightmap(DTerrainHeightmap* InHeightmap) -> void
	{
		if (Heightmap == InHeightmap) return;
		Heightmap = InHeightmap;
		++CollisionRevision;
		MarkPackageDirty();
		MarkRenderStateDirty();
		RecreatePhysicsState();
	}

	auto DTerrainComponent::SetSampleSpacing(double InSpacingX, double InSpacingY) -> bool
	{
		if (!std::isfinite(InSpacingX) || InSpacingX <= 0.0
			|| !std::isfinite(InSpacingY) || InSpacingY <= 0.0) return false;
		if (SpacingX == InSpacingX && SpacingY == InSpacingY) return true;
		SpacingX = InSpacingX;
		SpacingY = InSpacingY;
		++CollisionRevision;
		MarkPackageDirty();
		MarkRenderStateDirty();
		RecreatePhysicsState();
		return true;
	}

	auto DTerrainComponent::SetHeightRange(double InScale, double InOffset) -> bool
	{
		if (!std::isfinite(InScale) || !std::isfinite(InOffset)) return false;
		if (HeightScale == InScale && HeightOffset == InOffset) return true;
		HeightScale = InScale;
		HeightOffset = InOffset;
		++CollisionRevision;
		MarkPackageDirty();
		MarkRenderStateDirty();
		RecreatePhysicsState();
		return true;
	}

	auto DTerrainComponent::SetMaterial(DMaterialInterface* InMaterial) -> void
	{
		if (Material == InMaterial) return;
		Material = InMaterial;
		++MaterialComponentRevision;
		MarkPackageDirty();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::MaterialBinding);
	}

	auto DTerrainComponent::ValidateProperties(std::string& OutError) const -> bool
	{
		if (!IsValidTerrainProperties(SpacingX, SpacingY, HeightScale, HeightOffset))
		{
			OutError = "Terrain spacing must be finite and positive, and height scale/offset must be finite.";
			return false;
		}
		OutError.clear();
		return true;
	}

	auto DTerrainComponent::PostLoad(std::string& OutError) -> bool
	{
		return Super::PostLoad(OutError) && ValidateProperties(OutError);
	}

	auto DTerrainComponent::PreEditChangeProperty(
		FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (!Proposal.MemberProperty || Proposal.DraftRootProperty != Proposal.MemberProperty
			|| !Proposal.DraftRootContainer) return true;
		const FName Name = Proposal.MemberProperty->NamePrivate;
		if (Name == FName("Heightmap") || Name == FName("Material"))
		{
			if (Proposal.DraftRootProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Object)
			{
				OutError = "Terrain object property metadata is unavailable.";
				return false;
			}
			DObject* Value = static_cast<const FObjectProperty*>(Proposal.DraftRootProperty)
				->GetObjectPropertyValue(Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			if (Name == FName("Heightmap") && Value && !Cast<DTerrainHeightmap>(Value))
			{
				OutError = "Selected asset is not a terrain heightmap.";
				return false;
			}
			if (Name == FName("Material") && Value && !Cast<DMaterialInterface>(Value))
			{
				OutError = "Selected asset is not a material.";
				return false;
			}
			return true;
		}
		if (Name != FName("SpacingX") && Name != FName("SpacingY")
			&& Name != FName("HeightScale") && Name != FName("HeightOffset")) return true;
		const double Value = *Proposal.DraftRootProperty->ContainerPtrToValuePtr<double>(
			Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
		if (!std::isfinite(Value) || ((Name == FName("SpacingX") || Name == FName("SpacingY")) && Value <= 0.0))
		{
			OutError = "Terrain spacing must be finite and positive, and height scale/offset must be finite.";
			return false;
		}
		return true;
	}

	auto DTerrainComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty) return;
		if (Event.MemberProperty->NamePrivate == FName("Material")) ++MaterialComponentRevision;
		if (Event.MemberProperty->NamePrivate == FName("Heightmap")
			|| Event.MemberProperty->NamePrivate == FName("SpacingX")
			|| Event.MemberProperty->NamePrivate == FName("SpacingY")
			|| Event.MemberProperty->NamePrivate == FName("HeightScale")
			|| Event.MemberProperty->NamePrivate == FName("HeightOffset"))
		{
			++CollisionRevision;
			RecreatePhysicsState();
		}
		MarkRenderStateDirty();
	}

	auto DTerrainComponent::CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy>
	{
		std::string Error;
		if (!ValidateProperties(Error))
		{
			RenderStatus = ETerrainRenderStatus::InvalidProperties;
			LastRenderDiagnostic = std::move(Error);
			return nullptr;
		}
		if (Heightmap == nullptr)
		{
			RenderStatus = ETerrainRenderStatus::MissingHeightmap;
			LastRenderDiagnostic = "Terrain rendering requires an assigned heightmap.";
			return nullptr;
		}
		std::shared_ptr<const FTerrainHeightmapPayload> Payload = Heightmap->GetPayload();
		if (!Payload || !Payload->IsValid() || Payload->Width < 2 || Payload->Height < 2)
		{
			RenderStatus = ETerrainRenderStatus::InvalidPayload;
			LastRenderDiagnostic = "Terrain rendering requires a valid heightmap with at least two samples on each axis.";
			return nullptr;
		}
		if (Payload->Width > MaximumTerrainRenderSamples
			|| Payload->Height > MaximumTerrainRenderSamples)
		{
			RenderStatus = ETerrainRenderStatus::ExtentRejected;
			LastRenderDiagnostic = std::format(
				"Terrain heightmap {}x{} exceeds the T1 render ceiling of {}x{} samples.",
				Payload->Width, Payload->Height, MaximumTerrainRenderSamples,
				MaximumTerrainRenderSamples);
			return nullptr;
		}
		std::vector<FTerrainPatchDescriptor> Patches;
		FBox Bounds;
		if (!BuildTerrainPatches(*Payload, SpacingX, SpacingY, HeightScale,
			HeightOffset, Patches, Bounds))
		{
			RenderStatus = ETerrainRenderStatus::InvalidPayload;
			LastRenderDiagnostic = "Terrain patch construction rejected the current payload or bounds.";
			return nullptr;
		}
		DMaterialInterface* AssignedMaterial = Material.Get();
		if (!AssignedMaterial)
			RecordMaterialFallbackReason(EMaterialFallbackReason::UnassignedDefault);
		RenderStatus = ETerrainRenderStatus::Ready;
		LastRenderDiagnostic.clear();
		return std::make_unique<FTerrainSceneProxy>(
			std::move(Payload), Heightmap->GetRevision(), SpacingX, SpacingY,
			HeightScale, HeightOffset, std::move(Patches), Bounds,
			AssignedMaterial ? AssignedMaterial->GetMaterialRenderProxy()
				: GetDefaultMaterialRenderProxy(), MaterialComponentRevision);
	}

	auto DTerrainComponent::SetCollisionFailure(
		ETerrainCollisionStatus Status, std::string Diagnostic) const -> bool
	{
		auto* Mutable = const_cast<DTerrainComponent*>(this);
		Mutable->CollisionStatus = Status;
		Mutable->LastCollisionDiagnostic = std::move(Diagnostic);
		CachedTerrainCollision = {};
		CachedCollisionPayload.reset();
		CachedHeightmapRevision = 0;
		CachedCollisionDiagnostics = {};
		return false;
	}

	auto DTerrainComponent::GetCollisionFacts() const -> FTerrainCollisionFacts
	{
		FTerrainCollisionFacts Facts;
		Facts.Status = CollisionStatus;
		Facts.AssetRevision = Heightmap ? Heightmap->GetRevision() : 0;
		Facts.CollisionRevision = CollisionRevision;
		Facts.ResourceIdentity = CachedTerrainCollision.GetIdentity();
		Facts.RetainedBytes = CachedTerrainCollision.GetRetainedBytes();
		Facts.EstimatedPeakBytes = CachedCollisionDiagnostics.EstimatedPeakBytes;
		Facts.Width = CachedTerrainCollision.GetHeightFieldWidth();
		Facts.Height = CachedTerrainCollision.GetHeightFieldHeight();
		Facts.Cells = Facts.Width > 0 && Facts.Height > 0
			? (Facts.Width - 1) * (Facts.Height - 1) : 0;
		Facts.Nodes = CachedTerrainCollision.GetNodeCount();
		Facts.MaximumDepth = CachedCollisionDiagnostics.MaximumDepth;
		Facts.BuildStatus = CachedCollisionDiagnostics.Status;
		return Facts;
	}

	auto DTerrainComponent::BuildCollisionGeometry(
		FCollisionGeometryRef& OutGeometry, FTransform& OutWorldTransform) const -> bool
	{
		OutGeometry = {};
		std::string Error;
		if (!ValidateProperties(Error))
			return SetCollisionFailure(ETerrainCollisionStatus::InvalidProperties, std::move(Error));
		if (!Heightmap)
			return SetCollisionFailure(ETerrainCollisionStatus::MissingHeightmap,
				"Terrain collision requires an assigned heightmap.");
		const std::shared_ptr<const FTerrainHeightmapPayload> Payload = Heightmap->GetPayload();
		if (!Payload || !Payload->IsValid() || Payload->Width < 2 || Payload->Height < 2)
			return SetCollisionFailure(ETerrainCollisionStatus::InvalidPayload,
				"Terrain collision requires a valid heightmap with at least two samples on each axis.");
		if (Payload->Width > MaximumTerrainRenderSamples || Payload->Height > MaximumTerrainRenderSamples)
			return SetCollisionFailure(ETerrainCollisionStatus::ExtentRejected, std::format(
				"Terrain heightmap {}x{} exceeds the T2 collision ceiling of {}x{} samples.",
				Payload->Width, Payload->Height, MaximumTerrainRenderSamples, MaximumTerrainRenderSamples));
		const uint64 HeightmapRevision = Heightmap->GetRevision();
		const bool bCacheMatches = CachedTerrainCollision.IsValid()
			&& CachedCollisionPayload == Payload && CachedHeightmapRevision == HeightmapRevision
			&& CachedCollisionSpacingX == SpacingX && CachedCollisionSpacingY == SpacingY
			&& CachedCollisionHeightScale == HeightScale && CachedCollisionHeightOffset == HeightOffset;
		if (!bCacheMatches)
		{
			FCollisionGeometryBuildDiagnostics Diagnostics;
			CachedTerrainCollision = FCollisionGeometryRef::BuildHeightField(
				Payload->Width, Payload->Height, Payload->Samples, SpacingX, SpacingY,
				HeightScale, HeightOffset, &Diagnostics);
			if (!CachedTerrainCollision.IsValid())
				return SetCollisionFailure(ETerrainCollisionStatus::BuildFailed, std::format(
					"Terrain collision build failed with status {}.", static_cast<uint32>(Diagnostics.Status)));
			CachedCollisionDiagnostics = Diagnostics;
			CachedCollisionPayload = Payload;
			CachedHeightmapRevision = HeightmapRevision;
			CachedCollisionSpacingX = SpacingX;
			CachedCollisionSpacingY = SpacingY;
			CachedCollisionHeightScale = HeightScale;
			CachedCollisionHeightOffset = HeightOffset;
		}
		OutGeometry = CachedTerrainCollision;
		OutWorldTransform = GetWorldTransform();
		auto* Mutable = const_cast<DTerrainComponent*>(this);
		Mutable->CollisionStatus = ETerrainCollisionStatus::Ready;
		Mutable->LastCollisionDiagnostic.clear();
		return true;
	}

	auto DTerrainComponent::BuildMaterialRenderProxyBindingUpdate(
		FMaterialRenderProxyBindingUpdate& OutUpdate) -> bool
	{
		DMaterialInterface* AssignedMaterial = Material.Get();
		if (!AssignedMaterial)
			RecordMaterialFallbackReason(EMaterialFallbackReason::UnassignedDefault);
		OutUpdate.SlotIndex = 0;
		OutUpdate.MaterialProxy = AssignedMaterial
			? AssignedMaterial->GetMaterialRenderProxy() : GetDefaultMaterialRenderProxy();
		OutUpdate.ComponentRevision = MaterialComponentRevision;
		return true;
	}

	auto DTerrainComponent::HandleHeightmapRevisionChanged(
		DTerrainHeightmap* ChangedHeightmap) -> void
	{
		if (ChangedHeightmap == Heightmap.Get())
		{
			++CollisionRevision;
			MarkRenderStateDirty();
			RecreatePhysicsState();
		}
	}

	auto DTerrainComponent::PrepareForHeightmapRevisionChange() -> void
	{
		DestroyRenderState();
		DestroyPhysicsState();
	}

#if DURIN_WITH_EDITOR
	auto DTerrainComponent::GetEditorPickingLocalBounds(
		FBox& OutBounds, EEditorPickingPrimitiveFamily& OutFamily) const -> bool
	{
		if (!Heightmap) return false;
		auto Payload = Heightmap->GetPayload();
		std::vector<FTerrainPatchDescriptor> Patches;
		if (!Payload || !BuildTerrainPatches(*Payload, SpacingX, SpacingY,
			HeightScale, HeightOffset, Patches, OutBounds)) return false;
		OutFamily = EEditorPickingPrimitiveFamily::Terrain;
		return true;
	}
#endif
} // namespace Durin
