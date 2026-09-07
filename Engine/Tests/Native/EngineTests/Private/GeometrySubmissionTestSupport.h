#pragma once

#include "Rendering/MeshBatch.h"
#include "Shader/GlobalShader.h"
#include "Resources/RenderTargetLayouts.h"
#include "Rendering/PrimitiveSceneProxy.h"
#include "Renderers/MeshVertexFactory.h"
#include "Renderers/MeshRendererShared.h"

namespace Durin::Tests
{
	class FProceduralBinding final : public FVertexFactoryInputBinding
	{
	public:
		bool bCustom = false;
		bool bForwardOnly = false;
		FVector4f DisplacementScale{0.0f, 0.0f, 0.0f, 1.0f};
		auto GetFactoryKey() const -> FXxHash64 override
		{
			if (bForwardOnly) return FXxHash64::HashBuffer("ForwardOnlyQualification", sizeof("ForwardOnlyQualification") - 1);
			return FXxHash64::HashBuffer(bCustom ? "QualificationVertexFactory" : "LocalVertexFactory",
				bCustom ? sizeof("QualificationVertexFactory") - 1 : sizeof("LocalVertexFactory") - 1);
		}
		auto GetLayoutKey() const -> FXxHash64 override
		{
			if (bForwardOnly) return FXxHash64::HashBuffer("ForwardOnlyBinding.v1", sizeof("ForwardOnlyBinding.v1") - 1);
			return FXxHash64::HashBuffer(bCustom ? "QualificationBinding.v1" : "LocalMeshBinding.v1",
				bCustom ? sizeof("QualificationBinding.v1") - 1 : sizeof("LocalMeshBinding.v1") - 1);
		}
	};

	// Independent interleaved geometry: no asset, LOD, section, or local factory.
	struct FProceduralGeometry
	{
		std::shared_ptr<FProceduralBinding> Binding;
		FGeometryBufferView Vertices;
		FGeometryBufferView Indices;
		FGeometryBufferView Instances;
		static auto Create(FRHICommandListImmediate& CommandList, uint64 Generation, bool bCustom) -> FProceduralGeometry
		{
			struct FVertex
			{
				FVector3f Position;
				FVector4f Normal{0, 0, 1, 0};
				FVector4f Tangent{1, 0, 0, 1};
				FVector2f UV[4]{};
				FVector4f Color{1};
			};
			const std::array<FVertex, 6> Data{{
				{{-0.8f, -0.7f, 0.5f}}, {{-0.1f, -0.7f, 0.5f}}, {{-0.45f, 0.7f, 0.5f}},
				{{0.1f, -0.7f, 0.5f}}, {{0.8f, -0.7f, 0.5f}}, {{0.45f, 0.7f, 0.5f}}
			}};
			auto Desc = FRHIBufferCreateDesc::Create("IndependentGeometry", sizeof(Data), sizeof(FVertex), EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::Static);
			Desc.InitialData = {Data.data(), sizeof(Data)};
			FProceduralGeometry Result;
			Result.Vertices = {GDynamicRHI->RHICreateBuffer(CommandList, Desc), {sizeof(Data), 0, sizeof(FVertex), sizeof(FVector3f)}, Generation, Generation};
			const std::array<uint32, 3> IndexData{0, 1, 2};
			auto IndexDesc = FRHIBufferCreateDesc::CreateIndex("IndependentIndices", sizeof(IndexData), sizeof(uint32));
			IndexDesc.Usage |= EBufferUsageFlags::Static;
			IndexDesc.InitialData = {IndexData.data(), sizeof(IndexData)};
			Result.Indices = {GDynamicRHI->RHICreateBuffer(CommandList, IndexDesc), {sizeof(IndexData), 0, sizeof(uint32), sizeof(uint32)}, Generation, Generation};
			Result.Binding = std::make_shared<FProceduralBinding>();
			auto& Binding = *Result.Binding;
			Binding.bCustom = bCustom;
			Binding.NumVertices = Data.size();
			auto& Elements = Binding.DeclarationElements;
			Elements[0] = FVertexElement(0, offsetof(FVertex, Position), EVertexElementType::Float3, 0, sizeof(FVertex));
			Elements[1] = FVertexElement(0, offsetof(FVertex, Normal), EVertexElementType::Float4, 1, sizeof(FVertex));
			Elements[2] = FVertexElement(0, offsetof(FVertex, Tangent), EVertexElementType::Float4, 2, sizeof(FVertex));
			for (uint8 I = 0; I < 4; ++I) Elements[3 + I] = FVertexElement(0, offsetof(FVertex, UV) + I * sizeof(FVector2f), EVertexElementType::Float2, 3 + I, sizeof(FVertex));
			Elements[7] = FVertexElement(0, offsetof(FVertex, Color), EVertexElementType::Float4, 7, sizeof(FVertex));
			Binding.Streams = {{0, Result.Vertices.Buffer, 0, sizeof(FVertex)}};
			if (bCustom)
			{
				const std::array<FVector4f, 4> InstanceData{{{0.5f,0,0,0}, {0.5f,0,0,0}, {0,0,0,0}, {0,0,0,0}}};
				auto InstanceDesc = FRHIBufferCreateDesc::Create("IndependentInstances", sizeof(InstanceData), sizeof(FVector4f), EBufferUsageFlags::VertexBuffer | EBufferUsageFlags::Static);
				InstanceDesc.InitialData = {InstanceData.data(), sizeof(InstanceData)};
				Result.Instances = {GDynamicRHI->RHICreateBuffer(CommandList, InstanceDesc), {sizeof(InstanceData), 0, sizeof(FVector4f), sizeof(FVector4f)}, Generation, Generation};
				Binding.Streams.push_back({1, Result.Instances.Buffer, 0, sizeof(FVector4f)});
				Elements[8] = FVertexElement(1, 0, EVertexElementType::Float4, 8, sizeof(FVector4f), FRHIVertexElementIdentity::EInputRate::Instance);
			}
			Binding.Declaration = GDynamicRHI->RHICreateVertexDeclaration(Elements);
			return Result;
		}
	};

	class FProceduralProxy final : public FPrimitiveSceneProxy
	{
	public:
		FProceduralGeometry Geometry;
		std::array<FMaterialRenderData, 2> Materials;
		bool bReverse = false;
		bool bFailShadow = false;
		auto GetLocalBounds() const -> FBox override { return FBox({-1,-1,0}, {1,1,1}); }
		auto CollectMeshBatches(const FMeshCollectionContext& Context, FMeshBatchCollector& Collector) const -> void override
		{
			if (bFailShadow && Context.Purpose == EMeshCollectionPurpose::Shadow)
			{
				Collector.RecordOutcome(EGeometrySubmissionOutcome::ResourceFailure);
				return;
			}
			for (uint64 I = 0; I < 2; ++I)
			{
				const uint64 Index = bReverse ? 1 - I : I;
				FMeshBatch Batch;
				Batch.PrimitiveId = Context.PrimitiveId;
				Batch.BatchId = (uint64{1} << 40) + Index;
				Batch.LocalToWorld = Context.LocalToWorld;
				Batch.WorldBounds = Context.WorldBounds;
				Batch.Binding = Geometry.Binding;
				Batch.FactoryKey = Geometry.Binding->GetFactoryKey();
				Batch.LayoutKey = Geometry.Binding->GetLayoutKey();
				Batch.bShadowCaster = Index == 0;
				FMeshBatchElement Element;
				Element.ElementId = (uint64{1} << 48) + Index;
				Element.Draw.bIndexed = Index == 0;
				Element.Draw.ElementCount = 3;
				Element.Draw.FirstElement = Index == 0 ? 0 : 3;
				Element.Draw.MaxVertexIndex = 2;
				Element.Draw.FirstInstance = 2;
				Element.Draw.InstanceCount = 2;
				Element.Vertices = Geometry.Vertices;
				Element.Indices = Geometry.Indices;
				if (Geometry.Instances.Buffer) Element.InstanceStreams.push_back(Geometry.Instances);
				Element.Material = Materials[Index];
				Element.LocalBounds = GetLocalBounds();
				Batch.Elements.push_back(std::move(Element));
				Collector.Add(std::move(Batch));
			}
		}
	};

	class FQualificationVertexShader final : public FMeshMaterialShader
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FQualificationVertexShader)
			DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
			DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Deformation);
		DURIN_END_SHADER_PARAMETERS();
		DURIN_DECLARE_MESH_MATERIAL_SHADER(FQualificationVertexShader, FMeshMaterialShader,
			"/GeometryQualification/Vertex", EShaderFrequency::Vertex, "VertexMain");
	};

	class FQualificationShaderBinding final : public RendererPrivate::FMeshVertexShaderBinding
	{
	public:
		explicit FQualificationShaderBinding(const FMaterialShaderMap& Map) : Shader(Map) {}
		auto GetRHIShader(bool bRequired) const -> FRHIShader* override { return Shader.GetRHIShader(bRequired); }
		auto Bind(FRHICommandListImmediate& CommandList, const FRHIUniformBufferRange& Transform,
			const FVertexFactoryBinding& Binding) const -> bool override
		{
			const auto* Typed = dynamic_cast<const FProceduralBinding*>(&Binding);
			if (!Typed || !Typed->bCustom) return false;
			FQualificationVertexShader::FParameters Parameters;
			Parameters.Transform = Transform;
			Parameters.Deformation = CommandList.AllocateDynamicUniformBuffer(&Typed->DisplacementScale, sizeof(Typed->DisplacementScale));
			SetShaderParameters(CommandList, Shader, Parameters);
			return true;
		}
	private:
		TMaterialShaderRef<FQualificationVertexShader> Shader;
	};

	class FQualificationFactory final : public RendererPrivate::FMeshVertexFactoryImplementation
	{
		bool bForwardOnly;
	public:
		explicit FQualificationFactory(bool InForwardOnly = false) : bForwardOnly(InForwardOnly) {}
		auto GetType() const -> const FVertexFactoryType& override
		{
			static FVertexFactoryType Type("QualificationVertexFactory");
			static FVertexFactoryType Forward("ForwardOnlyQualification");
			return bForwardOnly ? Forward : Type;
		}
		auto GetLayoutKey() const -> FXxHash64 override { FProceduralBinding Binding; Binding.bCustom = true; Binding.bForwardOnly = bForwardOnly; return Binding.GetLayoutKey(); }
		auto GetShaderType(uint32 Pass) const -> FShaderType* override { return Pass <= RendererPrivate::MaterialMeshPassShadow && !(bForwardOnly && Pass == RendererPrivate::MaterialMeshPassGBuffer) ? &FQualificationVertexShader::StaticType() : nullptr; }
		auto Resolve(const FMaterialShaderMap& Map, uint32 Pass) const -> std::shared_ptr<const RendererPrivate::FMeshVertexShaderBinding> override
		{
			return GetShaderType(Pass) ? std::make_shared<FQualificationShaderBinding>(Map) : nullptr;
		}
	};
}

namespace Durin::Tests
{
	inline constexpr std::string_view QualificationVertexSource = R"SLANG(
module GeometryQualification;
struct Input {
 float3 position: POSITION; float4 normal: NORMAL; float4 tangent: TANGENT;
 float2 uv0:TEXCOORD0; float2 uv1:TEXCOORD1; float2 uv2:TEXCOORD2; float2 uv3:TEXCOORD3; float4 color:COLOR; float4 instanceOffset:TEXCOORD4;
};
struct Output {
 float4 pos:SV_Position; float4 color:COLOR; float3 worldPosition:TEXCOORD0;
 float3 worldNormal:TEXCOORD1; float4 worldTangent:TEXCOORD2;
 float2 uv0:TEXCOORD3; float2 uv1:TEXCOORD4; float2 uv2:TEXCOORD5; float2 uv3:TEXCOORD6;
};
struct TransformData { float4x4 LocalToClip; float4x4 LocalToWorld; float4x4 NormalToWorld; float4 TransformParams; };
struct DeformationData { float4 displacementScale; };
[[vk::binding(0,0)]] ConstantBuffer<TransformData> Transform;
[[vk::binding(24,0)]] ConstantBuffer<DeformationData> Deformation;
[shader("vertex")]
Output VertexMain(Input input) {
 float3 p = input.position * Deformation.displacementScale.w + Deformation.displacementScale.xyz + input.instanceOffset.xyz;
 Output o; o.pos=mul(Transform.LocalToClip,float4(p,1)); o.color=input.color;
 o.worldPosition=mul(Transform.LocalToWorld,float4(p,1)).xyz;
 o.worldNormal=mul(Transform.NormalToWorld,float4(input.normal.xyz,0)).xyz;
 o.worldTangent=float4(mul(Transform.LocalToWorld,float4(input.tangent.xyz,0)).xyz,input.tangent.w*Transform.TransformParams.x);
 o.uv0=input.uv0; o.uv1=input.uv1; o.uv2=input.uv2; o.uv3=input.uv3; return o;
}
)SLANG";
}

namespace Durin::Tests
{
	class FDepthCaptureVertexShader final : public FGlobalShader
	{
	public:
		DURIN_DECLARE_GLOBAL_SHADER(FDepthCaptureVertexShader, FGlobalShader,
			"/GeometryQualification/Depth", EShaderFrequency::Vertex, "VertexMain");
	};
	class FDepthCaptureFragmentShader final : public FGlobalShader
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FDepthCaptureFragmentShader)
			DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);
		DURIN_END_SHADER_PARAMETERS();
		DURIN_DECLARE_GLOBAL_SHADER(FDepthCaptureFragmentShader, FGlobalShader,
			"/GeometryQualification/Depth", EShaderFrequency::Fragment, "FragmentMain");
	};
	class FShadowCaptureFragmentShader final : public FGlobalShader
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FShadowCaptureFragmentShader)
			DURIN_SHADER_PARAMETER_TEXTURE(ShadowDepth);
		DURIN_END_SHADER_PARAMETERS();
		DURIN_DECLARE_GLOBAL_SHADER(FShadowCaptureFragmentShader, FGlobalShader,
			"/GeometryQualification/Depth", EShaderFrequency::Fragment, "ShadowMain");
	};
	inline constexpr std::string_view DepthCaptureSource = R"SLANG(
[[vk::binding(0,0)]] Texture2D<float> SceneDepth;
[[vk::binding(1,0)]] Texture2DArray<float> ShadowDepth;
[shader("vertex")]
float4 VertexMain(uint id : SV_VertexID) : SV_Position {
 float2 p = float2((id << 1) & 2, id & 2);
 return float4(p * 2.0 - 1.0, 0, 1);
}
[shader("fragment")]
float FragmentMain(float4 p : SV_Position) : SV_Target0 {
 return SceneDepth.Load(int3(int2(p.xy), 0));
}
[shader("fragment")]
float ShadowMain(float4 p : SV_Position) : SV_Target0 {
 return ShadowDepth.Load(int4(int2(p.xy), 0, 0));
}
)SLANG";
	inline auto ReadGeometryTexture(FRHICommandListImmediate& CommandList, FRHITexture* Source, FByteBuffer& Pixels, bool bDepth = false) -> void
	{
		if (bDepth)
		{
			FShaderMapBase Map;
			const std::array<const FShaderType*, 2> Types{&FDepthCaptureVertexShader::StaticType(), Source->GetArraySize() > 1 ? static_cast<const FShaderType*>(&FShadowCaptureFragmentShader::StaticType()) : &FDepthCaptureFragmentShader::StaticType()};
			std::string Error;
			check(Map.InitializeFromShaderTypes(Types, {}, Error));
			const auto FloatTexture = GDynamicRHI->RHICreateTexture(CommandList,
				FRHITextureCreateDesc::Create2D("GeometryDepthReadback", Source->GetSizeX(), Source->GetSizeY(), EPixelFormat::R32_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::CPUReadback | ETextureCreateFlags::ShaderResource));
			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderTargetLayout = RenderTargetLayouts::MakeGBufferDebugOutput();
			Initializer.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format = EPixelFormat::R32_FLOAT;
			Initializer.BoundShaders.VertexShader = Map.GetOrCreateShaderRHI(Types[0]);
			Initializer.BoundShaders.FragmentShader = Map.GetOrCreateShaderRHI(Types[1]);
			Initializer.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration({});
			Initializer.RasterizerState.CullMode = ERHICullMode::None;
			Initializer.PipelineLayout = Map.GetMergedPipelineLayout();
			const auto Pipeline = GDynamicRHI->RHICreateGraphicsPipelineState("GeometryDepthCapture", Initializer);
			FRHIRenderPassInfo Pass;
			Pass.RenderTargetLayout = Initializer.RenderTargetLayout;
			Pass.ColorRenderTargets[0] = FloatTexture;
			CommandList.BeginRenderPass(Pass, "GeometryDepthCapture");
			CommandList.SetGraphicsPipelineState(*Pipeline);
			CommandList.SetViewport(0, 0, 0, Source->GetSizeX(), Source->GetSizeY(), 1);
			CommandList.SetScissor(0, 0, Source->GetSizeX(), Source->GetSizeY());
			if (Source->GetArraySize() > 1)
			{
				TShaderRef<FShadowCaptureFragmentShader> Fragment(static_cast<FShadowCaptureFragmentShader*>(Map.GetShader(Types[1])), &Map);
				FShadowCaptureFragmentShader::FParameters Parameters;
				Parameters.ShadowDepth = Source;
				SetShaderParameters(CommandList, Fragment, Parameters);
			}
			else
			{
				TShaderRef<FDepthCaptureFragmentShader> Fragment(static_cast<FDepthCaptureFragmentShader*>(Map.GetShader(Types[1])), &Map);
				FDepthCaptureFragmentShader::FParameters Parameters;
				Parameters.SceneDepth = Source;
				SetShaderParameters(CommandList, Fragment, Parameters);
			}
			CommandList.Draw(FRHIDrawArguments{.VertexCount = 3});
			CommandList.EndRenderPass();
			check(GDynamicRHI->RHIReadTexture2D(CommandList, FloatTexture, 0, 0, Pixels));
			return;
		}

		const auto Desc = FRHITextureCreateDesc::Create2D("GeometryReadback", Source->GetSizeX(), Source->GetSizeY(), Source->GetFormat())
			.SetFlags(ETextureCreateFlags::DestinationCopy | ETextureCreateFlags::CPUReadback | ETextureCreateFlags::ShaderResource);
		const auto Readback = GDynamicRHI->RHICreateTexture(CommandList, Desc);
		const FRHITextureSubresourceRange Whole{bDepth ? ERHITextureAspect::Depth : ERHITextureAspect::Color, 0, 1, 0, 1};
		CommandList.TransitionTextures(std::array{
			FRHITextureTransition{Source, Whole, ERHIAccess::GraphicsShaderRead, ERHIAccess::TransferRead},
			FRHITextureTransition{Readback, Whole, ERHIAccess::Discard, ERHIAccess::TransferWrite}});
		FRHITextureCopyRegion Region;
		Region.Extent = {Source->GetSizeX(), Source->GetSizeY(), 1};
		CommandList.CopyTexture(Source, Readback, std::array{Region});
		CommandList.TransitionTextures(std::array{
			FRHITextureTransition{Source, Whole, ERHIAccess::TransferRead, ERHIAccess::GraphicsShaderRead},
			FRHITextureTransition{Readback, Whole, ERHIAccess::TransferWrite, ERHIAccess::GraphicsShaderRead}});
		check(GDynamicRHI->RHIReadTexture2D(CommandList, Readback, 0, 0, Pixels));
	}
}
