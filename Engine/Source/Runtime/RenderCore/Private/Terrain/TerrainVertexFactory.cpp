#include "Terrain/TerrainVertexFactory.h"

namespace Durin
{
	auto FTerrainVertexFactory::Initialize(
		FBufferRHIRef InGridCoordinates, uint32 InVertexCount) -> bool
	{
		if (IsInitialized() || !InGridCoordinates || InVertexCount == 0) return false;
		GridCoordinates = std::move(InGridCoordinates);
		VertexCount = InVertexCount;
		return true;
	}

	auto FTerrainVertexFactory::InitRHI(FRHICommandListBase& RHICmdList) -> void
	{
		if (!GridCoordinates || VertexCount == 0) return;
		FVertexDeclarationElementList Elements{};
		Elements[0] = FVertexElement(0, 0, EVertexElementType::UShort2, 0, sizeof(uint16) * 2);
		SetDeclarationElements(Elements);
		SetStreams({{0, GridCoordinates, 0, sizeof(uint16) * 2}});
		FVertexFactory::InitRHI(RHICmdList);
	}
} // namespace Durin
