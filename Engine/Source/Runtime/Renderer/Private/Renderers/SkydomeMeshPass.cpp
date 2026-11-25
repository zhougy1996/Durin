#include "Renderers/SkydomeMeshPass.h"

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
		class FSkydomeVertexShader : public FGlobalShader
		{
		public:
			DURIN_DECLARE_GLOBAL_SHADER(
				FSkydomeVertexShader,
				FGlobalShader,
				"/Engine/Skydome",
				EShaderFrequency::Vertex,
				"VertexMain"
			);
		};

		DURIN_IMPLEMENT_GLOBAL_SHADER(FSkydomeVertexShader);
		const FGlobalShaderSetRegistration GSkyBackgroundShaderSet(
			"Renderer", "Skydome.Default",
			EShaderRequestEligibility::GameAndEditor,
			{&FSkydomeVertexShader::StaticType()});
	} // namespace

	struct FSkydomeVertex
	{
		FVector3f Direction;
		FVector2f UV;
	};

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
		std::array<FVector4f, 3> Projection;
		std::vector<FRHIShaderParameterResource> Bindings;
		std::vector<TRefCountPtr<FRHIResource>> Resources;
	};

	struct FSkydomeMeshPass::FState
	{
		TShaderMapRef<FSkydomeVertexShader> VertexShader;
		FVertexDeclarationRHIRef VertexDeclaration;
		FBufferRHIRef VertexBuffer, IndexBuffer;
		uint32 IndexCount = 0;
		std::vector<std::shared_ptr<const FSkyMaterialPipeline>> Programs;
	};

	FSkydomeMeshPass::FSkydomeMeshPass(
		FDefaultTextureResources& InDefaultTextures, RendererPrivate::FSurfaceMaterialResources& InMaterials
	)
		: DefaultTextures(InDefaultTextures)
		, Materials(InMaterials)
		, State(std::make_unique<FState>())
	{
	}

	FSkydomeMeshPass::~FSkydomeMeshPass() = default;

	auto FSkydomeMeshPass::EnsureResources_RenderThread() -> bool
	{
		State->VertexShader = GetGlobalShaderMap().GetShader<FSkydomeVertexShader>();
		if (!State->VertexShader.GetRHIShader(false)) return false;
		if (!State->VertexDeclaration)
		{
			FVertexDeclarationElementList Elements{};
			Elements[0] = {0, offsetof(FSkydomeVertex, Direction), EVertexElementType::Float3, 0, sizeof(FSkydomeVertex)};
			Elements[1] = {0, offsetof(FSkydomeVertex, UV), EVertexElementType::Float2, 1, sizeof(FSkydomeVertex)};
			State->VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(Elements);
		}

		if (!State->VertexBuffer || !State->IndexBuffer)
		{
			constexpr uint32 Slices = 32, Rings = 16;
			constexpr double Pi = 3.14159265358979323846;
			std::array<FSkydomeVertex, (Slices + 1) * (Rings + 1)> Vertices;
			std::vector<uint32> Indices;
			for (uint32 Ring = 0; Ring <= Rings; ++Ring)
			{
				const double V = static_cast<double>(Ring) / Rings;
				const double Radius = Ring == 0 || Ring == Rings ? 0.0 : std::sin(Pi * V);
				for (uint32 Slice = 0; Slice <= Slices; ++Slice)
				{
					const double U = static_cast<double>(Slice) / Slices;
					const double Angle = Slice == Slices ? 0.0 : 2.0 * Pi * U;
					const uint32 A = Ring * (Slices + 1) + Slice;
					Vertices[A] = {FVector3f(Radius * std::cos(Angle), Radius * std::sin(Angle), std::cos(Pi * V)),
						FVector2f(U, V)};
					if (Ring == Rings || Slice == Slices) continue;
					const uint32 B = A + Slices + 1;
					if (Ring != 0) Indices.insert(Indices.end(), {A, B, A + 1});
					if (Ring + 1 != Rings) Indices.insert(Indices.end(), {A + 1, B, B + 1});
				}
			}
			auto VertexDesc = FRHIBufferCreateDesc::CreateVertex("SkydomeVertices", sizeof(Vertices));
			VertexDesc.Usage |= EBufferUsageFlags::Static;
			VertexDesc.InitialData = {Vertices.data(), sizeof(Vertices)};
			State->VertexBuffer = RHICreateBuffer(VertexDesc);
			State->IndexCount = static_cast<uint32>(Indices.size());
			const uint32 IndexBytes = State->IndexCount * sizeof(uint32);
			auto IndexDesc = FRHIBufferCreateDesc::CreateIndex("SkydomeIndices", IndexBytes, sizeof(uint32));
			IndexDesc.Usage |= EBufferUsageFlags::Static;
			IndexDesc.InitialData = {Indices.data(), IndexBytes};
			State->IndexBuffer = RHICreateBuffer(IndexDesc);
		}
		return State->VertexDeclaration && State->VertexBuffer && State->IndexBuffer;
	}

	auto FSkydomeMeshPass::PrepareMaterial_RenderThread(FRHICommandListImmediate& Commands, const FSceneView& View,
		FScene* Scene, FPreparedEnvironment& Environment) -> bool
	{
		using namespace RendererPrivate;
		const auto& Sky = Environment.SkyBox;
		const bool bProcedural = Sky.ProceduralSky.ToSunEnabled.w > 0.5f;
		const auto Proxy = Sky.MaterialOverride ? Sky.MaterialOverride : GetDefaultSkyMaterialProxy_RenderThread(bProcedural);
		if (!Proxy) return false;
		const auto& Data = Proxy->QueryMaterial_RenderThread();
		if (Data.Representation.IsPending())
		{
			FRenderPipelinePreparationBatch::MarkResourcePending();
			return false;
		}
		const auto Program = Data.CompiledProgram;
		if (!Program) return false;
		auto* Vertex = State->VertexShader.GetRHIShader(false);
		auto Found = std::ranges::find_if(State->Programs, [&](const auto& Pipeline) {
			return Pipeline->Program == Program && Pipeline->Vertex == Vertex;
		});
		if (Found == State->Programs.end())
		{
			const auto& Map = Program->GetShaderMap();
			const auto Entry = std::ranges::find_if(Map.Entries, [](const auto& E) {
				return E.Shader.ShaderType == "FSkydomeMaterialShader";
			});
			if (Entry == Map.Entries.end()) return false;
			const auto& Code = Map.CompiledShaders.at(Entry - Map.Entries.begin());
			auto Pipeline = std::make_shared<FSkyMaterialPipeline>();
			Pipeline->Program = Program;
			Pipeline->Layout = FCompiledSurfaceBindingLayout(Entry->Bindings);
			auto Desc = FRHIShaderCreateDesc::Create(Entry->Shader.ShaderType.c_str(), Code.Frequency, Code.Code, Code.CodeHash);
			Desc.SetEntryPoint(Code.BinaryEntryPoint.c_str());
			Pipeline->Vertex = Vertex;
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
			auto& Forward = Pipeline->Forward;
			Forward.RenderTargetLayout = RenderTargetLayouts::MakeGraphSceneTargets();
			Forward.BoundShaders.VertexShader = Pipeline->Vertex;
			Forward.BoundShaders.FragmentShader = Pipeline->Fragment;
			Forward.VertexDeclaration = Pipeline->Declaration;
			Forward.RasterizerState = RHICreateRasterizerState({.CullMode = ERHICullMode::None});
			Layout.PushConstantRanges.push_back({EShaderStageFlags::Vertex, 0, sizeof(FPreparedSkyMaterial::Projection)});
			Forward.PipelineLayout = Layout;
			Pipeline->Hybrid = Forward;
			Pipeline->Hybrid.RenderTargetLayout = RenderTargetLayouts::MakeGraphHybridSceneBootstrap();
			Pipeline->Hybrid.DepthStencilState = RHICreateDepthStencilState({
				.bEnableTest = true, .bEnableWrite = false, .CompareOp = ERHIDepthCompareOp::Equal});
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
		Draw->Projection = Uniform.DomeProjection;
		const auto MaterialBuffer = Payload.empty() ? FRHIUniformBufferRange{} : Commands.CreateUniformBufferRange(Payload.data(),Payload.size());
		const auto SkyBuffer = Commands.CreateUniformBufferRange(&Uniform.ProceduralSky, sizeof(Uniform.ProceduralSky));
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

	auto FSkydomeMeshPass::Draw_RenderThread(FRHICommandListImmediate& CommandList,
		const FPreparedEnvironment& Environment, bool bHybridBootstrap) -> bool
	{
		const auto& Draw = Environment.Draw;
		if (!Draw) return false;
		SetGraphicsPipelineState(CommandList, bHybridBootstrap ? Draw->Pipeline->Hybrid : Draw->Pipeline->Forward);
		CommandList.SetShaderParameters(Draw->Pipeline->Fragment, Draw->Bindings);
		CommandList.BindVertexBuffer(0, State->VertexBuffer, 0);
		CommandList.PushConstants(EShaderStageFlags::Vertex, 0, sizeof(Draw->Projection), Draw->Projection.data());
		CommandList.BindIndexBuffer(State->IndexBuffer, 0);
		CommandList.DrawIndexed(State->IndexCount, 0, 0);
		return true;
	}

	auto FSkydomeMeshPass::ReleaseResources_RenderThread() -> void
	{
		*State = {};
	}
} // namespace Durin
