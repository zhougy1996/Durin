#include "Renderers/SceneRenderingService.h"
#include "Resources/RenderTargetLayouts.h"
#include "Renderers/MeshRendererShared.h"
#include "Materials/MaterialShaderMap.h"
#include "Renderers/MeshPipelineDescription.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/SceneViewPreparation.h"
#include "Renderers/SurfaceMaterial.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Shader/ShaderCookedLibrary.h"
#include "Logging/Logger.h"
#include "Materials/MaterialRenderProxy.h"
#include "Scene.h"
#include "CoreGlobals.h"

namespace Durin
{
	using namespace RendererPrivate;
	namespace
	{
		auto MakeHitProxyMapKey(FScene* Scene, const FHitProxyRenderRequest& Request, const FRenderResourceGeneration& RendererGeneration,
			bool bFrameDependent, FXxHash128 Materials, const FSceneRenderPreparationConfig& Config) -> FXxHash128
		{
			FXxHash128Builder Hash;
			const auto Add = [&]<typename T>(const T& Value) { Hash.UpdateValue(Value); };
			const auto Matrix = [&](const FMatrix& Value) {
				for (uint32 Column = 0; Column < 4; ++Column)
					for (uint32 Row = 0; Row < 4; ++Row) Add(Value[Column][Row]);
			};
			Add(reinterpret_cast<uintptr_t>(Scene));
			Add(Scene ? Scene->GetHitProxyRevision_RenderThread() : 0);
			Add(Materials);
			Add(Config.Version);
			Add(RendererGeneration.Shader); Add(RendererGeneration.Device); Add(RendererGeneration.Manual);
			Add(bFrameDependent);
			if (bFrameDependent) Add(GRenderFrameCounterRenderThread);
			auto View = Request.View;
			View.FeatureLevel = Config.FeatureLevel;
			Matrix(View.ViewMatrix); Matrix(View.ProjectionMatrix); Matrix(View.ViewProjectionMatrix);
			Add(View.ViewLocation.x); Add(View.ViewLocation.y); Add(View.ViewLocation.z);
			Add(View.ViewportX); Add(View.ViewportY); Add(View.ViewportWidth); Add(View.ViewportHeight);
			Add(View.MaterialTimeSeconds); Add(View.DepthConvention); Add(View.FeatureLevel);
			Add(View.AspectRatioConstraint); Add(View.bDiscardHistory);
			Add(View.NearClipDistance); Add(View.FarClipDistance); Add(View.ViewFadeStart); Add(View.ViewRenderDistance);
			Add(View.Settings.Mode.VisibilityMode); Add(View.Settings.Mode.LODMode);
			Add(View.Settings.Mode.TranslucentSortPolicy);
			Add(Request.bSceneGeometry); Add(Request.Primitives.size());
			for (const auto& Primitive : Request.Primitives) { Add(Primitive.PrimitiveId); Add(Primitive.Id.Value); }
			Add(Request.Overlays.size());
			for (const auto& Overlay : Request.Overlays)
			{
				Add(Overlay.Id.Value); Add(Overlay.Priority); Add(Overlay.bForeground);
				for (const auto& Vertex : Overlay.Vertices)
				{
					for (uint32 Axis = 0; Axis < 4; ++Axis) Add(Vertex.ClipPosition[Axis]);
					Add(Vertex.Distance);
				}
			}
			return Hash.Finalize();
		}

		class FHitProxyFragmentShader final : public FCompiledSurfaceMaterialShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FHitProxyFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(HitProxy);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(Material);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(MaterialView);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(MaterialPrimitive);
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
			Layout.ColorAttachments[0].RenderTarget.Format = FHitProxyPixel::Format;
			return Layout;
		}

		struct FHitProxyDraw : FMeshPassPipeline
		{
			const FPreparedStaticMeshPrimitive* Primitive = nullptr;
			const FPreparedStaticMeshDraw* Draw = nullptr;
			FPreparedSurfaceMaterial Material;
			FRHIUniformBufferRange Transform;
			FRHIUniformBufferRange Id;
			FMeshDrawCommandHandle Command;
		};
	}

	auto FSceneRenderingService::RenderHitProxies_RenderThread(FRHICommandListImmediate& Commands,
		FScene* Scene, const FHitProxyRenderRequest& Request) -> std::shared_ptr<const FHitProxyMap>
	{
		check(IsInRenderingThread());
		auto View = Request.View;
		View.FeatureLevel = Scene ? Scene->GetFeatureLevel() : PreparationConfigRenderThread.FeatureLevel;
		const auto Config = GetPreparationConfig_RenderThread(View.GetFeatureLevel());
		if (!Config.Version || !Config.Settings.bHitProxy) return {};
		const uint64 Width = uint64(View.ViewportX) + View.ViewportWidth;
		const uint64 Height = uint64(View.ViewportY) + View.ViewportHeight;
		if ((Request.bSceneGeometry && !Scene) || !View.ViewportWidth || !View.ViewportHeight || !FHitProxyMap::IsValidExtent(Width, Height) || Commands.IsInsideRenderPass()) { return {}; }
		auto& Preparations = Request.Cache ? Request.Cache->GetPreparations_RenderThread() : *Request.Preparations;
		FRenderPipelineRequestScope PreparationScope(Preparations, Coordinator.GetGeneration_RenderThread());
		struct FReleasePreparations
		{
			FRenderPipelineRequestScope& Scope;
			~FReleasePreparations() { if (!Scope.HasPending()) Scope.ReleasePreparations(); }
		} ReleasePreparations{PreparationScope};
		FViewRenderTelemetry Telemetry;
		FSceneVisibilityResult Visibility;
		if (Request.bSceneGeometry) PrepareSceneVisibility(*Scene, View, Telemetry, Visibility);
		const auto Prepared = PrepareStaticMeshView_RenderThread(Commands, Visibility.SceneInfos, View, ERasterMode::Solid);
		if (Prepared.bResourceFailure) { return {}; }
		FXxHash128Builder MaterialHash;
		std::vector<FTextureRHIRef> TextureLeases;
		ForEachBasePassBucket(Prepared, [&](const auto& Bucket, EMeshBasePass) {
			for (const auto& Draw : Bucket)
			{
				const auto& Representation = Draw.Command->Material.Representation;
				MaterialHash.UpdateValue(Representation.GetRecordId());
				for (const auto& Reference : Representation.GetResources())
				{
					FTextureRHIRef Texture = Reference ? Reference->GetReferencedTexture_RenderThread() : nullptr;
					MaterialHash.UpdateValue(reinterpret_cast<uintptr_t>(Texture.GetReference()));
					if (Texture) TextureLeases.push_back(std::move(Texture));
				}
			}
		});
		const auto MaterialKey = MaterialHash.Finalize();
		const auto CacheKey = MakeHitProxyMapKey(Scene, Request, Coordinator.GetGeneration_RenderThread(),
			Request.Cache && Request.Cache->IsFrameDependent_RenderThread(), MaterialKey, Config);
		if (!View.bDiscardHistory && Request.Cache)
			if (auto Map = Request.Cache->Find_RenderThread(CacheKey))
			{
				Request.Cache->GetPreparations_RenderThread().Reset();
				return Map;
			}
		std::unordered_map<uint64, uint32> Ids;
		for (const auto& Entry : Request.Primitives)
		{
			if (!Entry.Id || !Entry.PrimitiveId || !Ids.emplace(Entry.PrimitiveId, Entry.Id.Value).second) { return {}; }
		}
		const auto ViewUniform = PrepareMeshViewUniform(Commands, View, false);
		std::vector<FHitProxyDraw> Draws;
		bool bReady = true;
		bool bFrameDependent = false;
		FMeshDrawCommandBindingCache BindingCache;
		FDynamicMeshDrawCommandStorage DrawCommands;
		ForEachBasePassBucket(Prepared, [&](const auto& Bucket, EMeshBasePass) {
			for (const auto& Draw : Bucket)
			{
				if (!bReady) break;
				FHitProxyDraw Item;
				Item.Primitive = Prepared.GetPrimitive(Draw);
				Item.Draw = &Draw;
				const auto Factory = FindMeshVertexFactory(Draw.Command->PipelineKey.FactoryKey);
				FMaterialRenderBinding Binding;
				if (!Item.Primitive || !Factory
					|| !ResolvePreparedMaterialBinding(Draw.Command->Material, Binding, "HitProxyMaterial")) { bReady = false; break; }
				// Until time dependence is a separate material requirement, conservatively
				// expire all live view-dependent programs once per render frame.
				const auto Program = Draw.Command->Material.CompiledProgram;
				bFrameDependent |= View.MaterialTimeSeconds < 0.0 && Program && Program->GetShaderMap().Requirements.bMaterialView;
				for (const auto& Texture : Binding.CompiledTextures)
					if (Texture && Texture->GetReferencedTexture_RenderThread())
						bFrameDependent |= EnumHasAnyFlags(Texture->GetReferencedTexture_RenderThread()->GetFlags(), ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::DepthStencilTargetable | ETextureCreateFlags::Storage);
				const auto& Processor = FPassProcessorManager::GetMeshPassProcessor(EMeshPipelinePass::HitProxy);
				const FMeshPassProcessorBatch Batch{Draw.Command->Material.PlanningPassIdentity, *Factory,
					{Draw.Command->PipelineKey.Rasterizer, Draw.Command->PipelineKey.Depth, Draw.Command->PipelineKey.ColorBlend}, Draw.Command->PipelineKey.Topology, {}, Draw.Command->bSupportsGBuffer,
					false, View.DepthConvention == ESceneDepthConvention::ReversedZ};
				const auto ShaderCompletion = Processor.PrepareShaders(Batch, Coordinator.GetGeneration_RenderThread(), Program.get(), {}, Item);
				if (ShaderCompletion.Status == EShaderBuildStatus::Pending) { PreparationScope.MarkPending(); FRenderPipelinePreparationBatch::MarkResourcePending(); bReady = false; break; }
				if (ShaderCompletion.Status == EShaderBuildStatus::Canceled) { bReady = false; break; }
				if (!ShaderCompletion) { DURIN_ERROR("Hit proxy material shader failed: {}", std::string(ShaderCompletion.Error)); bReady = false; break; }
				if (!SurfaceMaterials.Ensure_RenderThread(Binding, ESurfaceMaterialPass::MaskedShadow)
					|| !SurfaceMaterials.Prepare_RenderThread(Commands, &Binding, ESurfaceMaterialPass::MaskedShadow, {}, nullptr, nullptr, Item.Material)) { bReady = false; break; }
				if (!Processor.PreparePipeline(Batch, "HitProxy", Item, Item.Primitive->CollectedBinding->Declaration, Item)) { bReady = false; break; }
				Item.Transform = PrepareMeshPrimitiveUniform(Commands, *Item.Primitive);
				const auto It = Ids.find(Item.Primitive->PrimitiveId.Value);
				struct FIdUniform { std::array<uint32, 4> Id; FVector4f ViewOrigin; };
				const FIdUniform Id{{It == Ids.end() ? 0u : It->second, 0, 0, 0}, FVector4f(0.f)};
				Item.Id = Commands.CreateUniformBufferRange(&Id, sizeof(Id));
				std::vector<FRHIUniformBufferRange> Collections;
				if (const auto Program = Draw.Command->Material.CompiledProgram)
					for (const auto& Required : Program->ActiveCollections)
					{
						FByteView Payload = Required.DefaultPayload;
						if (Scene)
							if (const auto Snapshot = Scene->GetMaterialParameterCollection_RenderThread(Required.CollectionId))
							{
								if (!Snapshot->Layout.HasCompatibleSchema(Required)) { bReady = false; break; }
								Payload = Snapshot->Payload;
							}
						if (Payload.size() != Required.UniformLayout.UniformPayloadSize) { bReady = false; break; }
						Collections.push_back(Commands.CreateUniformBufferRange(Payload.data(), Payload.size()));
					}
				if (!bReady) break;
				const FResolvedStaticMeshView::FMaterialUniform Material{Item.Material.Surface, Item.Material.Uniform, Collections};
				const FMeshDrawCommandBuildInput Input{Commands, *Item.Primitive, Draw,
					std::make_shared<const FMeshPassPipeline>(static_cast<const FMeshPassPipeline&>(Item)), Material,
					Item.Transform, ViewUniform, {}, Item.Id};
				FDynamicPassMeshDrawListContext BindingContext(DrawCommands, {EMeshPipelinePass::HitProxy}, Item.Command);
				if (!Processor.AddMeshBatch(Batch, Input, BindingCache, BindingContext)) { bReady = false; break; }
				Draws.push_back(std::move(Item));
			}
		});
		if (!bReady) { return {}; }
		if (Request.Overlays.size() > 65536) { return {}; }
		FGlobalShaderSetRef OverlayShaders;
		TShaderMapRef<FHitProxyOverlayFragmentShader> OverlayFragment;
		FBufferRHIRef OverlayBuffer;
		std::array<FGraphicsPipelineStateRHIRef, 2> OverlayPipelines;
		std::vector<FRHIUniformBufferRange> OverlayIds;
		std::vector<uint32> OverlayOrder;
		if (!Request.Overlays.empty())
		{
			const std::array<const FGlobalShaderType*, 2> Types{&FHitProxyOverlayVertexShader::StaticType(), &FHitProxyOverlayFragmentShader::StaticType()};
			const auto ShaderCompletion = GetGlobalShaderMap().ResolveShaderSet("HitProxy.Overlay", Types, true, ReportRendererResourceCreateDiagnostic);
			if (ShaderCompletion.Status == EShaderBuildStatus::Canceled) { return {}; }
			if (!ShaderCompletion) { return {}; }
			OverlayShaders = *ShaderCompletion;
			TShaderMapRef<FHitProxyOverlayVertexShader> Vertex(OverlayShaders);
			OverlayFragment = TShaderMapRef<FHitProxyOverlayFragmentShader>(OverlayShaders);
			FVertexDeclarationElementList Elements{};
			Elements[0] = FVertexElement(0, 0, EVertexElementType::Float4, 0, sizeof(FHitProxyOverlayVertex));
			Elements[1] = FVertexElement(0, 16, EVertexElementType::Float1, 1, sizeof(FHitProxyOverlayVertex));
			const auto Declaration = GDynamicRHI->RHICreateVertexDeclaration(Elements);
			if (!Declaration) { return {}; }
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
				if (!OverlayPipelines[Foreground]) { return {}; }
			}
			std::vector<FHitProxyOverlayVertex> Vertices;
			Vertices.reserve(Request.Overlays.size() * 6);
			for (const auto& Overlay : Request.Overlays)
			{
				if (!Overlay.Id) { return {}; }
				for (const auto& Vertex : Overlay.Vertices)
					if (!Math::IsFinite(Vertex.ClipPosition) || !std::isfinite(Vertex.Distance) || Vertex.Distance < 0.f) { return {}; }
				Vertices.insert(Vertices.end(), Overlay.Vertices.begin(), Overlay.Vertices.end());
				const std::array<uint32, 4> Id{Overlay.Id.Value, 0, 0, 0};
				OverlayIds.push_back(Commands.CreateUniformBufferRange(Id.data(), sizeof(Id)));
			}
			for (uint32 Index = 0; Index < Request.Overlays.size(); ++Index) OverlayOrder.push_back(Index);
			std::stable_sort(OverlayOrder.begin(), OverlayOrder.end(), [&](uint32 A, uint32 B) {
				return Request.Overlays[A].Priority < Request.Overlays[B].Priority;
			});
			const uint32 Bytes = uint32(Vertices.size() * sizeof(FHitProxyOverlayVertex));
			auto BufferDesc = FRHIBufferCreateDesc::CreateVertex("HitProxyOverlays", Bytes);
			BufferDesc.Usage |= EBufferUsageFlags::DestinationCopy;
			OverlayBuffer = GDynamicRHI->RHICreateBuffer(Commands, BufferDesc);
			if (!OverlayBuffer) { return {}; }
			Commands.WriteBuffer(OverlayBuffer, Vertices.data(), Bytes, 0);
			const std::array BufferTransitions{FRHIBufferTransition{
				OverlayBuffer, 0, Bytes, ERHIAccess::TransferWrite, ERHIAccess::VertexBufferRead}};
			Commands.TransitionBuffers(BufferTransitions);
		}
		const auto Color = RHICreateTexture(FRHITextureCreateDesc::Create2D("HitProxyIds", uint32(Width), uint32(Height), FHitProxyPixel::Format)
			.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::CPUReadback));
		const auto Depth = RHICreateTexture(FRHITextureCreateDesc::Create2D("HitProxyDepth", uint32(Width), uint32(Height), EPixelFormat::D32)
			.SetFlags(ETextureCreateFlags::DepthStencilTargetable));
		if (!Color || !Depth) { return {}; }
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
			const auto Command = DrawCommands.GetCommand(Item.Command);
			if (!Command || !Command->SubmitDraw(Commands)) { bReady = false; break; }
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
		if (!bReady) { return {}; }
		FByteBuffer Pixels;
		if (!Commands.ReadTexture2D(Color, 0, 0, Pixels)) return {};
		const auto Map = FHitProxyMap::Create(uint32(Width), uint32(Height), std::move(Pixels));
		if (!Map) return {};
		if (Request.Cache)
			Request.Cache->Publish_RenderThread(MakeHitProxyMapKey(Scene, Request, Coordinator.GetGeneration_RenderThread(), bFrameDependent, MaterialKey, Config), Map, bFrameDependent, std::move(TextureLeases));
		return Map;
	}
}

namespace Durin::RendererPrivate
{
	namespace
	{
		class FHitProxyPassMeshProcessor final : public FMeshPassProcessor
		{
		public:
			using FMeshPassProcessor::FMeshPassProcessor;
			auto AddMeshBatch(const FMeshPassProcessorBatch& Batch, const FMeshDrawCommandBuildInput& Input,
				FMeshDrawCommandBindingCache& Cache, FMeshPassDrawListContext& Context,
				std::shared_ptr<const FMeshDrawCommand> Existing) const -> FShaderCompletion override
			{ return BuildMeshDrawCommands(Batch, Input, Cache, Context, std::move(Existing)); }
			auto GetPass() const -> EMeshPipelinePass override { return EMeshPipelinePass::HitProxy; }
			auto IsEnabled(const FSceneRenderPreparationConfig& Config) const -> bool override { return Config.Settings.bHitProxy; }
			auto GetVertexFactoryPass(bool) const -> uint32 override { return MaterialMeshPassForward; }
			auto GetFragmentShader(FMaterialShaderBlendModeKey) const -> FMeshPassFragmentShader override
			{
				FMeshPassFragmentShader Shader{&FHitProxyFragmentShader::StaticType(), +[](const FMeshMaterialShaderMap& Map) { return TMaterialShaderRef<FHitProxyFragmentShader>(Map).GetRHIShader(false); }};
				Shader.FixedModule = "HitProxy"; Shader.FixedRequest = "Surface.HitProxy";
				Shader.ResolveLayout = +[](const FMeshMaterialShaderMap& Map) -> const FCompiledSurfaceBindingLayout* { return &TMaterialShaderRef<FHitProxyFragmentShader>(Map).GetShader()->GetSurfaceLayout(); };
				return Shader;
			}
			auto GetPSOPrecacheVariants(const FSceneRenderPreparationConfig& Config, const FPSOPrecacheParams& Params) const -> std::vector<FMeshPSOPrecacheVariant> override
			{ return RasterVariants(Config, Params, 4, false, true); }
			auto DescribeMeshDraw(const FMeshPipelineState& State, FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology,
				FMaterialShaderBlendModeKey, bool bReversedZ) const -> FMeshPipelineDescription override
			{
				FMeshPipelineDescription Result{.RenderTargets = RenderTargetLayouts::MakeSceneTargets(), .Topology = Topology, .Rasterizer = State.Rasterizer};
				Result.RenderTargets.ColorAttachments[0].RenderTarget.Format = FHitProxyPixel::Format;
				Result.Rasterizer.PolygonMode = ERHIPolygonMode::Fill;
				Result.Depth.bEnableTest = Result.Depth.bEnableWrite = true;
				Result.Depth.CompareOp = bReversedZ ? ERHIDepthCompareOp::GreaterOrEqual : ERHIDepthCompareOp::LessOrEqual;
				return Result;
			}
		};
		auto CreatePickingMeshPassProcessor(FMeshPSOCollectorContext Context) -> std::unique_ptr<FMeshPassProcessor>
		{ return std::make_unique<FHitProxyPassMeshProcessor>(Context); }
		const FRegisterPassProcessorCreateFunction RegisterPickingMeshPass(&CreatePickingMeshPassProcessor, EMeshPipelinePass::HitProxy);

	}
}
