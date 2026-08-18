#include "Terrain/TerrainLOD.h"

#include "Math/Operations.h"
#include "Renderers/ViewPreparationMath.h"
#include "Terrain/TerrainTopology.h"

namespace Durin
{
	namespace
	{
		auto TransformBounds(const FBox& Bounds, const FMatrix& Transform) -> FBox
		{
			FBox Result;
			if (!Bounds.bIsValid || !Math::IsFinite(Transform)) return Result;
			for (uint32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector3 Point(
					(Corner & 1u) ? Bounds.Max.x : Bounds.Min.x,
					(Corner & 2u) ? Bounds.Max.y : Bounds.Min.y,
					(Corner & 4u) ? Bounds.Max.z : Bounds.Min.z);
				Result.AddPoint(FVector3(Transform * FVector4(Point, 1.0)));
			}
			return Result;
		}

		auto AddEdgeMask(uint8& Mask, ETerrainStitchEdge Edge) -> void
		{
			Mask |= static_cast<uint8>(Edge);
		}
	}

	auto SelectTerrainPatchLOD(
		const FSceneView& View, const FMatrix& LocalToWorld,
		const FTerrainPatchDescriptor& Patch) -> FTerrainLODSelection
	{
		if (View.Settings.Mode.LODMode == EViewLODMode::ForceLOD0) return {};
		if (Patch.LODSteps.empty() || Patch.LODSteps.size() != Patch.LODErrors.size()
			|| Patch.LODSteps.front() != 1 || View.ViewportWidth == 0
			|| View.ViewportHeight == 0 || !Math::IsFinite(LocalToWorld)
			|| !Patch.LocalBounds.bIsValid || !Math::IsFinite(Patch.LocalBounds.Min)
			|| !Math::IsFinite(Patch.LocalBounds.Max)) return {0, true};
		const float Threshold = TerrainScreenErrorPixels
			/ static_cast<float>(std::min(View.ViewportWidth, View.ViewportHeight));
		uint32 Selected = 0;
		const FVector3 Center = Patch.LocalBounds.GetCenter();
		for (uint32 LOD = 1; LOD < Patch.LODErrors.size(); ++LOD)
		{
			if (Patch.LODSteps[LOD] != Patch.LODSteps[LOD - 1] * 2
				|| Patch.CellCountX % Patch.LODSteps[LOD] != 0
				|| Patch.CellCountY % Patch.LODSteps[LOD] != 0) return {0, true};
			const double Error = Patch.LODErrors[LOD];
			if (!std::isfinite(Error) || Error < 0.0
				|| Error < Patch.LODErrors[LOD - 1]) return {0, true};
			if (Error == 0.0)
			{
				Selected = LOD;
				continue;
			}
			const FBox ErrorBounds(
				{Center.x, Center.y, Center.z - Error},
				{Center.x, Center.y, Center.z + Error});
			const auto Projection = ComputeProjectedScreenSize(
				View, TransformBounds(ErrorBounds, LocalToWorld));
			if (Projection.Status != EProjectedScreenSizeStatus::Valid) return {0, true};
			// Strict comparison preserves the finer LOD at exact threshold equality.
			if (Projection.NormalizedScreenSize < Threshold) Selected = LOD;
			else break;
		}
		return {Selected, false};
	}

	auto ResolveTerrainPatchAdjacency(
		std::span<const FTerrainPatchDescriptor> Patches,
		std::span<const uint32> RequestedLODs) -> FTerrainLODResolution
	{
		FTerrainLODResolution Result;
		if (Patches.empty() || RequestedLODs.size() != Patches.size()) return Result;
		uint32 CountX = 0;
		uint32 CountY = 0;
		size_t MaximumWork = 0;
		for (size_t Index = 0; Index < Patches.size(); ++Index)
		{
			const auto& Patch = Patches[Index];
			if (Patch.LODSteps.empty() || Patch.LODSteps.size() != Patch.LODErrors.size()
				|| RequestedLODs[Index] >= Patch.LODSteps.size()) return Result;
			CountX = std::max(CountX, static_cast<uint32>(Patch.GridX) + 1);
			CountY = std::max(CountY, static_cast<uint32>(Patch.GridY) + 1);
			MaximumWork += Patch.LODSteps.size();
		}
		if (static_cast<size_t>(CountX) * CountY != Patches.size()) return Result;
		for (size_t Index = 0; Index < Patches.size(); ++Index)
			if (Patches[Index].GridX != Index % CountX
				|| Patches[Index].GridY != Index / CountX) return Result;

		Result.ResolvedLODs.assign(RequestedLODs.begin(), RequestedLODs.end());
		for (size_t Pass = 0; Pass <= MaximumWork; ++Pass)
		{
			bool bChanged = false;
			++Result.Iterations;
			auto ResolvePair = [&](size_t A, size_t B) {
				uint32& LODA = Result.ResolvedLODs[A];
				uint32& LODB = Result.ResolvedLODs[B];
				while (LODA > LODB + 1) { --LODA; ++Result.Promotions; bChanged = true; }
				while (LODB > LODA + 1) { --LODB; ++Result.Promotions; bChanged = true; }
			};
			for (uint32 Y = 0; Y < CountY; ++Y)
				for (uint32 X = 0; X < CountX; ++X)
				{
					const size_t Index = static_cast<size_t>(Y) * CountX + X;
					if (X + 1 < CountX) ResolvePair(Index, Index + 1);
					if (Y + 1 < CountY) ResolvePair(Index, Index + CountX);
				}
			if (!bChanged) break;
			if (Pass == MaximumWork) return {};
		}

		Result.StitchMasks.assign(Patches.size(), 0);
		auto StitchPair = [&](size_t Fine, size_t Coarse, ETerrainStitchEdge Edge) -> bool {
			const uint32 FineStep = Patches[Fine].LODSteps[Result.ResolvedLODs[Fine]];
			const uint32 CoarseStep = Patches[Coarse].LODSteps[Result.ResolvedLODs[Coarse]];
			if (CoarseStep == FineStep) return true;
			if (CoarseStep != FineStep * 2) return false;
			AddEdgeMask(Result.StitchMasks[Fine], Edge);
			return true;
		};
		for (uint32 Y = 0; Y < CountY; ++Y)
			for (uint32 X = 0; X < CountX; ++X)
			{
				const size_t Index = static_cast<size_t>(Y) * CountX + X;
				if (X + 1 < CountX)
				{
					const size_t East = Index + 1;
					if (Result.ResolvedLODs[Index] < Result.ResolvedLODs[East])
					{ if (!StitchPair(Index, East, ETerrainStitchEdge::East)) return {}; }
					else if (Result.ResolvedLODs[East] < Result.ResolvedLODs[Index])
					{ if (!StitchPair(East, Index, ETerrainStitchEdge::West)) return {}; }
				}
				if (Y + 1 < CountY)
				{
					const size_t South = Index + CountX;
					if (Result.ResolvedLODs[Index] < Result.ResolvedLODs[South])
					{ if (!StitchPair(Index, South, ETerrainStitchEdge::South)) return {}; }
					else if (Result.ResolvedLODs[South] < Result.ResolvedLODs[Index])
					{ if (!StitchPair(South, Index, ETerrainStitchEdge::North)) return {}; }
				}
			}
		Result.bValid = true;
		return Result;
	}
}
