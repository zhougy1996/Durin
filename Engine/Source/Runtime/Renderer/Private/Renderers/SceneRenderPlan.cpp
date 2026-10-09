#include "Renderers/SceneRenderPlan.h"

namespace Durin
{
	auto PrepareCombinedTranslucentGeometry(
		FPreparedReceiverGeometry& Geometry) -> void
	{
		Geometry.TranslucentGeometry.clear();
		Geometry.TranslucentGeometry.reserve(
			Geometry.StaticMeshes.Translucent.size());
		for (uint32 Index = 0;
			 Index < Geometry.StaticMeshes.Translucent.size(); ++Index)
		{
			const auto& Draw = Geometry.StaticMeshes.Translucent[Index];
			Geometry.TranslucentGeometry.push_back({
				EPreparedTranslucentGeometryFamily::StaticMesh, Index,
				Draw.TranslucentSortDepth, Draw.SortKey});
		}
		std::ranges::sort(Geometry.TranslucentGeometry,
			[](const auto& A, const auto& B) {
				if (A.SortDepth != B.SortDepth)
					return A.SortDepth > B.SortDepth;
				if (const auto Order = A.SortKey <=> B.SortKey; Order != 0)
					return Order < 0;
				return static_cast<uint8>(A.Family)
					< static_cast<uint8>(B.Family);
			});
	}
} // namespace Durin
