#include "Renderers/EditorAssistance/OverlayLineRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/RendererResourceCoordinator.h"
#include "CoreGlobals.h"
#include "Math/Operations.h"
#include "Misc/AssertionMacros.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	using namespace RendererEditorAssistance;

	namespace
	{
		class FOverlayLineVertexShader : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(
				FOverlayLineVertexShader,
				FShader,
				"/Engine/Gizmo",
				EShaderFrequency::Vertex,
				"LineVertexMain");
		};

		class FOverlayLineFragmentShader : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FOverlayLineFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Style);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(
				FOverlayLineFragmentShader,
				FShader,
				"/Engine/Gizmo",
				EShaderFrequency::Fragment,
				"LineFragmentMain");
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

		auto BuildGeometry(
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
						const FVector4 Intersection = Math::Lerp(Start, End, T);
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
				{
					continue;
				}
				const FVector2 NdcStart = FVector2(ClipStart) / ClipStart.w;
				const FVector2 NdcEnd = FVector2(ClipEnd) / ClipEnd.w;
				const FVector2f PixelDelta{
					static_cast<float>(
						(NdcEnd.x - NdcStart.x) * 0.5 * View.ViewportWidth),
					static_cast<float>(
						(NdcEnd.y - NdcStart.y) * 0.5 * View.ViewportHeight),
				};
				const float PixelLength = Math::Length(PixelDelta);
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
				const uint32 Base = static_cast<uint32>(OutVertices.size());
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
					Base,
					Base + 1,
					Base + 2,
					Base + 2,
					Base + 1,
					Base + 3,
				});
			}
		}

		auto GetDepthName(EDepthMode DepthMode) -> std::string_view
		{
			return DepthMode == EDepthMode::XRay ? "XRay" : "Visible";
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
			EDepthMode DepthMode) -> FGraphicsPipelineStateRHIRef
		{
			const auto It = std::ranges::find_if(
				Prepared.Pipelines,
				[DepthMode](const FPreparedPipeline& Pipeline) {
					return Pipeline.Key.Feature == EFeature::OverlayLine
						&& Pipeline.Key.DepthMode == DepthMode;
				});
			return It != Prepared.Pipelines.end() ? It->Pipeline : nullptr;
		}
	} // namespace

	struct FOverlayLineRenderer::FState
	{
		struct FBasePayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FOverlayLineVertexShader> VertexShader;
			TShaderRef<FOverlayLineFragmentShader> FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration;
		};

		struct FPipelineEntry
		{
			FPipelineKey Key;
			TRenderResourceCreationSlot<FGraphicsPipelineStateRHIRef> Slot{
				ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device};
		};

		TRenderResourceCreationSlot<FBasePayload> Base{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
		std::vector<FPipelineEntry> Pipelines;
		FBufferRHIRef VertexBuffer;
		FBufferRHIRef IndexBuffer;
		uint32 VertexCapacity = 0;
		uint32 IndexCapacity = 0;
	};

	FOverlayLineRenderer::FOverlayLineRenderer(
		FRendererResourceCoordinator& InCoordinator)
		: Coordinator(InCoordinator)
		, State(std::make_unique<FState>())
	{
	}

	FOverlayLineRenderer::~FOverlayLineRenderer() = default;

	auto FOverlayLineRenderer::Prepare_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		RenderTargetLayouts::EViewportOutput Output,
		FPrepared& Prepared) -> void
	{
		check(IsInRenderingThread());
		std::vector<FOverlayLineVertex> Vertices;
		std::vector<uint32> Indices;
		Vertices.reserve(View.OverlayLines.size() * 4);
		Indices.reserve(View.OverlayLines.size() * 6);
		BuildGeometry(View, Vertices, Indices);
		if (Vertices.empty() || Indices.empty())
			return;

		using FBasePayload = FState::FBasePayload;
		using FBaseResult = TRenderResourceCreateResult<FBasePayload>;
		FBasePayload* Base = State->Base.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this]() -> FBaseResult {
				FShaderCompileOptions CompileOptions;
				CompileOptions.bForceRecompile =
					Coordinator.ShouldForceShaderRecompile_RenderThread();
				FShaderType& VertexShaderType =
					FOverlayLineVertexShader::StaticType();
				FShaderType& FragmentShaderType =
					FOverlayLineFragmentShader::StaticType();
				const std::array<const FShaderType*, 2> ShaderTypes = {
					&VertexShaderType,
					&FragmentShaderType};

				FBasePayload Candidate;
				Candidate.ShaderMap = std::make_shared<FShaderMapBase>();
				std::string ErrorMessage;
				if (!Candidate.ShaderMap->InitializeFromShaderTypes(
						ShaderTypes, CompileOptions, ErrorMessage))
				{
					return FBaseResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"OverlayLine",
							"base",
							std::move(ErrorMessage),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}
				auto* VertexShader =
					static_cast<FOverlayLineVertexShader*>(
						Candidate.ShaderMap->GetShader(&VertexShaderType));
				auto* FragmentShader =
					static_cast<FOverlayLineFragmentShader*>(
						Candidate.ShaderMap->GetShader(&FragmentShaderType));
				if (VertexShader == nullptr || FragmentShader == nullptr)
				{
					return FBaseResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"OverlayLine",
							"base",
							"Compiled shader map is missing a typed shader.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
				}
				Candidate.VertexShader =
					TShaderRef<FOverlayLineVertexShader>(
						VertexShader, Candidate.ShaderMap.get());
				Candidate.FragmentShader =
					TShaderRef<FOverlayLineFragmentShader>(
						FragmentShader, Candidate.ShaderMap.get());
				FVertexDeclarationElementList Elements;
				Elements[0] = FVertexElement(
					0,
					static_cast<uint8>(
						offsetof(FOverlayLineVertex, Position)),
					EVertexElementType::Float4,
					0,
					sizeof(FOverlayLineVertex));
				Elements[1] = FVertexElement(
					0,
					static_cast<uint8>(offsetof(FOverlayLineVertex, Color)),
					EVertexElementType::Float4,
					1,
					sizeof(FOverlayLineVertex));
				Elements[2] = FVertexElement(
					0,
					static_cast<uint8>(
						offsetof(FOverlayLineVertex, Pattern)),
					EVertexElementType::Float2,
					2,
					sizeof(FOverlayLineVertex));
				Candidate.VertexDeclaration =
					GDynamicRHI->RHICreateVertexDeclaration(Elements);
				if (Candidate.VertexDeclaration == nullptr)
				{
					return FBaseResult::Failure(
						MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"OverlayLine",
							"base",
							"RHI vertex declaration creation returned null.",
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}
				return FBaseResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic);
		if (Base == nullptr)
			return;

		const uint32 VertexBytes =
			static_cast<uint32>(Vertices.size() * sizeof(FOverlayLineVertex));
		const uint32 IndexBytes =
			static_cast<uint32>(Indices.size() * sizeof(uint32));
		if (VertexBytes > State->VertexCapacity)
		{
			const uint32 Capacity = std::bit_ceil(VertexBytes);
			FRHIBufferCreateDesc Desc =
				FRHIBufferCreateDesc::CreateVertex(
					"OverlayLineVertexBuffer", Capacity);
			Desc.Usage |= EBufferUsageFlags::Dynamic;
			FBufferRHIRef Buffer =
				GDynamicRHI->RHICreateBuffer(CommandList, Desc);
			if (Buffer == nullptr)
				return;
			State->VertexBuffer = std::move(Buffer);
			State->VertexCapacity = Capacity;
		}
		if (IndexBytes > State->IndexCapacity)
		{
			const uint32 Capacity = std::bit_ceil(IndexBytes);
			FRHIBufferCreateDesc Desc =
				FRHIBufferCreateDesc::CreateIndex(
					"OverlayLineIndexBuffer", Capacity, sizeof(uint32));
			Desc.Usage |= EBufferUsageFlags::Dynamic;
			FBufferRHIRef Buffer =
				GDynamicRHI->RHICreateBuffer(CommandList, Desc);
			if (Buffer == nullptr)
				return;
			State->IndexBuffer = std::move(Buffer);
			State->IndexCapacity = Capacity;
		}
		if (State->VertexBuffer == nullptr || State->IndexBuffer == nullptr)
			return;
		CommandList.WriteBuffer(
			State->VertexBuffer, Vertices.data(), VertexBytes, 0);
		CommandList.WriteBuffer(
			State->IndexBuffer, Indices.data(), IndexBytes, 0);
		Prepared.OverlayLineIndexCount =
			static_cast<uint32>(Indices.size());

		for (const EDepthMode DepthMode : {
				EDepthMode::XRay,
				EDepthMode::Visible})
		{
			const FPipelineKey Key{
				.Feature = EFeature::OverlayLine,
				.Output = Output,
				.DepthMode = DepthMode,
			};
			auto EntryIt = std::ranges::find(
				State->Pipelines, Key, &FState::FPipelineEntry::Key);
			if (EntryIt == State->Pipelines.end())
			{
				EntryIt = State->Pipelines.emplace(
					State->Pipelines.end(),
					FState::FPipelineEntry{.Key = Key});
			}
			FRenderResourceGeneration PipelineGeneration =
				Coordinator.GetGeneration_RenderThread();
			PipelineGeneration.Shader =
				State->Base.GetPayloadGeneration().Shader;
			const std::string PipelineName = std::format(
				"OverlayLine{}{}Pipeline",
				GetDepthName(DepthMode),
				GetOutputName(Output));
			using FPipelineResult =
				TRenderResourceCreateResult<FGraphicsPipelineStateRHIRef>;
			auto* Pipeline = EntryIt->Slot.Resolve(
				PipelineGeneration,
				[Base, Key, PipelineName]() -> FPipelineResult {
					FGraphicsPipelineStateInitializer Initializer;
					Initializer.RenderTargetLayout =
						RenderTargetLayouts::MakeEditorAssistanceOutput(
							Key.Output);
					Initializer.BoundShaders.VertexShader =
						Base->VertexShader.GetRHIShader();
					Initializer.BoundShaders.FragmentShader =
						Base->FragmentShader.GetRHIShader();
					Initializer.VertexDeclaration =
						Base->VertexDeclaration;
					Initializer.bEnableAlphaBlend = true;
					Initializer.bEnableBackFaceCulling = false;
					Initializer.bEnableDepthTest =
						Key.DepthMode == EDepthMode::Visible;
					Initializer.bEnableDepthWrite = false;
					Initializer.PipelineLayout =
						Base->ShaderMap->GetMergedPipelineLayout();
					FGraphicsPipelineStateRHIRef Candidate =
						GDynamicRHI->RHICreateGraphicsPipelineState(
							FName(PipelineName), Initializer);
					if (Candidate == nullptr)
					{
						return FPipelineResult::Failure(
							MakeRendererResourceCreateError(
								ERenderResourceCreateErrorCategory::
									GraphicsPipeline,
								"OverlayLine",
								PipelineName,
								"RHI graphics pipeline creation returned null.",
								ERenderResourceGenerationDependency::Shader
									| ERenderResourceGenerationDependency::
										Device
									| ERenderResourceGenerationDependency::
										Manual));
					}
					return FPipelineResult::Success(std::move(Candidate));
				},
				ReportRendererResourceCreateDiagnostic);
			if (Pipeline != nullptr)
			{
				Prepared.Pipelines.push_back({
					.Key = Key,
					.Pipeline = *Pipeline,
				});
			}
		}
	}

	auto FOverlayLineRenderer::Draw_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FPrepared& Prepared,
		EDepthMode DepthMode) -> void
	{
		check(IsInRenderingThread());
		const FState::FBasePayload* Base = State->Base.GetPayload();
		const FGraphicsPipelineStateRHIRef Pipeline =
			FindPreparedPipeline(Prepared, DepthMode);
		if (Prepared.OverlayLineIndexCount == 0
			|| Base == nullptr
			|| Pipeline == nullptr
			|| State->VertexBuffer == nullptr
			|| State->IndexBuffer == nullptr)
		{
			return;
		}
		CommandList.SetGraphicsPipelineState(*Pipeline);
		CommandList.BindVertexBuffer(0, State->VertexBuffer, 0);
		CommandList.BindIndexBuffer(State->IndexBuffer, 0);
		const FOverlayLineStyleUniform Style{
			DepthMode == EDepthMode::XRay ? 0.32f : 1.0f};
		FOverlayLineFragmentShader::FParameters Parameters;
		Parameters.Style = CommandList.AllocateDynamicUniformBuffer(
			&Style, sizeof(Style));
		SetShaderParameters(
			CommandList, Base->FragmentShader, Parameters);
		CommandList.DrawIndexed(Prepared.OverlayLineIndexCount, 0, 0);
	}

	auto FOverlayLineRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->Base.Reset();
		for (FState::FPipelineEntry& Entry : State->Pipelines)
			Entry.Slot.Reset();
		State->Pipelines.clear();
		State->VertexBuffer = nullptr;
		State->IndexBuffer = nullptr;
		State->VertexCapacity = 0;
		State->IndexCapacity = 0;
	}
} // namespace Durin
