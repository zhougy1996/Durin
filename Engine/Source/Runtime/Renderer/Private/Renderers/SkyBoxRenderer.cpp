#include "Renderers/SkyBoxRenderer.h"

#include "RenderPipelineCreation.h"
#include "Resources/DefaultTextureResources.h"
#include "Resources/RenderTargetLayouts.h"
#include "SkyBoxRendering.h"
#include "Scene.h"
#include "Renderers/MaterialBindingResolution.h"
#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneViewPreparation.h"
#include "Renderers/SurfaceMaterial.h"
#include "Materials/DefaultMaterialService.h"
#include "Materials/MaterialShaderMap.h"
#include "CoreGlobals.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "Shader/GlobalShader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	namespace
	{
		class FSkyBoxVertexShader : public FGlobalShader
		{
		public:
			DURIN_DECLARE_GLOBAL_SHADER(
				FSkyBoxVertexShader,
				FGlobalShader,
				"/Engine/SkyBox",
				EShaderFrequency::Vertex,
				"VertexMain"
			);
		};

		class FSkyBoxFragmentShader : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSkyBoxFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SkyTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(SkySampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Sky);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_GLOBAL_SHADER(
				FSkyBoxFragmentShader,
				FGlobalShader,
				"/Engine/SkyBox",
				EShaderFrequency::Fragment,
				"FragmentMain"
			);
		};
		DURIN_IMPLEMENT_GLOBAL_SHADER(FSkyBoxVertexShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FSkyBoxFragmentShader);
		const FGlobalShaderSetRegistration GSkyBoxShaderSet(
			"Renderer", "SkyBox.Default",
			EShaderRequestEligibility::GameAndEditor,
			{&FSkyBoxVertexShader::StaticType(),
			 &FSkyBoxFragmentShader::StaticType()});
	} // namespace

	struct FSkyMaterialPipeline
	{
		std::shared_ptr<const FMaterialProgram> Program;
		FShaderRHIRef Vertex, Fragment;
		FVertexDeclarationRHIRef Declaration;
		FGraphicsPipelineStateInitializer Forward, Hybrid;
		RendererPrivate::FCompiledSurfaceBindingLayout Layout;
	};

	struct FPreparedSkyMaterial
	{
		std::shared_ptr<const FSkyMaterialPipeline> Pipeline;
		std::vector<FRHIShaderParameterResource> Bindings;
		std::vector<TRefCountPtr<FRHIResource>> Resources;
	};

	struct FSkyBoxRenderer::FState
	{
		struct FPayload
		{
			TShaderMapRef<FSkyBoxVertexShader> VertexShader;
			TShaderMapRef<FSkyBoxFragmentShader> FragmentShader;
			FGraphicsPipelineStateInitializer PipelineInitializer;
			FGraphicsPipelineStateInitializer HybridBootstrapPipelineInitializer;
		};

		FVertexDeclarationRHIRef VertexDeclaration;
		FSamplerRHIRef Sampler;
		FBufferRHIRef IndexBuffer;
		std::optional<FPayload> Resources;
		std::vector<std::shared_ptr<const FSkyMaterialPipeline>> Programs;
	};

	FSkyBoxRenderer::FSkyBoxRenderer(
		FDefaultTextureResources& InDefaultTextures, RendererPrivate::FSurfaceMaterialResources& InMaterials
	)
		: DefaultTextures(InDefaultTextures)
		, Materials(InMaterials)
		, State(std::make_unique<FState>())
	{
	}

	FSkyBoxRenderer::~FSkyBoxRenderer() = default;

	auto FSkyBoxRenderer::EnsureResources_RenderThread() -> bool
	{
		using FPayload = FState::FPayload;

		const auto& Map = GetGlobalShaderMap();
		const auto Vertex = Map.GetShader<FSkyBoxVertexShader>();
		const auto Fragment = Map.GetShader<FSkyBoxFragmentShader>();
		State->Resources.reset();
		FPayload Candidate;
		Candidate.VertexShader = Vertex;
		Candidate.FragmentShader = Fragment;
		FRHIShader* VertexRHI =
			Candidate.VertexShader.GetRHIShader(false);
		FRHIShader* FragmentRHI =
			Candidate.FragmentShader.GetRHIShader(false);
		if (VertexRHI == nullptr || FragmentRHI == nullptr)
		{
			return false;
		}

		if (!State->VertexDeclaration)
			State->VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration({});

		FGraphicsPipelineStateInitializer Initializer;
		Initializer.RenderTargetLayout =
			RenderTargetLayouts::MakeGraphSceneTargets();
		Initializer.BoundShaders.VertexShader =
			VertexRHI;
		Initializer.BoundShaders.FragmentShader =
			FragmentRHI;
		Initializer.VertexDeclaration =
			State->VertexDeclaration;
		Initializer.RasterizerState = RHICreateRasterizerState({.CullMode = ERHICullMode::None});
		Initializer.PipelineLayout =
			Fragment.GetPipelineLayout();
		Candidate.PipelineInitializer = Initializer;
		Initializer.RenderTargetLayout =
			RenderTargetLayouts::MakeGraphHybridSceneBootstrap();
		Initializer.DepthStencilState = RHICreateDepthStencilState({
			.bEnableTest = true,
			.bEnableWrite = false,
			.CompareOp = ERHIDepthCompareOp::Equal});
		Candidate.HybridBootstrapPipelineInitializer = Initializer;

		if (!State->IndexBuffer)
		{
			const std::array<uint32, 3> FullscreenIndices = {0, 1, 2};
			FRHIBufferCreateDesc IndexBufferDesc =
				FRHIBufferCreateDesc::CreateIndex(
					"SkyBoxFullscreenIndexBuffer",
					sizeof(FullscreenIndices),
					sizeof(uint32)
				);
			IndexBufferDesc.Usage |= EBufferUsageFlags::Static;
			IndexBufferDesc.InitialData = {
				FullscreenIndices.data(),
				sizeof(FullscreenIndices)
			};
			State->IndexBuffer = RHICreateBuffer(IndexBufferDesc);
		}
		if (!State->Sampler) State->Sampler =
			RHICreateSampler(FRHISamplerDesc::LinearClamp());
		if (!State->VertexDeclaration || !State->IndexBuffer || !State->Sampler) return false;
		const auto& Payload = State->Resources.emplace(std::move(Candidate));
		if (FRenderPipelinePreparationBatch::ShouldPrecachePipelines())
		{
			auto PipelineState = PipelineStateCache::PrecacheGraphicsPipelineState(Payload.PipelineInitializer, "SkyBoxPipeline");
			if (!PipelineState) return false;
			FRenderPipelinePreparationBatch::Add(PipelineState);
			if (PipelineState->GetState() == ERHIPipelineState::Failed || PipelineState->GetState() == ERHIPipelineState::Canceled) return false;
			auto HybridBootstrapPipelineState = PipelineStateCache::PrecacheGraphicsPipelineState(Payload.HybridBootstrapPipelineInitializer, "SkyBoxHybridBootstrapPipeline");
			if (!HybridBootstrapPipelineState) return false;
			FRenderPipelinePreparationBatch::Add(HybridBootstrapPipelineState);
			if (HybridBootstrapPipelineState->GetState() == ERHIPipelineState::Failed || HybridBootstrapPipelineState->GetState() == ERHIPipelineState::Canceled) return false;
		}
		return true;
	}

	auto FSkyBoxRenderer::PrepareMaterial_RenderThread(FRHICommandListImmediate& Commands, const FSceneView& View,
		FScene* Scene, FPreparedEnvironment& Environment) -> bool
	{
		using namespace RendererPrivate;
		const auto& Sky = Environment.SkyBox;
		const bool bProcedural = Sky.ProceduralSky.ToSunEnabled.w > 0.5f;
		const auto Proxy = Sky.MaterialOverride ? Sky.MaterialOverride : GetDefaultSkyMaterialProxy_RenderThread(bProcedural);
		if (!Proxy) return false;
		const auto& Data = Proxy->QueryMaterial_RenderThread();
		const auto Program = Data.CompiledProgram;
		if (!Program) return false;
		auto Found = std::ranges::find_if(State->Programs, [&](const auto& P) { return P->Program == Program; });
		if (Found == State->Programs.end())
		{
			const auto& Map = Program->GetShaderMap();
			const auto Entry = std::ranges::find_if(Map.Entries, [](const auto& E) {
				return E.Shader.ShaderType == "FSkyBackgroundMaterialShader";
			});
			if (Entry == Map.Entries.end()) return false;
			const auto& Code = Map.CompiledShaders.at(Entry - Map.Entries.begin());
			auto Pipeline = std::make_shared<FSkyMaterialPipeline>();
			Pipeline->Program = Program;
			Pipeline->Layout = FCompiledSurfaceBindingLayout(Entry->Bindings);
			auto Desc = FRHIShaderCreateDesc::Create("FSkyBackgroundMaterialShader", Code.Frequency, Code.Code, Code.CodeHash);
			Desc.SetEntryPoint(Code.BinaryEntryPoint.c_str());
			Pipeline->Vertex = State->Resources->VertexShader.GetRHIShader(false);
			Pipeline->Declaration = State->VertexDeclaration;
			Pipeline->Fragment = GDynamicRHI->RHICreateShader(Desc);
			FPipelineLayoutDesc Layout;
			if (!Pipeline->Fragment || !Pipeline->Layout.IsValid() || !BuildPipelineLayoutFromShaders(std::span(&Code,1),Layout)) return false;
			for (const auto& Parameter : Entry->Bindings)
			{
				auto& Set = Layout.BindingLayouts.at(Parameter.SetIndex).BindingLayouts;
				auto Item = std::ranges::find(Set, Parameter.BindingIndex, &FBindingLayoutItem::Slot);
				if (Item == Set.end() || Item->Type != Parameter.Type) return false;
				Item->Type = Parameter.RuntimeType;
			}
			Pipeline->Forward = State->Resources->PipelineInitializer;
			Pipeline->Hybrid = State->Resources->HybridBootstrapPipelineInitializer;
			for (auto* Initializer : {&Pipeline->Forward, &Pipeline->Hybrid})
			{
				Initializer->BoundShaders.FragmentShader = Pipeline->Fragment;
				Initializer->PipelineLayout = Layout;
			}
			if (State->Programs.size() == 8) State->Programs.erase(State->Programs.begin());
			Found = State->Programs.insert(State->Programs.end(), std::move(Pipeline));
		}
		auto Draw = std::make_shared<FPreparedSkyMaterial>();
		Draw->Pipeline = *Found;
		if (FRenderPipelinePreparationBatch::ShouldPrecachePipelines())
		{
			for (auto* Initializer : {&Draw->Pipeline->Forward,&Draw->Pipeline->Hybrid})
			{
				auto Request = PipelineStateCache::PrecacheGraphicsPipelineState(*Initializer,"SkyBackgroundMaterial");
				if (!Request) return false;
				FRenderPipelinePreparationBatch::Add(Request);
				if (Request->GetState() == ERHIPipelineState::Failed || Request->GetState() == ERHIPipelineState::Canceled) return false;
			}
		}
		FMaterialRenderBinding Binding;
		FResolvedSurfaceMaterial Material;
		if (!ResolvePreparedMaterialBinding(Data, Binding, "SkyBackground")
			|| !Materials.Ensure_RenderThread(Binding, ESurfaceMaterialPass::Background)
			|| !Materials.Resolve_RenderThread(Binding, ESurfaceMaterialPass::Background, {}, nullptr, nullptr, Material)) return false;
		FByteBuffer Payload(Binding.CompiledUniformPayload.begin(), Binding.CompiledUniformPayload.end());
		if (!bProcedural && !Sky.MaterialOverride)
		{
			auto* Texture = Environment.Texture;
			if (!Texture && Sky.TextureReference) Texture = Sky.TextureReference->GetReferencedTexture_RenderThread();
			if (!Texture) Texture = DefaultTextures.GetCube_RenderThread();
			if (!Texture) return false;
			// Persistent parameter IDs from the DefaultCubeSky graph survive display-name edits and layout repacking.
			for (const auto& Field : Binding.CompiledLayout->Fields)
			{
				if (Field.ParameterId == FGuid{0x534b5942,2,1,1}) Material.CompiledTextures.at(Field.CompactIndex) = Texture;
				else if (Field.ParameterId == FGuid{0x534b5942,2,1,2})
				{
					const FVector4f Tint(Sky.Tint, 1.0f);
					if (Field.Offset + sizeof(Tint) > Payload.size()) return false;
					std::memcpy(Payload.data()+Field.Offset, &Tint, sizeof(Tint));
				}
				else if (Field.ParameterId == FGuid{0x534b5942,2,1,3})
				{
					if (Field.Offset + sizeof(float) > Payload.size()) return false;
					std::memcpy(Payload.data()+Field.Offset, &Sky.Intensity, sizeof(float));
				}
			}
		}
		SkyBoxRendering::FSkyBoxUniform Uniform;
		if (!SkyBoxRendering::BuildUniform(View, Sky, Uniform)) return false;
		const auto MaterialBuffer = Payload.empty() ? FRHIUniformBufferRange{} : Commands.CreateUniformBufferRange(Payload.data(),Payload.size());
		const auto SkyBuffer = Commands.CreateUniformBufferRange(&Uniform,sizeof(Uniform));
		const auto ViewBuffer = PrepareMeshViewUniform(Commands, View, false);
		std::vector<FRHIUniformBufferRange> Collections;
		for (const auto& Required : Program->ActiveCollections)
		{
			auto Snapshot = Scene ? Scene->GetMaterialParameterCollection_RenderThread(Required.CollectionId) : nullptr;
			if (Snapshot && !Snapshot->Layout.HasCompatibleSchema(Required)) return false;
			const FByteView Bytes = Snapshot ? FByteView(Snapshot->Payload) : FByteView(Required.DefaultPayload);
			Collections.push_back(Commands.CreateUniformBufferRange(Bytes.data(), Bytes.size()));
		}
		FMeshDrawShaderBindings Bindings;
		if (!PrepareCompiledSurfaceMaterial(Draw->Pipeline->Fragment, Draw->Pipeline->Layout, Material, MaterialBuffer,
			{}, {}, ViewBuffer, {}, Collections, Bindings, {}, {}, SkyBuffer)) return false;
		Draw->Bindings = std::move(Bindings.Parameters);
		for (const auto& Item : Draw->Bindings) if (Item.Resource) Draw->Resources.emplace_back(Item.Resource);
		Environment.Draw = std::move(Draw);
		return true;
	}

	auto FSkyBoxRenderer::Draw_RenderThread(FRHICommandListImmediate& CommandList, const FSceneView& View,
		const FPreparedEnvironment& Environment, bool bHybridBootstrap) -> bool
	{
		if (const auto& Draw = Environment.Draw)
		{
			SetGraphicsPipelineState(CommandList, bHybridBootstrap ? Draw->Pipeline->Hybrid : Draw->Pipeline->Forward);
			CommandList.SetShaderParameters(Draw->Pipeline->Fragment, Draw->Bindings);
			CommandList.BindIndexBuffer(State->IndexBuffer, 0);
			CommandList.DrawIndexed(3,0,0);
			return true;
		}
		auto* Texture = Environment.Texture;
		const auto& SkyBox = Environment.SkyBox;
		const auto& Payload = State->Resources;
		const auto* Pipeline = Payload ? &(bHybridBootstrap ? Payload->HybridBootstrapPipelineInitializer : Payload->PipelineInitializer) : nullptr;
		if (Texture == nullptr || !Payload
			|| !Pipeline
			|| State->Sampler == nullptr
			|| !Payload->VertexShader
			|| !Payload->FragmentShader
			|| State->IndexBuffer == nullptr)
		{
			return false;
		}

		SkyBoxRendering::FSkyBoxUniform Uniform;
		if (!SkyBoxRendering::BuildUniform(View, SkyBox, Uniform))
		{
			return false;
		}

		SetGraphicsPipelineState(CommandList, *Pipeline);
		CommandList.BindIndexBuffer(State->IndexBuffer, 0);
		FSkyBoxFragmentShader::FParameters Parameters;
		Parameters.SkyTexture = Texture;
		Parameters.SkySampler = State->Sampler;
		Parameters.Sky = CommandList.CreateUniformBufferRange(
			&Uniform,
			sizeof(Uniform)
		);
		SetShaderParameters(
			CommandList,
			Payload->FragmentShader,
			Parameters
		);
		CommandList.DrawIndexed(3, 0, 0);
		return true;
	}

	auto FSkyBoxRenderer::ReleaseResources_RenderThread() -> void
	{
		*State = {};
	}
} // namespace Durin
