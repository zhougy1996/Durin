#pragma once

#include "EngineAPI.h"
#include "VertexFactory.h"

namespace Durin
{
	struct FSkeletalMeshVertexBuffers;

	class FSkeletalMeshVertexFactory : public FVertexFactory
	{
	public:
		struct FDataType
		{
			FVertexStreamComponent PositionComponent;
			std::array<FVertexStreamComponent, 2> TangentBasisComponents;
			std::array<FVertexStreamComponent, 4> TextureCoordinates;
			FVertexStreamComponent ColorComponent;
			FVertexStreamComponent JointIndicesComponent;
			FVertexStreamComponent JointWeightsComponent;
			uint32 NumVertices = 0;
		};

		ENGINE_API auto SetData(const FSkeletalMeshVertexBuffers& VertexBuffers) -> bool;
		ENGINE_API auto InitRHI(FRHICommandListBase& RHICmdList) -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FSkeletalMeshVertexFactory";
		}
		auto GetTypeName() const -> std::string_view override
		{
			return "FSkeletalMeshVertexFactory";
		}
		static constexpr auto GetShaderModuleName() -> std::string_view
		{
			return "VertexFactory.SkeletalMeshVertexFactory";
		}
		auto GetData() const -> const FDataType& { return Data; }
		ENGINE_API auto GetDeclarationElements() const -> FVertexDeclarationElementList;
		ENGINE_API auto IsDataValid() const -> bool;
		auto IsReady() const -> bool
		{
			return IsDataValid() && FVertexFactory::IsReady();
		}

	private:
		FDataType Data;
	};
}
