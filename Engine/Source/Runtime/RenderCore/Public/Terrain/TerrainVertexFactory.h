#pragma once

#include "RenderCoreAPI.h"
#include "VertexFactory.h"

namespace Durin
{
	// Binds compact unsigned sample coordinates for terrain height reconstruction.
	class FTerrainVertexFactory final : public FVertexFactory
	{
	public:
		RENDERCORE_API auto Initialize(
			FBufferRHIRef InGridCoordinates, uint32 InVertexCount) -> bool;
		RENDERCORE_API auto InitRHI(FRHICommandListBase& RHICmdList) -> void override;
		auto GetFriendlyName() const -> std::string override { return "FTerrainVertexFactory"; }
		auto GetTypeName() const -> std::string_view override { return "FTerrainVertexFactory"; }
		static constexpr auto GetShaderModuleName() -> std::string_view
		{
			return "VertexFactory.TerrainVertexFactory";
		}
		auto GetVertexCount() const -> uint32 { return VertexCount; }
		auto GetGridCoordinates() const -> FRHIBuffer* { return GridCoordinates; }

	private:
		FBufferRHIRef GridCoordinates;
		uint32 VertexCount = 0;
	};
} // namespace Durin
