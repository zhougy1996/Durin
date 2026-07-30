#include "RendererEditorAssistance.h"

#include "CoreGlobals.h"
#include "Misc/AssertionMacros.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"

#include <glm/gtc/constants.hpp>
#include <limits>

namespace Durin::RendererEditorAssistance
{
	namespace
	{
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

		struct FGizmoState
		{
			struct FBasePayload
			{
				std::shared_ptr<FShaderMapBase> ShaderMap;
				TShaderRef<FGizmoVertexShader> VertexShader;
				TShaderRef<FGizmoFragmentShader> FragmentShader;
				FVertexDeclarationRHIRef VertexDeclaration;
				FBufferRHIRef VertexBuffer;
				FBufferRHIRef IndexBuffer;
				std::array<FGizmoMeshRange, 6> MeshRanges{};
			};

			TRenderResourceCreationSlot<FBasePayload> Base{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FGizmoVertexShader> VertexShader;
			TShaderRef<FGizmoFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FBufferRHIRef VertexBuffer;
			FBufferRHIRef IndexBuffer;
			std::array<FGizmoMeshRange, 6> MeshRanges{};
		};

		struct FOverlayLineState
		{
			struct FBasePayload
			{
				std::shared_ptr<FShaderMapBase> ShaderMap;
				TShaderRef<FOverlayLineVertexShader> VertexShader;
				TShaderRef<FOverlayLineFragmentShader> FragmentShader;
				FVertexDeclarationRHIRef VertexDeclaration;
			};

			TRenderResourceCreationSlot<FBasePayload> Base{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FOverlayLineVertexShader> VertexShader;
			TShaderRef<FOverlayLineFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FBufferRHIRef VertexBuffer;
			FBufferRHIRef IndexBuffer;
			uint32 VertexCapacity = 0;
			uint32 IndexCapacity = 0;
		};

		struct FOverlayIconState
		{
			struct FBasePayload
			{
				std::shared_ptr<FShaderMapBase> ShaderMap;
				TShaderRef<FOverlayIconVertexShader> VertexShader;
				TShaderRef<FOverlayIconFragmentShader> FragmentShader;
				FVertexDeclarationRHIRef VertexDeclaration;
				FTextureRHIRef Atlas;
				FSamplerRHIRef AtlasSampler;
			};

			TRenderResourceCreationSlot<FBasePayload> Base{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FOverlayIconVertexShader> VertexShader;
			TShaderRef<FOverlayIconFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FBufferRHIRef VertexBuffer;
			FBufferRHIRef IndexBuffer;
			FTextureRHIRef Atlas;
			FSamplerRHIRef AtlasSampler;
			uint32 VertexCapacity = 0;
			uint32 IndexCapacity = 0;
		};

		struct FEditorGridState
		{
			struct FBasePayload
			{
				std::shared_ptr<FShaderMapBase> ShaderMap;
				TShaderRef<FEditorGridVertexShader> VertexShader;
				TShaderRef<FEditorGridFragmentShader> FragmentShader;
				FVertexDeclarationRHIRef VertexDeclaration;
			};

			TRenderResourceCreationSlot<FBasePayload> Base{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FEditorGridVertexShader> VertexShader;
			TShaderRef<FEditorGridFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
		};

		struct FPipelineEntry
		{
			FPipelineKey Key;
			TRenderResourceCreationSlot<FGraphicsPipelineStateRHIRef> Slot{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
		};

		// Owns reusable assistance resources; prepared view data never enters this state.
		struct FRendererState
		{
			FRenderResourceGeneration Generation;
			std::optional<uint64> ForceRecompileShaderGeneration;
			FGizmoState Gizmo;
			FOverlayLineState OverlayLine;
			FOverlayIconState OverlayIcon;
			FEditorGridState EditorGrid;
			std::vector<FPipelineEntry> Pipelines;
		};

		FRendererState GState;

		auto ConfigureShaderCompileOptions(
			FShaderCompileOptions& CompileOptions) -> void
		{
			CompileOptions.bForceRecompile =
				GState.ForceRecompileShaderGeneration
					== GState.Generation.Shader;
		}

		auto ToShaderMatrix(const FMatrix& Matrix) -> glm::mat4
		{
			return glm::transpose(glm::mat4(Matrix));
		}

		auto BeginGizmoMesh(const std::vector<uint32>& Indices) -> FGizmoMeshRange
		{
			FGizmoMeshRange Range;
			Range.FirstIndex = static_cast<uint32>(Indices.size());
			return Range;
		}

		auto EndGizmoMesh(
			FGizmoMeshRange& Range,
			const std::vector<uint32>& Indices) -> void
		{
			Range.IndexCount =
				static_cast<uint32>(Indices.size()) - Range.FirstIndex;
		}

		auto AppendCylinder(
			std::vector<FVector3f>& Vertices,
			std::vector<uint32>& Indices,
			float StartX,
			float EndX,
			float Radius,
			uint32 Segments) -> void
		{
			const uint32 Base = static_cast<uint32>(Vertices.size());
			for (uint32 Ring = 0; Ring < 2; ++Ring)
			{
				const float X = Ring == 0 ? StartX : EndX;
				for (uint32 Segment = 0; Segment < Segments; ++Segment)
				{
					const float Angle = glm::two_pi<float>()
						* static_cast<float>(Segment)
						/ static_cast<float>(Segments);
					Vertices.emplace_back(
						X, std::cos(Angle) * Radius, std::sin(Angle) * Radius);
				}
			}
			for (uint32 Segment = 0; Segment < Segments; ++Segment)
			{
				const uint32 Next = (Segment + 1) % Segments;
				Indices.insert(Indices.end(), {
					Base + Segment,
					Base + Segments + Segment,
					Base + Segments + Next,
					Base + Segment,
					Base + Segments + Next,
					Base + Next,
				});
			}
		}

		auto AppendCone(
			std::vector<FVector3f>& Vertices,
			std::vector<uint32>& Indices,
			float BaseX,
			float TipX,
			float Radius,
			uint32 Segments) -> void
		{
			const uint32 Base = static_cast<uint32>(Vertices.size());
			for (uint32 Segment = 0; Segment < Segments; ++Segment)
			{
				const float Angle = glm::two_pi<float>()
					* static_cast<float>(Segment)
					/ static_cast<float>(Segments);
				Vertices.emplace_back(
					BaseX, std::cos(Angle) * Radius, std::sin(Angle) * Radius);
			}
			const uint32 Tip = static_cast<uint32>(Vertices.size());
			Vertices.emplace_back(TipX, 0.0f, 0.0f);
			for (uint32 Segment = 0; Segment < Segments; ++Segment)
			{
				const uint32 Next = (Segment + 1) % Segments;
				Indices.insert(
					Indices.end(), {Base + Segment, Tip, Base + Next});
			}
		}

		auto AppendBox(
			std::vector<FVector3f>& Vertices,
			std::vector<uint32>& Indices) -> void
		{
			const uint32 Base = static_cast<uint32>(Vertices.size());
			for (uint32 Corner = 0; Corner < 8; ++Corner)
			{
				Vertices.emplace_back(
					(Corner & 1) ? 0.5f : -0.5f,
					(Corner & 2) ? 0.5f : -0.5f,
					(Corner & 4) ? 0.5f : -0.5f);
			}
			static constexpr uint32 BoxIndices[] = {
				0,2,3,0,3,1,4,5,7,4,7,6,0,1,5,0,5,4,
				2,6,7,2,7,3,0,4,6,0,6,2,1,3,7,1,7,5,
			};
			for (const uint32 Index : BoxIndices)
				Indices.push_back(Base + Index);
		}

		auto AppendWireBox(
			std::vector<FVector3f>& Vertices,
			std::vector<uint32>& Indices) -> void
		{
			const uint32 Base = static_cast<uint32>(Vertices.size());
			for (uint32 Corner = 0; Corner < 8; ++Corner)
			{
				Vertices.emplace_back(
					(Corner & 1) ? 0.5f : -0.5f,
					(Corner & 2) ? 0.5f : -0.5f,
					(Corner & 4) ? 0.5f : -0.5f);
			}
			static constexpr uint32 BoxEdgeIndices[] = {
				0,1,0,2,0,4,1,3,1,5,2,3,2,6,3,7,4,5,4,6,5,7,6,7,
			};
			for (const uint32 Index : BoxEdgeIndices)
				Indices.push_back(Base + Index);
		}

		auto AppendPlane(
			std::vector<FVector3f>& Vertices,
			std::vector<uint32>& Indices) -> void
		{
			const uint32 Base = static_cast<uint32>(Vertices.size());
			Vertices.insert(Vertices.end(), {
				{0.0f, 0.0f, 0.0f},
				{1.0f, 0.0f, 0.0f},
				{1.0f, 1.0f, 0.0f},
				{0.0f, 1.0f, 0.0f},
			});
			Indices.insert(Indices.end(), {
				Base, Base + 1, Base + 2,
				Base, Base + 2, Base + 3,
				Base, Base + 2, Base + 1,
				Base, Base + 3, Base + 2,
			});
		}

		auto AppendRing(
			std::vector<FVector3f>& Vertices,
			std::vector<uint32>& Indices,
			uint32 Segments) -> void
		{
			const uint32 Base = static_cast<uint32>(Vertices.size());
			constexpr uint32 TubeSegments = 8;
			constexpr float MajorRadius = 1.0f;
			constexpr float MinorRadius = 0.032f;
			for (uint32 Segment = 0; Segment < Segments; ++Segment)
			{
				const float Major = glm::two_pi<float>()
					* static_cast<float>(Segment)
					/ static_cast<float>(Segments);
				for (uint32 Tube = 0; Tube < TubeSegments; ++Tube)
				{
					const float Minor = glm::two_pi<float>()
						* static_cast<float>(Tube)
						/ static_cast<float>(TubeSegments);
					const float Radius =
						MajorRadius + std::cos(Minor) * MinorRadius;
					Vertices.emplace_back(
						std::sin(Minor) * MinorRadius,
						std::cos(Major) * Radius,
						std::sin(Major) * Radius);
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
					const uint32 C =
						Base + NextSegment * TubeSegments + NextTube;
					const uint32 D = Base + Segment * TubeSegments + NextTube;
					Indices.insert(Indices.end(), {A, B, C, A, C, D});
				}
			}
		}

		auto MakeCreateError(
			ERenderResourceCreateErrorCategory Category,
			std::string Context,
			std::string Identity,
			std::string Message)
			-> FRenderResourceCreateError
		{
			return {
				.Category = Category,
				.Context = std::move(Context),
				.Identity = std::move(Identity),
				.Message = std::move(Message),
				.RetryDependencies =
					ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device
					| ERenderResourceGenerationDependency::Manual,
			};
		}

		auto ReportCreateDiagnostic(
			const FRenderResourceCreateDiagnostic& Diagnostic) -> void
		{
			if (!Diagnostic.Error)
				return;
			const FRenderResourceCreateError& Error = *Diagnostic.Error;
			if (Diagnostic.Kind
				== ERenderResourceCreateDiagnosticKind::Recovery)
			{
				DURIN_INFO(
					"Recovered editor assistance resource: context={}, identity={}",
					Error.Context,
					Error.Identity);
				return;
			}
			DURIN_ERROR(
				"Editor assistance resource creation failed: category={}, context={}, identity={}, generation={}/{}/{}, retained={}, message={}",
				static_cast<uint8>(Error.Category),
				Error.Context,
				Error.Identity,
				Error.AttemptedGeneration.Shader,
				Error.AttemptedGeneration.Device,
				Error.AttemptedGeneration.Manual,
				Error.bRetainedFallback,
				Error.Message);
		}

		auto EnsureGizmoBase(FRHICommandListImmediate& CommandList) -> bool
		{
			FGizmoState& State = GState.Gizmo;
			using FResult =
				TRenderResourceCreateResult<FGizmoState::FBasePayload>;
			FGizmoState::FBasePayload* Payload = State.Base.Resolve(
				GState.Generation,
				[&CommandList]() -> FResult {
					FShaderCompileOptions CompileOptions;
					ConfigureShaderCompileOptions(CompileOptions);
					FShaderType& VertexShaderType =
						FGizmoVertexShader::StaticType();
					FShaderType& FragmentShaderType =
						FGizmoFragmentShader::StaticType();
					const std::array<const FShaderType*, 2> ShaderTypes = {
						&VertexShaderType, &FragmentShaderType};
					auto ShaderMap = std::make_shared<FShaderMapBase>();
					std::string ErrorMessage;
					if (!ShaderMap->InitializeFromShaderTypes(
							ShaderTypes, CompileOptions, ErrorMessage))
						return FResult::Failure(MakeCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"EditorAssistanceBase",
							"Gizmo",
							std::move(ErrorMessage)));
					auto* VertexShader = static_cast<FGizmoVertexShader*>(
						ShaderMap->GetShader(&VertexShaderType));
					auto* FragmentShader = static_cast<FGizmoFragmentShader*>(
						ShaderMap->GetShader(&FragmentShaderType));
					if (VertexShader == nullptr || FragmentShader == nullptr)
						return FResult::Failure(MakeCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"EditorAssistanceBase",
							"Gizmo",
							"Compiled shader map is missing a typed shader."));

					FGizmoState::FBasePayload Candidate;
					Candidate.ShaderMap = std::move(ShaderMap);
					Candidate.VertexShader = TShaderRef<FGizmoVertexShader>(
						VertexShader, Candidate.ShaderMap.get());
					Candidate.FragmentShader =
						TShaderRef<FGizmoFragmentShader>(
							FragmentShader, Candidate.ShaderMap.get());
					FVertexDeclarationElementList Elements;
					Elements[0] = FVertexElement(
						0, 0, EVertexElementType::Float3, 0,
						sizeof(FVector3f));
					Candidate.VertexDeclaration =
						GDynamicRHI->RHICreateVertexDeclaration(Elements);

					std::vector<FVector3f> Vertices;
					std::vector<uint32> Indices;
					auto& MeshRanges = Candidate.MeshRanges;
					MeshRanges[static_cast<size_t>(
						EViewOverlayShape::Arrow)] = BeginGizmoMesh(Indices);
					AppendCylinder(
						Vertices, Indices, 0.0f, 0.76f, 0.032f, 12);
					AppendCone(
						Vertices, Indices, 0.72f, 1.0f, 0.085f, 12);
					EndGizmoMesh(
						MeshRanges[static_cast<size_t>(
							EViewOverlayShape::Arrow)],
						Indices);
					MeshRanges[static_cast<size_t>(
						EViewOverlayShape::Axis)] = BeginGizmoMesh(Indices);
					AppendCylinder(
						Vertices, Indices, 0.0f, 0.94f, 0.032f, 12);
					EndGizmoMesh(
						MeshRanges[static_cast<size_t>(
							EViewOverlayShape::Axis)],
						Indices);
					MeshRanges[static_cast<size_t>(
						EViewOverlayShape::Plane)] = BeginGizmoMesh(Indices);
					AppendPlane(Vertices, Indices);
					EndGizmoMesh(
						MeshRanges[static_cast<size_t>(
							EViewOverlayShape::Plane)],
						Indices);
					MeshRanges[static_cast<size_t>(
						EViewOverlayShape::Ring)] = BeginGizmoMesh(Indices);
					AppendRing(Vertices, Indices, 64);
					EndGizmoMesh(
						MeshRanges[static_cast<size_t>(
							EViewOverlayShape::Ring)],
						Indices);
					MeshRanges[static_cast<size_t>(
						EViewOverlayShape::Box)] = BeginGizmoMesh(Indices);
					AppendBox(Vertices, Indices);
					EndGizmoMesh(
						MeshRanges[static_cast<size_t>(
							EViewOverlayShape::Box)],
						Indices);
					MeshRanges[static_cast<size_t>(
						EViewOverlayShape::WireBox)] =
						BeginGizmoMesh(Indices);
					AppendWireBox(Vertices, Indices);
					EndGizmoMesh(
						MeshRanges[static_cast<size_t>(
							EViewOverlayShape::WireBox)],
						Indices);

					FRHIBufferCreateDesc VertexDesc =
						FRHIBufferCreateDesc::CreateVertex(
							"GizmoVertexBuffer",
							static_cast<uint32>(
								Vertices.size() * sizeof(FVector3f)));
					VertexDesc.Usage |= EBufferUsageFlags::Static;
					VertexDesc.InitialData = {
						Vertices.data(),
						static_cast<uint32>(
							Vertices.size() * sizeof(FVector3f))};
					Candidate.VertexBuffer =
						GDynamicRHI->RHICreateBuffer(
							CommandList, VertexDesc);
					FRHIBufferCreateDesc IndexDesc =
						FRHIBufferCreateDesc::CreateIndex(
							"GizmoIndexBuffer",
							static_cast<uint32>(
								Indices.size() * sizeof(uint32)),
							sizeof(uint32));
					IndexDesc.Usage |= EBufferUsageFlags::Static;
					IndexDesc.InitialData = {
						Indices.data(),
						static_cast<uint32>(
							Indices.size() * sizeof(uint32))};
					Candidate.IndexBuffer =
						GDynamicRHI->RHICreateBuffer(
							CommandList, IndexDesc);
					if (Candidate.VertexDeclaration == nullptr
						|| Candidate.VertexBuffer == nullptr
						|| Candidate.IndexBuffer == nullptr)
						return FResult::Failure(MakeCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"EditorAssistanceBase",
							"Gizmo",
							"RHI creation returned null."));
					return FResult::Success(std::move(Candidate));
				},
				ReportCreateDiagnostic);
			if (Payload == nullptr)
				return false;
			State.ShaderMap = Payload->ShaderMap;
			State.VertexShader = Payload->VertexShader;
			State.FragmentShader = Payload->FragmentShader;
			State.VertexDeclaration = Payload->VertexDeclaration;
			State.VertexBuffer = Payload->VertexBuffer;
			State.IndexBuffer = Payload->IndexBuffer;
			State.MeshRanges = Payload->MeshRanges;
			return true;
		}

		auto EnsureOverlayLineBase() -> bool
		{
			FOverlayLineState& State = GState.OverlayLine;
			using FResult = TRenderResourceCreateResult<
				FOverlayLineState::FBasePayload>;
			auto* Payload = State.Base.Resolve(
				GState.Generation,
				[]() -> FResult {
					FShaderCompileOptions CompileOptions;
					ConfigureShaderCompileOptions(CompileOptions);
					FShaderType& VertexShaderType =
						FOverlayLineVertexShader::StaticType();
					FShaderType& FragmentShaderType =
						FOverlayLineFragmentShader::StaticType();
					const std::array<const FShaderType*, 2> ShaderTypes = {
						&VertexShaderType, &FragmentShaderType};
					auto ShaderMap = std::make_shared<FShaderMapBase>();
					std::string ErrorMessage;
					if (!ShaderMap->InitializeFromShaderTypes(
							ShaderTypes, CompileOptions, ErrorMessage))
						return FResult::Failure(MakeCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"EditorAssistanceBase",
							"OverlayLine",
							std::move(ErrorMessage)));
					auto* VertexShader =
						static_cast<FOverlayLineVertexShader*>(
							ShaderMap->GetShader(&VertexShaderType));
					auto* FragmentShader =
						static_cast<FOverlayLineFragmentShader*>(
							ShaderMap->GetShader(&FragmentShaderType));
					if (VertexShader == nullptr || FragmentShader == nullptr)
						return FResult::Failure(MakeCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"EditorAssistanceBase",
							"OverlayLine",
							"Compiled shader map is missing a typed shader."));
					FOverlayLineState::FBasePayload Candidate;
					Candidate.ShaderMap = std::move(ShaderMap);
					Candidate.VertexShader =
						TShaderRef<FOverlayLineVertexShader>(
							VertexShader, Candidate.ShaderMap.get());
					Candidate.FragmentShader =
						TShaderRef<FOverlayLineFragmentShader>(
							FragmentShader, Candidate.ShaderMap.get());
					FVertexDeclarationElementList Elements;
					Elements[0] = FVertexElement(
						0,
						static_cast<uint8>(offsetof(
							FOverlayLineVertex, Position)),
						EVertexElementType::Float4, 0,
						sizeof(FOverlayLineVertex));
					Elements[1] = FVertexElement(
						0,
						static_cast<uint8>(offsetof(
							FOverlayLineVertex, Color)),
						EVertexElementType::Float4, 1,
						sizeof(FOverlayLineVertex));
					Elements[2] = FVertexElement(
						0,
						static_cast<uint8>(offsetof(
							FOverlayLineVertex, Pattern)),
						EVertexElementType::Float2, 2,
						sizeof(FOverlayLineVertex));
					Candidate.VertexDeclaration =
						GDynamicRHI->RHICreateVertexDeclaration(Elements);
					if (Candidate.VertexDeclaration == nullptr)
						return FResult::Failure(MakeCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"EditorAssistanceBase",
							"OverlayLine",
							"RHI vertex declaration creation returned null."));
					return FResult::Success(std::move(Candidate));
				},
				ReportCreateDiagnostic);
			if (Payload == nullptr)
				return false;
			State.ShaderMap = Payload->ShaderMap;
			State.VertexShader = Payload->VertexShader;
			State.FragmentShader = Payload->FragmentShader;
			State.VertexDeclaration = Payload->VertexDeclaration;
			return true;
		}

		auto BuildEditorIconAtlasPixels() -> std::array<uint8, 128 * 64 * 4>
		{
			constexpr uint32 Size = 64;
			constexpr uint32 SamplesPerAxis = 4;
			constexpr uint32 AtlasWidth = Size * 2;
			std::array<uint8, AtlasWidth * Size * 4> Pixels{};
			auto InsideCircle = [](
				float X, float Y, float CenterX, float CenterY, float Radius) {
				const float DX = X - CenterX;
				const float DY = Y - CenterY;
				return DX * DX + DY * DY <= Radius * Radius;
			};
			auto InsideLens = [](float X, float Y) {
				if (X < 42.0f || X > 57.0f)
					return false;
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
						for (uint32 SampleX = 0;
							SampleX < SamplesPerAxis;
							++SampleX)
						{
							const float PX = static_cast<float>(X)
								+ (static_cast<float>(SampleX) + 0.5f)
									/ SamplesPerAxis;
							const float PY = static_cast<float>(Y)
								+ (static_cast<float>(SampleY) + 0.5f)
									/ SamplesPerAxis;
							const bool bBody =
								PX >= 9.0f && PX <= 44.0f
								&& PY >= 27.0f && PY <= 49.0f;
							const bool bReels =
								InsideCircle(PX, PY, 19.0f, 21.0f, 10.0f)
								|| InsideCircle(PX, PY, 37.0f, 20.0f, 9.0f);
							const bool bReelHole =
								InsideCircle(PX, PY, 19.0f, 21.0f, 3.5f)
								|| InsideCircle(PX, PY, 37.0f, 20.0f, 3.0f);
							if ((bBody || bReels || InsideLens(PX, PY))
								&& !bReelHole)
								++CoveredSamples;
						}
					}
					const size_t Offset =
						(static_cast<size_t>(Y) * AtlasWidth + X) * 4;
					Pixels[Offset + 0] = 255;
					Pixels[Offset + 1] = 255;
					Pixels[Offset + 2] = 255;
					Pixels[Offset + 3] = static_cast<uint8>(
						CoveredSamples * 255
						/ (SamplesPerAxis * SamplesPerAxis));
				}
			}
			for (uint32 Y = 0; Y < Size; ++Y)
			{
				for (uint32 X = 0; X < Size; ++X)
				{
					uint32 CoveredSamples = 0;
					for (uint32 SampleY = 0; SampleY < SamplesPerAxis; ++SampleY)
					{
						for (uint32 SampleX = 0;
							SampleX < SamplesPerAxis;
							++SampleX)
						{
							const float PX = static_cast<float>(X)
								+ (static_cast<float>(SampleX) + 0.5f)
									/ SamplesPerAxis
								- 32.0f;
							const float PY = static_cast<float>(Y)
								+ (static_cast<float>(SampleY) + 0.5f)
									/ SamplesPerAxis
								- 32.0f;
							const float Radius = std::sqrt(PX * PX + PY * PY);
							const float Angle = std::atan2(PY, PX);
							const float RayAxisDistance =
								std::abs(std::sin(Angle * 4.0f)) * Radius;
							const bool bDisc = Radius <= 12.0f;
							const bool bRay = Radius >= 17.0f
								&& Radius <= 27.0f
								&& RayAxisDistance <= 2.2f;
							if (bDisc || bRay)
								++CoveredSamples;
						}
					}
					const size_t Offset =
						(static_cast<size_t>(Y) * AtlasWidth + Size + X) * 4;
					Pixels[Offset + 0] = 255;
					Pixels[Offset + 1] = 255;
					Pixels[Offset + 2] = 255;
					Pixels[Offset + 3] = static_cast<uint8>(
						CoveredSamples * 255
						/ (SamplesPerAxis * SamplesPerAxis));
				}
			}
			return Pixels;
		}

		auto EnsureOverlayIconBase(
			FRHICommandListImmediate& CommandList) -> bool
		{
			FOverlayIconState& State = GState.OverlayIcon;
			using FResult = TRenderResourceCreateResult<
				FOverlayIconState::FBasePayload>;
			auto* Payload = State.Base.Resolve(
				GState.Generation,
				[&CommandList]() -> FResult {
					FShaderCompileOptions CompileOptions;
					ConfigureShaderCompileOptions(CompileOptions);
					FShaderType& VertexShaderType =
						FOverlayIconVertexShader::StaticType();
					FShaderType& FragmentShaderType =
						FOverlayIconFragmentShader::StaticType();
					const std::array<const FShaderType*, 2> ShaderTypes = {
						&VertexShaderType, &FragmentShaderType};
					auto ShaderMap = std::make_shared<FShaderMapBase>();
					std::string ErrorMessage;
					if (!ShaderMap->InitializeFromShaderTypes(
							ShaderTypes, CompileOptions, ErrorMessage))
						return FResult::Failure(MakeCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"EditorAssistanceBase",
							"OverlayIcon",
							std::move(ErrorMessage)));
					auto* VertexShader =
						static_cast<FOverlayIconVertexShader*>(
							ShaderMap->GetShader(&VertexShaderType));
					auto* FragmentShader =
						static_cast<FOverlayIconFragmentShader*>(
							ShaderMap->GetShader(&FragmentShaderType));
					if (VertexShader == nullptr || FragmentShader == nullptr)
						return FResult::Failure(MakeCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"EditorAssistanceBase",
							"OverlayIcon",
							"Compiled shader map is missing a typed shader."));
					FOverlayIconState::FBasePayload Candidate;
					Candidate.ShaderMap = std::move(ShaderMap);
					Candidate.VertexShader =
						TShaderRef<FOverlayIconVertexShader>(
							VertexShader, Candidate.ShaderMap.get());
					Candidate.FragmentShader =
						TShaderRef<FOverlayIconFragmentShader>(
							FragmentShader, Candidate.ShaderMap.get());
					FVertexDeclarationElementList Elements;
					Elements[0] = FVertexElement(
						0, static_cast<uint8>(offsetof(
							FOverlayIconVertex, Position)),
						EVertexElementType::Float4, 0,
						sizeof(FOverlayIconVertex));
					Elements[1] = FVertexElement(
						0, static_cast<uint8>(offsetof(
							FOverlayIconVertex, UV)),
						EVertexElementType::Float2, 1,
						sizeof(FOverlayIconVertex));
					Elements[2] = FVertexElement(
						0, static_cast<uint8>(offsetof(
							FOverlayIconVertex, Color)),
						EVertexElementType::Float4, 2,
						sizeof(FOverlayIconVertex));
					Candidate.VertexDeclaration =
						GDynamicRHI->RHICreateVertexDeclaration(Elements);
					const auto Pixels = BuildEditorIconAtlasPixels();
					FRHITextureCreateDesc TextureDesc =
						FRHITextureCreateDesc::Create2D(
							"EditorOverlayIconAtlas", 128, 64,
							EPixelFormat::RGBA8_UNORM)
						.SetFlags(ETextureCreateFlags::ShaderResource);
					Candidate.Atlas = GDynamicRHI->RHICreateTexture(
						CommandList, TextureDesc);
					if (Candidate.Atlas != nullptr)
					{
						const FUpdateTextureRegion2D Region(
							0, 0, 0, 0, 128, 64);
						GDynamicRHI->RHIUpdateTexture2D(
							CommandList, Candidate.Atlas, 0, 0, Region,
							128 * 4, Pixels.data());
					}
					Candidate.AtlasSampler =
						RHICreateSampler(FRHISamplerDesc::LinearClamp());
					if (Candidate.VertexDeclaration == nullptr
						|| Candidate.Atlas == nullptr
						|| Candidate.AtlasSampler == nullptr)
						return FResult::Failure(MakeCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"EditorAssistanceBase",
							"OverlayIcon",
							"RHI creation returned null."));
					return FResult::Success(std::move(Candidate));
				},
				ReportCreateDiagnostic);
			if (Payload == nullptr)
				return false;
			State.ShaderMap = Payload->ShaderMap;
			State.VertexShader = Payload->VertexShader;
			State.FragmentShader = Payload->FragmentShader;
			State.VertexDeclaration = Payload->VertexDeclaration;
			State.Atlas = Payload->Atlas;
			State.AtlasSampler = Payload->AtlasSampler;
			return true;
		}

		auto EnsureEditorGridBase() -> bool
		{
			FEditorGridState& State = GState.EditorGrid;
			using FResult = TRenderResourceCreateResult<
				FEditorGridState::FBasePayload>;
			auto* Payload = State.Base.Resolve(
				GState.Generation,
				[]() -> FResult {
					FShaderCompileOptions CompileOptions;
					ConfigureShaderCompileOptions(CompileOptions);
					FShaderType& VertexShaderType =
						FEditorGridVertexShader::StaticType();
					FShaderType& FragmentShaderType =
						FEditorGridFragmentShader::StaticType();
					const std::array<const FShaderType*, 2> ShaderTypes = {
						&VertexShaderType, &FragmentShaderType};
					auto ShaderMap = std::make_shared<FShaderMapBase>();
					std::string ErrorMessage;
					if (!ShaderMap->InitializeFromShaderTypes(
							ShaderTypes, CompileOptions, ErrorMessage))
						return FResult::Failure(MakeCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"EditorAssistanceBase",
							"EditorGrid",
							std::move(ErrorMessage)));
					auto* VertexShader =
						static_cast<FEditorGridVertexShader*>(
							ShaderMap->GetShader(&VertexShaderType));
					auto* FragmentShader =
						static_cast<FEditorGridFragmentShader*>(
							ShaderMap->GetShader(&FragmentShaderType));
					if (VertexShader == nullptr || FragmentShader == nullptr)
						return FResult::Failure(MakeCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"EditorAssistanceBase",
							"EditorGrid",
							"Compiled shader map is missing a typed shader."));
					FEditorGridState::FBasePayload Candidate;
					Candidate.ShaderMap = std::move(ShaderMap);
					Candidate.VertexShader =
						TShaderRef<FEditorGridVertexShader>(
							VertexShader, Candidate.ShaderMap.get());
					Candidate.FragmentShader =
						TShaderRef<FEditorGridFragmentShader>(
							FragmentShader, Candidate.ShaderMap.get());
					FVertexDeclarationElementList Elements;
					Elements[0] = FVertexElement(
						0,
						offsetof(
							FFullscreenGeometryResources::FVertex, Position),
						EVertexElementType::Float2,
						0,
						sizeof(FFullscreenGeometryResources::FVertex));
					Candidate.VertexDeclaration =
						GDynamicRHI->RHICreateVertexDeclaration(Elements);
					if (Candidate.VertexDeclaration == nullptr)
						return FResult::Failure(MakeCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"EditorAssistanceBase",
							"EditorGrid",
							"RHI vertex declaration creation returned null."));
					return FResult::Success(std::move(Candidate));
				},
				ReportCreateDiagnostic);
			if (Payload == nullptr)
				return false;
			State.ShaderMap = Payload->ShaderMap;
			State.VertexShader = Payload->VertexShader;
			State.FragmentShader = Payload->FragmentShader;
			State.VertexDeclaration = Payload->VertexDeclaration;
			return true;
		}

		auto GetFeatureName(EFeature Feature) -> std::string_view
		{
			switch (Feature)
			{
			case EFeature::EditorGrid: return "EditorGrid";
			case EFeature::Gizmo: return "Gizmo";
			case EFeature::OverlayLine: return "OverlayLine";
			case EFeature::OverlayIcon: return "OverlayIcon";
			}
			return "Unknown";
		}

		auto GetOutputName(
			RenderTargetLayouts::EViewportOutput Output)
			-> std::string_view
		{
			return Output
					== RenderTargetLayouts::EViewportOutput::Present
				? "Present"
				: "Offscreen";
		}

		auto GetDepthName(EDepthMode DepthMode) -> std::string_view
		{
			return DepthMode == EDepthMode::XRay ? "XRay" : "Visible";
		}

		auto GetTopologyName(EGizmoTopology Topology) -> std::string_view
		{
			switch (Topology)
			{
			case EGizmoTopology::NotApplicable: return "Default";
			case EGizmoTopology::Solid: return "Solid";
			case EGizmoTopology::Wire: return "Wire";
			}
			return "Unknown";
		}

		auto GetBasePayloadGeneration(EFeature Feature)
			-> const FRenderResourceGeneration&
		{
			switch (Feature)
			{
			case EFeature::EditorGrid:
				return GState.EditorGrid.Base.GetPayloadGeneration();
			case EFeature::Gizmo:
				return GState.Gizmo.Base.GetPayloadGeneration();
			case EFeature::OverlayLine:
				return GState.OverlayLine.Base.GetPayloadGeneration();
			case EFeature::OverlayIcon:
				return GState.OverlayIcon.Base.GetPayloadGeneration();
			}
			return GState.EditorGrid.Base.GetPayloadGeneration();
		}

		auto GetPipelineGeneration(EFeature Feature)
			-> FRenderResourceGeneration
		{
			FRenderResourceGeneration Generation = GState.Generation;
			Generation.Shader = GetBasePayloadGeneration(Feature).Shader;
			return Generation;
		}

		auto EnsurePipeline(const FPipelineKey& Key)
			-> FGraphicsPipelineStateRHIRef
		{
			auto EntryIt = std::ranges::find(
				GState.Pipelines, Key, &FPipelineEntry::Key);
			if (EntryIt == GState.Pipelines.end())
			{
				EntryIt = GState.Pipelines.emplace(
					GState.Pipelines.end(), FPipelineEntry{.Key = Key});
			}
			FPipelineEntry& Entry = *EntryIt;
			const FRenderResourceGeneration Generation =
				GetPipelineGeneration(Key.Feature);
			const std::string PipelineName = std::format(
				"{}{}{}{}Pipeline",
				GetFeatureName(Key.Feature),
				Key.Feature == EFeature::Gizmo
					? GetTopologyName(Key.GizmoTopology)
					: std::string_view{},
				GetDepthName(Key.DepthMode),
				GetOutputName(Key.Output));
			using FResult =
				TRenderResourceCreateResult<FGraphicsPipelineStateRHIRef>;
			auto* Payload = Entry.Slot.Resolve(
				Generation,
				[&Key, &PipelineName]() -> FResult {
					FGraphicsPipelineStateInitializer Initializer;
					std::shared_ptr<FShaderMapBase> ShaderMap;
					switch (Key.Feature)
					{
					case EFeature::EditorGrid:
						ShaderMap = GState.EditorGrid.ShaderMap;
						Initializer.BoundShaders.VertexShader =
							GState.EditorGrid.VertexShader.GetRHIShader();
						Initializer.BoundShaders.FragmentShader =
							GState.EditorGrid.FragmentShader.GetRHIShader();
						Initializer.VertexDeclaration =
							GState.EditorGrid.VertexDeclaration;
						break;
					case EFeature::Gizmo:
						ShaderMap = GState.Gizmo.ShaderMap;
						Initializer.BoundShaders.VertexShader =
							GState.Gizmo.VertexShader.GetRHIShader();
						Initializer.BoundShaders.FragmentShader =
							GState.Gizmo.FragmentShader.GetRHIShader();
						Initializer.VertexDeclaration =
							GState.Gizmo.VertexDeclaration;
						if (Key.GizmoTopology == EGizmoTopology::Wire)
							Initializer.PrimitiveTopology =
								FGraphicsPipelineStateInitializer::
									EPrimitiveTopology::LineList;
						break;
					case EFeature::OverlayLine:
						ShaderMap = GState.OverlayLine.ShaderMap;
						Initializer.BoundShaders.VertexShader =
							GState.OverlayLine.VertexShader.GetRHIShader();
						Initializer.BoundShaders.FragmentShader =
							GState.OverlayLine.FragmentShader.GetRHIShader();
						Initializer.VertexDeclaration =
							GState.OverlayLine.VertexDeclaration;
						break;
					case EFeature::OverlayIcon:
						ShaderMap = GState.OverlayIcon.ShaderMap;
						Initializer.BoundShaders.VertexShader =
							GState.OverlayIcon.VertexShader.GetRHIShader();
						Initializer.BoundShaders.FragmentShader =
							GState.OverlayIcon.FragmentShader.GetRHIShader();
						Initializer.VertexDeclaration =
							GState.OverlayIcon.VertexDeclaration;
						break;
					}
					Initializer.RenderTargetLayout =
						RenderTargetLayouts::
							MakeEditorAssistanceOutput(Key.Output);
					Initializer.bEnableAlphaBlend = true;
					Initializer.bEnableBackFaceCulling = false;
					Initializer.bEnableDepthTest =
						Key.DepthMode == EDepthMode::Visible;
					Initializer.bEnableDepthWrite = false;
					if (ShaderMap != nullptr)
						Initializer.PipelineLayout =
							ShaderMap->GetMergedPipelineLayout();
					FGraphicsPipelineStateRHIRef Candidate =
						GDynamicRHI->RHICreateGraphicsPipelineState(
							FName(PipelineName), Initializer);
					if (Candidate == nullptr)
						return FResult::Failure(MakeCreateError(
							ERenderResourceCreateErrorCategory::
								GraphicsPipeline,
							"EditorAssistancePipeline",
							PipelineName,
							"RHI graphics pipeline creation returned null."));
					return FResult::Success(std::move(Candidate));
				},
				ReportCreateDiagnostic);
			return Payload != nullptr ? *Payload : nullptr;
		}

		auto AddPreparedPipeline(FPrepared& Prepared, const FPipelineKey& Key)
			-> void
		{
			if (FGraphicsPipelineStateRHIRef Pipeline = EnsurePipeline(Key);
				Pipeline != nullptr)
			{
				Prepared.Pipelines.push_back({
					.Key = Key,
					.Pipeline = std::move(Pipeline),
				});
			}
		}

		auto BuildOverlayLineGeometry(
			const FSceneView& View,
			std::vector<FOverlayLineVertex>& OutVertices,
			std::vector<uint32>& OutIndices) -> void
		{
			constexpr double ClipEpsilon = 1.e-8;
			auto ClipSegment = [](FVector4& Start, FVector4& End) {
				auto ClipPlane = [&](auto PlaneDistance, double Minimum) {
					const double StartDistance = PlaneDistance(Start);
					const double EndDistance = PlaneDistance(End);
					if (StartDistance < Minimum && EndDistance < Minimum)
						return false;
					if (StartDistance < Minimum || EndDistance < Minimum)
					{
						const double T =
							(Minimum - StartDistance)
							/ (EndDistance - StartDistance);
						const FVector4 Intersection = glm::mix(Start, End, T);
						if (StartDistance < Minimum)
							Start = Intersection;
						else
							End = Intersection;
					}
					return true;
				};
				return ClipPlane(
						   [](const FVector4& Value) { return Value.w; },
						   ClipEpsilon)
					&& ClipPlane(
						[](const FVector4& Value) { return Value.z; }, 0.0);
			};
			for (const FViewOverlayLine& Line : View.OverlayLines)
			{
				FVector4 ClipStart =
					View.ViewProjectionMatrix * FVector4(Line.Start, 1.0);
				FVector4 ClipEnd =
					View.ViewProjectionMatrix * FVector4(Line.End, 1.0);
				if (!std::isfinite(ClipStart.w)
					|| !std::isfinite(ClipEnd.w)
					|| !std::isfinite(ClipStart.z)
					|| !std::isfinite(ClipEnd.z)
					|| !ClipSegment(ClipStart, ClipEnd))
					continue;
				const FVector2 NdcStart = FVector2(ClipStart) / ClipStart.w;
				const FVector2 NdcEnd = FVector2(ClipEnd) / ClipEnd.w;
				const FVector2f PixelDelta{
					static_cast<float>(
						(NdcEnd.x - NdcStart.x) * 0.5 * View.ViewportWidth),
					static_cast<float>(
						(NdcEnd.y - NdcStart.y) * 0.5 * View.ViewportHeight),
				};
				const float PixelLength = glm::length(PixelDelta);
				if (!std::isfinite(PixelLength) || PixelLength <= 0.001f)
					continue;
				const FVector2f PixelNormal{
					-PixelDelta.y / PixelLength,
					PixelDelta.x / PixelLength,
				};
				const float HalfWidth =
					std::max(0.5f, Line.WidthPixels * 0.5f);
				const FVector2 NdcOffset{
					static_cast<double>(
						PixelNormal.x * HalfWidth * 2.0f
						/ std::max(1u, View.ViewportWidth)),
					static_cast<double>(
						PixelNormal.y * HalfWidth * 2.0f
						/ std::max(1u, View.ViewportHeight)),
				};
				auto MakePosition = [](
					const FVector4& Clip, const FVector2& Offset) {
					return FVector4f(
						static_cast<float>(Clip.x + Offset.x * Clip.w),
						static_cast<float>(Clip.y + Offset.y * Clip.w),
						static_cast<float>(Clip.z),
						static_cast<float>(Clip.w));
				};
				const float PatternPeriod =
					Line.Pattern == EViewOverlayLinePattern::Dashed
					? std::max(2.0f, Line.PatternPeriodPixels)
					: 0.0f;
				const uint32 Base =
					static_cast<uint32>(OutVertices.size());
				OutVertices.push_back({
					MakePosition(ClipStart, NdcOffset),
					Line.Color,
					{0.0f, PatternPeriod},
				});
				OutVertices.push_back({
					MakePosition(ClipStart, -NdcOffset),
					Line.Color,
					{0.0f, PatternPeriod},
				});
				OutVertices.push_back({
					MakePosition(ClipEnd, NdcOffset),
					Line.Color,
					{PixelLength, PatternPeriod},
				});
				OutVertices.push_back({
					MakePosition(ClipEnd, -NdcOffset),
					Line.Color,
					{PixelLength, PatternPeriod},
				});
				OutIndices.insert(OutIndices.end(), {
					Base, Base + 1, Base + 2,
					Base + 2, Base + 1, Base + 3,
				});
			}
		}

		auto BuildOverlayIconGeometry(
			const FSceneView& View,
			std::vector<FOverlayIconVertex>& OutVertices,
			std::vector<uint32>& OutIndices) -> void
		{
			for (const FViewOverlayIcon& Icon : View.OverlayIcons)
			{
				if (!std::isfinite(Icon.SizePixels) || Icon.SizePixels <= 0.0f)
					continue;
				const float MinU =
					Icon.Icon == EViewOverlayIcon::DirectionalLight
					? 0.5f
					: 0.0f;
				const float MaxU = MinU + 0.5f;
				const FVector4 Clip =
					View.ViewProjectionMatrix
					* FVector4(Icon.WorldPosition, 1.0);
				if (!std::isfinite(Clip.x) || !std::isfinite(Clip.y)
					|| !std::isfinite(Clip.z) || !std::isfinite(Clip.w)
					|| Clip.w <= 1.e-8 || Clip.z < 0.0)
					continue;
				const double HalfNdcX =
					static_cast<double>(Icon.SizePixels)
					/ std::max(1u, View.ViewportWidth);
				const double HalfNdcY =
					static_cast<double>(Icon.SizePixels)
					/ std::max(1u, View.ViewportHeight);
				auto MakePosition = [&](double X, double Y) {
					return FVector4f(
						static_cast<float>(Clip.x + X * Clip.w),
						static_cast<float>(Clip.y + Y * Clip.w),
						static_cast<float>(Clip.z),
						static_cast<float>(Clip.w));
				};
				const uint32 Base =
					static_cast<uint32>(OutVertices.size());
				OutVertices.push_back({
					MakePosition(-HalfNdcX, -HalfNdcY),
					{MinU, 0.0f},
					Icon.Color,
				});
				OutVertices.push_back({
					MakePosition(HalfNdcX, -HalfNdcY),
					{MaxU, 0.0f},
					Icon.Color,
				});
				OutVertices.push_back({
					MakePosition(-HalfNdcX, HalfNdcY),
					{MinU, 1.0f},
					Icon.Color,
				});
				OutVertices.push_back({
					MakePosition(HalfNdcX, HalfNdcY),
					{MaxU, 1.0f},
					Icon.Color,
				});
				OutIndices.insert(OutIndices.end(), {
					Base, Base + 1, Base + 2,
					Base + 2, Base + 1, Base + 3,
				});
			}
		}

		template<typename VertexType, typename StateType>
		auto UploadDynamicGeometry(
			FRHICommandListImmediate& CommandList,
			const std::vector<VertexType>& Vertices,
			const std::vector<uint32>& Indices,
			StateType& State,
			const char* VertexBufferName,
			const char* IndexBufferName) -> uint32
		{
			if (Vertices.empty() || Indices.empty())
				return 0;
			const uint32 VertexBytes =
				static_cast<uint32>(Vertices.size() * sizeof(VertexType));
			const uint32 IndexBytes =
				static_cast<uint32>(Indices.size() * sizeof(uint32));
			if (VertexBytes > State.VertexCapacity)
			{
				const uint32 Capacity = std::bit_ceil(VertexBytes);
				FRHIBufferCreateDesc Desc =
					FRHIBufferCreateDesc::CreateVertex(
						VertexBufferName, Capacity);
				Desc.Usage |= EBufferUsageFlags::Dynamic;
				FBufferRHIRef Buffer =
					GDynamicRHI->RHICreateBuffer(CommandList, Desc);
				if (Buffer == nullptr)
					return 0;
				State.VertexBuffer = std::move(Buffer);
				State.VertexCapacity = Capacity;
			}
			if (IndexBytes > State.IndexCapacity)
			{
				const uint32 Capacity = std::bit_ceil(IndexBytes);
				FRHIBufferCreateDesc Desc =
					FRHIBufferCreateDesc::CreateIndex(
						IndexBufferName, Capacity, sizeof(uint32));
				Desc.Usage |= EBufferUsageFlags::Dynamic;
				FBufferRHIRef Buffer =
					GDynamicRHI->RHICreateBuffer(CommandList, Desc);
				if (Buffer == nullptr)
					return 0;
				State.IndexBuffer = std::move(Buffer);
				State.IndexCapacity = Capacity;
			}
			if (State.VertexBuffer == nullptr || State.IndexBuffer == nullptr)
				return 0;
			CommandList.WriteBuffer(
				State.VertexBuffer, Vertices.data(), VertexBytes, 0);
			CommandList.WriteBuffer(
				State.IndexBuffer, Indices.data(), IndexBytes, 0);
			return static_cast<uint32>(Indices.size());
		}

		auto FindPreparedPipeline(
			const FPrepared& Prepared,
			EFeature Feature,
			EDepthMode DepthMode,
			EGizmoTopology Topology = EGizmoTopology::NotApplicable)
			-> FGraphicsPipelineStateRHIRef
		{
			const auto It = std::ranges::find_if(
				Prepared.Pipelines,
				[Feature, DepthMode, Topology](
					const FPreparedPipeline& PreparedPipeline) {
					return PreparedPipeline.Key.Feature == Feature
						&& PreparedPipeline.Key.DepthMode == DepthMode
						&& PreparedPipeline.Key.GizmoTopology == Topology;
				});
			return It != Prepared.Pipelines.end() ? It->Pipeline : nullptr;
		}

		auto DrawEditorGrid(
			FRHICommandListImmediate& CommandList,
			const FPrepared& Prepared) -> void
		{
			if (!Prepared.EditorGridUniform.has_value())
				return;
			const FGraphicsPipelineStateRHIRef Pipeline =
				FindPreparedPipeline(
					Prepared, EFeature::EditorGrid, EDepthMode::Visible);
			if (Pipeline == nullptr
				|| GetFullscreenGeometryResources()
					.GetVertexBuffer_RenderThread() == nullptr
				|| GetFullscreenGeometryResources()
					.GetIndexBuffer_RenderThread() == nullptr)
				return;
			CommandList.SetGraphicsPipelineState(*Pipeline);
			CommandList.BindVertexBuffer(
				0,
				GetFullscreenGeometryResources()
					.GetVertexBuffer_RenderThread(),
				0);
			CommandList.BindIndexBuffer(
				GetFullscreenGeometryResources()
					.GetIndexBuffer_RenderThread(),
				0);
			FEditorGridFragmentShader::FParameters Parameters;
			Parameters.Grid = CommandList.AllocateDynamicUniformBuffer(
				&*Prepared.EditorGridUniform,
				sizeof(*Prepared.EditorGridUniform));
			SetShaderParameters(
				CommandList, GState.EditorGrid.FragmentShader, Parameters);
			CommandList.DrawIndexed(3, 0, 0);
		}

		auto DrawGizmos(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FPrepared& Prepared,
			EDepthMode DepthMode) -> void
		{
			if (GState.Gizmo.VertexBuffer == nullptr
				|| GState.Gizmo.IndexBuffer == nullptr)
				return;
			CommandList.BindVertexBuffer(0, GState.Gizmo.VertexBuffer, 0);
			CommandList.BindIndexBuffer(GState.Gizmo.IndexBuffer, 0);
			for (const FViewOverlayPrimitive& Primitive : View.OverlayPrimitives)
			{
				const EGizmoTopology Topology =
					Primitive.Shape == EViewOverlayShape::WireBox
					? EGizmoTopology::Wire
					: EGizmoTopology::Solid;
				const FGraphicsPipelineStateRHIRef Pipeline =
					FindPreparedPipeline(
						Prepared, EFeature::Gizmo, DepthMode, Topology);
				if (Pipeline == nullptr)
					continue;
				const size_t ShapeIndex = static_cast<size_t>(Primitive.Shape);
				if (ShapeIndex >= GState.Gizmo.MeshRanges.size())
					continue;
				CommandList.SetGraphicsPipelineState(*Pipeline);
				const FGizmoMeshRange& Range =
					GState.Gizmo.MeshRanges[ShapeIndex];
				FGizmoTransformUniform Uniform;
				Uniform.LocalToClip = ToShaderMatrix(
					View.ViewProjectionMatrix * Primitive.LocalToWorld);
				Uniform.Color = Primitive.Color;
				if (DepthMode == EDepthMode::XRay)
					Uniform.Color.a *= 0.32f;
				FGizmoVertexShader::FParameters Parameters;
				Parameters.Transform =
					CommandList.AllocateDynamicUniformBuffer(
						&Uniform, sizeof(Uniform));
				SetShaderParameters(
					CommandList, GState.Gizmo.VertexShader, Parameters);
				CommandList.DrawIndexed(
					Range.IndexCount, Range.FirstIndex, Range.VertexOffset);
			}
		}

		auto DrawOverlayLines(
			FRHICommandListImmediate& CommandList,
			const FPrepared& Prepared,
			EDepthMode DepthMode) -> void
		{
			if (Prepared.OverlayLineIndexCount == 0
				|| GState.OverlayLine.VertexBuffer == nullptr
				|| GState.OverlayLine.IndexBuffer == nullptr)
				return;
			const FGraphicsPipelineStateRHIRef Pipeline =
				FindPreparedPipeline(
					Prepared, EFeature::OverlayLine, DepthMode);
			if (Pipeline == nullptr)
				return;
			CommandList.SetGraphicsPipelineState(*Pipeline);
			CommandList.BindVertexBuffer(
				0, GState.OverlayLine.VertexBuffer, 0);
			CommandList.BindIndexBuffer(GState.OverlayLine.IndexBuffer, 0);
			const FOverlayLineStyleUniform Style{
				DepthMode == EDepthMode::XRay ? 0.32f : 1.0f};
			FOverlayLineFragmentShader::FParameters Parameters;
			Parameters.Style = CommandList.AllocateDynamicUniformBuffer(
				&Style, sizeof(Style));
			SetShaderParameters(
				CommandList, GState.OverlayLine.FragmentShader, Parameters);
			CommandList.DrawIndexed(
				Prepared.OverlayLineIndexCount, 0, 0);
		}

		auto DrawOverlayIcons(
			FRHICommandListImmediate& CommandList,
			const FPrepared& Prepared,
			EDepthMode DepthMode) -> void
		{
			if (Prepared.OverlayIconIndexCount == 0
				|| GState.OverlayIcon.VertexBuffer == nullptr
				|| GState.OverlayIcon.IndexBuffer == nullptr
				|| GState.OverlayIcon.Atlas == nullptr
				|| GState.OverlayIcon.AtlasSampler == nullptr)
				return;
			const FGraphicsPipelineStateRHIRef Pipeline =
				FindPreparedPipeline(
					Prepared, EFeature::OverlayIcon, DepthMode);
			if (Pipeline == nullptr)
				return;
			CommandList.SetGraphicsPipelineState(*Pipeline);
			CommandList.BindVertexBuffer(
				0, GState.OverlayIcon.VertexBuffer, 0);
			CommandList.BindIndexBuffer(GState.OverlayIcon.IndexBuffer, 0);
			const FOverlayIconStyleUniform Style{
				DepthMode == EDepthMode::XRay ? 0.3f : 1.0f};
			FOverlayIconFragmentShader::FParameters Parameters;
			Parameters.Atlas = GState.OverlayIcon.Atlas;
			Parameters.AtlasSampler = GState.OverlayIcon.AtlasSampler;
			Parameters.IconStyle =
				CommandList.AllocateDynamicUniformBuffer(
					&Style, sizeof(Style));
			SetShaderParameters(
				CommandList, GState.OverlayIcon.FragmentShader, Parameters);
			CommandList.DrawIndexed(
				Prepared.OverlayIconIndexCount, 0, 0);
		}
	}

	auto Prepare(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FRequest& Request) -> FPrepared
	{
		FPrepared Prepared;
		if (Request.bEditorGrid)
		{
			EditorGridRendering::FEditorGridUniform Uniform;
			if (EditorGridRendering::BuildUniform(View, Uniform)
				&& EnsureEditorGridBase())
			{
				Prepared.EditorGridUniform = Uniform;
				AddPreparedPipeline(Prepared, {
					.Feature = EFeature::EditorGrid,
					.Output = Request.Output,
					.DepthMode = EDepthMode::Visible,
				});
				if (Prepared.Pipelines.empty())
					Prepared.EditorGridUniform.reset();
			}
		}

		if ((Request.bSolidGizmos || Request.bWireGizmos)
			&& EnsureGizmoBase(CommandList))
		{
			Prepared.bSolidGizmos = Request.bSolidGizmos;
			Prepared.bWireGizmos = Request.bWireGizmos;
			for (const EGizmoTopology Topology : {
					EGizmoTopology::Solid, EGizmoTopology::Wire})
			{
				const bool bRequested =
					Topology == EGizmoTopology::Solid
					? Request.bSolidGizmos
					: Request.bWireGizmos;
				if (!bRequested)
					continue;
				for (const EDepthMode DepthMode : {
						EDepthMode::XRay, EDepthMode::Visible})
				{
					AddPreparedPipeline(Prepared, {
						.Feature = EFeature::Gizmo,
						.Output = Request.Output,
						.DepthMode = DepthMode,
						.GizmoTopology = Topology,
					});
				}
			}
		}

		if (Request.bOverlayLines)
		{
			std::vector<FOverlayLineVertex> Vertices;
			std::vector<uint32> Indices;
			Vertices.reserve(View.OverlayLines.size() * 4);
			Indices.reserve(View.OverlayLines.size() * 6);
			BuildOverlayLineGeometry(View, Vertices, Indices);
			if (!Vertices.empty() && !Indices.empty()
				&& EnsureOverlayLineBase())
			{
				Prepared.OverlayLineIndexCount = UploadDynamicGeometry(
					CommandList,
					Vertices,
					Indices,
					GState.OverlayLine,
					"OverlayLineVertexBuffer",
					"OverlayLineIndexBuffer");
				if (Prepared.OverlayLineIndexCount > 0)
				{
					for (const EDepthMode DepthMode : {
							EDepthMode::XRay, EDepthMode::Visible})
					{
						AddPreparedPipeline(Prepared, {
							.Feature = EFeature::OverlayLine,
							.Output = Request.Output,
							.DepthMode = DepthMode,
						});
					}
				}
			}
		}

		if (Request.bOverlayIcons)
		{
			std::vector<FOverlayIconVertex> Vertices;
			std::vector<uint32> Indices;
			Vertices.reserve(View.OverlayIcons.size() * 4);
			Indices.reserve(View.OverlayIcons.size() * 6);
			BuildOverlayIconGeometry(View, Vertices, Indices);
			if (!Vertices.empty() && !Indices.empty()
				&& EnsureOverlayIconBase(CommandList))
			{
				Prepared.OverlayIconIndexCount = UploadDynamicGeometry(
					CommandList,
					Vertices,
					Indices,
					GState.OverlayIcon,
					"OverlayIconVertexBuffer",
					"OverlayIconIndexBuffer");
				if (Prepared.OverlayIconIndexCount > 0)
				{
					for (const EDepthMode DepthMode : {
							EDepthMode::XRay, EDepthMode::Visible})
					{
						AddPreparedPipeline(Prepared, {
							.Feature = EFeature::OverlayIcon,
							.Output = Request.Output,
							.DepthMode = DepthMode,
						});
					}
				}
			}
		}
		return Prepared;
	}

	auto Draw(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FPrepared& Prepared) -> void
	{
		CommandList.SetViewport(
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			0.0f,
			static_cast<float>(View.ViewportX + View.ViewportWidth),
			static_cast<float>(View.ViewportY + View.ViewportHeight),
			1.0f);
		CommandList.SetScissor(
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			static_cast<float>(View.ViewportWidth),
			static_cast<float>(View.ViewportHeight));

		for (const EDrawOperation Operation : GetDrawOrder())
		{
			switch (Operation)
			{
			case EDrawOperation::EditorGrid:
				DrawEditorGrid(CommandList, Prepared);
				break;
			case EDrawOperation::XRayGizmos:
				DrawGizmos(
					CommandList, View, Prepared, EDepthMode::XRay);
				break;
			case EDrawOperation::XRayOverlayLines:
				DrawOverlayLines(
					CommandList, Prepared, EDepthMode::XRay);
				break;
			case EDrawOperation::XRayOverlayIcons:
				DrawOverlayIcons(
					CommandList, Prepared, EDepthMode::XRay);
				break;
			case EDrawOperation::VisibleGizmos:
				DrawGizmos(
					CommandList, View, Prepared, EDepthMode::Visible);
				break;
			case EDrawOperation::VisibleOverlayLines:
				DrawOverlayLines(
					CommandList, Prepared, EDepthMode::Visible);
				break;
			case EDrawOperation::VisibleOverlayIcons:
				DrawOverlayIcons(
					CommandList, Prepared, EDepthMode::Visible);
				break;
			}
		}
	}

	auto ReleaseResources() -> void
	{
		GState.Gizmo.Base.Reset();
		GState.OverlayLine.Base.Reset();
		GState.OverlayIcon.Base.Reset();
		GState.EditorGrid.Base.Reset();
		for (FPipelineEntry& Entry : GState.Pipelines)
			Entry.Slot.Reset();
		GState = {};
	}

	auto InvalidateShaderResources(bool bForceRecompile) -> void
	{
		GState.Generation.Advance(
			ERenderResourceGenerationDependency::Shader);
		GState.ForceRecompileShaderGeneration =
			bForceRecompile
				? std::optional<uint64>(GState.Generation.Shader)
				: std::nullopt;
	}

	auto InvalidateDeviceResources() -> void
	{
		FRenderResourceGeneration Generation = GState.Generation;
		Generation.Advance(ERenderResourceGenerationDependency::Device);
		ReleaseResources();
		GState.Generation = Generation;
	}

	auto RetryFailedResources() -> void
	{
		GState.Generation.Advance(
			ERenderResourceGenerationDependency::Manual);
	}
} // namespace Durin::RendererEditorAssistance
