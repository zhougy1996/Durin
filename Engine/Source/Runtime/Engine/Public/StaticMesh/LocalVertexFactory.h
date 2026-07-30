#pragma once

#include "EngineAPI.h"

#include "VertexFactory.h"

namespace Durin
{
	struct FStaticMeshVertexBuffers;

	class FLocalVertexFactory : public FVertexFactory
	{
	public:
		struct FDataType
		{
			FVertexStreamComponent PositionComponent;
			std::array<FVertexStreamComponent, 2> TangentBasisComponents;
			std::array<FVertexStreamComponent, 4> TextureCoordinates;
			FVertexStreamComponent ColorComponent;
			uint32 NumVertices = 0;
		};

		ENGINE_API auto SetData(
			const FStaticMeshVertexBuffers& VertexBuffers) -> bool;
		ENGINE_API auto InitRHI(
			FRHICommandListBase& RHICmdList) -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FLocalVertexFactory";
		}
		auto GetTypeName() const -> std::string_view override
		{
			return "FLocalVertexFactory";
		}
		auto GetData() const -> const FDataType& { return Data; }
		ENGINE_API auto GetDeclarationElements() const
			-> FVertexDeclarationElementList;
		ENGINE_API auto IsDataValid() const -> bool;
		auto IsReady() const -> bool
		{
			return IsDataValid() && FVertexFactory::IsReady();
		}

	private:
		FDataType Data;
	};

	struct FStaticMeshVertexFactories
	{
		FLocalVertexFactory VertexFactory;
	};
}
