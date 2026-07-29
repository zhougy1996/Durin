#include "RendererModule.h"
#include "Profiling/Profiling.h"

#include "CoreGlobals.h"
#include "DefaultTextures.h"
#include "EditorGridRendering.h"
#include "RendererEditorAssistance.h"
#include "RendererRenderTargetLayouts.h"
#include "SkyBoxRendering.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Texture/Texture2DRenderResource.h"
#include "Texture/TextureCubeRenderResource.h"

#include <glm/mat4x4.hpp>

namespace Durin
{
	namespace
	{
		struct FDefaultTextureState
		{
			FTextureRHIRef White;
			FTextureRHIRef Black;
			FTextureRHIRef FlatNormal;
			FTextureRHIRef BlackCube;
		};

		FDefaultTextureState GDefaultTextures;

		auto CreateSolidTexture(FRHICommandListImmediate& CommandList, const char* DebugName, const std::array<uint8, 4>& Color) -> FTextureRHIRef
		{
			FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(DebugName, 1, 1, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource);
			FTextureRHIRef Texture = GDynamicRHI->RHICreateTexture(CommandList, Desc);
			if (Texture != nullptr)
			{
				const FUpdateTextureRegion2D Region(0, 0, 0, 0, 1, 1);
				GDynamicRHI->RHIUpdateTexture2D(CommandList, Texture, 0, 0, Region, 4, Color.data());
			}
			return Texture;
		}

		auto CreateSolidCubeTexture(FRHICommandListImmediate& CommandList, const char* DebugName, const std::array<uint8, 4>& Color) -> FTextureRHIRef
		{
			FRHITextureCreateDesc Desc = FRHITextureCreateDesc::CreateCube(DebugName)
				.SetExtent(1)
				.SetFormat(EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource);
			FTextureRHIRef Texture = GDynamicRHI->RHICreateTexture(CommandList, Desc);
			if (Texture != nullptr)
			{
				const FUpdateTextureRegion2D Region(0, 0, 0, 0, 1, 1);
				for (uint32 ArraySlice = 0; ArraySlice < TextureCubeFaceCount; ++ArraySlice)
				{
					GDynamicRHI->RHIUpdateTexture2D(CommandList, Texture, 0, ArraySlice, Region, 4, Color.data());
				}
			}
			return Texture;
		}

		auto InitializeDefaultTextures_RenderThread(FRHICommandListImmediate& CommandList) -> void
		{
			check(IsInRenderingThread());
			if (GDefaultTextures.White != nullptr) return;
			GDefaultTextures.White = CreateSolidTexture(CommandList, "DefaultWhite", {255, 255, 255, 255});
			GDefaultTextures.Black = CreateSolidTexture(CommandList, "DefaultBlack", {0, 0, 0, 255});
			GDefaultTextures.FlatNormal = CreateSolidTexture(CommandList, "DefaultFlatNormal", {128, 128, 255, 255});
			GDefaultTextures.BlackCube = CreateSolidCubeTexture(CommandList, "DefaultBlackCube", {0, 0, 0, 255});
		}

		auto GetViewportOutput(bool bPresent) -> RendererRenderTargetLayouts::EViewportOutput
		{
			return bPresent ? RendererRenderTargetLayouts::EViewportOutput::Present : RendererRenderTargetLayouts::EViewportOutput::Offscreen;
		}

		class FStaticMeshVertexShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FStaticMeshVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(FStaticMeshVertexShader, FShader, "/Engine/StaticMesh", EShaderFrequency::Vertex, "VertexMain");
		};

		class FStaticMeshFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FStaticMeshFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Lighting);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Material);
				DURIN_SHADER_PARAMETER_TEXTURE(BaseColorTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(BaseColorSampler);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FStaticMeshFragmentShader, FShader, "/Engine/StaticMesh", EShaderFrequency::Fragment, "FragmentMain");
		};

		class FTextureCubeThumbnailVertexShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FTextureCubeThumbnailVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FTextureCubeThumbnailVertexShader, FShader, "/Engine/TextureCubeThumbnail", EShaderFrequency::Vertex, "VertexMain");
		};

		class FTextureCubeThumbnailFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FTextureCubeThumbnailFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
				DURIN_SHADER_PARAMETER_TEXTURE(CubeTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(CubeSampler);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FTextureCubeThumbnailFragmentShader, FShader, "/Engine/TextureCubeThumbnail", EShaderFrequency::Fragment, "FragmentMain");
		};

		class FGizmoVertexShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGizmoVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FGizmoVertexShader, FShader, "/Engine/Gizmo", EShaderFrequency::Vertex, "VertexMain");
		};

		class FGizmoFragmentShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FGizmoFragmentShader, FShader, "/Engine/Gizmo", EShaderFrequency::Fragment, "FragmentMain");
		};

		class FOverlayLineVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FOverlayLineVertexShader, FShader, "/Engine/Gizmo", EShaderFrequency::Vertex, "LineVertexMain");
		};

		class FOverlayLineFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FOverlayLineFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Style);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FOverlayLineFragmentShader, FShader, "/Engine/Gizmo", EShaderFrequency::Fragment, "LineFragmentMain");
		};

		class FOverlayIconVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FOverlayIconVertexShader, FShader, "/Engine/Gizmo", EShaderFrequency::Vertex, "IconVertexMain");
		};

		class FOverlayIconFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FOverlayIconFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(Atlas);
				DURIN_SHADER_PARAMETER_SAMPLER(AtlasSampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(IconStyle);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FOverlayIconFragmentShader, FShader, "/Engine/Gizmo", EShaderFrequency::Fragment, "IconFragmentMain");
		};

		class FEditorGridVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FEditorGridVertexShader, FShader, "/Engine/EditorGrid", EShaderFrequency::Vertex, "VertexMain");
		};

		class FEditorGridFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FEditorGridFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Grid);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FEditorGridFragmentShader, FShader, "/Engine/EditorGrid", EShaderFrequency::Fragment, "FragmentMain");
		};

		class FSkyBoxVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FSkyBoxVertexShader, FShader, "/Engine/SkyBox", EShaderFrequency::Vertex, "VertexMain");
		};

		class FSkyBoxFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSkyBoxFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SkyTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(SkySampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Sky);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FSkyBoxFragmentShader, FShader, "/Engine/SkyBox", EShaderFrequency::Fragment, "FragmentMain");
		};

		class FPostProcessVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FPostProcessVertexShader, FShader, "/Engine/PostProcess", EShaderFrequency::Vertex, "VertexMain");
		};

		class FCopySceneColorFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FCopySceneColorFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SceneColor);
				DURIN_SHADER_PARAMETER_SAMPLER(SceneColorSampler);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(FCopySceneColorFragmentShader, FShader, "/Engine/PostProcess", EShaderFrequency::Fragment, "CopyFragmentMain");
		};

		class FFXAAFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FFXAAFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SceneColor);
				DURIN_SHADER_PARAMETER_SAMPLER(SceneColorSampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(View);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(FFXAAFragmentShader, FShader, "/Engine/PostProcess", EShaderFrequency::Fragment, "FXAAFragmentMain");
		};

		struct FStaticMeshTransformUniform
		{
			glm::mat4 LocalToClip{1.0f};
			glm::mat4 LocalToWorld{1.0f};
			glm::mat4 NormalToWorld{1.0f};
			FVector4f TransformParams{1.0f, 0.0f, 0.0f, 0.0f};
		};

		struct FTextureCubeThumbnailTransformUniform
		{
			glm::mat4 LocalToClip{1.0f};
			glm::mat4 LocalToWorld{1.0f};
			glm::mat4 NormalToWorld{1.0f};
			FVector4f ViewPosition{0.0f};
		};

		struct FStaticMeshLightingUniform
		{
			FVector4f LightDirection{-0.5f, -0.5f, -1.0f, 0.0f};
			FVector4f LightColorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
			FVector4f ViewPositionAmbient{0.0f, 0.0f, 0.0f, 0.08f};
		};

		struct FStaticMeshMaterialUniform
		{
			FVector4f BaseColor{1.0f};
			FVector4f Params{0.35f, 32.0f, 1.0f, 0.0f};
		};

		struct FPostProcessViewUniform
		{
			FVector2f InvRenderTargetSize{1.0f, 1.0f};
			FVector2f Padding{0.0f, 0.0f};
		};

		struct FPostProcessVertex
		{
			FVector2f Position;
			FVector2f UV;
		};

		struct FGizmoTransformUniform
		{
			glm::mat4 LocalToClip{1.0f};
			FVector4f Color{1.0f};
		};

		struct FOverlayLineStyleUniform
		{
			float AlphaScale = 1.0f;
			FVector3f Padding{0.0f};
		};

		struct FOverlayLineVertex
		{
			FVector4f Position{0.0f};
			FVector4f Color{1.0f};
			FVector2f Pattern{0.0f};
		};

		struct FOverlayIconStyleUniform
		{
			float AlphaScale = 1.0f;
			FVector3f Padding{0.0f};
		};

		struct FOverlayIconVertex
		{
			FVector4f Position{0.0f};
			FVector2f UV{0.0f};
			FVector4f Color{1.0f};
		};

		struct FGizmoMeshRange
		{
			uint32 FirstIndex = 0;
			uint32 IndexCount = 0;
			int32 VertexOffset = 0;
		};

		struct FEditorAssistanceOutputPipelines
		{
			FGraphicsPipelineStateRHIRef Offscreen;
			FGraphicsPipelineStateRHIRef Present;
		};

		struct FGizmoRendererState
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FGizmoVertexShader> VertexShader;
			TShaderRef<FGizmoFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FEditorAssistanceOutputPipelines XRayOutputPipelines;
			FEditorAssistanceOutputPipelines VisibleOutputPipelines;
			FEditorAssistanceOutputPipelines WireXRayOutputPipelines;
			FEditorAssistanceOutputPipelines WireVisibleOutputPipelines;
			FBufferRHIRef VertexBuffer;
			FBufferRHIRef IndexBuffer;
			std::array<FGizmoMeshRange, 6> MeshRanges{};
			bool bCreateAttempted = false;
		};

		struct FOverlayLineRendererState
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FOverlayLineVertexShader> VertexShader;
			TShaderRef<FOverlayLineFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FEditorAssistanceOutputPipelines XRayOutputPipelines;
			FEditorAssistanceOutputPipelines VisibleOutputPipelines;
			FBufferRHIRef VertexBuffer;
			FBufferRHIRef IndexBuffer;
			uint32 VertexCapacity = 0;
			uint32 IndexCapacity = 0;
			uint32 IndexCount = 0;
			bool bCreateAttempted = false;
		};

		struct FOverlayIconRendererState
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FOverlayIconVertexShader> VertexShader;
			TShaderRef<FOverlayIconFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FEditorAssistanceOutputPipelines XRayOutputPipelines;
			FEditorAssistanceOutputPipelines VisibleOutputPipelines;
			FBufferRHIRef VertexBuffer;
			FBufferRHIRef IndexBuffer;
			FTextureRHIRef Atlas;
			FSamplerRHIRef AtlasSampler;
			uint32 VertexCapacity = 0;
			uint32 IndexCapacity = 0;
			uint32 IndexCount = 0;
			bool bCreateAttempted = false;
		};

		struct FEditorGridRendererState
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FEditorGridVertexShader> VertexShader;
			TShaderRef<FEditorGridFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FEditorAssistanceOutputPipelines OutputPipelines;
			bool bCreateAttempted = false;
		};

		struct FStaticMeshRendererState
		{
			struct FShaderMapEntry
			{
				FMaterialShaderMapIdentity Identity;
				std::shared_ptr<FShaderMapBase> ShaderMap;
				TShaderRef<FStaticMeshVertexShader> VertexShader;
				TShaderRef<FStaticMeshFragmentShader> FragmentShader;
			};

			struct FPipelineEntry
			{
				FMaterialPipelineIdentity Identity;
				std::shared_ptr<FShaderMapBase> ShaderMap;
				TShaderRef<FStaticMeshVertexShader> VertexShader;
				TShaderRef<FStaticMeshFragmentShader> FragmentShader;
				FGraphicsPipelineStateRHIRef SolidPipelineState;
				FGraphicsPipelineStateRHIRef WireframePipelineState;
			};

			FVertexDeclarationRHIRef VertexDeclaration;
			FSamplerRHIRef BaseColorSampler;
			std::vector<FShaderMapEntry> ShaderMapEntries;
			std::vector<FPipelineEntry> PipelineEntries;
			bool bBaseResourcesCreateAttempted = false;
		};

		struct FTextureCubeThumbnailRendererState
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FTextureCubeThumbnailVertexShader> VertexShader;
			TShaderRef<FTextureCubeThumbnailFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef PipelineState;
			FSamplerRHIRef Sampler;
			bool bCreateAttempted = false;
		};

		struct FSkyBoxRendererState
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FSkyBoxVertexShader> VertexShader;
			TShaderRef<FSkyBoxFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef PipelineState;
			FBufferRHIRef IndexBuffer;
			FSamplerRHIRef Sampler;
			bool bCreateAttempted = false;
		};

		struct FPostProcessRendererState
		{
			struct FSceneTargets
			{
				FTextureRHIRef Color;
				FTextureRHIRef Depth;
			};

			std::shared_ptr<FShaderMapBase> CopyShaderMap;
			std::shared_ptr<FShaderMapBase> FXAAShaderMap;
			TShaderRef<FPostProcessVertexShader> CopyVertexShader;
			TShaderRef<FPostProcessVertexShader> FXAAVertexShader;
			TShaderRef<FCopySceneColorFragmentShader> CopyFragmentShader;
			TShaderRef<FFXAAFragmentShader> FXAAFragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef CopyOffscreenPipelineState;
			FGraphicsPipelineStateRHIRef CopyPresentPipelineState;
			FGraphicsPipelineStateRHIRef FXAAOffscreenPipelineState;
			FGraphicsPipelineStateRHIRef FXAAPresentPipelineState;
			FBufferRHIRef VertexBuffer;
			FBufferRHIRef IndexBuffer;
			FSamplerRHIRef SceneColorSampler;
			std::unordered_map<uint64, FSceneTargets> SceneTargetsBySize;
			bool bCreateAttempted = false;
			std::atomic_bool bEnableFXAA = true;
		};

		FStaticMeshRendererState GStaticMeshState;
		FTextureCubeThumbnailRendererState GTextureCubeThumbnailState;
		FSkyBoxRendererState GSkyBoxState;
		FPostProcessRendererState GPostProcessState;
		FGizmoRendererState GGizmoState;
		FOverlayLineRendererState GOverlayLineState;
		FOverlayIconRendererState GOverlayIconState;
		FEditorGridRendererState GEditorGridState;
		std::atomic<ERenderMode> GRenderMode = ERenderMode::Lit;
		std::atomic<ERasterMode> GRasterMode = ERasterMode::Solid;
		bool GEditorAssistancePipelineFailureLogged = false;

		auto CreateEditorAssistanceOutputPipelines(
			FName OffscreenPipelineName,
			FName PresentPipelineName,
			FGraphicsPipelineStateInitializer Initializer
		) -> FEditorAssistanceOutputPipelines
		{
			Initializer.RenderTargetLayout = RendererRenderTargetLayouts::MakeEditorAssistanceOutput(
				RendererRenderTargetLayouts::EViewportOutput::Offscreen
			);
			FGraphicsPipelineStateRHIRef Offscreen = GDynamicRHI->RHICreateGraphicsPipelineState(OffscreenPipelineName, Initializer);
			Initializer.RenderTargetLayout = RendererRenderTargetLayouts::MakeEditorAssistanceOutput(
				RendererRenderTargetLayouts::EViewportOutput::Present
			);
			FGraphicsPipelineStateRHIRef Present = GDynamicRHI->RHICreateGraphicsPipelineState(PresentPipelineName, Initializer);
			return {.Offscreen = std::move(Offscreen), .Present = std::move(Present)};
		}

		auto AreEditorAssistanceOutputPipelinesReady() -> bool
		{
			auto IsReady = [](const FEditorAssistanceOutputPipelines& Pipelines) {
				return Pipelines.Offscreen != nullptr && Pipelines.Present != nullptr;
			};
			return IsReady(GEditorGridState.OutputPipelines)
				&& IsReady(GGizmoState.XRayOutputPipelines)
				&& IsReady(GGizmoState.VisibleOutputPipelines)
				&& IsReady(GGizmoState.WireXRayOutputPipelines)
				&& IsReady(GGizmoState.WireVisibleOutputPipelines)
				&& IsReady(GOverlayLineState.XRayOutputPipelines)
				&& IsReady(GOverlayLineState.VisibleOutputPipelines)
				&& IsReady(GOverlayIconState.XRayOutputPipelines)
				&& IsReady(GOverlayIconState.VisibleOutputPipelines);
		}

		auto GetEditorAssistanceOutputPipeline(const FEditorAssistanceOutputPipelines& Pipelines, bool bPresentOutput)
			-> FGraphicsPipelineStateRHIRef
		{
			return bPresentOutput ? Pipelines.Present : Pipelines.Offscreen;
		}

		auto BeginGizmoMesh(const std::vector<FVector3f>& Vertices, const std::vector<uint32>& Indices) -> FGizmoMeshRange
		{
			FGizmoMeshRange Range;
			Range.FirstIndex = static_cast<uint32>(Indices.size());
			Range.VertexOffset = 0;
			return Range;
		}

		auto EndGizmoMesh(FGizmoMeshRange& Range, const std::vector<uint32>& Indices) -> void
		{
			Range.IndexCount = static_cast<uint32>(Indices.size()) - Range.FirstIndex;
		}

		auto AppendCylinder(std::vector<FVector3f>& Vertices, std::vector<uint32>& Indices, float StartX, float EndX, float Radius, uint32 Segments) -> void
		{
			const uint32 Base = static_cast<uint32>(Vertices.size());
			for (uint32 Ring = 0; Ring < 2; ++Ring)
			{
				const float X = Ring == 0 ? StartX : EndX;
				for (uint32 Segment = 0; Segment < Segments; ++Segment)
				{
					const float Angle = glm::two_pi<float>() * static_cast<float>(Segment) / static_cast<float>(Segments);
					Vertices.emplace_back(X, std::cos(Angle) * Radius, std::sin(Angle) * Radius);
				}
			}
			for (uint32 Segment = 0; Segment < Segments; ++Segment)
			{
				const uint32 Next = (Segment + 1) % Segments;
				Indices.insert(Indices.end(), {Base + Segment, Base + Segments + Segment, Base + Segments + Next, Base + Segment, Base + Segments + Next, Base + Next});
			}
		}

		auto AppendCone(std::vector<FVector3f>& Vertices, std::vector<uint32>& Indices, float BaseX, float TipX, float Radius, uint32 Segments) -> void
		{
			const uint32 Base = static_cast<uint32>(Vertices.size());
			for (uint32 Segment = 0; Segment < Segments; ++Segment)
			{
				const float Angle = glm::two_pi<float>() * static_cast<float>(Segment) / static_cast<float>(Segments);
				Vertices.emplace_back(BaseX, std::cos(Angle) * Radius, std::sin(Angle) * Radius);
			}
			const uint32 Tip = static_cast<uint32>(Vertices.size());
			Vertices.emplace_back(TipX, 0.0f, 0.0f);
			for (uint32 Segment = 0; Segment < Segments; ++Segment)
			{
				const uint32 Next = (Segment + 1) % Segments;
				Indices.insert(Indices.end(), {Base + Segment, Tip, Base + Next});
			}
		}

		auto AppendBox(std::vector<FVector3f>& Vertices, std::vector<uint32>& Indices) -> void
		{
			const uint32 Base = static_cast<uint32>(Vertices.size());
			for (uint32 Corner = 0; Corner < 8; ++Corner)
			{
				Vertices.emplace_back((Corner & 1) ? 0.5f : -0.5f, (Corner & 2) ? 0.5f : -0.5f, (Corner & 4) ? 0.5f : -0.5f);
			}
			static constexpr uint32 BoxIndices[] = {0,2,3,0,3,1,4,5,7,4,7,6,0,1,5,0,5,4,2,6,7,2,7,3,0,4,6,0,6,2,1,3,7,1,7,5};
			for (uint32 Index : BoxIndices) Indices.push_back(Base + Index);
		}

		auto AppendWireBox(std::vector<FVector3f>& Vertices, std::vector<uint32>& Indices) -> void
		{
			const uint32 Base = static_cast<uint32>(Vertices.size());
			for (uint32 Corner = 0; Corner < 8; ++Corner)
			{
				Vertices.emplace_back((Corner & 1) ? 0.5f : -0.5f, (Corner & 2) ? 0.5f : -0.5f, (Corner & 4) ? 0.5f : -0.5f);
			}
			static constexpr uint32 BoxEdgeIndices[] = {0,1,0,2,0,4,1,3,1,5,2,3,2,6,3,7,4,5,4,6,5,7,6,7};
			for (uint32 Index : BoxEdgeIndices) Indices.push_back(Base + Index);
		}

		auto AppendPlane(std::vector<FVector3f>& Vertices, std::vector<uint32>& Indices) -> void
		{
			const uint32 Base = static_cast<uint32>(Vertices.size());
			Vertices.insert(Vertices.end(), {{0.0f,0.0f,0.0f},{1.0f,0.0f,0.0f},{1.0f,1.0f,0.0f},{0.0f,1.0f,0.0f}});
			Indices.insert(Indices.end(), {Base,Base+1,Base+2,Base,Base+2,Base+3,Base,Base+2,Base+1,Base,Base+3,Base+2});
		}

		auto AppendRing(std::vector<FVector3f>& Vertices, std::vector<uint32>& Indices, uint32 Segments) -> void
		{
			const uint32 Base = static_cast<uint32>(Vertices.size());
			constexpr uint32 TubeSegments = 8;
			constexpr float MajorRadius = 1.0f;
			constexpr float MinorRadius = 0.032f;
			for (uint32 Segment = 0; Segment < Segments; ++Segment)
			{
				const float Major = glm::two_pi<float>() * static_cast<float>(Segment) / static_cast<float>(Segments);
				for (uint32 Tube = 0; Tube < TubeSegments; ++Tube)
				{
					const float Minor = glm::two_pi<float>() * static_cast<float>(Tube) / static_cast<float>(TubeSegments);
					const float Radius = MajorRadius + std::cos(Minor) * MinorRadius;
					Vertices.emplace_back(std::sin(Minor) * MinorRadius, std::cos(Major) * Radius, std::sin(Major) * Radius);
				}
			}
			for (uint32 Segment = 0; Segment < Segments; ++Segment)
			{
				const uint32 NextSegment = (Segment + 1) % Segments;
				for (uint32 Tube = 0; Tube < TubeSegments; ++Tube)
				{
					const uint32 NextTube = (Tube + 1) % TubeSegments;
					const uint32 A = Base + Segment * TubeSegments + Tube;
					const uint32 B = Base + NextSegment * TubeSegments + Tube;
					const uint32 C = Base + NextSegment * TubeSegments + NextTube;
					const uint32 D = Base + Segment * TubeSegments + NextTube;
					Indices.insert(Indices.end(), {A,B,C,A,C,D});
				}
			}
		}

		auto EnsureGizmoResources(FRHICommandListImmediate& CommandList) -> void
		{
			if (GGizmoState.bCreateAttempted) return;
			GGizmoState.bCreateAttempted = true;

			FShaderCompileOptions CompileOptions;
			FShaderType& VertexShaderType = FGizmoVertexShader::StaticType();
			FShaderType& FragmentShaderType = FGizmoFragmentShader::StaticType();
			std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};
			auto ShaderMap = std::make_shared<FShaderMapBase>();
			std::string ErrorMessage;
			if (!ShaderMap->InitializeFromShaderTypes(ShaderTypes, CompileOptions, ErrorMessage))
			{
				DURIN_ERROR("Failed to initialize Gizmo shader map: {}", ErrorMessage);
				return;
			}
			auto* VertexShader = static_cast<FGizmoVertexShader*>(ShaderMap->GetShader(&VertexShaderType));
			auto* FragmentShader = static_cast<FGizmoFragmentShader*>(ShaderMap->GetShader(&FragmentShaderType));
			GGizmoState.ShaderMap = ShaderMap;
			GGizmoState.VertexShader = TShaderRef<FGizmoVertexShader>(VertexShader, ShaderMap.get());
			GGizmoState.FragmentShader = TShaderRef<FGizmoFragmentShader>(FragmentShader, ShaderMap.get());

			FVertexDeclarationElementList Elements;
			Elements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, sizeof(FVector3f));
			GGizmoState.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(Elements);
			FGraphicsPipelineStateInitializer Initializer;
			Initializer.BoundShaders.VertexShader = GGizmoState.VertexShader.GetRHIShader();
			Initializer.BoundShaders.FragmentShader = GGizmoState.FragmentShader.GetRHIShader();
			Initializer.VertexDeclaration = GGizmoState.VertexDeclaration;
			Initializer.bEnableAlphaBlend = true;
			Initializer.bEnableBackFaceCulling = false;
			Initializer.bEnableDepthWrite = false;
			Initializer.PipelineLayout = ShaderMap->GetMergedPipelineLayout();
			Initializer.bEnableDepthTest = false;
			GGizmoState.XRayOutputPipelines = CreateEditorAssistanceOutputPipelines(
				"GizmoXRayOffscreenPipeline", "GizmoXRayPresentPipeline", Initializer
			);
			Initializer.bEnableDepthTest = true;
			GGizmoState.VisibleOutputPipelines = CreateEditorAssistanceOutputPipelines(
				"GizmoVisibleOffscreenPipeline", "GizmoVisiblePresentPipeline", Initializer
			);
			Initializer.PrimitiveTopology = FGraphicsPipelineStateInitializer::EPrimitiveTopology::LineList;
			Initializer.bEnableDepthTest = false;
			GGizmoState.WireXRayOutputPipelines = CreateEditorAssistanceOutputPipelines(
				"GizmoWireXRayOffscreenPipeline", "GizmoWireXRayPresentPipeline", Initializer
			);
			Initializer.bEnableDepthTest = true;
			GGizmoState.WireVisibleOutputPipelines = CreateEditorAssistanceOutputPipelines(
				"GizmoWireVisibleOffscreenPipeline", "GizmoWireVisiblePresentPipeline", Initializer
			);

			std::vector<FVector3f> Vertices;
			std::vector<uint32> Indices;
			GGizmoState.MeshRanges[static_cast<size_t>(EViewOverlayShape::Arrow)] = BeginGizmoMesh(Vertices, Indices);
			AppendCylinder(Vertices, Indices, 0.0f, 0.76f, 0.032f, 12);
			AppendCone(Vertices, Indices, 0.72f, 1.0f, 0.085f, 12);
			EndGizmoMesh(GGizmoState.MeshRanges[static_cast<size_t>(EViewOverlayShape::Arrow)], Indices);
			GGizmoState.MeshRanges[static_cast<size_t>(EViewOverlayShape::Axis)] = BeginGizmoMesh(Vertices, Indices);
			AppendCylinder(Vertices, Indices, 0.0f, 0.94f, 0.032f, 12);
			EndGizmoMesh(GGizmoState.MeshRanges[static_cast<size_t>(EViewOverlayShape::Axis)], Indices);
			GGizmoState.MeshRanges[static_cast<size_t>(EViewOverlayShape::Plane)] = BeginGizmoMesh(Vertices, Indices);
			AppendPlane(Vertices, Indices);
			EndGizmoMesh(GGizmoState.MeshRanges[static_cast<size_t>(EViewOverlayShape::Plane)], Indices);
			GGizmoState.MeshRanges[static_cast<size_t>(EViewOverlayShape::Ring)] = BeginGizmoMesh(Vertices, Indices);
			AppendRing(Vertices, Indices, 64);
			EndGizmoMesh(GGizmoState.MeshRanges[static_cast<size_t>(EViewOverlayShape::Ring)], Indices);
			GGizmoState.MeshRanges[static_cast<size_t>(EViewOverlayShape::Box)] = BeginGizmoMesh(Vertices, Indices);
			AppendBox(Vertices, Indices);
			EndGizmoMesh(GGizmoState.MeshRanges[static_cast<size_t>(EViewOverlayShape::Box)], Indices);
			GGizmoState.MeshRanges[static_cast<size_t>(EViewOverlayShape::WireBox)] = BeginGizmoMesh(Vertices, Indices);
			AppendWireBox(Vertices, Indices);
			EndGizmoMesh(GGizmoState.MeshRanges[static_cast<size_t>(EViewOverlayShape::WireBox)], Indices);

			FRHIBufferCreateDesc VertexDesc = FRHIBufferCreateDesc::CreateVertex("GizmoVertexBuffer", static_cast<uint32>(Vertices.size() * sizeof(FVector3f)));
			VertexDesc.Usage |= EBufferUsageFlags::Static;
			VertexDesc.InitialData = {Vertices.data(), static_cast<uint32>(Vertices.size() * sizeof(FVector3f))};
			GGizmoState.VertexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, VertexDesc);
			FRHIBufferCreateDesc IndexDesc = FRHIBufferCreateDesc::CreateIndex("GizmoIndexBuffer", static_cast<uint32>(Indices.size() * sizeof(uint32)), sizeof(uint32));
			IndexDesc.Usage |= EBufferUsageFlags::Static;
			IndexDesc.InitialData = {Indices.data(), static_cast<uint32>(Indices.size() * sizeof(uint32))};
			GGizmoState.IndexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, IndexDesc);
		}

		auto EnsureOverlayLineResources() -> void
		{
			if (GOverlayLineState.bCreateAttempted) return;
			GOverlayLineState.bCreateAttempted = true;

			FShaderCompileOptions CompileOptions;
			FShaderType& VertexShaderType = FOverlayLineVertexShader::StaticType();
			FShaderType& FragmentShaderType = FOverlayLineFragmentShader::StaticType();
			std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};
			auto ShaderMap = std::make_shared<FShaderMapBase>();
			std::string ErrorMessage;
			if (!ShaderMap->InitializeFromShaderTypes(ShaderTypes, CompileOptions, ErrorMessage))
			{
				DURIN_ERROR("Failed to initialize overlay line shader map: {}", ErrorMessage);
				return;
			}
			GOverlayLineState.ShaderMap = ShaderMap;
			GOverlayLineState.VertexShader = TShaderRef<FOverlayLineVertexShader>(static_cast<FOverlayLineVertexShader*>(ShaderMap->GetShader(&VertexShaderType)), ShaderMap.get());
			GOverlayLineState.FragmentShader = TShaderRef<FOverlayLineFragmentShader>(static_cast<FOverlayLineFragmentShader*>(ShaderMap->GetShader(&FragmentShaderType)), ShaderMap.get());

			FVertexDeclarationElementList Elements;
			Elements[0] = FVertexElement(0, static_cast<uint8>(offsetof(FOverlayLineVertex, Position)), EVertexElementType::Float4, 0, sizeof(FOverlayLineVertex));
			Elements[1] = FVertexElement(0, static_cast<uint8>(offsetof(FOverlayLineVertex, Color)), EVertexElementType::Float4, 1, sizeof(FOverlayLineVertex));
			Elements[2] = FVertexElement(0, static_cast<uint8>(offsetof(FOverlayLineVertex, Pattern)), EVertexElementType::Float2, 2, sizeof(FOverlayLineVertex));
			GOverlayLineState.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(Elements);

			FGraphicsPipelineStateInitializer Initializer;
			Initializer.BoundShaders.VertexShader = GOverlayLineState.VertexShader.GetRHIShader();
			Initializer.BoundShaders.FragmentShader = GOverlayLineState.FragmentShader.GetRHIShader();
			Initializer.VertexDeclaration = GOverlayLineState.VertexDeclaration;
			Initializer.bEnableAlphaBlend = true;
			Initializer.bEnableBackFaceCulling = false;
			Initializer.bEnableDepthWrite = false;
			Initializer.PipelineLayout = ShaderMap->GetMergedPipelineLayout();
			Initializer.bEnableDepthTest = false;
			GOverlayLineState.XRayOutputPipelines = CreateEditorAssistanceOutputPipelines(
				"OverlayLineXRayOffscreenPipeline", "OverlayLineXRayPresentPipeline", Initializer
			);
			Initializer.bEnableDepthTest = true;
			GOverlayLineState.VisibleOutputPipelines = CreateEditorAssistanceOutputPipelines(
				"OverlayLineVisibleOffscreenPipeline", "OverlayLineVisiblePresentPipeline", Initializer
			);
		}

		auto BuildOverlayLineGeometry(const FSceneView& View, std::vector<FOverlayLineVertex>& OutVertices, std::vector<uint32>& OutIndices) -> void
		{
			constexpr double ClipEpsilon = 1.e-8;
			auto ClipSegment = [](FVector4& Start, FVector4& End) {
				auto ClipPlane = [&](auto PlaneDistance, double Minimum) {
					const double StartDistance = PlaneDistance(Start);
					const double EndDistance = PlaneDistance(End);
					if (StartDistance < Minimum && EndDistance < Minimum) return false;
					if (StartDistance < Minimum || EndDistance < Minimum)
					{
						const double T = (Minimum - StartDistance) / (EndDistance - StartDistance);
						const FVector4 Intersection = glm::mix(Start, End, T);
						if (StartDistance < Minimum) Start = Intersection;
						else End = Intersection;
					}
					return true;
				};
				// Durin's perspective projection uses z=0 for the near plane and w=view depth.
				return ClipPlane([](const FVector4& Value) { return Value.w; }, ClipEpsilon)
					&& ClipPlane([](const FVector4& Value) { return Value.z; }, 0.0);
			};
			for (const FViewOverlayLine& Line : View.OverlayLines)
			{
				FVector4 ClipStart = View.ViewProjectionMatrix * FVector4(Line.Start, 1.0);
				FVector4 ClipEnd = View.ViewProjectionMatrix * FVector4(Line.End, 1.0);
				if (!std::isfinite(ClipStart.w) || !std::isfinite(ClipEnd.w) || !std::isfinite(ClipStart.z) || !std::isfinite(ClipEnd.z)
					|| !ClipSegment(ClipStart, ClipEnd)) continue;
				const FVector2 NdcStart = FVector2(ClipStart) / ClipStart.w;
				const FVector2 NdcEnd = FVector2(ClipEnd) / ClipEnd.w;
				FVector2f PixelDelta{
					static_cast<float>((NdcEnd.x - NdcStart.x) * 0.5 * View.ViewportWidth),
					static_cast<float>((NdcEnd.y - NdcStart.y) * 0.5 * View.ViewportHeight)
				};
				const float PixelLength = glm::length(PixelDelta);
				if (!std::isfinite(PixelLength) || PixelLength <= 0.001f) continue;
				const FVector2f PixelNormal{-PixelDelta.y / PixelLength, PixelDelta.x / PixelLength};
				const float HalfWidth = std::max(0.5f, Line.WidthPixels * 0.5f);
				const FVector2 NdcOffset{
					static_cast<double>(PixelNormal.x * HalfWidth * 2.0f / std::max(1u, View.ViewportWidth)),
					static_cast<double>(PixelNormal.y * HalfWidth * 2.0f / std::max(1u, View.ViewportHeight))
				};
				auto MakePosition = [](const FVector4& Clip, const FVector2& Offset) {
					return FVector4f(
						static_cast<float>(Clip.x + Offset.x * Clip.w),
						static_cast<float>(Clip.y + Offset.y * Clip.w),
						static_cast<float>(Clip.z),
						static_cast<float>(Clip.w)
					);
				};
				const float PatternPeriod = Line.Pattern == EViewOverlayLinePattern::Dashed ? std::max(2.0f, Line.PatternPeriodPixels) : 0.0f;
				const uint32 Base = static_cast<uint32>(OutVertices.size());
				OutVertices.push_back({MakePosition(ClipStart, NdcOffset), Line.Color, {0.0f, PatternPeriod}});
				OutVertices.push_back({MakePosition(ClipStart, -NdcOffset), Line.Color, {0.0f, PatternPeriod}});
				OutVertices.push_back({MakePosition(ClipEnd, NdcOffset), Line.Color, {PixelLength, PatternPeriod}});
				OutVertices.push_back({MakePosition(ClipEnd, -NdcOffset), Line.Color, {PixelLength, PatternPeriod}});
				OutIndices.insert(OutIndices.end(), {Base, Base + 1, Base + 2, Base + 2, Base + 1, Base + 3});
			}
		}

		auto BuildEditorIconAtlasPixels() -> std::array<uint8, 128 * 64 * 4>
		{
			// A deterministic supersampled mask keeps this editor-only glyph crisp and avoids
			// introducing a separately licensed source image or a game-thread asset dependency.
			constexpr uint32 Size = 64;
			constexpr uint32 SamplesPerAxis = 4;
			constexpr uint32 AtlasWidth = Size * 2;
			std::array<uint8, AtlasWidth * Size * 4> Pixels{};
			auto InsideCircle = [](float X, float Y, float CenterX, float CenterY, float Radius) {
				const float DX = X - CenterX;
				const float DY = Y - CenterY;
				return DX * DX + DY * DY <= Radius * Radius;
			};
			auto InsideLens = [](float X, float Y) {
				if (X < 42.0f || X > 57.0f) return false;
				const float T = (X - 42.0f) / 15.0f;
				const float Top = glm::mix(29.0f, 20.0f, T);
				const float Bottom = glm::mix(43.0f, 52.0f, T);
				return Y >= Top && Y <= Bottom;
			};
			for (uint32 Y = 0; Y < Size; ++Y)
			{
				for (uint32 X = 0; X < Size; ++X)
				{
					uint32 CoveredSamples = 0;
					for (uint32 SampleY = 0; SampleY < SamplesPerAxis; ++SampleY)
					{
						for (uint32 SampleX = 0; SampleX < SamplesPerAxis; ++SampleX)
						{
							const float PX = static_cast<float>(X) + (static_cast<float>(SampleX) + 0.5f) / SamplesPerAxis;
							const float PY = static_cast<float>(Y) + (static_cast<float>(SampleY) + 0.5f) / SamplesPerAxis;
							const bool bBody = PX >= 9.0f && PX <= 44.0f && PY >= 27.0f && PY <= 49.0f;
							const bool bReels = InsideCircle(PX, PY, 19.0f, 21.0f, 10.0f) || InsideCircle(PX, PY, 37.0f, 20.0f, 9.0f);
							const bool bReelHole = InsideCircle(PX, PY, 19.0f, 21.0f, 3.5f) || InsideCircle(PX, PY, 37.0f, 20.0f, 3.0f);
							if ((bBody || bReels || InsideLens(PX, PY)) && !bReelHole) ++CoveredSamples;
						}
					}
					const size_t Offset = (static_cast<size_t>(Y) * AtlasWidth + X) * 4;
					Pixels[Offset + 0] = 255;
					Pixels[Offset + 1] = 255;
					Pixels[Offset + 2] = 255;
					Pixels[Offset + 3] = static_cast<uint8>(CoveredSamples * 255 / (SamplesPerAxis * SamplesPerAxis));
				}
			}
			// The second cell is a sun glyph: a solid disc plus eight separated rays.
			for (uint32 Y = 0; Y < Size; ++Y)
			{
				for (uint32 X = 0; X < Size; ++X)
				{
					uint32 CoveredSamples = 0;
					for (uint32 SampleY = 0; SampleY < SamplesPerAxis; ++SampleY)
					{
						for (uint32 SampleX = 0; SampleX < SamplesPerAxis; ++SampleX)
						{
							const float PX = static_cast<float>(X) + (static_cast<float>(SampleX) + 0.5f) / SamplesPerAxis - 32.0f;
							const float PY = static_cast<float>(Y) + (static_cast<float>(SampleY) + 0.5f) / SamplesPerAxis - 32.0f;
							const float Radius = std::sqrt(PX * PX + PY * PY);
							const float Angle = std::atan2(PY, PX);
							const float RayAxisDistance = std::abs(std::sin(Angle * 4.0f)) * Radius;
							const bool bDisc = Radius <= 12.0f;
							const bool bRay = Radius >= 17.0f && Radius <= 27.0f && RayAxisDistance <= 2.2f;
							if (bDisc || bRay) ++CoveredSamples;
						}
					}
					const size_t Offset = (static_cast<size_t>(Y) * AtlasWidth + Size + X) * 4;
					Pixels[Offset + 0] = 255;
					Pixels[Offset + 1] = 255;
					Pixels[Offset + 2] = 255;
					Pixels[Offset + 3] = static_cast<uint8>(CoveredSamples * 255 / (SamplesPerAxis * SamplesPerAxis));
				}
			}
			return Pixels;
		}

		auto EnsureOverlayIconResources(FRHICommandListImmediate& CommandList) -> void
		{
			if (GOverlayIconState.bCreateAttempted) return;
			GOverlayIconState.bCreateAttempted = true;

			FShaderCompileOptions CompileOptions;
			FShaderType& VertexShaderType = FOverlayIconVertexShader::StaticType();
			FShaderType& FragmentShaderType = FOverlayIconFragmentShader::StaticType();
			std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};
			auto ShaderMap = std::make_shared<FShaderMapBase>();
			std::string ErrorMessage;
			if (!ShaderMap->InitializeFromShaderTypes(ShaderTypes, CompileOptions, ErrorMessage))
			{
				DURIN_ERROR("Failed to initialize overlay icon shader map: {}", ErrorMessage);
				return;
			}
			GOverlayIconState.ShaderMap = ShaderMap;
			GOverlayIconState.VertexShader = TShaderRef<FOverlayIconVertexShader>(static_cast<FOverlayIconVertexShader*>(ShaderMap->GetShader(&VertexShaderType)), ShaderMap.get());
			GOverlayIconState.FragmentShader = TShaderRef<FOverlayIconFragmentShader>(static_cast<FOverlayIconFragmentShader*>(ShaderMap->GetShader(&FragmentShaderType)), ShaderMap.get());

			FVertexDeclarationElementList Elements;
			Elements[0] = FVertexElement(0, static_cast<uint8>(offsetof(FOverlayIconVertex, Position)), EVertexElementType::Float4, 0, sizeof(FOverlayIconVertex));
			Elements[1] = FVertexElement(0, static_cast<uint8>(offsetof(FOverlayIconVertex, UV)), EVertexElementType::Float2, 1, sizeof(FOverlayIconVertex));
			Elements[2] = FVertexElement(0, static_cast<uint8>(offsetof(FOverlayIconVertex, Color)), EVertexElementType::Float4, 2, sizeof(FOverlayIconVertex));
			GOverlayIconState.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(Elements);

			FGraphicsPipelineStateInitializer Initializer;
			Initializer.BoundShaders.VertexShader = GOverlayIconState.VertexShader.GetRHIShader();
			Initializer.BoundShaders.FragmentShader = GOverlayIconState.FragmentShader.GetRHIShader();
			Initializer.VertexDeclaration = GOverlayIconState.VertexDeclaration;
			Initializer.bEnableAlphaBlend = true;
			Initializer.bEnableBackFaceCulling = false;
			Initializer.bEnableDepthWrite = false;
			Initializer.PipelineLayout = ShaderMap->GetMergedPipelineLayout();
			Initializer.bEnableDepthTest = false;
			GOverlayIconState.XRayOutputPipelines = CreateEditorAssistanceOutputPipelines(
				"OverlayIconXRayOffscreenPipeline", "OverlayIconXRayPresentPipeline", Initializer
			);
			Initializer.bEnableDepthTest = true;
			GOverlayIconState.VisibleOutputPipelines = CreateEditorAssistanceOutputPipelines(
				"OverlayIconVisibleOffscreenPipeline", "OverlayIconVisiblePresentPipeline", Initializer
			);

			const std::array<uint8, 128 * 64 * 4> Pixels = BuildEditorIconAtlasPixels();
			FRHITextureCreateDesc TextureDesc = FRHITextureCreateDesc::Create2D("EditorOverlayIconAtlas", 128, 64, EPixelFormat::RGBA8_UNORM)
				.SetFlags(ETextureCreateFlags::ShaderResource);
			GOverlayIconState.Atlas = GDynamicRHI->RHICreateTexture(CommandList, TextureDesc);
			if (GOverlayIconState.Atlas != nullptr)
			{
				const FUpdateTextureRegion2D Region(0, 0, 0, 0, 128, 64);
				GDynamicRHI->RHIUpdateTexture2D(CommandList, GOverlayIconState.Atlas, 0, 0, Region, 128 * 4, Pixels.data());
			}
			GOverlayIconState.AtlasSampler = RHICreateSampler(FRHISamplerDesc::LinearClamp());
		}

		auto EnsureEditorGridResources() -> void
		{
			if (GEditorGridState.bCreateAttempted) return;
			GEditorGridState.bCreateAttempted = true;

			FShaderCompileOptions CompileOptions;
			FShaderType& VertexShaderType = FEditorGridVertexShader::StaticType();
			FShaderType& FragmentShaderType = FEditorGridFragmentShader::StaticType();
			std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};
			auto ShaderMap = std::make_shared<FShaderMapBase>();
			std::string ErrorMessage;
			if (!ShaderMap->InitializeFromShaderTypes(ShaderTypes, CompileOptions, ErrorMessage))
			{
				DURIN_ERROR("Failed to initialize editor grid shader map: {}", ErrorMessage);
				return;
			}
			GEditorGridState.ShaderMap = ShaderMap;
			GEditorGridState.VertexShader = TShaderRef<FEditorGridVertexShader>(static_cast<FEditorGridVertexShader*>(ShaderMap->GetShader(&VertexShaderType)), ShaderMap.get());
			GEditorGridState.FragmentShader = TShaderRef<FEditorGridFragmentShader>(static_cast<FEditorGridFragmentShader*>(ShaderMap->GetShader(&FragmentShaderType)), ShaderMap.get());

			// Reuse the renderer's fullscreen triangle buffer; only POSITION is consumed here.
			FVertexDeclarationElementList Elements;
			Elements[0] = FVertexElement(0, offsetof(FPostProcessVertex, Position), EVertexElementType::Float2, 0, sizeof(FPostProcessVertex));
			GEditorGridState.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(Elements);

			FGraphicsPipelineStateInitializer Initializer;
			Initializer.BoundShaders.VertexShader = GEditorGridState.VertexShader.GetRHIShader();
			Initializer.BoundShaders.FragmentShader = GEditorGridState.FragmentShader.GetRHIShader();
			Initializer.VertexDeclaration = GEditorGridState.VertexDeclaration;
			Initializer.bEnableAlphaBlend = true;
			Initializer.bEnableBackFaceCulling = false;
			Initializer.bEnableDepthTest = true;
			Initializer.bEnableDepthWrite = false;
			Initializer.PipelineLayout = ShaderMap->GetMergedPipelineLayout();
			GEditorGridState.OutputPipelines = CreateEditorAssistanceOutputPipelines(
				"EditorWorldGridOffscreenPipeline", "EditorWorldGridPresentPipeline", Initializer
			);
		}

		auto EnsureSkyBoxResources() -> void
		{
			if (GSkyBoxState.bCreateAttempted) return;
			GSkyBoxState.bCreateAttempted = true;

			FShaderCompileOptions CompileOptions;
			FShaderType& VertexShaderType = FSkyBoxVertexShader::StaticType();
			FShaderType& FragmentShaderType = FSkyBoxFragmentShader::StaticType();
			std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};
			auto ShaderMap = std::make_shared<FShaderMapBase>();
			std::string ErrorMessage;
			if (!ShaderMap->InitializeFromShaderTypes(ShaderTypes, CompileOptions, ErrorMessage))
			{
				DURIN_ERROR("Failed to initialize SkyBox shader map: {}", ErrorMessage);
				return;
			}

			GSkyBoxState.ShaderMap = ShaderMap;
			GSkyBoxState.VertexShader = TShaderRef<FSkyBoxVertexShader>(
				static_cast<FSkyBoxVertexShader*>(ShaderMap->GetShader(&VertexShaderType)), ShaderMap.get());
			GSkyBoxState.FragmentShader = TShaderRef<FSkyBoxFragmentShader>(
				static_cast<FSkyBoxFragmentShader*>(ShaderMap->GetShader(&FragmentShaderType)), ShaderMap.get());

			// The sky vertex shader derives the fullscreen triangle from SV_VertexID.
			// Vulkan still requires a vertex declaration object, but it has no elements
			// and no vertex buffer is bound.
			FVertexDeclarationElementList EmptyVertexElements{};
			GSkyBoxState.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(EmptyVertexElements);

			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderTargetLayout = RendererRenderTargetLayouts::MakeSceneTargets();
			Initializer.BoundShaders.VertexShader = GSkyBoxState.VertexShader.GetRHIShader();
			Initializer.BoundShaders.FragmentShader = GSkyBoxState.FragmentShader.GetRHIShader();
			Initializer.VertexDeclaration = GSkyBoxState.VertexDeclaration;
			Initializer.bEnableAlphaBlend = false;
			Initializer.bEnableBackFaceCulling = false;
			Initializer.bEnableDepthTest = false;
			Initializer.bEnableDepthWrite = false;
			Initializer.PipelineLayout = ShaderMap->GetMergedPipelineLayout();
			GSkyBoxState.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState("SkyBoxPipeline", Initializer);
			const std::array<uint32, 3> FullscreenIndices = {0, 1, 2};
			FRHIBufferCreateDesc IndexBufferDesc = FRHIBufferCreateDesc::CreateIndex(
				"SkyBoxFullscreenIndexBuffer", sizeof(FullscreenIndices), sizeof(uint32));
			IndexBufferDesc.Usage |= EBufferUsageFlags::Static;
			IndexBufferDesc.InitialData = {FullscreenIndices.data(), sizeof(FullscreenIndices)};
			GSkyBoxState.IndexBuffer = RHICreateBuffer(IndexBufferDesc);
			GSkyBoxState.Sampler = RHICreateSampler(FRHISamplerDesc::LinearClamp());
		}

		auto EnsureStaticMeshBaseResources() -> bool
		{
			if (GStaticMeshState.bBaseResourcesCreateAttempted)
			{
				return GStaticMeshState.VertexDeclaration != nullptr
					&& GStaticMeshState.BaseColorSampler != nullptr;
			}

			GStaticMeshState.bBaseResourcesCreateAttempted = true;

			const FVertexDeclarationElementList VertexDeclElements =
				GetStaticMeshVertexDeclarationElements();
			GStaticMeshState.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(VertexDeclElements);
			GStaticMeshState.BaseColorSampler = RHICreateSampler(FRHISamplerDesc::LinearRepeat());
			return GStaticMeshState.VertexDeclaration != nullptr
				&& GStaticMeshState.BaseColorSampler != nullptr;
		}

		auto GetOrCreateStaticMeshShaderMap(
			const FMaterialShaderMapIdentity& Identity
		) -> FStaticMeshRendererState::FShaderMapEntry*
		{
			const auto Existing = std::ranges::find(
				GStaticMeshState.ShaderMapEntries,
				Identity,
				&FStaticMeshRendererState::FShaderMapEntry::Identity);
			if (Existing != GStaticMeshState.ShaderMapEntries.end())
			{
				return Existing->ShaderMap != nullptr
					&& Existing->VertexShader
					&& Existing->FragmentShader
					? &*Existing
					: nullptr;
			}

			FStaticMeshRendererState::FShaderMapEntry& Entry =
				GStaticMeshState.ShaderMapEntries.emplace_back();
			Entry.Identity = Identity;

			FShaderCompileOptions CompileOptions;
			CompileOptions.Macros.emplace_back(
				"DURIN_MATERIAL_BLEND_MODE",
				std::to_string(static_cast<uint8>(Identity.BlendMode)));
			CompileOptions.Macros.emplace_back(
				"DURIN_MATERIAL_SHADING_MODEL",
				std::to_string(static_cast<uint8>(Identity.ShadingModel)));
			CompileOptions.Macros.emplace_back(
				"DURIN_MATERIAL_OPACITY_MASK_THRESHOLD_BITS",
				std::to_string(std::bit_cast<uint32>(Identity.OpacityMaskThreshold)));
			FShaderType& VertexShaderType = FStaticMeshVertexShader::StaticType();
			FShaderType& FragmentShaderType = FStaticMeshFragmentShader::StaticType();
			std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};
			std::shared_ptr<FShaderMapBase> ShaderMap = std::make_shared<FShaderMapBase>();
			std::string ErrorMessage;
			if (!ShaderMap->InitializeFromShaderTypes(ShaderTypes, CompileOptions, ErrorMessage))
			{
				DURIN_ERROR("Failed to initialize StaticMesh material shader map: {}", ErrorMessage);
				return nullptr;
			}

			auto* VertexShader = static_cast<FStaticMeshVertexShader*>(ShaderMap->GetShader(&VertexShaderType));
			auto* FragmentShader = static_cast<FStaticMeshFragmentShader*>(ShaderMap->GetShader(&FragmentShaderType));
			check(VertexShader);
			check(FragmentShader);
			Entry.ShaderMap = ShaderMap;
			Entry.VertexShader = TShaderRef<FStaticMeshVertexShader>(VertexShader, ShaderMap.get());
			Entry.FragmentShader = TShaderRef<FStaticMeshFragmentShader>(FragmentShader, ShaderMap.get());
			return &Entry;
		}

		auto GetOrCreateStaticMeshPipeline(
			const FMaterialPipelineIdentity& Identity
		) -> FStaticMeshRendererState::FPipelineEntry*
		{
			if (!EnsureStaticMeshBaseResources()) return nullptr;
			const auto Existing = std::ranges::find(
				GStaticMeshState.PipelineEntries,
				Identity,
				&FStaticMeshRendererState::FPipelineEntry::Identity);
			if (Existing != GStaticMeshState.PipelineEntries.end())
			{
				return Existing->SolidPipelineState != nullptr
					&& Existing->WireframePipelineState != nullptr
					&& Existing->VertexShader
					&& Existing->FragmentShader
					? &*Existing
					: nullptr;
			}

			FStaticMeshRendererState::FShaderMapEntry* ShaderMapEntry =
				GetOrCreateStaticMeshShaderMap(Identity.ShaderMap);
			if (ShaderMapEntry == nullptr) return nullptr;

			FStaticMeshRendererState::FPipelineEntry& Entry =
				GStaticMeshState.PipelineEntries.emplace_back();
			Entry.Identity = Identity;
			Entry.ShaderMap = ShaderMapEntry->ShaderMap;
			Entry.VertexShader = ShaderMapEntry->VertexShader;
			Entry.FragmentShader = ShaderMapEntry->FragmentShader;

			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderTargetLayout = RendererRenderTargetLayouts::MakeSceneTargets();
			Initializer.BoundShaders.VertexShader = Entry.VertexShader.GetRHIShader();
			Initializer.BoundShaders.FragmentShader = Entry.FragmentShader.GetRHIShader();
			Initializer.VertexDeclaration = GStaticMeshState.VertexDeclaration;
			Initializer.bEnableAlphaBlend = false;
			Initializer.bEnableDepthTest = true;
			Initializer.bEnableDepthWrite = true;
			Initializer.bEnableBackFaceCulling = false;
			Initializer.PipelineLayout = Entry.ShaderMap->GetMergedPipelineLayout();
			const size_t PipelineIndex = GStaticMeshState.PipelineEntries.size() - 1;
			Entry.SolidPipelineState = GDynamicRHI->RHICreateGraphicsPipelineState(
				FName(std::format("StaticMeshSolidPipeline_{}", PipelineIndex)),
				Initializer);
			Initializer.PolygonMode = FGraphicsPipelineStateInitializer::EPolygonMode::Line;
			Initializer.bEnableBackFaceCulling = false;
			Entry.WireframePipelineState = GDynamicRHI->RHICreateGraphicsPipelineState(
				FName(std::format("StaticMeshWireframePipeline_{}", PipelineIndex)),
				Initializer);
			if (Entry.SolidPipelineState == nullptr || Entry.WireframePipelineState == nullptr)
			{
				DURIN_ERROR("Failed to initialize StaticMesh material pipeline {}", PipelineIndex);
				return nullptr;
			}
			return &Entry;
		}

		auto EnsureTextureCubeThumbnailPipeline() -> void
		{
			if (GTextureCubeThumbnailState.bCreateAttempted) return;
			GTextureCubeThumbnailState.bCreateAttempted = true;

			FShaderCompileOptions CompileOptions;
			FShaderType& VertexShaderType = FTextureCubeThumbnailVertexShader::StaticType();
			FShaderType& FragmentShaderType = FTextureCubeThumbnailFragmentShader::StaticType();
			std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};
			std::shared_ptr<FShaderMapBase> ShaderMap = std::make_shared<FShaderMapBase>();
			std::string ErrorMessage;
			if (!ShaderMap->InitializeFromShaderTypes(ShaderTypes, CompileOptions, ErrorMessage))
			{
				DURIN_ERROR("Failed to initialize TextureCube thumbnail shader map: {}", ErrorMessage);
				return;
			}

			auto* VertexShader = static_cast<FTextureCubeThumbnailVertexShader*>(
				ShaderMap->GetShader(&VertexShaderType));
			auto* FragmentShader = static_cast<FTextureCubeThumbnailFragmentShader*>(
				ShaderMap->GetShader(&FragmentShaderType));
			check(VertexShader);
			check(FragmentShader);
			GTextureCubeThumbnailState.ShaderMap = ShaderMap;
			GTextureCubeThumbnailState.VertexShader =
				TShaderRef<FTextureCubeThumbnailVertexShader>(VertexShader, ShaderMap.get());
			GTextureCubeThumbnailState.FragmentShader =
				TShaderRef<FTextureCubeThumbnailFragmentShader>(FragmentShader, ShaderMap.get());

			const FVertexDeclarationElementList VertexDeclElements =
				GetStaticMeshVertexDeclarationElements();
			GTextureCubeThumbnailState.VertexDeclaration =
				GDynamicRHI->RHICreateVertexDeclaration(VertexDeclElements);

			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderTargetLayout = RendererRenderTargetLayouts::MakeSceneTargets();
			Initializer.BoundShaders.VertexShader =
				GTextureCubeThumbnailState.VertexShader.GetRHIShader();
			Initializer.BoundShaders.FragmentShader =
				GTextureCubeThumbnailState.FragmentShader.GetRHIShader();
			Initializer.VertexDeclaration = GTextureCubeThumbnailState.VertexDeclaration;
			Initializer.bEnableAlphaBlend = false;
			Initializer.bEnableDepthTest = true;
			Initializer.bEnableDepthWrite = true;
			Initializer.bEnableBackFaceCulling = false;
			Initializer.PipelineLayout = ShaderMap->GetMergedPipelineLayout();
			GTextureCubeThumbnailState.PipelineState =
				GDynamicRHI->RHICreateGraphicsPipelineState(
					"TextureCubeThumbnailPipeline", Initializer);
			GTextureCubeThumbnailState.Sampler =
				RHICreateSampler(FRHISamplerDesc::LinearClamp());
		}

		auto CreatePostProcessPipeline(
			FName PipelineName,
			FRHIShader* VertexShader,
			FRHIShader* FragmentShader,
			const FPipelineLayoutDesc& PipelineLayout
		) -> FGraphicsPipelineStateRHIRef
		{
			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderTargetLayout = RendererRenderTargetLayouts::MakeScenePostProcessOutput();
			Initializer.BoundShaders.VertexShader = VertexShader;
			Initializer.BoundShaders.FragmentShader = FragmentShader;
			Initializer.VertexDeclaration = GPostProcessState.VertexDeclaration;
			Initializer.bEnableAlphaBlend = false;
			Initializer.bEnableBackFaceCulling = false;
			Initializer.PipelineLayout = PipelineLayout;
			return GDynamicRHI->RHICreateGraphicsPipelineState(PipelineName, Initializer);
		}

		auto EnsurePostProcessResources(FRHICommandListImmediate& CommandList) -> void
		{
			if (GPostProcessState.bCreateAttempted)
			{
				return;
			}

			GPostProcessState.bCreateAttempted = true;

			FShaderCompileOptions CompileOptions;
			FShaderType& VertexShaderType = FPostProcessVertexShader::StaticType();
			FShaderType& CopyFragmentShaderType = FCopySceneColorFragmentShader::StaticType();
			FShaderType& FXAAFragmentShaderType = FFXAAFragmentShader::StaticType();
			std::array<const FShaderType*, 2> CopyShaderTypes = {&VertexShaderType, &CopyFragmentShaderType};
			std::array<const FShaderType*, 2> FXAAShaderTypes = {&VertexShaderType, &FXAAFragmentShaderType};
			std::shared_ptr<FShaderMapBase> CopyShaderMap = std::make_shared<FShaderMapBase>();
			std::shared_ptr<FShaderMapBase> FXAAShaderMap = std::make_shared<FShaderMapBase>();
			std::string ErrorMessage;
			if (!CopyShaderMap->InitializeFromShaderTypes(CopyShaderTypes, CompileOptions, ErrorMessage))
			{
				DURIN_ERROR("Failed to initialize PostProcess copy shader map: {}", ErrorMessage);
				return;
			}
			if (!FXAAShaderMap->InitializeFromShaderTypes(FXAAShaderTypes, CompileOptions, ErrorMessage))
			{
				DURIN_ERROR("Failed to initialize PostProcess FXAA shader map: {}", ErrorMessage);
				return;
			}

			auto* CopyVertexShader = static_cast<FPostProcessVertexShader*>(CopyShaderMap->GetShader(&VertexShaderType));
			auto* FXAAVertexShader = static_cast<FPostProcessVertexShader*>(FXAAShaderMap->GetShader(&VertexShaderType));
			auto* CopyFragmentShader = static_cast<FCopySceneColorFragmentShader*>(CopyShaderMap->GetShader(&CopyFragmentShaderType));
			auto* FXAAFragmentShader = static_cast<FFXAAFragmentShader*>(FXAAShaderMap->GetShader(&FXAAFragmentShaderType));
			check(CopyVertexShader);
			check(FXAAVertexShader);
			check(CopyFragmentShader);
			check(FXAAFragmentShader);

			GPostProcessState.CopyShaderMap = CopyShaderMap;
			GPostProcessState.FXAAShaderMap = FXAAShaderMap;
			GPostProcessState.CopyVertexShader = TShaderRef<FPostProcessVertexShader>(CopyVertexShader, CopyShaderMap.get());
			GPostProcessState.FXAAVertexShader = TShaderRef<FPostProcessVertexShader>(FXAAVertexShader, FXAAShaderMap.get());
			GPostProcessState.CopyFragmentShader = TShaderRef<FCopySceneColorFragmentShader>(CopyFragmentShader, CopyShaderMap.get());
			GPostProcessState.FXAAFragmentShader = TShaderRef<FFXAAFragmentShader>(FXAAFragmentShader, FXAAShaderMap.get());

			FVertexDeclarationElementList VertexDeclElements;
			constexpr uint32 VertexStride = sizeof(FPostProcessVertex);
			VertexDeclElements[0] = FVertexElement(0, offsetof(FPostProcessVertex, Position), EVertexElementType::Float2, 0, VertexStride);
			VertexDeclElements[1] = FVertexElement(0, offsetof(FPostProcessVertex, UV), EVertexElementType::Float2, 1, VertexStride);
			GPostProcessState.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(VertexDeclElements);

			const std::array<FPostProcessVertex, 3> FullscreenVertices = {
				FPostProcessVertex{FVector2f{-1.0f, -1.0f}, FVector2f{0.0f, 0.0f}},
				FPostProcessVertex{FVector2f{3.0f, -1.0f}, FVector2f{2.0f, 0.0f}},
				FPostProcessVertex{FVector2f{-1.0f, 3.0f}, FVector2f{0.0f, 2.0f}},
			};
			const std::array<uint32, 3> FullscreenIndices = {0, 1, 2};

			FRHIBufferCreateDesc VertexBufferDesc = FRHIBufferCreateDesc::CreateVertex("PostProcessFullscreenVertexBuffer", sizeof(FPostProcessVertex) * static_cast<uint32>(FullscreenVertices.size()));
			VertexBufferDesc.Usage |= EBufferUsageFlags::Static;
			VertexBufferDesc.InitialData = {FullscreenVertices.data(), static_cast<uint32>(sizeof(FPostProcessVertex) * FullscreenVertices.size())};
			GPostProcessState.VertexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, VertexBufferDesc);

			FRHIBufferCreateDesc IndexBufferDesc = FRHIBufferCreateDesc::CreateIndex("PostProcessFullscreenIndexBuffer", sizeof(uint32) * static_cast<uint32>(FullscreenIndices.size()), sizeof(uint32));
			IndexBufferDesc.Usage |= EBufferUsageFlags::Static;
			IndexBufferDesc.InitialData = {FullscreenIndices.data(), static_cast<uint32>(sizeof(uint32) * FullscreenIndices.size())};
			GPostProcessState.IndexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, IndexBufferDesc);

			GPostProcessState.SceneColorSampler = RHICreateSampler(FRHISamplerDesc::LinearClamp());

			GPostProcessState.CopyOffscreenPipelineState = CreatePostProcessPipeline(
				"PostProcessCopyOffscreenPipeline",
				GPostProcessState.CopyVertexShader.GetRHIShader(),
				GPostProcessState.CopyFragmentShader.GetRHIShader(),
				CopyShaderMap->GetMergedPipelineLayout()
			);
			GPostProcessState.CopyPresentPipelineState = CreatePostProcessPipeline(
				"PostProcessCopyPresentPipeline",
				GPostProcessState.CopyVertexShader.GetRHIShader(),
				GPostProcessState.CopyFragmentShader.GetRHIShader(),
				CopyShaderMap->GetMergedPipelineLayout()
			);
			GPostProcessState.FXAAOffscreenPipelineState = CreatePostProcessPipeline(
				"PostProcessFXAAOffscreenPipeline",
				GPostProcessState.FXAAVertexShader.GetRHIShader(),
				GPostProcessState.FXAAFragmentShader.GetRHIShader(),
				FXAAShaderMap->GetMergedPipelineLayout()
			);
			GPostProcessState.FXAAPresentPipelineState = CreatePostProcessPipeline(
				"PostProcessFXAAPresentPipeline",
				GPostProcessState.FXAAVertexShader.GetRHIShader(),
				GPostProcessState.FXAAFragmentShader.GetRHIShader(),
				FXAAShaderMap->GetMergedPipelineLayout()
			);

		}

		auto EnsureSceneTargets(uint32 Width, uint32 Height) -> FPostProcessRendererState::FSceneTargets*
		{
			const uint64 Key = (static_cast<uint64>(Width) << 32) | Height;
			if (auto It = GPostProcessState.SceneTargetsBySize.find(Key); It != GPostProcessState.SceneTargetsBySize.end())
			{
				return &It->second;
			}

			FRHITextureCreateDesc SceneColorDesc = FRHITextureCreateDesc::Create2D("SceneColor", Width, Height, EPixelFormat::SRGBA8_UNORM);
			SceneColorDesc.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource);
			SceneColorDesc.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f));
			FRHITextureCreateDesc SceneDepthDesc = FRHITextureCreateDesc::Create2D("SceneDepth", Width, Height, EPixelFormat::D32);
			SceneDepthDesc.SetFlags(ETextureCreateFlags::DepthStencilTargetable);
			SceneDepthDesc.SetClearValue(FClearValueBinding(1.0f, 0u));
			auto [It, bInserted] = GPostProcessState.SceneTargetsBySize.emplace(Key, FPostProcessRendererState::FSceneTargets{
				.Color = RHICreateTexture(SceneColorDesc),
				.Depth = RHICreateTexture(SceneDepthDesc),
			});
			// Interactive viewport resizing can produce many transient dimensions. Keep a
			// small pool so the main view and camera previews reuse their stable sizes
			// without retaining every intermediate drag size for the entire session.
			if (GPostProcessState.SceneTargetsBySize.size() > 8)
			{
				const auto EvictionIt = std::ranges::find_if(GPostProcessState.SceneTargetsBySize, [Key](const auto& Entry) { return Entry.first != Key; });
				if (EvictionIt != GPostProcessState.SceneTargetsBySize.end()) GPostProcessState.SceneTargetsBySize.erase(EvictionIt);
			}
			return bInserted ? &It->second : nullptr;
		}

		auto ToShaderMatrix(const FMatrix& Matrix) -> glm::mat4
		{
			return glm::transpose(glm::mat4(Matrix));
		}

		auto DrawEditorGrid(FRHICommandListImmediate& CommandList, const FSceneView& View, bool bPresentOutput) -> void
		{
			const FGraphicsPipelineStateRHIRef Pipeline = GetEditorAssistanceOutputPipeline(GEditorGridState.OutputPipelines, bPresentOutput);
			if (!View.EditorGrid.bVisible || Pipeline == nullptr
				|| !GEditorGridState.VertexShader || !GEditorGridState.FragmentShader
				|| GPostProcessState.VertexBuffer == nullptr || GPostProcessState.IndexBuffer == nullptr) return;

			EditorGridRendering::FEditorGridUniform Uniform;
			if (!EditorGridRendering::BuildUniform(View, Uniform)) return;

			CommandList.SetGraphicsPipelineState(*Pipeline);
			CommandList.BindVertexBuffer(0, GPostProcessState.VertexBuffer, 0);
			CommandList.BindIndexBuffer(GPostProcessState.IndexBuffer, 0);
			const FRHIUniformBufferRange GridBuffer = CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
			FEditorGridFragmentShader::FParameters FragmentParameters;
			FragmentParameters.Grid = GridBuffer;
			SetShaderParameters(CommandList, GEditorGridState.FragmentShader, FragmentParameters);
			CommandList.DrawIndexed(3, 0, 0);
		}

		auto DrawSkyBox(FRHICommandListImmediate& CommandList, IScene& Scene, const FSceneView& View) -> void
		{
			FSkyBoxSceneData SkyBox;
			if (!Scene.GetActiveSkyBox_RenderThread(SkyBox)) return;

			if (GSkyBoxState.PipelineState == nullptr || GSkyBoxState.Sampler == nullptr
				|| !GSkyBoxState.VertexShader || !GSkyBoxState.FragmentShader
				|| GSkyBoxState.IndexBuffer == nullptr) return;

			SkyBoxRendering::FSkyBoxUniform Uniform;
			if (!SkyBoxRendering::BuildUniform(View, SkyBox, Uniform)) return;

			FRHITexture* Texture = SkyBox.TextureReference != nullptr
				? SkyBox.TextureReference->GetReferencedTexture_RenderThread()
				: nullptr;
			if (Texture == nullptr)
				Texture = GetDefaultCubeTexture_RenderThread();
			if (Texture == nullptr) return;

			CommandList.SetGraphicsPipelineState(*GSkyBoxState.PipelineState);
			CommandList.BindIndexBuffer(GSkyBoxState.IndexBuffer, 0);
			FSkyBoxFragmentShader::FParameters Parameters;
			Parameters.SkyTexture = Texture;
			Parameters.SkySampler = GSkyBoxState.Sampler;
			Parameters.Sky = CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
			SetShaderParameters(CommandList, GSkyBoxState.FragmentShader, Parameters);
			CommandList.DrawIndexed(3, 0, 0);
		}

		auto DrawStaticMeshProxy(FRHICommandListImmediate& CommandList, const FSceneView& View, const FDirectionalLightSceneData& Light, ERenderMode RenderMode, ERasterMode RasterMode, const FStaticMeshSceneProxy& Proxy) -> void
		{
			const FStaticMeshRenderData* RenderData = Proxy.GetRenderData();
			if (RenderData == nullptr || RenderData->LODResources.empty())
			{
				return;
			}

			if (!RenderData->IsReadyForRendering())
			{
				return;
			}

			const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
			FStaticMeshTransformUniform TransformUniform;
			TransformUniform.LocalToClip = ToShaderMatrix(View.ViewProjectionMatrix * Proxy.GetLocalToWorld());
			TransformUniform.LocalToWorld = ToShaderMatrix(Proxy.GetLocalToWorld());
			TransformUniform.NormalToWorld = ToShaderMatrix(glm::transpose(glm::inverse(Proxy.GetLocalToWorld())));
			const float TransformDeterminant = glm::determinant(glm::mat3(glm::mat4(Proxy.GetLocalToWorld())));
			TransformUniform.TransformParams.x = TransformDeterminant < 0.0f ? -1.0f : 1.0f;
			const FRHIUniformBufferRange TransformUniformBuffer = CommandList.AllocateDynamicUniformBuffer(&TransformUniform, sizeof(TransformUniform));

			FStaticMeshLightingUniform LightingUniform;
			LightingUniform.LightDirection =
				FVector4f(FVector3f(Light.Direction), Light.RimLightIntensity);
			LightingUniform.LightColorIntensity = FVector4f(Light.Color, Light.Intensity);
			LightingUniform.ViewPositionAmbient = FVector4f(FVector3f(View.ViewLocation), Light.AmbientIntensity);
			const FRHIUniformBufferRange LightingUniformBuffer = CommandList.AllocateDynamicUniformBuffer(&LightingUniform, sizeof(LightingUniform));

			CommandList.BindVertexBuffer(
				0,
				LOD.VertexBuffers.PositionVertexBuffer.GetRHI(),
				0);
			CommandList.BindVertexBuffer(
				1,
				LOD.VertexBuffers.StaticMeshVertexBuffer.GetRHI(),
				0);
			CommandList.BindIndexBuffer(
				LOD.IndexBuffer.GetRHI(), 0);
			const auto& Indices = LOD.IndexBuffer.GetIndices();
			for (const FStaticMeshSection& Section : LOD.Sections)
			{
				if (Section.IndexCount == 0
					|| static_cast<uint64>(Section.FirstIndex)
						+ Section.IndexCount
						> Indices.size())
				{
					continue;
				}
				const FMaterialRenderData& Material = Proxy.GetMaterialRenderData(Section.MaterialSlotIndex);
				FStaticMeshRendererState::FPipelineEntry* PipelineEntry =
					GetOrCreateStaticMeshPipeline(Material.PipelineIdentity);
				if (PipelineEntry == nullptr) continue;
				const FGraphicsPipelineStateRHIRef Pipeline =
					RasterMode == ERasterMode::Wireframe
						? PipelineEntry->WireframePipelineState
						: PipelineEntry->SolidPipelineState;
				CommandList.SetGraphicsPipelineState(*Pipeline);

				FStaticMeshVertexShader::FParameters VertexShaderParameters;
				VertexShaderParameters.Transform = TransformUniformBuffer;
				SetShaderParameters(CommandList, PipelineEntry->VertexShader, VertexShaderParameters);

				FStaticMeshMaterialUniform MaterialUniform;
				MaterialUniform.BaseColor = Material.BaseColor;
				MaterialUniform.Params = FVector4f(Material.SpecularStrength, Material.Shininess, RenderMode == ERenderMode::Lit ? 1.0f : 0.0f, 0.0f);
				const FRHIUniformBufferRange MaterialUniformBuffer = CommandList.AllocateDynamicUniformBuffer(&MaterialUniform, sizeof(MaterialUniform));
				FStaticMeshFragmentShader::FParameters FragmentShaderParameters;
				FragmentShaderParameters.Lighting = LightingUniformBuffer;
				FragmentShaderParameters.Material = MaterialUniformBuffer;
				FragmentShaderParameters.BaseColorTexture = ResolveTexture_RenderThread(Material.BaseColorTexture, EDefaultTexture::White);
				FragmentShaderParameters.BaseColorSampler = GStaticMeshState.BaseColorSampler;
				SetShaderParameters(CommandList, PipelineEntry->FragmentShader, FragmentShaderParameters);
				CommandList.DrawIndexed(Section.IndexCount, Section.FirstIndex, 0);
			}
		}

		auto DrawTextureCubeThumbnailProxy(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FTextureCubePreviewSceneProxy& Proxy) -> void
		{
			const FRHITextureReferenceRef& TextureReference =
				Proxy.GetTextureReference();
			if (TextureReference == nullptr) return;
			FRHITexture* Texture =
				TextureReference->GetReferencedTexture_RenderThread();
			if (Texture == nullptr) return;

			if (GSkyBoxState.PipelineState == nullptr
				|| GSkyBoxState.Sampler == nullptr
				|| !GSkyBoxState.VertexShader
				|| !GSkyBoxState.FragmentShader
				|| GSkyBoxState.IndexBuffer == nullptr)
				return;

			// Content Browser thumbnails favor recognition over inspection: show a
			// wide environment view here and reserve the reflective sphere for the
			// interactive TextureCube editor.
			constexpr float EnvironmentVerticalFieldOfViewDegrees = 100.0f;
			FSceneView EnvironmentView = View;
			const float AspectRatio = static_cast<float>(View.ViewportWidth)
				/ static_cast<float>(std::max(1u, View.ViewportHeight));
			const float YScale = 1.0f
				/ std::tan(
					glm::radians(EnvironmentVerticalFieldOfViewDegrees) * 0.5f);
			EnvironmentView.ProjectionMatrix[1][0] =
				YScale / std::max(AspectRatio, 0.001f);
			EnvironmentView.ProjectionMatrix[2][1] = -YScale;
			EnvironmentView.ViewProjectionMatrix =
				EnvironmentView.ProjectionMatrix * EnvironmentView.ViewMatrix;

			SkyBoxRendering::FSkyBoxUniform Uniform;
			if (!SkyBoxRendering::BuildUniform(
					EnvironmentView, FSkyBoxSceneData{}, Uniform))
				return;

			CommandList.SetGraphicsPipelineState(*GSkyBoxState.PipelineState);
			CommandList.BindIndexBuffer(GSkyBoxState.IndexBuffer, 0);
			FSkyBoxFragmentShader::FParameters FragmentParameters;
			FragmentParameters.SkyTexture = Texture;
			FragmentParameters.SkySampler = GSkyBoxState.Sampler;
			FragmentParameters.Sky =
				CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
			SetShaderParameters(
				CommandList,
				GSkyBoxState.FragmentShader,
				FragmentParameters);
			CommandList.DrawIndexed(3, 0, 0);
		}

		auto DrawGizmoPrimitives(FRHICommandListImmediate& CommandList, const FSceneView& View, bool bXRay, bool bPresentOutput) -> void
		{
			if (View.OverlayPrimitives.empty() || GGizmoState.VertexBuffer == nullptr || GGizmoState.IndexBuffer == nullptr) return;
			CommandList.BindVertexBuffer(0, GGizmoState.VertexBuffer, 0);
			CommandList.BindIndexBuffer(GGizmoState.IndexBuffer, 0);
			for (const FViewOverlayPrimitive& Primitive : View.OverlayPrimitives)
			{
				const bool bWire = Primitive.Shape == EViewOverlayShape::WireBox;
				const FEditorAssistanceOutputPipelines& OutputPipelines = bWire
					? (bXRay ? GGizmoState.WireXRayOutputPipelines : GGizmoState.WireVisibleOutputPipelines)
					: (bXRay ? GGizmoState.XRayOutputPipelines : GGizmoState.VisibleOutputPipelines);
				const FGraphicsPipelineStateRHIRef Pipeline = GetEditorAssistanceOutputPipeline(OutputPipelines, bPresentOutput);
				if (Pipeline == nullptr) continue;
				CommandList.SetGraphicsPipelineState(*Pipeline);
				const size_t ShapeIndex = static_cast<size_t>(Primitive.Shape);
				if (ShapeIndex >= GGizmoState.MeshRanges.size()) continue;
				const FGizmoMeshRange& Range = GGizmoState.MeshRanges[ShapeIndex];
				FGizmoTransformUniform Uniform;
				Uniform.LocalToClip = ToShaderMatrix(View.ViewProjectionMatrix * Primitive.LocalToWorld);
				Uniform.Color = Primitive.Color;
				// The depth-independent pass keeps handles legible through the selected object; the
				// depth-tested pass drawn afterward restores the full color of visible surfaces.
				if (bXRay) Uniform.Color.a *= 0.32f;
				const FRHIUniformBufferRange Buffer = CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
				FGizmoVertexShader::FParameters Parameters;
				Parameters.Transform = Buffer;
				SetShaderParameters(CommandList, GGizmoState.VertexShader, Parameters);
				CommandList.DrawIndexed(Range.IndexCount, Range.FirstIndex, Range.VertexOffset);
			}
		}

		auto PrepareOverlayLines(FRHICommandListImmediate& CommandList, const FSceneView& View) -> void
		{
			GOverlayLineState.IndexCount = 0;
			if (View.OverlayLines.empty()) return;
			EnsureOverlayLineResources();
			if (!GOverlayLineState.VertexShader || !GOverlayLineState.FragmentShader) return;

			std::vector<FOverlayLineVertex> Vertices;
			std::vector<uint32> Indices;
			Vertices.reserve(View.OverlayLines.size() * 4);
			Indices.reserve(View.OverlayLines.size() * 6);
			BuildOverlayLineGeometry(View, Vertices, Indices);
			if (Vertices.empty() || Indices.empty()) return;

			const uint32 VertexBytes = static_cast<uint32>(Vertices.size() * sizeof(FOverlayLineVertex));
			const uint32 IndexBytes = static_cast<uint32>(Indices.size() * sizeof(uint32));
			if (VertexBytes > GOverlayLineState.VertexCapacity)
			{
				GOverlayLineState.VertexCapacity = std::bit_ceil(VertexBytes);
				FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateVertex("OverlayLineVertexBuffer", GOverlayLineState.VertexCapacity);
				Desc.Usage |= EBufferUsageFlags::Dynamic;
				GOverlayLineState.VertexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, Desc);
			}
			if (IndexBytes > GOverlayLineState.IndexCapacity)
			{
				GOverlayLineState.IndexCapacity = std::bit_ceil(IndexBytes);
				FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateIndex("OverlayLineIndexBuffer", GOverlayLineState.IndexCapacity, sizeof(uint32));
				Desc.Usage |= EBufferUsageFlags::Dynamic;
				GOverlayLineState.IndexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, Desc);
			}
			if (GOverlayLineState.VertexBuffer == nullptr || GOverlayLineState.IndexBuffer == nullptr) return;
			CommandList.WriteBuffer(GOverlayLineState.VertexBuffer, Vertices.data(), VertexBytes, 0);
			CommandList.WriteBuffer(GOverlayLineState.IndexBuffer, Indices.data(), IndexBytes, 0);
			GOverlayLineState.IndexCount = static_cast<uint32>(Indices.size());
		}

		auto DrawOverlayLines(FRHICommandListImmediate& CommandList, bool bXRay, bool bPresentOutput) -> void
		{
			if (GOverlayLineState.IndexCount == 0 || GOverlayLineState.VertexBuffer == nullptr || GOverlayLineState.IndexBuffer == nullptr) return;
			const FGraphicsPipelineStateRHIRef Pipeline = GetEditorAssistanceOutputPipeline(
				bXRay ? GOverlayLineState.XRayOutputPipelines : GOverlayLineState.VisibleOutputPipelines,
				bPresentOutput
			);
			if (Pipeline == nullptr) return;
			CommandList.SetGraphicsPipelineState(*Pipeline);
			CommandList.BindVertexBuffer(0, GOverlayLineState.VertexBuffer, 0);
			CommandList.BindIndexBuffer(GOverlayLineState.IndexBuffer, 0);
			const FOverlayLineStyleUniform Style{bXRay ? 0.32f : 1.0f};
			FOverlayLineFragmentShader::FParameters Parameters;
			Parameters.Style = CommandList.AllocateDynamicUniformBuffer(&Style, sizeof(Style));
			SetShaderParameters(CommandList, GOverlayLineState.FragmentShader, Parameters);
			CommandList.DrawIndexed(GOverlayLineState.IndexCount, 0, 0);
		}

		auto PrepareOverlayIcons(FRHICommandListImmediate& CommandList, const FSceneView& View) -> void
		{
			GOverlayIconState.IndexCount = 0;
			if (View.OverlayIcons.empty()) return;
			EnsureOverlayIconResources(CommandList);
			if (!GOverlayIconState.VertexShader || !GOverlayIconState.FragmentShader || GOverlayIconState.Atlas == nullptr) return;

			std::vector<FOverlayIconVertex> Vertices;
			std::vector<uint32> Indices;
			Vertices.reserve(View.OverlayIcons.size() * 4);
			Indices.reserve(View.OverlayIcons.size() * 6);
			for (const FViewOverlayIcon& Icon : View.OverlayIcons)
			{
				if (!std::isfinite(Icon.SizePixels) || Icon.SizePixels <= 0.0f) continue;
				const float MinU = Icon.Icon == EViewOverlayIcon::DirectionalLight ? 0.5f : 0.0f;
				const float MaxU = MinU + 0.5f;
				const FVector4 Clip = View.ViewProjectionMatrix * FVector4(Icon.WorldPosition, 1.0);
				if (!std::isfinite(Clip.x) || !std::isfinite(Clip.y) || !std::isfinite(Clip.z) || !std::isfinite(Clip.w)
					|| Clip.w <= 1.e-8 || Clip.z < 0.0) continue;
				const double HalfNdcX = static_cast<double>(Icon.SizePixels) / std::max(1u, View.ViewportWidth);
				const double HalfNdcY = static_cast<double>(Icon.SizePixels) / std::max(1u, View.ViewportHeight);
				auto MakePosition = [&](double X, double Y) {
					return FVector4f(
						static_cast<float>(Clip.x + X * Clip.w),
						static_cast<float>(Clip.y + Y * Clip.w),
						static_cast<float>(Clip.z),
						static_cast<float>(Clip.w)
					);
				};
				const uint32 Base = static_cast<uint32>(Vertices.size());
				Vertices.push_back({MakePosition(-HalfNdcX, -HalfNdcY), {MinU, 0.0f}, Icon.Color});
				Vertices.push_back({MakePosition(HalfNdcX, -HalfNdcY), {MaxU, 0.0f}, Icon.Color});
				Vertices.push_back({MakePosition(-HalfNdcX, HalfNdcY), {MinU, 1.0f}, Icon.Color});
				Vertices.push_back({MakePosition(HalfNdcX, HalfNdcY), {MaxU, 1.0f}, Icon.Color});
				Indices.insert(Indices.end(), {Base, Base + 1, Base + 2, Base + 2, Base + 1, Base + 3});
			}
			if (Vertices.empty() || Indices.empty()) return;

			const uint32 VertexBytes = static_cast<uint32>(Vertices.size() * sizeof(FOverlayIconVertex));
			const uint32 IndexBytes = static_cast<uint32>(Indices.size() * sizeof(uint32));
			if (VertexBytes > GOverlayIconState.VertexCapacity)
			{
				GOverlayIconState.VertexCapacity = std::bit_ceil(VertexBytes);
				FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateVertex("OverlayIconVertexBuffer", GOverlayIconState.VertexCapacity);
				Desc.Usage |= EBufferUsageFlags::Dynamic;
				GOverlayIconState.VertexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, Desc);
			}
			if (IndexBytes > GOverlayIconState.IndexCapacity)
			{
				GOverlayIconState.IndexCapacity = std::bit_ceil(IndexBytes);
				FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::CreateIndex("OverlayIconIndexBuffer", GOverlayIconState.IndexCapacity, sizeof(uint32));
				Desc.Usage |= EBufferUsageFlags::Dynamic;
				GOverlayIconState.IndexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, Desc);
			}
			if (GOverlayIconState.VertexBuffer == nullptr || GOverlayIconState.IndexBuffer == nullptr) return;
			CommandList.WriteBuffer(GOverlayIconState.VertexBuffer, Vertices.data(), VertexBytes, 0);
			CommandList.WriteBuffer(GOverlayIconState.IndexBuffer, Indices.data(), IndexBytes, 0);
			GOverlayIconState.IndexCount = static_cast<uint32>(Indices.size());
		}

		auto DrawOverlayIcons(FRHICommandListImmediate& CommandList, bool bXRay, bool bPresentOutput) -> void
		{
			if (GOverlayIconState.IndexCount == 0 || GOverlayIconState.VertexBuffer == nullptr || GOverlayIconState.IndexBuffer == nullptr
				|| GOverlayIconState.Atlas == nullptr || GOverlayIconState.AtlasSampler == nullptr) return;
			const FGraphicsPipelineStateRHIRef Pipeline = GetEditorAssistanceOutputPipeline(
				bXRay ? GOverlayIconState.XRayOutputPipelines : GOverlayIconState.VisibleOutputPipelines,
				bPresentOutput
			);
			if (Pipeline == nullptr) return;
			CommandList.SetGraphicsPipelineState(*Pipeline);
			CommandList.BindVertexBuffer(0, GOverlayIconState.VertexBuffer, 0);
			CommandList.BindIndexBuffer(GOverlayIconState.IndexBuffer, 0);
			const FOverlayIconStyleUniform Style{bXRay ? 0.3f : 1.0f};
			FOverlayIconFragmentShader::FParameters Parameters;
			Parameters.Atlas = GOverlayIconState.Atlas;
			Parameters.AtlasSampler = GOverlayIconState.AtlasSampler;
			Parameters.IconStyle = CommandList.AllocateDynamicUniformBuffer(&Style, sizeof(Style));
			SetShaderParameters(CommandList, GOverlayIconState.FragmentShader, Parameters);
			CommandList.DrawIndexed(GOverlayIconState.IndexCount, 0, 0);
		}

		auto DrawPostProcess(FRHICommandListImmediate& CommandList, FRHITexture* SceneColor, uint32 Width, uint32 Height, bool bPresentOutput) -> void
		{
			const bool bUseFXAA = GPostProcessState.bEnableFXAA.load(std::memory_order_relaxed);
			FGraphicsPipelineStateRHIRef PipelineState = bUseFXAA
				? (bPresentOutput ? GPostProcessState.FXAAPresentPipelineState : GPostProcessState.FXAAOffscreenPipelineState)
				: (bPresentOutput ? GPostProcessState.CopyPresentPipelineState : GPostProcessState.CopyOffscreenPipelineState);
			if (PipelineState == nullptr || GPostProcessState.VertexBuffer == nullptr || GPostProcessState.IndexBuffer == nullptr)
			{
				return;
			}

			CommandList.SetGraphicsPipelineState(*PipelineState);
			CommandList.SetViewport(0.0f, 0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height), 1.0f);
			CommandList.SetScissor(0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height));
			CommandList.BindVertexBuffer(0, GPostProcessState.VertexBuffer, 0);
			CommandList.BindIndexBuffer(GPostProcessState.IndexBuffer, 0);

			if (bUseFXAA)
			{
				FPostProcessViewUniform ViewUniform;
				ViewUniform.InvRenderTargetSize = FVector2f(1.0f / static_cast<float>(Width), 1.0f / static_cast<float>(Height));
				const FRHIUniformBufferRange ViewUniformBuffer = CommandList.AllocateDynamicUniformBuffer(&ViewUniform, sizeof(ViewUniform));

				FFXAAFragmentShader::FParameters FragmentParameters;
				FragmentParameters.SceneColor = SceneColor;
				FragmentParameters.SceneColorSampler = GPostProcessState.SceneColorSampler;
				FragmentParameters.View = ViewUniformBuffer;
				SetShaderParameters(CommandList, GPostProcessState.FXAAFragmentShader, FragmentParameters);
			}
			else
			{
				FCopySceneColorFragmentShader::FParameters FragmentParameters;
				FragmentParameters.SceneColor = SceneColor;
				FragmentParameters.SceneColorSampler = GPostProcessState.SceneColorSampler;
				SetShaderParameters(CommandList, GPostProcessState.CopyFragmentShader, FragmentParameters);
			}

			CommandList.DrawIndexed(3, 0, 0);
		}

		auto ForEachStaticMeshProxy(IScene* Scene, const std::function<void(FStaticMeshSceneProxy&)>& Function) -> void
		{
			auto* RendererScene = dynamic_cast<FScene*>(Scene);
			if (RendererScene == nullptr)
			{
				return;
			}

			for (PrimitiveSceneProxy* Proxy : RendererScene->GetPrimitiveSceneProxies())
			{
				if (auto* StaticMeshProxy = dynamic_cast<FStaticMeshSceneProxy*>(Proxy))
				{
					Function(*StaticMeshProxy);
				}
			}
		}

		auto ForEachTextureCubeThumbnailProxy(
			IScene* Scene,
			const std::function<void(FTextureCubePreviewSceneProxy&)>& Function) -> void
		{
			auto* RendererScene = dynamic_cast<FScene*>(Scene);
			if (RendererScene == nullptr) return;
			for (PrimitiveSceneProxy* Proxy : RendererScene->GetPrimitiveSceneProxies())
			{
				if (auto* TextureCubeProxy =
						dynamic_cast<FTextureCubePreviewSceneProxy*>(Proxy))
					Function(*TextureCubeProxy);
			}
		}
	}

	auto GetDefaultTexture_RenderThread(EDefaultTexture Texture) -> FRHITexture*
	{
		check(IsInRenderingThread());
		switch (Texture)
		{
		case EDefaultTexture::White: return GDefaultTextures.White;
		case EDefaultTexture::Black: return GDefaultTextures.Black;
		case EDefaultTexture::FlatNormal: return GDefaultTextures.FlatNormal;
		}
		return GDefaultTextures.White;
	}

	auto ResolveTexture_RenderThread(
		const FRHITextureReferenceRef& TextureReference,
		EDefaultTexture Fallback) -> FRHITexture*
	{
		check(IsInRenderingThread());
		if (TextureReference != nullptr)
		{
			if (FRHITexture* Texture =
				TextureReference->GetReferencedTexture_RenderThread())
			{
				return Texture;
			}
		}
		return GetDefaultTexture_RenderThread(Fallback);
	}

	auto GetDefaultCubeTexture_RenderThread() -> FRHITexture*
	{
		check(IsInRenderingThread());
		return GDefaultTextures.BlackCube;
	}

	auto FRendererModule::StartupModule() -> void
	{
		bAcceptingSceneCreation = true;
		PreExitHandle =
			AddOnEnginePreExit([this]() { StopAcceptingSceneCreation(); });
		check(PreExitHandle.IsValid());
		if (GDynamicRHI != nullptr)
		{
			ENQUEUE_RENDER_COMMAND(InitializeDefaultTextures)(
				[](FRHICommandListImmediate& CommandList) {
					InitializeDefaultTextures_RenderThread(CommandList);
				});
		}
	}

	auto FRendererModule::ReleaseResources() -> void
	{
		ENQUEUE_RENDER_COMMAND(ReleaseRendererResources)([](FRHICommandListImmediate&) {
			GDefaultTextures = {};
			GStaticMeshState = {};
			GTextureCubeThumbnailState = {};
			GSkyBoxState.Sampler = nullptr;
			GOverlayIconState.Atlas = nullptr;
			GOverlayIconState.AtlasSampler = nullptr;
		});
	}

	auto FRendererModule::ShutdownModule() -> void
	{
		StopAcceptingSceneCreation();
		if (PreExitHandle.IsValid())
		{
			RemoveOnEnginePreExit(PreExitHandle);
			PreExitHandle.Reset();
		}
		checkf(GDefaultTextures.White == nullptr && GDefaultTextures.Black == nullptr && GDefaultTextures.FlatNormal == nullptr
				&& GDefaultTextures.BlackCube == nullptr,
			"Renderer defaults must be released before the rendering thread stops");
		GStaticMeshState = {};
		GTextureCubeThumbnailState = {};
		GSkyBoxState = {};
		GGizmoState = {};
		GOverlayLineState = {};
		GOverlayIconState = {};
		GEditorGridState = {};
		GPostProcessState.CopyShaderMap.reset();
		GPostProcessState.FXAAShaderMap.reset();
		GPostProcessState.CopyVertexShader = {};
		GPostProcessState.FXAAVertexShader = {};
		GPostProcessState.CopyFragmentShader = {};
		GPostProcessState.FXAAFragmentShader = {};
		GPostProcessState.VertexDeclaration = nullptr;
		GPostProcessState.CopyOffscreenPipelineState = nullptr;
		GPostProcessState.CopyPresentPipelineState = nullptr;
		GPostProcessState.FXAAOffscreenPipelineState = nullptr;
		GPostProcessState.FXAAPresentPipelineState = nullptr;
		GPostProcessState.VertexBuffer = nullptr;
		GPostProcessState.IndexBuffer = nullptr;
		GPostProcessState.SceneColorSampler = nullptr;
		GPostProcessState.SceneTargetsBySize.clear();
		GPostProcessState.bCreateAttempted = false;
		GPostProcessState.bEnableFXAA.store(true, std::memory_order_relaxed);
		GEditorAssistancePipelineFailureLogged = false;
	}

	auto FRendererModule::CreateScene() -> std::unique_ptr<IScene>
	{
		if (!bAcceptingSceneCreation)
		{
			DURIN_WARN("Renderer scene creation rejected after renderer quiescence.");
			return nullptr;
		}
		return std::make_unique<FScene>();
	}

	auto FRendererModule::StopAcceptingSceneCreation() -> void
	{
		if (!bAcceptingSceneCreation) return;
		bAcceptingSceneCreation = false;
		DURIN_DEBUG("Renderer stopped accepting scene creation.");
	}

	auto FRendererModule::GetViewSettings() const -> FRendererViewSettings
	{
		FRendererViewSettings Settings;
		Settings.bEnableFXAA = GPostProcessState.bEnableFXAA.load(std::memory_order_relaxed);
		Settings.RenderMode = GRenderMode.load(std::memory_order_relaxed);
		Settings.RasterMode = GRasterMode.load(std::memory_order_relaxed);
		return Settings;
	}

	auto FRendererModule::SetViewSettings(const FRendererViewSettings& InSettings) -> void
	{
		SetFXAAEnabled(InSettings.bEnableFXAA);
		SetRenderMode(InSettings.RenderMode);
		SetRasterMode(InSettings.RasterMode);
	}

	auto FRendererModule::SetFXAAEnabled(bool bInEnabled) -> void
	{
		GPostProcessState.bEnableFXAA.store(bInEnabled, std::memory_order_relaxed);
	}

	auto FRendererModule::IsFXAAEnabled() const -> bool
	{
		return GPostProcessState.bEnableFXAA.load(std::memory_order_relaxed);
	}

	auto FRendererModule::SetRenderMode(ERenderMode Mode) -> void
	{
		GRenderMode.store(Mode, std::memory_order_relaxed);
	}

	auto FRendererModule::GetRenderMode() const -> ERenderMode
	{
		return GRenderMode.load(std::memory_order_relaxed);
	}

	auto FRendererModule::SetRasterMode(ERasterMode Mode) -> void
	{
		GRasterMode.store(Mode, std::memory_order_relaxed);
	}

	auto FRendererModule::GetRasterMode() const -> ERasterMode
	{
		return GRasterMode.load(std::memory_order_relaxed);
	}

	auto FRendererModule::RenderView(FRHICommandListImmediate& CommandList, IScene* Scene, const FSceneView& View, FRHITexture* OutputTarget, bool bPresentOutput) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.RenderView");
		const uint32 Width = OutputTarget != nullptr ? OutputTarget->GetSizeX() : 0;
		const uint32 Height = OutputTarget != nullptr ? OutputTarget->GetSizeY() : 0;
		if (OutputTarget == nullptr || Width == 0 || Height == 0)
		{
			return;
		}

		EnsurePostProcessResources(CommandList);
		// Sky resources include a static index upload, so initialize them before
		// entering the Scene Color render pass.
		EnsureSkyBoxResources();
		FPostProcessRendererState::FSceneTargets* SceneTargets = EnsureSceneTargets(Width, Height);
		if (SceneTargets == nullptr || SceneTargets->Color == nullptr || SceneTargets->Depth == nullptr)
		{
			return;
		}
		FRHITexture* SceneColor = SceneTargets->Color;
		EnsureGizmoResources(CommandList);
		EnsureOverlayLineResources();
		EnsureOverlayIconResources(CommandList);
		EnsureEditorGridResources();
		const bool bEditorAssistanceOutputPipelinesReady = AreEditorAssistanceOutputPipelinesReady();
		if (!bEditorAssistanceOutputPipelinesReady && !GEditorAssistancePipelineFailureLogged)
		{
			GEditorAssistancePipelineFailureLogged = true;
			DURIN_ERROR("Editor assistance pipeline creation is incomplete; editor assistance drawing is disabled");
		}

		FRHIRenderPassInfo ScenePassInfo{};
		ScenePassInfo.RenderTargetLayout = RendererRenderTargetLayouts::MakeSceneTargets();
		ScenePassInfo.ColorRenderTargets[0] = SceneColor;
		ScenePassInfo.DepthStencilRenderTarget = SceneTargets->Depth;
		ScenePassInfo.ColorClearValues[0] = FClearValueBinding(
			View.ClearColor.r, View.ClearColor.g, View.ClearColor.b, View.ClearColor.a);
		ScenePassInfo.DepthStencilClearValue = FClearValueBinding(1.0f, 0u);
		CommandList.BeginRenderPass(ScenePassInfo, "SceneColorRenderPass");
		FSceneView RenderView = View;
		RenderView.ViewportX = 0;
		RenderView.ViewportY = 0;
		RenderView.ViewportWidth = Width;
		RenderView.ViewportHeight = Height;
		if (RenderView.AspectRatioConstraint > 0.0f)
		{
			uint32 ContentWidth = Width;
			uint32 ContentHeight = static_cast<uint32>(std::round(ContentWidth / RenderView.AspectRatioConstraint));
			if (ContentHeight > Height)
			{
				ContentHeight = Height;
				ContentWidth = static_cast<uint32>(std::round(ContentHeight * RenderView.AspectRatioConstraint));
			}
			RenderView.ViewportWidth = std::max(1u, ContentWidth);
			RenderView.ViewportHeight = std::max(1u, ContentHeight);
			RenderView.ViewportX = (Width - RenderView.ViewportWidth) / 2;
			RenderView.ViewportY = (Height - RenderView.ViewportHeight) / 2;
		}
		RenderScene(CommandList, Scene, RenderView, SceneColor);
		CommandList.EndRenderPass();

		FRHIRenderPassInfo PostProcessPassInfo{};
		PostProcessPassInfo.RenderTargetLayout = RendererRenderTargetLayouts::MakeScenePostProcessOutput();
		PostProcessPassInfo.ColorRenderTargets[0] = OutputTarget;
		PostProcessPassInfo.ColorClearValues[0] = FClearValueBinding(
			View.ClearColor.r, View.ClearColor.g, View.ClearColor.b, View.ClearColor.a);
		CommandList.BeginRenderPass(PostProcessPassInfo, bPresentOutput ? "PostProcessPresentRenderPass" : "PostProcessOffscreenRenderPass");
		DrawPostProcess(CommandList, SceneColor, Width, Height, bPresentOutput);
		CommandList.EndRenderPass();
		if (bEditorAssistanceOutputPipelinesReady)
		{
			PrepareEditorAssistance(CommandList, RenderView);
		}

		FRHIRenderPassInfo EditorAssistancePassInfo{};
		EditorAssistancePassInfo.RenderTargetLayout = RendererRenderTargetLayouts::MakeEditorAssistanceOutput(GetViewportOutput(bPresentOutput));
		EditorAssistancePassInfo.ColorRenderTargets[0] = OutputTarget;
		EditorAssistancePassInfo.DepthStencilRenderTarget = SceneTargets->Depth;
		CommandList.BeginRenderPass(EditorAssistancePassInfo,
			bPresentOutput ? "EditorAssistancePresentRenderPass" : "EditorAssistanceOffscreenRenderPass");
		if (bEditorAssistanceOutputPipelinesReady)
		{
			DrawEditorAssistance(CommandList, RenderView, bPresentOutput);
		}
		CommandList.EndRenderPass();
	}

	auto FRendererModule::RenderScene(FRHICommandListImmediate& CommandList, IScene* Scene, const FSceneView& View, FRHITexture* RenderTarget) -> void
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.RenderScene");
		const uint32 Width = View.ViewportWidth;
		const uint32 Height = View.ViewportHeight;
		if (Scene == nullptr || RenderTarget == nullptr || Width == 0 || Height == 0)
		{
			return;
		}

		CommandList.SetViewport(static_cast<float>(View.ViewportX), static_cast<float>(View.ViewportY), 0.0f,
			static_cast<float>(View.ViewportX + Width), static_cast<float>(View.ViewportY + Height), 1.0f);
		CommandList.SetScissor(static_cast<float>(View.ViewportX), static_cast<float>(View.ViewportY), static_cast<float>(Width), static_cast<float>(Height));

		DrawSkyBox(CommandList, *Scene, View);

		if (!EnsureStaticMeshBaseResources()) return;

		const ERenderMode RenderMode = GRenderMode.load(std::memory_order_relaxed);
		const ERasterMode RasterMode = GRasterMode.load(std::memory_order_relaxed);
		FDirectionalLightSceneData Light;
		Scene->GetDirectionalLight(Light);
		ForEachStaticMeshProxy(Scene, [&CommandList, &View, &Light, RenderMode, RasterMode](FStaticMeshSceneProxy& Proxy) {
			if (RenderMode == ERenderMode::Unlit || RenderMode == ERenderMode::Lit)
			{
				DrawStaticMeshProxy(CommandList, View, Light, RenderMode, RasterMode, Proxy);
			}
		});
		ForEachTextureCubeThumbnailProxy(
			Scene,
			[&CommandList, &View](FTextureCubePreviewSceneProxy& Proxy) {
				DrawTextureCubeThumbnailProxy(CommandList, View, Proxy);
			});
	}

	auto FRendererModule::PrepareEditorAssistance(FRHICommandListImmediate& CommandList, const FSceneView& View) -> void
	{
		PrepareOverlayLines(CommandList, View);
		PrepareOverlayIcons(CommandList, View);
	}

	auto FRendererModule::DrawEditorAssistance(FRHICommandListImmediate& CommandList, const FSceneView& View, bool bPresentOutput) -> void
	{
		CommandList.SetViewport(static_cast<float>(View.ViewportX), static_cast<float>(View.ViewportY), 0.0f,
			static_cast<float>(View.ViewportX + View.ViewportWidth), static_cast<float>(View.ViewportY + View.ViewportHeight), 1.0f);
		CommandList.SetScissor(static_cast<float>(View.ViewportX), static_cast<float>(View.ViewportY),
			static_cast<float>(View.ViewportWidth), static_cast<float>(View.ViewportHeight));

		using enum RendererEditorAssistance::EDrawOperation;
		for (const RendererEditorAssistance::EDrawOperation Operation : RendererEditorAssistance::GetDrawOrder())
		{
			switch (Operation)
			{
			case EditorGrid: DrawEditorGrid(CommandList, View, bPresentOutput); break;
			case XRayGizmos: DrawGizmoPrimitives(CommandList, View, true, bPresentOutput); break;
			case XRayOverlayLines: DrawOverlayLines(CommandList, true, bPresentOutput); break;
			case XRayOverlayIcons: DrawOverlayIcons(CommandList, true, bPresentOutput); break;
			case VisibleGizmos: DrawGizmoPrimitives(CommandList, View, false, bPresentOutput); break;
			case VisibleOverlayLines: DrawOverlayLines(CommandList, false, bPresentOutput); break;
			case VisibleOverlayIcons: DrawOverlayIcons(CommandList, false, bPresentOutput); break;
			}
		}
	}

	IMPLEMENT_MODULE(FRendererModule, Renderer)
}
