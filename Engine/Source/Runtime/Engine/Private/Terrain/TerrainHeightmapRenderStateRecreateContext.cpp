#include "Terrain/TerrainHeightmapRenderStateRecreateContext.h"

#include "Components/TerrainComponent.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin
{
	FTerrainHeightmapRenderStateRecreateContext::FTerrainHeightmapRenderStateRecreateContext(
		DTerrainHeightmap* Heightmap) : HeightmapHandle(MakeObjectHandle(Heightmap))
	{
		if (!IsValid(Heightmap) || IsObjectHandleNull(HeightmapHandle)) return;
		for (DObject* Object : GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly))
		{
			auto* Component = Cast<DTerrainComponent>(Object);
			if (!IsValid(Component) || !Component->IsRegistered()
				|| Component->GetHeightmap() != Heightmap) continue;
			const FObjectHandle Handle = MakeObjectHandle(Component);
			if (!IsObjectHandleNull(Handle)) ComponentHandles.push_back(Handle);
		}
		std::ranges::sort(ComponentHandles, [](FObjectHandle A, FObjectHandle B) {
			return std::tie(A.Index, A.Generation) < std::tie(B.Index, B.Generation);
		});
		for (FObjectHandle Handle : ComponentHandles)
			if (auto* Component = Cast<DTerrainComponent>(ResolveObjectHandle(Handle));
				IsValid(Component) && Component->GetHeightmap() == Heightmap)
				Component->PrepareForHeightmapRevisionChange();
	}

	FTerrainHeightmapRenderStateRecreateContext::~FTerrainHeightmapRenderStateRecreateContext()
	{
		auto* Heightmap = Cast<DTerrainHeightmap>(ResolveObjectHandle(HeightmapHandle));
		if (!IsValid(Heightmap)) return;
		for (FObjectHandle Handle : ComponentHandles)
			if (auto* Component = Cast<DTerrainComponent>(ResolveObjectHandle(Handle));
				IsValid(Component) && Component->GetHeightmap() == Heightmap)
				Component->HandleHeightmapRevisionChanged(Heightmap);
	}
} // namespace Durin
