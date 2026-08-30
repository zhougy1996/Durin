#include "Renderers/EditorAssistance/GizmoRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/RendererResourceCoordinator.h"
#include "CoreGlobals.h"
#include "Math/Operations.h"
#include "Misc/AssertionMacros.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Shader/GlobalShader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	using namespace RendererEditorAssistance;

	namespace
	{
		class FGizmoVertexShader : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FGizmoVertexShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Transform);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_GLOBAL_SHADER(
				FGizmoVertexShader,
				FGlobalShader,
				"/Engine/Gizmo",
				EShaderFrequency::Vertex,
				"VertexMain");
		};

		class FGizmoFragmentShader : public FGlobalShader
		{
		public:
			DURIN_DECLARE_GLOBAL_SHADER(
				FGizmoFragmentShader,
				FGlobalShader,
				"/Engine/Gizmo",
				EShaderFrequency::Fragment,
				"FragmentMain");
		};

		DURIN_IMPLEMENT_GLOBAL_SHADER(FGizmoVertexShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FGizmoFragmentShader);
		const FGlobalShaderSetRegistration GGizmoShaderSet(
			"Renderer", "EditorAssistance.Gizmo",
			EShaderRequestEligibility::EditorOnly,
			{&FGizmoVertexShader::StaticType(),
			 &FGizmoFragmentShader::StaticType()});

		struct FGizmoTransformUniform
		{
			FMatrix4f LocalToClip{1.0f};
			FVector4f Color{1.0f};
		};

		struct FGizmoMeshRange
		{
			uint32 FirstIndex = 0;
			uint32 IndexCount = 0;
			int32 VertexOffset = 0;
		};

		auto BeginMesh(const std::vector<uint32>& Indices) -> FGizmoMeshRange
		{
			return {.FirstIndex = static_cast<uint32>(Indices.size())};
		}

		auto EndMesh(
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
					const float Angle = Math::TwoPi<float>()
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
				const float Angle = Math::TwoPi<float>()
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
				const float Major = Math::TwoPi<float>()
					* static_cast<float>(Segment)
					/ static_cast<float>(Segments);
				for (uint32 Tube = 0; Tube < TubeSegments; ++Tube)
				{
					const float Minor = Math::TwoPi<float>()
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
					const uint32 B =
						Base + NextSegment * TubeSegments + Tube;
					const uint32 C =
						Base + NextSegment * TubeSegments + NextTube;
					const uint32 D = Base + Segment * TubeSegments + NextTube;
					Indices.insert(Indices.end(), {A, B, C, A, C, D});
				}
			}
		}

		auto GetDepthName(EDepthMode DepthMode) -> std::string_view
		{
			return DepthMode == EDepthMode::XRay ? "XRay" : "Visible";
		}

		auto GetTopologyName(EGizmoTopology Topology) -> std::string_view
		{
			return "Solid";
		}

		auto GetOutputName(RenderTargetLayouts::EViewportOutput Output)
			-> std::string_view
		{
			return Output == RenderTargetLayouts::EViewportOutput::Present
				? "Present"
				: "Offscreen";
		}

		auto FindPreparedPipeline(
			const FPrepared& Prepared,
			EDepthMode DepthMode,
			EGizmoTopology Topology) -> FGraphicsPipelineStateRHIRef
		{
			const auto It = std::ranges::find_if(
				Prepared.Pipelines,
				[DepthMode, Topology](const FPreparedPipeline& Pipeline) {
					return Pipeline.Key.Feature == EFeature::Gizmo
						&& Pipeline.Key.DepthMode == DepthMode
						&& Pipeline.Key.GizmoTopology == Topology;
				});
			return It != Prepared.Pipelines.end() ? It->Pipeline : nullptr;
		}
	} // namespace

	struct FGizmoRenderer::FState
	{
		struct FBasePayload
		{
			FGlobalShaderSetRef ShaderSet;
			TShaderMapRef<FGizmoVertexShader> VertexShader;
			TShaderMapRef<FGizmoFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
			FBufferRHIRef VertexBuffer;
			FBufferRHIRef IndexBuffer;
			std::array<FGizmoMeshRange, 5> MeshRanges{};
		};

		struct FPipelinePayload
		{
			FGraphicsPipelineStateRHIRef Pipeline;
			FGlobalShaderSetRef ShaderSet;
		};

		struct FPipelineEntry
		{
			FPipelineKey Key;
			TRenderResourceCreationSlot<FPipelinePayload> Slot{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
		};

		TRenderResourceCreationSlot<FBasePayload> Base{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
		std::vector<FPipelineEntry> Pipelines;
	};

	FGizmoRenderer::FGizmoRenderer(FRendererResourceCoordinator& InCoordinator)
		: Coordinator(InCoordinator)
		, State(std::make_unique<FState>())
	{
	}

	FGizmoRenderer::~FGizmoRenderer() = default;

	auto FGizmoRenderer::Prepare_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FRequest& Request,
		FPrepared& Prepared) -> void
	{
		check(IsInRenderingThread());
		using FBasePayload = FState::FBasePayload;
		using FBaseResult = TRenderResourceCreateResult<FBasePayload>;
		FBasePayload* Base = State->Base.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this, &CommandList]() -> FBaseResult {
				FBasePayload Candidate;
				const std::array<const FGlobalShaderType*, 2> ShaderTypes = {
					&FGizmoVertexShader::StaticType(),
					&FGizmoFragmentShader::StaticType()};
				Candidate.ShaderSet = GetGlobalShaderMap().ResolveShaderSet(
					"EditorAssistance.Gizmo", ShaderTypes, true,
					ReportRendererResourceCreateDiagnostic);
				if (!Candidate.ShaderSet)
				{
					return FBaseResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"Gizmo",
							"base",
							"Global shader set is unavailable.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}
				Candidate.VertexShader = TShaderMapRef<FGizmoVertexShader>(Candidate.ShaderSet);
				Candidate.FragmentShader = TShaderMapRef<FGizmoFragmentShader>(Candidate.ShaderSet);
				FVertexDeclarationElementList Elements;
				Elements[0] = FVertexElement(
					0, 0, EVertexElementType::Float3, 0, sizeof(FVector3f));
				Candidate.VertexDeclaration =
					GDynamicRHI->RHICreateVertexDeclaration(Elements);

				std::vector<FVector3f> Vertices;
				std::vector<uint32> Indices;
				auto& Ranges = Candidate.MeshRanges;
				Ranges[static_cast<size_t>(EViewOverlayShape::Arrow)] =
					BeginMesh(Indices);
				AppendCylinder(Vertices, Indices, 0.0f, 0.76f, 0.032f, 12);
				AppendCone(Vertices, Indices, 0.72f, 1.0f, 0.085f, 12);
				EndMesh(
					Ranges[static_cast<size_t>(EViewOverlayShape::Arrow)],
					Indices);
				Ranges[static_cast<size_t>(EViewOverlayShape::Axis)] =
					BeginMesh(Indices);
				AppendCylinder(Vertices, Indices, 0.0f, 0.94f, 0.032f, 12);
				EndMesh(
					Ranges[static_cast<size_t>(EViewOverlayShape::Axis)],
					Indices);
				Ranges[static_cast<size_t>(EViewOverlayShape::Plane)] =
					BeginMesh(Indices);
				AppendPlane(Vertices, Indices);
				EndMesh(
					Ranges[static_cast<size_t>(EViewOverlayShape::Plane)],
					Indices);
				Ranges[static_cast<size_t>(EViewOverlayShape::Ring)] =
					BeginMesh(Indices);
				AppendRing(Vertices, Indices, 64);
				EndMesh(
					Ranges[static_cast<size_t>(EViewOverlayShape::Ring)],
					Indices);
				Ranges[static_cast<size_t>(EViewOverlayShape::Box)] =
					BeginMesh(Indices);
				AppendBox(Vertices, Indices);
				EndMesh(
					Ranges[static_cast<size_t>(EViewOverlayShape::Box)],
					Indices);
				FRHIBufferCreateDesc VertexDesc =
					FRHIBufferCreateDesc::CreateVertex(
						"GizmoVertexBuffer",
						static_cast<uint32>(
							Vertices.size() * sizeof(FVector3f)));
				VertexDesc.Usage |= EBufferUsageFlags::Static;
				VertexDesc.InitialData = {
					Vertices.data(),
					static_cast<uint32>(Vertices.size() * sizeof(FVector3f))};
				Candidate.VertexBuffer =
					GDynamicRHI->RHICreateBuffer(CommandList, VertexDesc);
				FRHIBufferCreateDesc IndexDesc =
					FRHIBufferCreateDesc::CreateIndex(
						"GizmoIndexBuffer",
						static_cast<uint32>(
							Indices.size() * sizeof(uint32)),
						sizeof(uint32));
				IndexDesc.Usage |= EBufferUsageFlags::Static;
				IndexDesc.InitialData = {
					Indices.data(),
					static_cast<uint32>(Indices.size() * sizeof(uint32))};
				Candidate.IndexBuffer =
					GDynamicRHI->RHICreateBuffer(CommandList, IndexDesc);
				if (Candidate.VertexShader.GetRHIShader(false) == nullptr
					|| Candidate.FragmentShader.GetRHIShader(false) == nullptr
					|| Candidate.VertexDeclaration == nullptr
					|| Candidate.VertexBuffer == nullptr
					|| Candidate.IndexBuffer == nullptr)
				{
					return FBaseResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"Gizmo",
							"base",
							"RHI creation returned null.",
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				return FBaseResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable);
		if (Base == nullptr)
			return;

		Prepared.bSolidGizmos = Request.bSolidGizmos;
		for (const EGizmoTopology Topology : {
				EGizmoTopology::Solid})
		{
			const bool bRequested = Request.bSolidGizmos;
			if (!bRequested)
				continue;
			for (const EDepthMode DepthMode : {
					EDepthMode::XRay,
					EDepthMode::Visible})
			{
				const FPipelineKey Key{
					.Feature = EFeature::Gizmo,
					.Output = Request.Output,
					.DepthMode = DepthMode,
					.DepthConvention = Request.DepthConvention,
					.GizmoTopology = Topology,
				};
				auto EntryIt = std::ranges::find(
					State->Pipelines, Key, &FState::FPipelineEntry::Key);
				if (EntryIt == State->Pipelines.end())
				{
					EntryIt = State->Pipelines.emplace(
						State->Pipelines.end(),
						FState::FPipelineEntry{.Key = Key});
				}
		FRenderResourceGeneration PipelineGeneration = Base->ShaderSet.GetGeneration();
				const std::string PipelineName = std::format(
					"Gizmo{}{}{}Pipeline",
					GetTopologyName(Topology),
					GetDepthName(DepthMode),
					GetOutputName(Request.Output));
				using FPipelineResult = TRenderResourceCreateResult<FState::FPipelinePayload>;
				auto* Pipeline = EntryIt->Slot.Resolve(
					PipelineGeneration,
					[Base, Key, PipelineName]() -> FPipelineResult {
						FGraphicsPipelineStateInitializer Initializer;
						Initializer.RenderTargetLayout =
							RenderTargetLayouts::
								MakeEditorAssistanceOutput(Key.Output);
						Initializer.BoundShaders.VertexShader =
							Base->VertexShader.GetRHIShader();
						Initializer.BoundShaders.FragmentShader =
							Base->FragmentShader.GetRHIShader();
						Initializer.VertexDeclaration =
							Base->VertexDeclaration;
						Initializer.ColorBlendStates[0] =
							FRHIColorBlendState::StraightAlpha();
						Initializer.RasterizerState.CullMode =
							ERHICullMode::None;
						Initializer.DepthStencilState.bEnableTest =
							Key.DepthMode == EDepthMode::Visible;
						Initializer.DepthStencilState.CompareOp =
							GetVisibleDepthCompareOp(Key.DepthConvention);
						Initializer.PipelineLayout =
							Base->ShaderSet.GetPipelineLayout();
						FGraphicsPipelineStateRHIRef Candidate =
							GDynamicRHI->RHICreateGraphicsPipelineState(
								FName(PipelineName), Initializer);
						if (Candidate == nullptr)
						{
							return FPipelineResult::Failure(
								MakeRendererResourceCreateError(
									ERenderResourceCreateErrorCategory::
										GraphicsPipeline,
									"Gizmo",
									PipelineName,
									"RHI graphics pipeline creation returned null.",
									ERenderResourceGenerationDependency::Shader
										| ERenderResourceGenerationDependency::
											Device
										| ERenderResourceGenerationDependency::
											Manual));
						}
						return FPipelineResult::Success({
							.Pipeline = std::move(Candidate),
							.ShaderSet = Base->ShaderSet});
					},
					ReportRendererResourceCreateDiagnostic);
				if (Pipeline != nullptr)
				{
					Prepared.Pipelines.push_back({
						.Key = Key,
						.Pipeline = Pipeline->Pipeline,
						.ShaderSet = Pipeline->ShaderSet,
					});
				}
			}
		}
	}

	auto FGizmoRenderer::Draw_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FPrepared& Prepared,
		EDepthMode DepthMode) -> void
	{
		check(IsInRenderingThread());
		const FState::FBasePayload* Base = State->Base.GetPayload();
		if (Base == nullptr
			|| Base->VertexBuffer == nullptr
			|| Base->IndexBuffer == nullptr)
		{
			return;
		}
		CommandList.BindVertexBuffer(0, Base->VertexBuffer, 0);
		CommandList.BindIndexBuffer(Base->IndexBuffer, 0);
		for (const FViewOverlayPrimitive& Primitive : View.OverlayPrimitives)
		{
			const EGizmoTopology Topology = EGizmoTopology::Solid;
			const FGraphicsPipelineStateRHIRef Pipeline =
				FindPreparedPipeline(Prepared, DepthMode, Topology);
			if (Pipeline == nullptr)
				continue;
			const size_t ShapeIndex = static_cast<size_t>(Primitive.Shape);
			if (ShapeIndex >= Base->MeshRanges.size())
				continue;
			CommandList.SetGraphicsPipelineState(*Pipeline);
			const FGizmoMeshRange& Range = Base->MeshRanges[ShapeIndex];
			FGizmoTransformUniform Uniform;
			Uniform.LocalToClip = Math::TransposeToFloat(
				View.ViewProjectionMatrix * Primitive.LocalToWorld);
			Uniform.Color = Primitive.Color;
			if (DepthMode == EDepthMode::XRay)
				Uniform.Color.a *= 0.32f;
			FGizmoVertexShader::FParameters Parameters;
			Parameters.Transform = CommandList.AllocateDynamicUniformBuffer(
				&Uniform, sizeof(Uniform));
			const auto PreparedIt = std::ranges::find_if(
				Prepared.Pipelines, [DepthMode, Topology](const FPreparedPipeline& Item) {
					return Item.Key.Feature == EFeature::Gizmo
						&& Item.Key.DepthMode == DepthMode
						&& Item.Key.GizmoTopology == Topology;
				});
			if (PreparedIt == Prepared.Pipelines.end())
				continue;
			SetShaderParameters(CommandList,
				TShaderMapRef<FGizmoVertexShader>(PreparedIt->ShaderSet).GetShaderRef(),
				Parameters);
			CommandList.DrawIndexed(
				Range.IndexCount, Range.FirstIndex, Range.VertexOffset);
		}
	}

	auto FGizmoRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->Base.Reset();
		for (FState::FPipelineEntry& Entry : State->Pipelines)
			Entry.Slot.Reset();
		State->Pipelines.clear();
	}
} // namespace Durin
