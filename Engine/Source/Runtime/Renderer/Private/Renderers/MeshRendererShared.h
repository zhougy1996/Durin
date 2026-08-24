#pragma once

#include "Renderers/MeshRenderingCommon.h"
#include "Renderers/SurfaceMaterial.h"
#include "Renderers/SkeletalMeshRenderPreparation.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/ViewPreparationMath.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/GBufferRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "RendererResourceSlotCache.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/EnvironmentLightingResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Engine/SkeletalMeshSceneProxy.h"
#include "Engine/SplineMeshSceneProxy.h"
#include "Engine/StaticMeshSceneProxy.h"
#include "IScene.h"
#include "Math/Operations.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "SceneView.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin::RendererPrivate
{
	inline auto InitializeCompiledMaterialShaderMap(
		const FShaderType& VertexType,
		const FShaderType& FragmentType,
		const FMaterialCompilerResult& MaterialProgram,
		std::string_view FragmentEntryPoint,
		const FShaderCompileOptions& VertexCompileOptions,
		std::shared_ptr<FShaderMapBase>& OutShaderMap,
		std::string& OutError) -> bool
	{
		const auto Generated = std::ranges::find(
			MaterialProgram.CompiledShaders, FragmentEntryPoint,
			&FCompiledShader::SourceEntryPoint);
		if (Generated == MaterialProgram.CompiledShaders.end())
		{
			OutError = std::format(
				"Accepted material program has no {} stage.",
				FragmentEntryPoint);
			return false;
		}
		FShaderMapBase VertexMap;
		const std::array<const FShaderType*, 1> VertexTypes{&VertexType};
		if (!VertexMap.InitializeFromShaderTypes(
			VertexTypes, VertexCompileOptions, OutError)) return false;
		const uint32* VertexIndex = VertexMap.FindShaderIndex(&VertexType);
		if (!VertexIndex || !VertexMap.GetCode())
		{
			OutError = "Fixed material vertex program produced no resource code.";
			return false;
		}
		FShaderCompilerOutput Combined;
		Combined.bSucceeded = true;
		Combined.CompiledShaders = {
			VertexMap.GetCode()->GetCompiledShader(*VertexIndex), *Generated};
		const std::array<const FShaderType*, 2> Types{
			&VertexType, &FragmentType};
		auto Candidate = std::make_shared<FShaderMapBase>();
		if (!Candidate->Initialize(
			Types, Combined, VertexCompileOptions, OutError)) return false;
		OutShaderMap = std::move(Candidate);
		return true;
	}

	class FStaticMeshVertexShader : public FShader
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FStaticMeshVertexShader)
			DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
		DURIN_END_SHADER_PARAMETERS();

		DURIN_DECLARE_SHADER(
			FStaticMeshVertexShader,
			FShader,
			"/Engine/StaticMeshBasePass",
			EShaderFrequency::Vertex,
			"VertexMain"
		);
	};

	class FSplineMeshVertexShader : public FShader
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FSplineMeshVertexShader)
			DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
			DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(SplineMesh);
		DURIN_END_SHADER_PARAMETERS();
		DURIN_DECLARE_SHADER(FSplineMeshVertexShader, FShader, "/Engine/StaticMeshBasePass", EShaderFrequency::Vertex, "VertexMain");
	};

	class FSkeletalMeshVertexShader : public FShader
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FSkeletalMeshVertexShader)
			DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
			DURIN_SHADER_PARAMETER_STORAGE_BUFFER(SkinPalette);
		DURIN_END_SHADER_PARAMETERS();

		DURIN_DECLARE_SHADER(
			FSkeletalMeshVertexShader,
			FShader,
			"/Engine/StaticMeshBasePass",
			EShaderFrequency::Vertex,
			"VertexMain"
		);
	};

	struct FStaticMeshTransformUniform
	{
		FMatrix4f LocalToClip{1.0f};
		FMatrix4f LocalToWorld{1.0f};
		FMatrix4f NormalToWorld{1.0f};
		FVector4f TransformParams{1.0f, 0.0f, 0.0f, 0.0f};
	};

	struct FSplineMeshUniform
	{
		FVector4f StartPosition{0.0f};
		FVector4f StartTangent{0.0f};
		FVector4f EndPosition{0.0f};
		FVector4f EndTangent{0.0f};
		FVector4f StartEndScale{1.0f};
		FVector4f StartEndOffset{0.0f};
		FVector4f RollUpAxis{0.0f};
		FVector4f SourceRangePolicy{0.0f};
	};

	inline auto MakeSplineMeshUniform(const FSplineMeshParams& Params) -> FSplineMeshUniform
	{
		FSplineMeshUniform Result;
		Result.StartPosition = FVector4f(FVector3f(Params.StartPosition), static_cast<float>(Params.StartRollRadians));
		Result.StartTangent = FVector4f(FVector3f(Params.StartTangent), static_cast<float>(Params.EndRollRadians));
		Result.EndPosition = FVector4f(FVector3f(Params.EndPosition), 0.0f);
		Result.EndTangent = FVector4f(FVector3f(Params.EndTangent), 0.0f);
		Result.StartEndScale = FVector4f(FVector2f(Params.StartScale), FVector2f(Params.EndScale));
		Result.StartEndOffset = FVector4f(FVector2f(Params.StartOffset), FVector2f(Params.EndOffset));
		Result.RollUpAxis = FVector4f(FVector3f(Params.SplineUpDirection), static_cast<float>(Params.ForwardAxis));
		Result.SourceRangePolicy = FVector4f(static_cast<float>(Params.SourceForwardMin), static_cast<float>(Params.SourceForwardMax), Params.Interpolation == ESplineMeshInterpolation::SmoothStep ? 1.0f : 0.0f, 0.0f);
		return Result;
	}

	inline auto GetIdentityText(
		const FMaterialShaderMapIdentity& Identity
	) -> std::string
	{
		return std::format(
			"layout-version={},layout-id={},program={},blend={},shading={},mask-bits={}",
			Identity.RenderLayout.Version,
			Identity.RenderLayout.Id.ToString(),
			Identity.ProgramIdentity.ToString(),
			static_cast<uint8>(Identity.BlendMode),
			static_cast<uint8>(Identity.ShadingModel),
			std::bit_cast<uint32>(Identity.OpacityMaskThreshold)
		);
	}

	inline auto GetIdentityText(
		const FMaterialPlanningPassIdentity& Identity
	) -> std::string
	{
		return std::format(
			"{},two-sided={},depth-write={}",
			GetIdentityText(Identity.ShaderMap),
			Identity.bTwoSided,
			static_cast<uint8>(Identity.DepthWritePolicy)
		);
	}

	inline auto GetIdentityText(
		const FEffectiveMeshPipelineKey& Identity
	) -> std::string
	{
		return std::format(
			"{},vertex-domain={},polygon={},cull={},front={},depth-test={},depth-write={},depth-compare={},blend={},color-src={},color-dst={},color-op={},alpha-src={},alpha-dst={},alpha-op={},write-mask={}",
			GetIdentityText(Identity.Material),
			static_cast<uint8>(Identity.VertexDomain),
			static_cast<uint8>(Identity.Rasterizer.PolygonMode),
			static_cast<uint8>(Identity.Rasterizer.CullMode),
			static_cast<uint8>(Identity.Rasterizer.FrontFace),
			Identity.Depth.bEnableTest,
			Identity.Depth.bEnableWrite,
			static_cast<uint8>(Identity.Depth.CompareOp),
			Identity.ColorBlend.bEnable,
			static_cast<uint8>(Identity.ColorBlend.SrcColorFactor),
			static_cast<uint8>(Identity.ColorBlend.DstColorFactor),
			static_cast<uint8>(Identity.ColorBlend.ColorOp),
			static_cast<uint8>(Identity.ColorBlend.SrcAlphaFactor),
			static_cast<uint8>(Identity.ColorBlend.DstAlphaFactor),
			static_cast<uint8>(Identity.ColorBlend.AlphaOp),
			static_cast<uint8>(Identity.ColorBlend.ColorWriteMask)
		);
	}

	inline auto MakeMeshDrawSortKey(
		EMeshBasePass Pass,
		const FEffectiveMeshPipelineKey& PipelineKey,
		const FMaterialRenderRepresentation& Representation,
		uint32 NumVertices,
		const FVertexDeclarationElementList& Elements,
		const std::array<uint32, 6>& Geometry,
		uint64 PrimitiveId,
		uint32 LODIndex,
		uint32 SectionIndex
	) -> FMeshDrawSortKey
	{
		FMeshDrawSortKey Result;
		const FMaterialPlanningPassIdentity& Material = PipelineKey.Material;
		const FMaterialShaderMapIdentity& Shader = Material.ShaderMap;
		const FGuid& LayoutId = Shader.RenderLayout.Id;
		const FXxHash128& ProgramDigest = Shader.ProgramIdentity.Digest;
		Result.Pipeline = {
			static_cast<uint32>(Pass), Shader.RenderLayout.Version,
			LayoutId.A, LayoutId.B, LayoutId.C, LayoutId.D,
			static_cast<uint32>(ProgramDigest.HashLow),
			static_cast<uint32>(ProgramDigest.HashLow >> 32),
			static_cast<uint32>(ProgramDigest.HashHigh),
			static_cast<uint32>(ProgramDigest.HashHigh >> 32),
			static_cast<uint32>(Shader.BlendMode),
			static_cast<uint32>(Shader.ShadingModel),
			std::bit_cast<uint32>(Shader.OpacityMaskThreshold),
			Material.bTwoSided ? 1u : 0u,
			static_cast<uint32>(Material.DepthWritePolicy),
			static_cast<uint32>(PipelineKey.VertexDomain),
			static_cast<uint32>(PipelineKey.Rasterizer.PolygonMode),
			static_cast<uint32>(PipelineKey.Rasterizer.CullMode),
			static_cast<uint32>(PipelineKey.Rasterizer.FrontFace),
			PipelineKey.Depth.bEnableTest ? 1u : 0u,
			PipelineKey.Depth.bEnableWrite ? 1u : 0u,
			static_cast<uint32>(PipelineKey.Depth.CompareOp),
			PipelineKey.ColorBlend.bEnable ? 1u : 0u,
			static_cast<uint32>(PipelineKey.ColorBlend.SrcColorFactor),
			static_cast<uint32>(PipelineKey.ColorBlend.DstColorFactor),
			static_cast<uint32>(PipelineKey.ColorBlend.ColorOp),
			static_cast<uint32>(PipelineKey.ColorBlend.SrcAlphaFactor),
			static_cast<uint32>(PipelineKey.ColorBlend.DstAlphaFactor),
			static_cast<uint32>(PipelineKey.ColorBlend.AlphaOp),
			static_cast<uint32>(PipelineKey.ColorBlend.ColorWriteMask)
		};
		const std::span<const std::byte> UniformPayload =
			Representation.GetUniformPayload();
		Result.MaterialUniform.reserve(UniformPayload.size());
		for (const std::byte Byte : UniformPayload)
		{
			Result.MaterialUniform.push_back(Byte);
		}

		if (NumVertices != 0)
		{
			Result.VertexFactory[0] = NumVertices;
			for (size_t Index = 0; Index < Elements.size(); ++Index)
			{
				const FVertexElement& Element = Elements[Index];
				const size_t Base = 1 + Index * 5;
				Result.VertexFactory[Base] = Element.StreamIndex;
				Result.VertexFactory[Base + 1] = Element.Offset;
				Result.VertexFactory[Base + 2] =
					static_cast<uint32>(Element.Type);
				Result.VertexFactory[Base + 3] = Element.AttributeIndex;
				Result.VertexFactory[Base + 4] = Element.Stride;
			}
		}
		Result.Geometry = Geometry;
		Result.PrimitiveId = PrimitiveId;
		Result.SelectedLODIndex = LODIndex;
		Result.SectionIndex = SectionIndex;
		return Result;
	}

	inline auto MakeStaticMeshDrawSortKey(
		const FPreparedStaticMeshPrimitive& Primitive,
		const FPreparedStaticMeshDraw& Draw
	) -> FMeshDrawSortKey
	{
		const auto Elements = Primitive.VertexFactory != nullptr ? Primitive.VertexFactory->GetDeclarationElements() : FVertexDeclarationElementList{};
		const std::array<uint32, 6> Geometry = Draw.Section != nullptr ? std::array<uint32, 6>{Draw.Section->FirstIndex, Draw.Section->IndexCount, Draw.Section->MinVertexIndex, Draw.Section->MaxVertexIndex, Draw.Section->MaterialSlotIndex, static_cast<uint32>(Primitive.LOD->IndexBuffer.GetIndices().size())} : std::array<uint32, 6>{};
		return MakeMeshDrawSortKey(Draw.Pass, Draw.PipelineKey, Draw.Material.Representation, Primitive.VertexFactory != nullptr ? Primitive.VertexFactory->GetData().NumVertices : 0u, Elements, Geometry, Primitive.PrimitiveId.Value, Primitive.SelectedLODIndex, Draw.SectionIndex);
	}

	inline auto MakeSkeletalMeshDrawSortKey(
		const FPreparedSkeletalMeshPrimitive& Primitive,
		const FPreparedSkeletalMeshDraw& Draw
	) -> FMeshDrawSortKey
	{
		const auto Elements = Primitive.VertexFactory != nullptr ? Primitive.VertexFactory->GetDeclarationElements() : FVertexDeclarationElementList{};
		const std::array<uint32, 6> Geometry = Draw.Section != nullptr
													   && Primitive.RenderData != nullptr ?
												   std::array<uint32, 6>{Draw.Section->FirstIndex, Draw.Section->IndexCount, Draw.Section->MinVertexIndex, Draw.Section->MaxVertexIndex, Draw.Section->MaterialSlotIndex, static_cast<uint32>(Primitive.RenderData->IndexBuffer.GetIndices().size())} :
												   std::array<uint32, 6>{};
		return MakeMeshDrawSortKey(Draw.Pass, Draw.PipelineKey, Draw.Material.Representation, Primitive.VertexFactory != nullptr ? Primitive.VertexFactory->GetData().NumVertices : 0u, Elements, Geometry, Primitive.PrimitiveId.Value, 0, Draw.SectionIndex);
	}

	enum class ESkeletalPaletteResolveResult : uint8
	{
		Uploaded,
		Reused,
		Rejected,
	};

	inline auto ResolveSkeletalPalette_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FPreparedSkeletalPaletteTable& PreparedTable,
		FResolvedSkeletalPaletteTable& ResolvedTable,
		const FPreparedSkeletalMeshPrimitive& Primitive,
		FRHIStorageBufferRange& OutRange
	) -> ESkeletalPaletteResolveResult
	{
		constexpr uint64 PaletteBudget = 64ull * 1024ull * 1024ull;
		++ResolvedTable.RequestedPalettes;
		if (Primitive.Pose == nullptr)
		{
			++ResolvedTable.RejectedPalettes;
			return ESkeletalPaletteResolveResult::Rejected;
		}

		const FPrimitiveSceneId PrimitiveId = Primitive.PrimitiveId;
		const auto It = PreparedTable.PrimitiveToEntry.find(PrimitiveId);
		if (It == PreparedTable.PrimitiveToEntry.end()
			|| It->second >= PreparedTable.Entries.size())
		{
			++ResolvedTable.RejectedPalettes;
			return ESkeletalPaletteResolveResult::Rejected;
		}
		const auto& PreparedEntry = PreparedTable.Entries[It->second];
		if (PreparedEntry.Pose != Primitive.Pose)
		{
			++ResolvedTable.RejectedPalettes;
			return ESkeletalPaletteResolveResult::Rejected;
		}
		if (ResolvedTable.Entries.size() < PreparedTable.Entries.size())
			ResolvedTable.Entries.resize(PreparedTable.Entries.size());
		auto& Entry = ResolvedTable.Entries[It->second];
		if (Entry.Range.Buffer != nullptr)
		{
			OutRange = Entry.Range;
			++ResolvedTable.ReusedPalettes;
			return ESkeletalPaletteResolveResult::Reused;
		}
		if (Entry.bUploadAttempted)
		{
			++ResolvedTable.RejectedPalettes;
			return ESkeletalPaletteResolveResult::Rejected;
		}
		Entry.bUploadAttempted = true;
		const uint64 Bytes = Primitive.Pose->Matrices.size() * sizeof(FMatrix4f);
		if (Bytes == 0 || ResolvedTable.UploadedBytes + Bytes > PaletteBudget)
		{
			++ResolvedTable.RejectedPalettes;
			return ESkeletalPaletteResolveResult::Rejected;
		}

		Entry.Range = CommandList.AllocateDynamicStorageBuffer(
			Primitive.Pose->Matrices.data(), static_cast<uint32>(Bytes)
		);
		if (Entry.Range.Buffer == nullptr || Entry.Range.Size != Bytes)
		{
			Entry.Range = {};
			++ResolvedTable.RejectedPalettes;
			return ESkeletalPaletteResolveResult::Rejected;
		}
		const std::array Transition{FRHIBufferTransition{
			Entry.Range.Buffer, Entry.Range.Offset, Entry.Range.Size,
			ERHIAccess::HostWrite, ERHIAccess::GraphicsShaderRead
		}};
		CommandList.TransitionBuffers(Transition);
		OutRange = Entry.Range;
		ResolvedTable.UploadedBytes += Bytes;
		++ResolvedTable.UploadedPalettes;
		ResolvedTable.UploadedMatrices += Primitive.Pose->Matrices.size();
		return ESkeletalPaletteResolveResult::Uploaded;
	}
} // namespace Durin::RendererPrivate
