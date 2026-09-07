#include "StaticMesh/StaticMeshLODSelection.h"

#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		auto ValidateStaticMeshLODResources(
			std::span<const FStaticMeshLODResources> LODResources) -> bool
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

	auto SelectStaticMeshLOD(
		float NormalizedScreenSize,
		std::span<const FStaticMeshLODResources> LODResources) -> uint32
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

	auto ResolveAvailableStaticMeshLOD(
		uint32 RequestedLOD,
		std::span<const FStaticMeshLODResources> LODResources) -> uint32
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
} // namespace Durin
