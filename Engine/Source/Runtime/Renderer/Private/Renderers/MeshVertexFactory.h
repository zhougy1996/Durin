#pragma once

#include "RendererAPI.h"
#include "VertexFactory.h"
#include "GeometrySubmission.h"
#include "Shader/MaterialShader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin::RendererPrivate
{
	// Vertex operations only; material policy and pass recording stay with passes.
	class FMeshVertexShaderBinding
	{
	public:
		virtual ~FMeshVertexShaderBinding() = default;
		virtual auto GetRHIShader(bool bRequired = true) const -> FRHIShader* = 0;
		virtual auto Bind(FRHICommandListImmediate& CommandList,
			const FRHIUniformBufferRange& Transform,
			const FVertexFactoryBinding& Binding) const -> bool = 0;
	};

	class FMeshVertexFactoryImplementation
	{
	public:
		virtual ~FMeshVertexFactoryImplementation() = default;
		virtual auto GetType() const -> const FVertexFactoryType& = 0;
		virtual auto GetLayoutKey() const -> FXxHash64 = 0;
		virtual auto GetShaderType(uint32 Pass) const -> FShaderType* = 0;
		auto GetRuntimeRequestName(uint32 Pass) const -> std::string
		{
			return std::format("MeshVertex.{}.{}.{}", GetType().GetName(), GetLayoutKey().HashValue, Pass);
		}
		virtual auto Supports(uint32 Pass, const FGeometryDrawRange& Draw) const -> bool
		{
			return GetShaderType(Pass) != nullptr
				&& (Draw.Topology == EGeometryTopology::TriangleList || Draw.Topology == EGeometryTopology::LineList);
		}
		virtual auto Resolve(const FMaterialShaderMap& Map, uint32 Pass) const
			-> std::shared_ptr<const FMeshVertexShaderBinding> = 0;
	};

	// Implementations are retained for module lifetime. Contributions must be
	// registered during initialization, before runtime shader inventory freezes.
	RENDERER_API auto RegisterMeshVertexFactory(
		std::shared_ptr<const FMeshVertexFactoryImplementation> Implementation) -> bool;
	RENDERER_API auto FindMeshVertexFactory(FXxHash64 FactoryKey)
		-> std::shared_ptr<const FMeshVertexFactoryImplementation>;
}
