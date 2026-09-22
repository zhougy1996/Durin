#include "StaticMesh/StaticMeshLODSelection.h"

#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		template<typename TLOD>
		auto ValidateStaticMeshLODResources(
			std::span<const TLOD> LODResources) -> bool
		{
			if (LODResources.empty() || LODResources.back().ScreenSize != 0.0f)
			{
				return false;
			}
			for (size_t LODIndex = 0; LODIndex < LODResources.size(); ++LODIndex)
			{
				const float ScreenSize = LODResources[LODIndex].ScreenSize;
				if (!std::isfinite(ScreenSize) || ScreenSize < 0.0f
					|| ScreenSize > 1.0f
					|| (LODIndex > 0
						&& LODResources[LODIndex - 1].ScreenSize <= ScreenSize))
				{
					return false;
				}
			}
			return true;
		}
	}

	template<typename TLOD>
	auto SelectLOD(
		float NormalizedScreenSize,
		std::span<const TLOD> LODResources) -> uint32
	{
		if (!std::isfinite(NormalizedScreenSize)
			|| NormalizedScreenSize < 0.0f || NormalizedScreenSize > 1.0f
			|| !ValidateStaticMeshLODResources(LODResources))
		{
			return 0;
		}
		for (uint32 LODIndex = 0;
			 LODIndex < static_cast<uint32>(LODResources.size());
			 ++LODIndex)
		{
			if (NormalizedScreenSize >= LODResources[LODIndex].ScreenSize)
			{
				return LODIndex;
			}
		}
		return static_cast<uint32>(LODResources.size() - 1);
	}

	template<typename TLOD>
	auto ResolveLOD(
		uint32 RequestedLOD,
		std::span<const TLOD> LODResources) -> uint32
	{
		if (RequestedLOD >= LODResources.size())
		{
			return InvalidStaticMeshLODIndex;
		}
		for (uint32 LODIndex = RequestedLOD;
			 LODIndex < static_cast<uint32>(LODResources.size());
			 ++LODIndex)
		{
			if (LODResources[LODIndex].bReadyForRendering)
			{
				return LODIndex;
			}
		}
		for (uint32 LODIndex = RequestedLOD; LODIndex > 0; --LODIndex)
		{
			if (LODResources[LODIndex - 1].bReadyForRendering)
			{
				return LODIndex - 1;
			}
		}
		return InvalidStaticMeshLODIndex;
	}

	auto CaptureStaticMeshLODSelection(std::span<const FStaticMeshLODResources> LODResources)
		-> FMeshLODSelectionSnapshot
	{
		FMeshLODSelectionSnapshot Result;
		Result.reserve(LODResources.size());
		for (const auto& LOD : LODResources) Result.push_back({LOD.ScreenSize, LOD.bReadyForRendering});
		return Result;
	}

	auto SelectStaticMeshLOD(float Size, std::span<const FStaticMeshLODResources> LODs) -> uint32
	{
		return SelectLOD(Size, LODs);
	}
	auto SelectStaticMeshLOD(float Size, std::span<const FMeshLODSelectionEntry> LODs) -> uint32
	{
		return SelectLOD(Size, LODs);
	}
	auto ResolveAvailableStaticMeshLOD(uint32 Requested, std::span<const FStaticMeshLODResources> LODs) -> uint32
	{
		return ResolveLOD(Requested, LODs);
	}
	auto ResolveAvailableStaticMeshLOD(uint32 Requested, std::span<const FMeshLODSelectionEntry> LODs) -> uint32
	{
		return ResolveLOD(Requested, LODs);
	}
} // namespace Durin
