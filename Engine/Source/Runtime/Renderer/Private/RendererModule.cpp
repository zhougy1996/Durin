#include "RendererModule.h"

#include "RHI.h"
#include "RHICommandList.h"
#include "Scene.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"
#include "StaticMesh/StaticMeshResources.h"

#include <glm/mat4x4.hpp>

namespace Durin
{
	namespace
	{
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
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FStaticMeshFragmentShader, FShader, "/Engine/StaticMesh", EShaderFrequency::Fragment, "FragmentMain");
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
			FVector4f Color{1.0f, 1.0f, 1.0f, 1.0f};
			glm::mat4 LocalToWorld{1.0f};
			glm::mat4 NormalToWorld{1.0f};
		};

		struct FStaticMeshLightingUniform
		{
			FVector4f LightDirection{-0.5f, -0.5f, -1.0f, 0.0f};
			FVector4f LightColorIntensity{1.0f, 1.0f, 1.0f, 1.0f};
			FVector4f ViewPositionAmbient{0.0f, 0.0f, 0.0f, 0.08f};
			FVector4f MaterialParams{0.35f, 32.0f, 1.0f, 0.0f};
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

		struct FGizmoMeshRange
		{
			uint32 FirstIndex = 0;
			uint32 IndexCount = 0;
			int32 VertexOffset = 0;
		};

		struct FGizmoRendererState
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FGizmoVertexShader> VertexShader;
			TShaderRef<FGizmoFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef XRayPipelineState;
			FGraphicsPipelineStateRHIRef VisiblePipelineState;
			FBufferRHIRef VertexBuffer;
			FBufferRHIRef IndexBuffer;
			std::array<FGizmoMeshRange, 5> MeshRanges{};
			bool bCreateAttempted = false;
		};

		struct FStaticMeshRendererState
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FStaticMeshVertexShader> VertexShader;
			TShaderRef<FStaticMeshFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FGraphicsPipelineStateRHIRef SolidPipelineState;
			FGraphicsPipelineStateRHIRef WireframePipelineState;
			bool bCreateAttempted = false;
		};

		struct FPostProcessRendererState
		{
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
			FTextureRHIRef SceneColor;
			FTextureRHIRef SceneDepth;
			uint32 SceneColorWidth = 0;
			uint32 SceneColorHeight = 0;
			bool bCreateAttempted = false;
			std::atomic_bool bEnableFXAA = true;
		};

		FStaticMeshRendererState GStaticMeshState;
		FPostProcessRendererState GPostProcessState;
		FGizmoRendererState GGizmoState;
		std::atomic<ERenderMode> GRenderMode = ERenderMode::Lit;
		std::atomic<ERasterMode> GRasterMode = ERasterMode::Solid;

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
			constexpr float MinorRadius = 0.025f;
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
			Initializer.RenderPassName = "SceneColorRenderPass";
			Initializer.BoundShaders.VertexShader = GGizmoState.VertexShader.GetRHIShader();
			Initializer.BoundShaders.FragmentShader = GGizmoState.FragmentShader.GetRHIShader();
			Initializer.VertexDeclaration = GGizmoState.VertexDeclaration;
			Initializer.PixelFormat = EPixelFormat::SRGBA8_UNORM;
			Initializer.DepthStencilFormat = EPixelFormat::D32;
			Initializer.bEnableAlphaBlend = true;
			Initializer.bEnableBackFaceCulling = false;
			Initializer.bEnableDepthWrite = false;
			Initializer.PipelineLayout = ShaderMap->GetMergedPipelineLayout();
			Initializer.bEnableDepthTest = false;
			GGizmoState.XRayPipelineState = GDynamicRHI->RHICreateGraphicsPipelineState("GizmoXRayPipeline", Initializer);
			Initializer.bEnableDepthTest = true;
			GGizmoState.VisiblePipelineState = GDynamicRHI->RHICreateGraphicsPipelineState("GizmoVisiblePipeline", Initializer);

			std::vector<FVector3f> Vertices;
			std::vector<uint32> Indices;
			GGizmoState.MeshRanges[static_cast<size_t>(EViewOverlayShape::Arrow)] = BeginGizmoMesh(Vertices, Indices);
			AppendCylinder(Vertices, Indices, 0.0f, 0.76f, 0.025f, 12);
			AppendCone(Vertices, Indices, 0.72f, 1.0f, 0.075f, 12);
			EndGizmoMesh(GGizmoState.MeshRanges[static_cast<size_t>(EViewOverlayShape::Arrow)], Indices);
			GGizmoState.MeshRanges[static_cast<size_t>(EViewOverlayShape::Axis)] = BeginGizmoMesh(Vertices, Indices);
			AppendCylinder(Vertices, Indices, 0.0f, 0.94f, 0.025f, 12);
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

			FRHIBufferCreateDesc VertexDesc = FRHIBufferCreateDesc::CreateVertex("GizmoVertexBuffer", static_cast<uint32>(Vertices.size() * sizeof(FVector3f)));
			VertexDesc.Usage |= EBufferUsageFlags::Static;
			VertexDesc.InitialData = {Vertices.data(), static_cast<uint32>(Vertices.size() * sizeof(FVector3f))};
			GGizmoState.VertexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, VertexDesc);
			FRHIBufferCreateDesc IndexDesc = FRHIBufferCreateDesc::CreateIndex("GizmoIndexBuffer", static_cast<uint32>(Indices.size() * sizeof(uint32)), sizeof(uint32));
			IndexDesc.Usage |= EBufferUsageFlags::Static;
			IndexDesc.InitialData = {Indices.data(), static_cast<uint32>(Indices.size() * sizeof(uint32))};
			GGizmoState.IndexBuffer = GDynamicRHI->RHICreateBuffer(CommandList, IndexDesc);
		}

		auto EnsureStaticMeshPipeline() -> void
		{
			if (GStaticMeshState.bCreateAttempted)
			{
				return;
			}

			GStaticMeshState.bCreateAttempted = true;

			FShaderCompileOptions CompileOptions;
			FShaderType& VertexShaderType = FStaticMeshVertexShader::StaticType();
			FShaderType& FragmentShaderType = FStaticMeshFragmentShader::StaticType();
			std::array<const FShaderType*, 2> ShaderTypes = {&VertexShaderType, &FragmentShaderType};
			std::shared_ptr<FShaderMapBase> ShaderMap = std::make_shared<FShaderMapBase>();
			std::string ErrorMessage;
			if (!ShaderMap->InitializeFromShaderTypes(ShaderTypes, CompileOptions, ErrorMessage))
			{
				DURIN_ERROR("Failed to initialize StaticMesh shader map: {}", ErrorMessage);
				return;
			}

			auto* VertexShader = static_cast<FStaticMeshVertexShader*>(ShaderMap->GetShader(&VertexShaderType));
			auto* FragmentShader = static_cast<FStaticMeshFragmentShader*>(ShaderMap->GetShader(&FragmentShaderType));
			check(VertexShader);
			check(FragmentShader);

			GStaticMeshState.ShaderMap = ShaderMap;
			GStaticMeshState.VertexShader = TShaderRef<FStaticMeshVertexShader>(VertexShader, ShaderMap.get());
			GStaticMeshState.FragmentShader = TShaderRef<FStaticMeshFragmentShader>(FragmentShader, ShaderMap.get());

			FVertexDeclarationElementList VertexDeclElements;
			constexpr uint32 VertexStride = sizeof(FVector3f);
			VertexDeclElements[0] = FVertexElement(0, 0, EVertexElementType::Float3, 0, VertexStride);
			VertexDeclElements[1] = FVertexElement(1, 0, EVertexElementType::Float3, 1, VertexStride);
			GStaticMeshState.VertexDeclaration = GDynamicRHI->RHICreateVertexDeclaration(VertexDeclElements);

			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderPassName = "SceneColorRenderPass";
			Initializer.BoundShaders.VertexShader = GStaticMeshState.VertexShader.GetRHIShader();
			Initializer.BoundShaders.FragmentShader = GStaticMeshState.FragmentShader.GetRHIShader();
			Initializer.VertexDeclaration = GStaticMeshState.VertexDeclaration;
			Initializer.PixelFormat = EPixelFormat::SRGBA8_UNORM;
			Initializer.DepthStencilFormat = EPixelFormat::D32;
			Initializer.bEnableAlphaBlend = false;
			Initializer.bEnableDepthTest = true;
			Initializer.bEnableDepthWrite = true;
			Initializer.bEnableBackFaceCulling = false;
			Initializer.PipelineLayout = ShaderMap->GetMergedPipelineLayout();
			GStaticMeshState.SolidPipelineState = GDynamicRHI->RHICreateGraphicsPipelineState("StaticMeshSolidPipeline", Initializer);
			Initializer.PolygonMode = FGraphicsPipelineStateInitializer::EPolygonMode::Line;
			Initializer.bEnableBackFaceCulling = false;
			GStaticMeshState.WireframePipelineState = GDynamicRHI->RHICreateGraphicsPipelineState("StaticMeshWireframePipeline", Initializer);
		}

		auto CreatePostProcessPipeline(
			FName PipelineName,
			FName RenderPassName,
			FRHIShader* VertexShader,
			FRHIShader* FragmentShader,
			const FPipelineLayoutDesc& PipelineLayout
		) -> FGraphicsPipelineStateRHIRef
		{
			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderPassName = RenderPassName;
			Initializer.BoundShaders.VertexShader = VertexShader;
			Initializer.BoundShaders.FragmentShader = FragmentShader;
			Initializer.VertexDeclaration = GPostProcessState.VertexDeclaration;
			Initializer.PixelFormat = EPixelFormat::SRGBA8_UNORM;
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
				"PostProcessOffscreenRenderPass",
				GPostProcessState.CopyVertexShader.GetRHIShader(),
				GPostProcessState.CopyFragmentShader.GetRHIShader(),
				CopyShaderMap->GetMergedPipelineLayout()
			);
			GPostProcessState.CopyPresentPipelineState = CreatePostProcessPipeline(
				"PostProcessCopyPresentPipeline",
				"PostProcessPresentRenderPass",
				GPostProcessState.CopyVertexShader.GetRHIShader(),
				GPostProcessState.CopyFragmentShader.GetRHIShader(),
				CopyShaderMap->GetMergedPipelineLayout()
			);
			GPostProcessState.FXAAOffscreenPipelineState = CreatePostProcessPipeline(
				"PostProcessFXAAOffscreenPipeline",
				"PostProcessOffscreenRenderPass",
				GPostProcessState.FXAAVertexShader.GetRHIShader(),
				GPostProcessState.FXAAFragmentShader.GetRHIShader(),
				FXAAShaderMap->GetMergedPipelineLayout()
			);
			GPostProcessState.FXAAPresentPipelineState = CreatePostProcessPipeline(
				"PostProcessFXAAPresentPipeline",
				"PostProcessPresentRenderPass",
				GPostProcessState.FXAAVertexShader.GetRHIShader(),
				GPostProcessState.FXAAFragmentShader.GetRHIShader(),
				FXAAShaderMap->GetMergedPipelineLayout()
			);
		}

		auto EnsureSceneColor(uint32 Width, uint32 Height) -> FRHITexture*
		{
			if (GPostProcessState.SceneColor != nullptr
				&& GPostProcessState.SceneColorWidth == Width
				&& GPostProcessState.SceneColorHeight == Height)
			{
				return GPostProcessState.SceneColor;
			}

			FRHITextureCreateDesc SceneColorDesc = FRHITextureCreateDesc::Create2D("SceneColor", Width, Height, EPixelFormat::SRGBA8_UNORM);
			SceneColorDesc.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource);
			SceneColorDesc.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f));
			GPostProcessState.SceneColor = RHICreateTexture(SceneColorDesc);
			FRHITextureCreateDesc SceneDepthDesc = FRHITextureCreateDesc::Create2D("SceneDepth", Width, Height, EPixelFormat::D32);
			SceneDepthDesc.SetFlags(ETextureCreateFlags::DepthStencilTargetable);
			SceneDepthDesc.SetClearValue(FClearValueBinding(1.0f, 0u));
			GPostProcessState.SceneDepth = RHICreateTexture(SceneDepthDesc);
			GPostProcessState.SceneColorWidth = Width;
			GPostProcessState.SceneColorHeight = Height;
			return GPostProcessState.SceneColor;
		}

		auto ToShaderMatrix(const FMatrix& Matrix) -> glm::mat4
		{
			return glm::transpose(glm::mat4(Matrix));
		}

		auto DrawStaticMeshProxy(FRHICommandListImmediate& CommandList, const FSceneView& View, const FDirectionalLightSceneData& Light, ERenderMode RenderMode, ERasterMode RasterMode, const FStaticMeshSceneProxy& Proxy) -> void
		{
			FStaticMeshRenderData* RenderData = Proxy.GetRenderData();
			if (RenderData == nullptr || RenderData->IndexCount == 0)
			{
				return;
			}

			if (!RenderData->IsReadyForRendering())
			{
				return;
			}

			FStaticMeshTransformUniform TransformUniform;
			TransformUniform.LocalToClip = ToShaderMatrix(View.ViewProjectionMatrix * Proxy.GetLocalToWorld());
			TransformUniform.Color = Proxy.GetMaterialRenderData().BaseColor;
			TransformUniform.LocalToWorld = ToShaderMatrix(Proxy.GetLocalToWorld());
			TransformUniform.NormalToWorld = ToShaderMatrix(glm::transpose(glm::inverse(Proxy.GetLocalToWorld())));
			const FRHIUniformBufferRange TransformUniformBuffer = CommandList.AllocateDynamicUniformBuffer(&TransformUniform, sizeof(TransformUniform));

			FGraphicsPipelineStateRHIRef Pipeline = RasterMode == ERasterMode::Wireframe ? GStaticMeshState.WireframePipelineState : GStaticMeshState.SolidPipelineState;
			CommandList.SetGraphicsPipelineState(*Pipeline);

			FStaticMeshVertexShader::FParameters VertexShaderParameters;
			VertexShaderParameters.Transform = TransformUniformBuffer;
			SetShaderParameters(CommandList, GStaticMeshState.VertexShader, VertexShaderParameters);

			const FMaterialRenderData& Material = Proxy.GetMaterialRenderData();
			FStaticMeshLightingUniform LightingUniform;
			LightingUniform.LightDirection = FVector4f(FVector3f(Light.Direction), 0.0f);
			LightingUniform.LightColorIntensity = FVector4f(Light.Color, Light.Intensity);
			LightingUniform.ViewPositionAmbient = FVector4f(FVector3f(View.ViewLocation), Light.AmbientIntensity);
			LightingUniform.MaterialParams = FVector4f(Material.SpecularStrength, Material.Shininess, RenderMode == ERenderMode::Lit ? 1.0f : 0.0f, 0.0f);
			const FRHIUniformBufferRange LightingUniformBuffer = CommandList.AllocateDynamicUniformBuffer(&LightingUniform, sizeof(LightingUniform));
			FStaticMeshFragmentShader::FParameters FragmentShaderParameters;
			FragmentShaderParameters.Lighting = LightingUniformBuffer;
			SetShaderParameters(CommandList, GStaticMeshState.FragmentShader, FragmentShaderParameters);

			CommandList.BindVertexBuffer(0, RenderData->PositionVertexBufferRHI, 0);
			CommandList.BindVertexBuffer(1, RenderData->NormalVertexBufferRHI, 0);
			CommandList.BindIndexBuffer(RenderData->IndexBufferRHI, 0);
			CommandList.DrawIndexed(RenderData->IndexCount, 0, 0);
		}

		auto DrawGizmoPrimitives(FRHICommandListImmediate& CommandList, const FSceneView& View, bool bXRay) -> void
		{
			if (View.OverlayPrimitives.empty() || GGizmoState.VertexBuffer == nullptr || GGizmoState.IndexBuffer == nullptr) return;
			const FGraphicsPipelineStateRHIRef Pipeline = bXRay ? GGizmoState.XRayPipelineState : GGizmoState.VisiblePipelineState;
			if (Pipeline == nullptr) return;
			CommandList.SetGraphicsPipelineState(*Pipeline);
			CommandList.BindVertexBuffer(0, GGizmoState.VertexBuffer, 0);
			CommandList.BindIndexBuffer(GGizmoState.IndexBuffer, 0);
			for (const FViewOverlayPrimitive& Primitive : View.OverlayPrimitives)
			{
				const size_t ShapeIndex = static_cast<size_t>(Primitive.Shape);
				if (ShapeIndex >= GGizmoState.MeshRanges.size()) continue;
				const FGizmoMeshRange& Range = GGizmoState.MeshRanges[ShapeIndex];
				FGizmoTransformUniform Uniform;
				Uniform.LocalToClip = ToShaderMatrix(View.ViewProjectionMatrix * Primitive.LocalToWorld);
				Uniform.Color = Primitive.Color;
				if (bXRay) Uniform.Color.a *= 0.18f;
				const FRHIUniformBufferRange Buffer = CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
				FGizmoVertexShader::FParameters Parameters;
				Parameters.Transform = Buffer;
				SetShaderParameters(CommandList, GGizmoState.VertexShader, Parameters);
				CommandList.DrawIndexed(Range.IndexCount, Range.FirstIndex, Range.VertexOffset);
			}
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
	}

	auto FRendererModule::ShutdownModule() -> void
	{
		GStaticMeshState = {};
		GGizmoState = {};
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
		GPostProcessState.SceneColor = nullptr;
		GPostProcessState.SceneDepth = nullptr;
		GPostProcessState.SceneColorWidth = 0;
		GPostProcessState.SceneColorHeight = 0;
		GPostProcessState.bCreateAttempted = false;
		GPostProcessState.bEnableFXAA.store(true, std::memory_order_relaxed);
	}

	auto FRendererModule::CreateScene() -> std::unique_ptr<IScene>
	{
		return std::make_unique<FScene>();
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

	auto FRendererModule::PrepareSceneResources(FRHICommandListImmediate& CommandList, IScene* Scene) -> void
	{
		ForEachStaticMeshProxy(Scene, [&CommandList](FStaticMeshSceneProxy& Proxy) {
			if (FStaticMeshRenderData* RenderData = Proxy.GetRenderData())
			{
				RenderData->InitResources(CommandList);
			}
		});
	}

	auto FRendererModule::RenderView(FRHICommandListImmediate& CommandList, IScene* Scene, const FSceneView& View, FRHITexture* OutputTarget, bool bPresentOutput) -> void
	{
		const uint32 Width = OutputTarget != nullptr ? OutputTarget->GetSizeX() : 0;
		const uint32 Height = OutputTarget != nullptr ? OutputTarget->GetSizeY() : 0;
		if (OutputTarget == nullptr || Width == 0 || Height == 0)
		{
			return;
		}

		EnsurePostProcessResources(CommandList);
		FRHITexture* SceneColor = EnsureSceneColor(Width, Height);
		if (SceneColor == nullptr)
		{
			return;
		}

		FRHIRenderPassInfo ScenePassInfo{};
		ScenePassInfo.ColorRenderTargets[0] = SceneColor;
		ScenePassInfo.DepthStencilRenderTarget = GPostProcessState.SceneDepth;
		ScenePassInfo.ColorClearValue = FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
		ScenePassInfo.DepthStencilClearValue = FClearValueBinding(1.0f, 0u);
		CommandList.BeginRenderPass(ScenePassInfo, "SceneColorRenderPass");
		FSceneView RenderView = View;
		RenderView.ViewportWidth = Width;
		RenderView.ViewportHeight = Height;
		RenderScene(CommandList, Scene, RenderView, SceneColor);
		CommandList.EndRenderPass();

		FRHIRenderPassInfo PostProcessPassInfo{};
		PostProcessPassInfo.ColorRenderTargets[0] = OutputTarget;
		PostProcessPassInfo.ColorClearValue = FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
		CommandList.BeginRenderPass(PostProcessPassInfo, bPresentOutput ? "PostProcessPresentRenderPass" : "PostProcessOffscreenRenderPass");
		DrawPostProcess(CommandList, SceneColor, Width, Height, bPresentOutput);
		CommandList.EndRenderPass();
	}

	auto FRendererModule::RenderScene(FRHICommandListImmediate& CommandList, IScene* Scene, const FSceneView& View, FRHITexture* RenderTarget) -> void
	{
		const uint32 Width = View.ViewportWidth;
		const uint32 Height = View.ViewportHeight;
		if (Scene == nullptr || RenderTarget == nullptr || Width == 0 || Height == 0)
		{
			return;
		}

		EnsureStaticMeshPipeline();
		if (!View.OverlayPrimitives.empty()) EnsureGizmoResources(CommandList);
		if (GStaticMeshState.SolidPipelineState == nullptr || GStaticMeshState.WireframePipelineState == nullptr || !GStaticMeshState.VertexShader || !GStaticMeshState.FragmentShader)
		{
			return;
		}

		CommandList.SetViewport(0.0f, 0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height), 1.0f);
		CommandList.SetScissor(0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height));

		const ERenderMode RenderMode = GRenderMode.load(std::memory_order_relaxed);
		const ERasterMode RasterMode = GRasterMode.load(std::memory_order_relaxed);
		FDirectionalLightSceneData Light;
		if (!Scene->GetDirectionalLight(Light)) Light.AmbientIntensity = 0.08f;
		ForEachStaticMeshProxy(Scene, [&CommandList, &View, &Light, RenderMode, RasterMode](FStaticMeshSceneProxy& Proxy) {
			if (RenderMode == ERenderMode::Unlit || RenderMode == ERenderMode::Lit)
			{
				DrawStaticMeshProxy(CommandList, View, Light, RenderMode, RasterMode, Proxy);
			}
		});
		DrawGizmoPrimitives(CommandList, View, true);
		DrawGizmoPrimitives(CommandList, View, false);
	}

	IMPLEMENT_MODULE(FRendererModule, Renderer)
}
