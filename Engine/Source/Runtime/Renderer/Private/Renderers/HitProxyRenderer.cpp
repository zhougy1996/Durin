#include "Renderers/SceneRenderingService.h"
#include "Renderers/StaticMeshDrawExecution.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Shader/ShaderCookedLibrary.h"

namespace Durin
{
	using namespace RendererPrivate;
	namespace
	{
		class FHitProxyFragmentShader final : public FCompiledSurfaceMaterialShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FHitProxyFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(HitProxy);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(Material);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(MeshView);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_MATERIAL_SHADER(FHitProxyFragmentShader, FCompiledSurfaceMaterialShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Fragment, "HitProxyFragmentMain");
		};
		DURIN_IMPLEMENT_MATERIAL_SHADER(FHitProxyFragmentShader);
		const auto GHitProxyShaderRequest = [] {
			const FShaderType* Type = &FHitProxyFragmentShader::StaticType();
			FShaderRequestRegistration Registration;
			(void)RegisterShaderRuntimeRequest({.Category = EShaderRuntimeRequestCategory::FeatureProgram,
				.Owner = "HitProxy", .Name = "Surface.HitProxy"},
				EShaderRequestEligibility::EditorOnly, Registration, std::span(&Type, 1));
			return Registration;
		}();

		class FHitProxyOverlayVertexShader final : public FGlobalShader
		{
		public:
			DURIN_DECLARE_GLOBAL_SHADER(FHitProxyOverlayVertexShader, FGlobalShader,
				"/Engine/HitProxyOverlay", EShaderFrequency::Vertex, "VertexMain");
		};
		class FHitProxyOverlayFragmentShader final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FHitProxyOverlayFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Id);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_GLOBAL_SHADER(FHitProxyOverlayFragmentShader, FGlobalShader,
				"/Engine/HitProxyOverlay", EShaderFrequency::Fragment, "FragmentMain");
		};
		DURIN_IMPLEMENT_GLOBAL_SHADER(FHitProxyOverlayVertexShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FHitProxyOverlayFragmentShader);
		const FGlobalShaderSetRegistration GHitProxyOverlaySet("Renderer", "HitProxy.Overlay",
			EShaderRequestEligibility::EditorOnly,
			{&FHitProxyOverlayVertexShader::StaticType(), &FHitProxyOverlayFragmentShader::StaticType()});

		auto MakeHitProxyLayout() -> FRHIRenderTargetLayout
		{
			auto Layout = RenderTargetLayouts::MakeSceneTargets();
			Layout.ColorAttachments[0].RenderTarget.Format = EPixelFormat::RG32_UINT;
			return Layout;
		}

		struct FHitProxyDraw
		{
			const FPreparedStaticMeshPrimitive* Primitive = nullptr;
			const FPreparedStaticMeshDraw* Draw = nullptr;
			FMaterialShaderMap ShaderMap;
			std::shared_ptr<const FMeshVertexShaderBinding> Vertex;
			TMaterialShaderRef<FHitProxyFragmentShader> Fragment;
			FGraphicsPipelineStateRHIRef Pipeline;
			FPreparedStaticMeshSurfaceMaterial Material;
			FRHIUniformBufferRange Transform;
			FRHIUniformBufferRange Id;
			FPreparedSurfaceMaterialBindings FragmentBindings;
			std::shared_ptr<const FRHIShaderParameterBatch> VertexBindings;
		};
	}

	auto FSceneRenderingService::RenderHitProxies_RenderThread(FRHICommandListImmediate& Commands,
		FScene* Scene, const FHitProxyRenderRequest& Request) -> void
	{
		check(IsInRenderingThread());
		if (!Request.Readback || Request.Readback->GetState() != ERHITextureReadbackState::Pending) return;
		const auto Fail = [&] { Request.Readback->Fail(); };
		const auto& View = Request.View;
		const uint64 Width = uint64(View.ViewportX) + View.ViewportWidth;
		const uint64 Height = uint64(View.ViewportY) + View.ViewportHeight;
		if ((Request.bSceneGeometry && !Scene) || !View.ViewportWidth || !View.ViewportHeight || Width > 8192 || Height > 8192
			|| Width * Height > 8ull * 1024 * 1024 || Commands.IsInsideRenderPass()) { Fail(); return; }
		FViewRenderTelemetry Telemetry;
		FSceneVisibilityResult Visibility;
		if (Request.bSceneGeometry) PrepareSceneVisibility(*Scene, View, Telemetry, Visibility);
		const auto Prepared = PrepareStaticMeshView_RenderThread(Commands, Visibility.SceneInfos, View, ERasterMode::Solid);
		if (Prepared.bResourceFailure) { Fail(); return; }
		std::unordered_map<uint64, uint32> Ids;
		for (const auto& Entry : Request.Primitives)
		{
			if (!Entry.Id || !Entry.PrimitiveId || !Ids.emplace(Entry.PrimitiveId, Entry.Id.Value).second) { Fail(); return; }
		}
		const auto ViewUniform = PrepareMeshViewUniform(Commands, View, false);
		std::vector<FHitProxyDraw> Draws;
		bool bReady = true;
		ForEachBasePassBucket(Prepared, [&](const auto& Bucket, EMeshBasePass) {
			for (const auto& Draw : Bucket)
			{
				if (!bReady) break;
				FHitProxyDraw Item;
				Item.Primitive = Prepared.GetPrimitive(Draw);
				Item.Draw = &Draw;
				const auto Factory = FindMeshVertexFactory(Draw.Command->PipelineKey.FactoryKey);
				FMaterialRenderBinding Binding;
				if (!Item.Primitive || !Factory || !Factory->GetShaderType(MaterialMeshPassForward)
					|| !ResolvePreparedMaterialBinding(Draw.Command->Material, Binding, "HitProxyMaterial")) { bReady = false; break; }
				FShaderCompileOptions Options;
				const auto& Identity = Draw.Command->Material.PlanningPassIdentity.ShaderMap;
				Options.Macros.emplace_back("DURIN_MATERIAL_BLEND_MODE", std::to_string(static_cast<uint8>(Identity.BlendMode)));
				Options.Macros.emplace_back("DURIN_MATERIAL_SHADING_MODEL", std::to_string(static_cast<uint8>(Identity.ShadingModel)));
				Options.Macros.emplace_back("DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS", std::to_string(std::bit_cast<uint32>(Identity.OpacityMaskThreshold)));
				if (!InitializeMaterialShaderMap(*Factory->GetShaderType(MaterialMeshPassForward), FHitProxyFragmentShader::StaticType(),
					Factory->GetType(), MaterialMeshPassForward, Identity, Coordinator.GetGeneration_RenderThread(),
					Draw.Command->Material.CompiledProgram.get(), Options, Item.ShaderMap)) { bReady = false; break; }
				Item.Vertex = Factory->Resolve(Item.ShaderMap, MaterialMeshPassForward);
				Item.Fragment = TMaterialShaderRef<FHitProxyFragmentShader>(Item.ShaderMap);
				if (!Item.Vertex || !Item.Vertex->GetRHIShader(false) || !Item.Fragment.GetRHIShader(false)) { bReady = false; break; }
				if (!SurfaceMaterials.Ensure_RenderThread(Binding, ESurfaceMaterialPass::MaskedShadow)
					|| !FStaticMeshSurfaceMaterialPreparer(Commands, SurfaceMaterials, &Binding)
						.Prepare(ESurfaceMaterialPass::MaskedShadow, nullptr, nullptr, Item.Material)) { bReady = false; break; }
				FGraphicsPipelineStateInitializer Pipeline;
				Pipeline.RenderTargetLayout = MakeHitProxyLayout();
				Pipeline.BoundShaders.VertexShader = Item.Vertex->GetRHIShader();
				Pipeline.BoundShaders.FragmentShader = Item.Fragment.GetRHIShader();
				Pipeline.VertexDeclaration = Item.Primitive->CollectedBinding->Declaration;
				Pipeline.PrimitiveTopology = Draw.Command->PipelineKey.Topology;
				Pipeline.RasterizerState = Draw.Command->PipelineKey.Rasterizer;
				Pipeline.RasterizerState.PolygonMode = ERHIPolygonMode::Fill;
				Pipeline.DepthStencilState.bEnableTest = true;
				Pipeline.DepthStencilState.bEnableWrite = true;
				Pipeline.DepthStencilState.CompareOp = View.DepthConvention == ESceneDepthConvention::ReversedZ
					? ERHIDepthCompareOp::GreaterOrEqual : ERHIDepthCompareOp::LessOrEqual;
				Pipeline.PipelineLayout = Item.ShaderMap.GetPipelineLayout();
				Item.Pipeline = FRenderPipelineRequestScope::Graphics("HitProxy", Pipeline);
				if (!Item.Pipeline) { bReady = false; break; }
				Item.Transform = FStaticMeshPrimitiveUniformPreparer(Commands, View).Prepare(*Item.Primitive).Transform;
				const auto It = Ids.find(Item.Primitive->PrimitiveId.Value);
				struct FIdUniform { std::array<uint32, 4> Id; FVector4f ViewOrigin; };
				const FIdUniform Id{{It == Ids.end() ? 0u : It->second, 0, 0, 0}, FVector4f(FVector3f(View.ViewLocation), 0.f)};
				Item.Id = Commands.AllocateDynamicUniformBuffer(&Id, sizeof(Id));
				if (!PrepareCompiledSurfaceMaterial(Item.Fragment.GetRHIShader(false), Item.Fragment.GetShader()->GetSurfaceLayout(),
					Item.Material.Surface, Item.Material.Uniform, {}, Item.Id, ViewUniform, Item.FragmentBindings)) { bReady = false; break; }
				Item.VertexBindings = Item.Vertex->Prepare(Commands, Item.Transform, *Item.Primitive->CollectedBinding);
				if (!Item.VertexBindings) { bReady = false; break; }
				Draws.push_back(std::move(Item));
			}
		});
		if (!bReady) { Fail(); return; }
		if (Request.Overlays.size() > 65536) { Fail(); return; }
		FGlobalShaderSetRef OverlayShaders;
		TShaderMapRef<FHitProxyOverlayFragmentShader> OverlayFragment;
		FBufferRHIRef OverlayBuffer;
		std::array<FGraphicsPipelineStateRHIRef, 2> OverlayPipelines;
		std::vector<FRHIUniformBufferRange> OverlayIds;
		std::vector<uint32> OverlayOrder;
		if (!Request.Overlays.empty())
		{
			const std::array<const FGlobalShaderType*, 2> Types{&FHitProxyOverlayVertexShader::StaticType(), &FHitProxyOverlayFragmentShader::StaticType()};
			OverlayShaders = GetGlobalShaderMap().ResolveShaderSet("HitProxy.Overlay", Types, true, ReportRendererResourceCreateDiagnostic);
			if (!OverlayShaders) { Fail(); return; }
			TShaderMapRef<FHitProxyOverlayVertexShader> Vertex(OverlayShaders);
			OverlayFragment = TShaderMapRef<FHitProxyOverlayFragmentShader>(OverlayShaders);
			FVertexDeclarationElementList Elements{};
			Elements[0] = FVertexElement(0, 0, EVertexElementType::Float4, 0, sizeof(FHitProxyOverlayVertex));
			Elements[1] = FVertexElement(0, 16, EVertexElementType::Float1, 1, sizeof(FHitProxyOverlayVertex));
			const auto Declaration = GDynamicRHI->RHICreateVertexDeclaration(Elements);
			if (!Declaration) { Fail(); return; }
			for (uint32 Foreground = 0; Foreground < 2; ++Foreground)
			{
				FGraphicsPipelineStateInitializer Pipeline;
				Pipeline.RenderTargetLayout = MakeHitProxyLayout();
				Pipeline.BoundShaders.VertexShader = Vertex.GetRHIShader();
				Pipeline.BoundShaders.FragmentShader = OverlayFragment.GetRHIShader();
				Pipeline.VertexDeclaration = Declaration;
				Pipeline.RasterizerState.CullMode = ERHICullMode::None;
				Pipeline.DepthStencilState.bEnableTest = !Foreground;
				Pipeline.DepthStencilState.bEnableWrite = !Foreground;
				Pipeline.DepthStencilState.CompareOp = View.DepthConvention == ESceneDepthConvention::ReversedZ ? ERHIDepthCompareOp::GreaterOrEqual : ERHIDepthCompareOp::LessOrEqual;
				Pipeline.PipelineLayout = OverlayShaders.GetPipelineLayout();
				OverlayPipelines[Foreground] = FRenderPipelineRequestScope::Graphics("HitProxyOverlay", Pipeline);
				if (!OverlayPipelines[Foreground]) { Fail(); return; }
			}
			std::vector<FHitProxyOverlayVertex> Vertices;
			Vertices.reserve(Request.Overlays.size() * 6);
			for (const auto& Overlay : Request.Overlays)
			{
				if (!Overlay.Id) { Fail(); return; }
				for (const auto& Vertex : Overlay.Vertices)
					if (!Math::IsFinite(Vertex.ClipPosition) || !std::isfinite(Vertex.Distance) || Vertex.Distance < 0.f) { Fail(); return; }
				Vertices.insert(Vertices.end(), Overlay.Vertices.begin(), Overlay.Vertices.end());
				const std::array<uint32, 4> Id{Overlay.Id.Value, 0, 0, 0};
				OverlayIds.push_back(Commands.AllocateDynamicUniformBuffer(Id.data(), sizeof(Id)));
			}
			for (uint32 Index = 0; Index < Request.Overlays.size(); ++Index) OverlayOrder.push_back(Index);
			std::stable_sort(OverlayOrder.begin(), OverlayOrder.end(), [&](uint32 A, uint32 B) {
				return Request.Overlays[A].Priority < Request.Overlays[B].Priority;
			});
			const uint32 Bytes = uint32(Vertices.size() * sizeof(FHitProxyOverlayVertex));
			auto BufferDesc = FRHIBufferCreateDesc::CreateVertex("HitProxyOverlays", Bytes);
			BufferDesc.Usage |= EBufferUsageFlags::DestinationCopy;
			OverlayBuffer = GDynamicRHI->RHICreateBuffer(Commands, BufferDesc);
			if (!OverlayBuffer) { Fail(); return; }
			Commands.WriteBuffer(OverlayBuffer, Vertices.data(), Bytes, 0);
		}
		const auto Color = RHICreateTexture(FRHITextureCreateDesc::Create2D("HitProxyIds", uint32(Width), uint32(Height), EPixelFormat::RG32_UINT)
			.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback));
		const auto Depth = RHICreateTexture(FRHITextureCreateDesc::Create2D("HitProxyDepth", uint32(Width), uint32(Height), EPixelFormat::D32)
			.SetFlags(ETextureCreateFlags::DepthStencilTargetable));
		if (!Color || !Depth) { Fail(); return; }
		FRHIRenderPassInfo Pass;
		Pass.RenderTargetLayout = MakeHitProxyLayout();
		Pass.ColorRenderTargets[0] = Color;
		Pass.ColorClearValues[0] = FClearValueBinding(0.f, 0.f, 0.f, 0.f);
		Pass.DepthStencilRenderTarget = Depth;
		Pass.DepthStencilClearValue = FClearValueBinding(View.DepthConvention == ESceneDepthConvention::ReversedZ ? 0.f : 1.f, 0u);
		Commands.BeginRenderPass(Pass, "HitProxies");
		Commands.SetViewport(float(View.ViewportX), float(View.ViewportY), 0.f, float(Width), float(Height), 1.f);
		for (const auto& Item : Draws)
		{
			Commands.SetGraphicsPipelineState(*Item.Pipeline);
			FStaticMeshGeometryBinding Geometry(*Item.Primitive, *Item.Draw);
			Geometry.Bind(Commands);
			Commands.SetPreparedShaderParameters(Item.VertexBindings);
			if (!Item.FragmentBindings.Bind(Commands)) { bReady = false; break; }
			Geometry.DrawIndexed(Commands);
		}
		if (bReady && OverlayBuffer)
		{
			Commands.BindVertexBuffer(0, OverlayBuffer, 0);
			for (uint32 Foreground = 0; Foreground < 2; ++Foreground)
			{
				Commands.SetGraphicsPipelineState(*OverlayPipelines[Foreground]);
				for (const uint32 Index : OverlayOrder)
				{
					if (Request.Overlays[Index].bForeground != bool(Foreground)) continue;
					FHitProxyOverlayFragmentShader::FParameters Parameters;
					Parameters.Id = OverlayIds[Index];
					SetShaderParameters(Commands, OverlayFragment, Parameters);
					Commands.Draw({.VertexCount = 6, .FirstVertex = Index * 6});
				}
			}
		}
		Commands.EndRenderPass();
		if (!bReady) { Fail(); return; }
		Commands.EnqueueTextureReadback(Color, 0, 0, Request.Readback);
	}
}
