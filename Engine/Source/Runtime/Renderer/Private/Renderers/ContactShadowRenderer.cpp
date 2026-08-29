#include "Renderers/ContactShadowRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Renderers/RendererTransientTargetPool.h"
#include "Math/Operations.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "Shader/GlobalShader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	namespace
	{
		std::atomic<FContactShadowVisibilityRenderer::FTimingQuerySink>
			GContactVisibilityTimingQuerySink = nullptr;
		class FContactVisibilityVertexShader final : public FGlobalShader
		{
		public:
			DURIN_DECLARE_GLOBAL_SHADER(FContactVisibilityVertexShader, FGlobalShader,
				"/Engine/ContactShadow", EShaderFrequency::Vertex, "VertexMain");
		};

		class FContactVisibilityFragmentShader final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FContactVisibilityFragmentShader)
				DURIN_SHADER_PARAMETER_GRAPH_TEXTURE(GBufferMaterial);
				DURIN_SHADER_PARAMETER_GRAPH_TEXTURE(GBufferNormals);
				DURIN_SHADER_PARAMETER_GRAPH_TEXTURE(GBufferSurface);
				DURIN_SHADER_PARAMETER_GRAPH_TEXTURE(GBufferEmissive);
				DURIN_SHADER_PARAMETER_GRAPH_TEXTURE(SceneDepth);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Params);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_GLOBAL_SHADER(FContactVisibilityFragmentShader, FGlobalShader,
				"/Engine/ContactShadow", EShaderFrequency::Fragment,
				"ContactVisibilityFragmentMain");
		};

		class FContactVisibilityComputeShader final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FContactVisibilityComputeShader)
				DURIN_SHADER_PARAMETER_GRAPH_TEXTURE(GBufferMaterial);
				DURIN_SHADER_PARAMETER_GRAPH_TEXTURE(GBufferNormals);
				DURIN_SHADER_PARAMETER_GRAPH_TEXTURE(GBufferSurface);
				DURIN_SHADER_PARAMETER_GRAPH_TEXTURE(GBufferEmissive);
				DURIN_SHADER_PARAMETER_GRAPH_TEXTURE(SceneDepth);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER(Params);
				DURIN_SHADER_PARAMETER_GRAPH_STORAGE_IMAGE(ContactVisibilityOutput);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_GLOBAL_SHADER(FContactVisibilityComputeShader, FGlobalShader,
				"/Engine/ContactShadow", EShaderFrequency::Compute,
				"ContactVisibilityComputeMain");
		};
		DURIN_IMPLEMENT_GLOBAL_SHADER(FContactVisibilityVertexShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FContactVisibilityFragmentShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FContactVisibilityComputeShader);

		struct alignas(16) FContactVisibilityUniform
		{
			float InverseViewProjection[16]{};
			float ViewProjection[16]{};
			float ToLightMaxDistance[4]{0.0f, 0.0f, 0.0f, 0.20f};
			float SurfaceStepsMinBiasReversedZ[4]{1.5f, 16.0f, 0.0005f, 0.0f};
			float Viewport[4]{1.0f, 1.0f, 0.0f, 0.0f};
			float Trace[4]{48.0f, 0.0f, 0.0f, 0.0f};
		};
		static_assert(sizeof(FContactVisibilityUniform) == 192);
	} // namespace

	struct FContactShadowVisibilityRenderer::FState
	{
		struct FFragmentPayload
		{
			FGlobalShaderSetRef ShaderSet;
			TShaderMapRef<FContactVisibilityVertexShader> VertexShader;
			TShaderMapRef<FContactVisibilityFragmentShader> FragmentShader;
			FGraphicsPipelineStateRHIRef PipelineState;
		};
		struct FComputePayload
		{
			FGlobalShaderSetRef ShaderSet;
			TShaderMapRef<FContactVisibilityComputeShader> ComputeShader;
			FComputePipelineStateRHIRef PipelineState;
		};
		TRenderResourceCreationSlot<FFragmentPayload> FragmentResources{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
		TRenderResourceCreationSlot<FComputePayload> ComputeResources{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
	};

	FContactShadowVisibilityRenderer::FContactShadowVisibilityRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry,
		FRendererTransientTargetPool& InTransientTargets)
		: Coordinator(InCoordinator), FullscreenGeometry(InFullscreenGeometry),
		  TransientTargets(InTransientTargets),
		  State(std::make_unique<FState>()) {}

	FContactShadowVisibilityRenderer::~FContactShadowVisibilityRenderer() = default;

	auto FContactShadowVisibilityRenderer::SetTimingQuerySink(FTimingQuerySink Sink)
		-> void
	{
		GContactVisibilityTimingQuerySink.store(Sink, std::memory_order_release);
	}

	auto FContactShadowVisibilityRenderer::SelectRoute(const FRouteInputs& Inputs)
		-> FRouteDecision
	{
		if (!Inputs.bRequested)
			return {ERoute::FactorOne, ERouteReason::DisabledOrUnneeded};
		if (!Inputs.bInputsValid)
			return {ERoute::FactorOne, ERouteReason::InvalidInputs};
		if (Inputs.Width == 0 || Inputs.Height == 0)
			return {ERoute::FactorOne, ERouteReason::InvalidExtent};
		const uint32 GroupCountX = CalculateGroupCount(Inputs.Width);
		const uint32 GroupCountY = CalculateGroupCount(Inputs.Height);
		const bool bComputeExtentSupported = Inputs.MaxGroupCountX != 0
			&& Inputs.MaxGroupCountY != 0 && GroupCountX <= Inputs.MaxGroupCountX
			&& GroupCountY <= Inputs.MaxGroupCountY;
		if (Inputs.bComputePayloadReady && Inputs.bComputeTargetReady
			&& bComputeExtentSupported)
			return {ERoute::Compute, ERouteReason::Compute};
		ERouteReason FragmentReason = ERouteReason::ComputePayloadUnavailable;
		if (Inputs.bComputePayloadReady && !Inputs.bComputeTargetReady)
			FragmentReason = ERouteReason::ComputeTargetUnavailable;
		else if (Inputs.bComputePayloadReady && Inputs.bComputeTargetReady
			&& !bComputeExtentSupported)
			FragmentReason = ERouteReason::ComputeExtentUnsupported;
		if (Inputs.bFragmentReady)
			return {ERoute::Fragment, FragmentReason};
		return {ERoute::FactorOne, ERouteReason::FragmentUnavailable};
	}

	auto FContactShadowVisibilityRenderer::EnsureTargets_RenderThread(
		uint32 Width, uint32 Height) -> std::optional<FTargets>
	{
		check(IsInRenderingThread());
		const std::array Descriptions{FRHITextureCreateDesc::Create2D(
			"DirectionalContactVisibility", Width, Height, EPixelFormat::R8_UNORM)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy)
			.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f))};
		auto Lease = TransientTargets.AcquireBundle_RenderThread(
			ERendererTransientTargetGroup::ContactFragment, Descriptions,
			MaximumRetainedBytesPerRoute);
		if (!Lease) return std::nullopt;
		return FTargets{.Visibility = Lease->Textures[0]};
	}
	auto FContactShadowVisibilityRenderer::EnsureComputeTargets_RenderThread(
		uint32 Width, uint32 Height) -> std::optional<FComputeTargets>
	{
		check(IsInRenderingThread());
		if (GDynamicRHI == nullptr) return std::nullopt;
		const std::array Descriptions{FRHITextureCreateDesc::Create2D(
			"DirectionalContactVisibilityCompute", Width, Height,
			EPixelFormat::R8_UNORM)
			.SetFlags(ETextureCreateFlags::Storage
				| ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::SourceCopy)};
		if (!GDynamicRHI->RHIIsTextureSupported(Descriptions[0]))
			return std::nullopt;
		auto Lease = TransientTargets.AcquireBundle_RenderThread(
			ERendererTransientTargetGroup::ContactCompute, Descriptions,
			MaximumRetainedBytesPerRoute);
		if (!Lease) return std::nullopt;
		FComputeTargets Targets{.Visibility = Lease->Textures[0]};
		Targets.SampledView =
			GDynamicRHI->RHIGetOrCreateTextureView(Targets.Visibility,
				MakeDefaultTextureViewDesc(*Targets.Visibility,
					ERHITextureViewUsage::Sampled));
		Targets.StorageView =
			GDynamicRHI->RHIGetOrCreateTextureView(Targets.Visibility,
				MakeDefaultTextureViewDesc(*Targets.Visibility,
					ERHITextureViewUsage::Storage));
		return Targets.SampledView && Targets.StorageView
			? std::optional<FComputeTargets>{std::move(Targets)} : std::nullopt;
	}
	auto FContactShadowVisibilityRenderer::Render_RenderThread(
		FRHICommandListImmediate& CommandList, bool bRequested,
		const FTargets* FragmentTargets,
		const FComputeTargets* ComputeTargets,
		FRHITexture* Material, FRHITexture* Normals, FRHITexture* Surface,
		FRHITexture* Emissive, FRHITexture* SceneDepth, const FSceneView& View,
		const FVector3& LightDirection, uint32 Width, uint32 Height,
		const FRenderPolicy& Policy) -> FRenderResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const double LightLengthSquared = Math::Dot(LightDirection, LightDirection);
		const bool bViewFits = View.ViewportX <= Width && View.ViewportY <= Height
			&& View.ViewportWidth <= Width - View.ViewportX
			&& View.ViewportHeight <= Height - View.ViewportY;
		const bool bPhysicalInputsValid = Material != nullptr && Normals != nullptr
			&& Surface != nullptr && Emissive != nullptr && SceneDepth != nullptr
			&& View.ViewportWidth != 0 && View.ViewportHeight != 0 && bViewFits
			&& std::isfinite(LightLengthSquared) && LightLengthSquared > 1.0e-8;
		const bool bInputsValid = Policy.bPreparationOnly
			? Policy.bInputsExpected && View.ViewportWidth != 0
				&& View.ViewportHeight != 0 && bViewFits
				&& std::isfinite(LightLengthSquared) && LightLengthSquared > 1.0e-8
			: bPhysicalInputsValid;

		using FComputePayload = FState::FComputePayload;
		using FComputeResult = TRenderResourceCreateResult<FComputePayload>;
		FComputePayload* ComputePayload = nullptr;
		if (bRequested && bInputsValid && Width != 0 && Height != 0)
		{
			ComputePayload = State->ComputeResources.Resolve(
				Coordinator.GetGeneration_RenderThread(), [this]() -> FComputeResult {
					const std::array<const FGlobalShaderType*, 1> Types{
						&FContactVisibilityComputeShader::StaticType()};
					FComputePayload Candidate;
					Candidate.ShaderSet = GetGlobalShaderMap().ResolveShaderSet(
						"ContactVisibility.Compute", Types, true,
						ReportRendererResourceCreateDiagnostic);
					if (!Candidate.ShaderSet)
						return FComputeResult::Failure(MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"ContactVisibilityCompute", "shader", "Global shader set is unavailable.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
					Candidate.ComputeShader = TShaderMapRef<FContactVisibilityComputeShader>(Candidate.ShaderSet);
					FRHIShader* ComputeRHI = Candidate.ComputeShader.GetRHIShader(false);
					if (ComputeRHI == nullptr)
						return FComputeResult::Failure(MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"ContactVisibilityCompute", "pipeline",
							"Compute shader RHI creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
					FComputePipelineStateInitializer Initializer;
					Initializer.ComputeShader = ComputeRHI;
					Initializer.PipelineLayout =
						Candidate.ShaderSet.GetPipelineLayout();
					Candidate.PipelineState = GDynamicRHI->RHICreateComputePipelineState(
						"ContactVisibilityComputePipeline", Initializer);
					if (Candidate.PipelineState == nullptr)
						return FComputeResult::Failure(MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::GraphicsPipeline,
							"ContactVisibilityCompute", "pipeline",
							"Compute pipeline creation returned null.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
					return FComputeResult::Success(std::move(Candidate));
				}, ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable);
		}

		const FRHICapabilities* Capabilities =
			GDynamicRHI != nullptr ? GDynamicRHI->RHIGetCapabilities() : nullptr;
		FRouteInputs RouteInputs{
			.bRequested = bRequested,
			.bInputsValid = bInputsValid,
			.bComputePayloadReady = ComputePayload != nullptr,
			.bComputeTargetReady = Policy.bPreparationOnly
				? Policy.bComputeTargetExpected
				: ComputeTargets != nullptr && ComputeTargets->Visibility != nullptr,
			.bFragmentReady = false,
			.Width = Width,
			.Height = Height,
			.MaxGroupCountX = Capabilities != nullptr
				? Capabilities->MaxComputeWorkGroupCount[0] : 0,
			.MaxGroupCountY = Capabilities != nullptr
				? Capabilities->MaxComputeWorkGroupCount[1] : 0};
		FRouteDecision Decision = SelectRoute(RouteInputs);

		using FFragmentPayload = FState::FFragmentPayload;
		using FFragmentResult = TRenderResourceCreateResult<FFragmentPayload>;
		FFragmentPayload* FragmentPayload = nullptr;
		if (Decision.Route != ERoute::Compute && bRequested && bInputsValid
			&& Width != 0 && Height != 0
			&& (Policy.bPreparationOnly ? Policy.bFragmentTargetExpected
				: FragmentTargets != nullptr
					&& FragmentTargets->Visibility != nullptr))
		{
			FragmentPayload = State->FragmentResources.Resolve(
			Coordinator.GetGeneration_RenderThread(), [this, &CommandList]() -> FFragmentResult {
				const std::array<const FGlobalShaderType*, 2> Types{
					&FContactVisibilityVertexShader::StaticType(),
					&FContactVisibilityFragmentShader::StaticType()};
				FFragmentPayload Candidate;
				Candidate.ShaderSet = GetGlobalShaderMap().ResolveShaderSet(
					"ContactVisibility.Fragment", Types, true,
					ReportRendererResourceCreateDiagnostic);
				if (!Candidate.ShaderSet)
					return FFragmentResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderCompile,
						"ContactVisibility", "shader", "Global shader set is unavailable.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				Candidate.VertexShader = TShaderMapRef<FContactVisibilityVertexShader>(Candidate.ShaderSet);
				Candidate.FragmentShader = TShaderMapRef<FContactVisibilityFragmentShader>(Candidate.ShaderSet);
				if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList))
					return FFragmentResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"ContactVisibility", "fullscreen-geometry",
						"Shared fullscreen geometry is unavailable.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				FRHIShader* VertexRHI = Candidate.VertexShader.GetRHIShader(false);
				FRHIShader* FragmentRHI = Candidate.FragmentShader.GetRHIShader(false);
				if (VertexRHI == nullptr || FragmentRHI == nullptr)
					return FFragmentResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"ContactVisibility", "pipeline",
						"RHI shader creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout = RenderTargetLayouts::MakeContactVisibilityOutput();
				Initializer.BoundShaders.VertexShader = VertexRHI;
				Initializer.BoundShaders.FragmentShader = FragmentRHI;
				Initializer.VertexDeclaration =
					FullscreenGeometry.GetVertexDeclaration_RenderThread();
				Initializer.RasterizerState.CullMode = ERHICullMode::None;
				Initializer.PipelineLayout = Candidate.ShaderSet.GetPipelineLayout();
				Candidate.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState(
					"ContactVisibilityPipeline", Initializer);
				if (Candidate.PipelineState == nullptr)
					return FFragmentResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::GraphicsPipeline,
						"ContactVisibility", "pipeline",
						"Graphics pipeline creation returned null.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				return FFragmentResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable);
		}
		RouteInputs.bFragmentReady = FragmentPayload != nullptr
			&& FullscreenGeometry.GetVertexBuffer_RenderThread() != nullptr
			&& FullscreenGeometry.GetIndexBuffer_RenderThread() != nullptr;
		Decision = SelectRoute(RouteInputs);
		if (Policy.bPreparationOnly)
			return {.Route = Decision.Route, .Reason = Decision.Reason};
		if (Decision.Route == ERoute::FactorOne)
			return {.Route = Decision.Route, .Reason = Decision.Reason};

		FMatrix InverseViewProjection;
		if (!Math::TryInverse(View.ViewProjectionMatrix, InverseViewProjection, 1.0e-8))
			return {.Route = ERoute::FactorOne, .Reason = ERouteReason::InvalidInputs};
		FContactVisibilityUniform Uniform;
		for (uint32 Row = 0; Row < 4; ++Row)
			for (uint32 Col = 0; Col < 4; ++Col)
			{
				Uniform.InverseViewProjection[Row * 4 + Col] =
					static_cast<float>(InverseViewProjection[Col][Row]);
				Uniform.ViewProjection[Row * 4 + Col] =
					static_cast<float>(View.ViewProjectionMatrix[Col][Row]);
			}
		const FVector3 ToLight = -Math::Normalize(LightDirection);
		Uniform.ToLightMaxDistance[0] = static_cast<float>(ToLight.x);
		Uniform.ToLightMaxDistance[1] = static_cast<float>(ToLight.y);
		Uniform.ToLightMaxDistance[2] = static_cast<float>(ToLight.z);
		Uniform.SurfaceStepsMinBiasReversedZ[3] =
			View.DepthConvention == ESceneDepthConvention::ReversedZ ? 1.0f : 0.0f;
		Uniform.Viewport[0] = 1.0f / static_cast<float>(View.ViewportWidth);
		Uniform.Viewport[1] = 1.0f / static_cast<float>(View.ViewportHeight);
		Uniform.Viewport[2] = static_cast<float>(View.ViewportX);
		Uniform.Viewport[3] = static_cast<float>(View.ViewportY);

		const FRHIUniformBufferRange UniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
		if (UniformBuffer.Buffer == nullptr || UniformBuffer.Size != sizeof(Uniform))
			return {.Route = ERoute::FactorOne, .Reason = ERouteReason::InvalidInputs};
		const FTimingQuerySink TimingSink =
			GContactVisibilityTimingQuerySink.load(std::memory_order_acquire);

		if (Decision.Route == ERoute::Compute)
		{
			const std::array InputTransitions{
				FRHITextureTransition::Whole(Material, ERHIAccess::GraphicsShaderRead,
					ERHIAccess::ComputeShaderRead),
				FRHITextureTransition::Whole(Normals, ERHIAccess::GraphicsShaderRead,
					ERHIAccess::ComputeShaderRead),
				FRHITextureTransition::Whole(Surface, ERHIAccess::GraphicsShaderRead,
					ERHIAccess::ComputeShaderRead),
				FRHITextureTransition::Whole(Emissive, ERHIAccess::GraphicsShaderRead,
					ERHIAccess::ComputeShaderRead),
				FRHITextureTransition::Whole(SceneDepth, ERHIAccess::GraphicsShaderRead,
					ERHIAccess::ComputeShaderRead),
				FRHITextureTransition::Whole(ComputeTargets->Visibility,
					ERHIAccess::Discard, ERHIAccess::ComputeShaderReadWrite)};
			if (!Policy.bGraphManagedTextureAccess)
				CommandList.TransitionTextures(InputTransitions);
			const std::array UniformTransition{FRHIBufferTransition{
				UniformBuffer.Buffer, UniformBuffer.Offset, UniformBuffer.Size,
				ERHIAccess::Discard, ERHIAccess::ComputeUniformRead}};
			CommandList.TransitionBuffers(UniformTransition);
			FGPUTimingQueryRHIRef TimingQuery;
			if (TimingSink != nullptr && GDynamicRHI != nullptr)
			{
				TimingQuery = GDynamicRHI->RHICreateGPUTimingQuery();
				if (TimingQuery) CommandList.BeginGPUTimingQuery(TimingQuery);
			}
			CommandList.SwitchPipeline(ERHIPipeline::Compute);
			CommandList.SetComputePipelineState(*ComputePayload->PipelineState);
			FContactVisibilityComputeShader::FParameters Parameters;
			Parameters.Params = UniformBuffer;
			if (Policy.GraphShaderParameters)
				SetShaderParameters(CommandList, ComputePayload->ComputeShader,
					*Policy.GraphShaderParameters, Parameters);
			else
			{
				Parameters.GBufferMaterial = Material;
				Parameters.GBufferNormals = Normals;
				Parameters.GBufferSurface = Surface;
				Parameters.GBufferEmissive = Emissive;
				Parameters.SceneDepth = SceneDepth;
				Parameters.ContactVisibilityOutput = ComputeTargets->Visibility;
				SetShaderParameters(CommandList, ComputePayload->ComputeShader,
					Parameters);
			}
			CommandList.Dispatch(CalculateGroupCount(Width),
				CalculateGroupCount(Height), 1);
			CommandList.SwitchPipeline(ERHIPipeline::Graphics);
			if (TimingQuery)
			{
				CommandList.EndGPUTimingQuery(TimingQuery);
				TimingSink(TimingQuery, Decision.Route);
			}
			const std::array OutputTransitions{
				FRHITextureTransition::Whole(Material, ERHIAccess::ComputeShaderRead,
					ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Normals, ERHIAccess::ComputeShaderRead,
					ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Surface, ERHIAccess::ComputeShaderRead,
					ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Emissive, ERHIAccess::ComputeShaderRead,
					ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(SceneDepth, ERHIAccess::ComputeShaderRead,
					ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(ComputeTargets->Visibility,
					ERHIAccess::ComputeShaderReadWrite,
					ERHIAccess::GraphicsShaderRead)};
			if (!Policy.bGraphManagedTextureAccess)
				CommandList.TransitionTextures(OutputTransitions);
			const std::array FinalUniformTransition{FRHIBufferTransition{
				UniformBuffer.Buffer, UniformBuffer.Offset, UniformBuffer.Size,
				ERHIAccess::ComputeUniformRead, ERHIAccess::GraphicsUniformRead}};
			CommandList.TransitionBuffers(FinalUniformTransition);
			return {.Visibility = ComputeTargets->Visibility,
				.Route = Decision.Route, .Reason = Decision.Reason};
		}

		FRHIBuffer* VertexBuffer = FullscreenGeometry.GetVertexBuffer_RenderThread();
		FRHIBuffer* IndexBuffer = FullscreenGeometry.GetIndexBuffer_RenderThread();
		FGPUTimingQueryRHIRef TimingQuery;
		if (TimingSink != nullptr && GDynamicRHI != nullptr)
		{
			TimingQuery = GDynamicRHI->RHICreateGPUTimingQuery();
			if (TimingQuery) CommandList.BeginGPUTimingQuery(TimingQuery);
		}
		const std::array VisibilityTransition{FRHITextureTransition::Whole(
			FragmentTargets->Visibility, ERHIAccess::Discard,
			ERHIAccess::ColorAttachmentReadWrite)};
		if (!Policy.bGraphManagedTextureAccess)
			CommandList.TransitionTextures(VisibilityTransition);
		FRHIRenderPassInfo PassInfo{};
		PassInfo.RenderTargetLayout = RenderTargetLayouts::MakeContactVisibilityOutput();
		PassInfo.ColorRenderTargets[0] = FragmentTargets->Visibility;
		PassInfo.ColorClearValues[0] = FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f);
		CommandList.BeginRenderPass(PassInfo, "ContactVisibilityRenderPass");
		CommandList.SetGraphicsPipelineState(*FragmentPayload->PipelineState);
		CommandList.SetViewport(static_cast<float>(View.ViewportX),
					static_cast<float>(View.ViewportY), 0.0f,
					static_cast<float>(View.ViewportX + View.ViewportWidth),
					static_cast<float>(View.ViewportY + View.ViewportHeight), 1.0f);
		CommandList.SetScissor(static_cast<float>(View.ViewportX),
					static_cast<float>(View.ViewportY),
					static_cast<float>(View.ViewportWidth),
					static_cast<float>(View.ViewportHeight));
		CommandList.BindVertexBuffer(0, VertexBuffer, 0);
		CommandList.BindIndexBuffer(IndexBuffer, 0);
		FContactVisibilityFragmentShader::FParameters Parameters;
		Parameters.Params = UniformBuffer;
		if (Policy.GraphShaderParameters)
			SetShaderParameters(CommandList, FragmentPayload->FragmentShader,
				*Policy.GraphShaderParameters, Parameters);
		else
		{
			Parameters.GBufferMaterial = Material;
			Parameters.GBufferNormals = Normals;
			Parameters.GBufferSurface = Surface;
			Parameters.GBufferEmissive = Emissive;
			Parameters.SceneDepth = SceneDepth;
			SetShaderParameters(CommandList, FragmentPayload->FragmentShader,
				Parameters);
		}
		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();
		if (TimingQuery)
		{
			CommandList.EndGPUTimingQuery(TimingQuery);
			TimingSink(TimingQuery, Decision.Route);
		}
		const std::array FinalVisibilityTransition{FRHITextureTransition::Whole(
			FragmentTargets->Visibility, ERHIAccess::ColorAttachmentReadWrite,
			ERHIAccess::GraphicsShaderRead)};
		if (!Policy.bGraphManagedTextureAccess)
			CommandList.TransitionTextures(FinalVisibilityTransition);
		return {.Visibility = FragmentTargets->Visibility,
			.Route = Decision.Route, .Reason = Decision.Reason};
	}

	auto FContactShadowVisibilityRenderer::GetRetainedTargetBytes_RenderThread() const
		-> uint64
	{
		check(IsInRenderingThread());
		return TransientTargets.GetRetainedBytes_RenderThread(
			ERendererTransientTargetGroup::ContactFragment)
			+ TransientTargets.GetRetainedBytes_RenderThread(
				ERendererTransientTargetGroup::ContactCompute);
	}

	auto FContactShadowVisibilityRenderer::ReleaseResources_RenderThread() -> void
	{
		State->FragmentResources.Reset();
		State->ComputeResources.Reset();
	}
} // namespace Durin
