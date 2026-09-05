#include "Renderers/VolumetricCloudShadowRenderer.h"

#include "Math/Operations.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
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
		std::atomic<FVolumetricCloudShadowRenderer::FTimingQuerySink>
			GVolumetricCloudShadowTimingQuerySink = nullptr;
		std::atomic<FVolumetricCloudShadowRenderer::FCaptureSink>
			GVolumetricCloudShadowCaptureSink = nullptr;
		class FCloudShadowVertexShader final : public FGlobalShader
		{
		public:
			DURIN_DECLARE_GLOBAL_SHADER(FCloudShadowVertexShader, FGlobalShader,
				"/Engine/VolumetricCloudShadow", EShaderFrequency::Vertex, "VertexMain");
		};
		class FCloudShadowFragmentShader final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FCloudShadowFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(BaseDensity);
				DURIN_SHADER_PARAMETER_TEXTURE(DetailDensity);
				DURIN_SHADER_PARAMETER_TEXTURE(WeatherTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);
				DURIN_SHADER_PARAMETER_SAMPLER(DensitySampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Params);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_GLOBAL_SHADER(FCloudShadowFragmentShader, FGlobalShader,
				"/Engine/VolumetricCloudShadow", EShaderFrequency::Fragment,
				"CloudVisibilityFragmentMain");
		};
		class FCloudShadowComputeShader final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FCloudShadowComputeShader)
				DURIN_SHADER_PARAMETER_TEXTURE(BaseDensity);
				DURIN_SHADER_PARAMETER_TEXTURE(DetailDensity);
				DURIN_SHADER_PARAMETER_TEXTURE(WeatherTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);
				DURIN_SHADER_PARAMETER_SAMPLER(DensitySampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER(Params);
				DURIN_SHADER_PARAMETER_STORAGE_IMAGE(CloudVisibilityOutput);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_GLOBAL_SHADER(FCloudShadowComputeShader, FGlobalShader,
				"/Engine/VolumetricCloudShadow", EShaderFrequency::Compute,
				"CloudVisibilityComputeMain");
		};
		DURIN_IMPLEMENT_GLOBAL_SHADER(FCloudShadowVertexShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FCloudShadowFragmentShader);
		DURIN_IMPLEMENT_GLOBAL_SHADER(FCloudShadowComputeShader);
		const FGlobalShaderSetRegistration GCloudShadowComputeShaderSet(
			"Renderer", "VolumetricCloudShadow.Compute",
			EShaderRequestEligibility::GameAndEditor,
			{&FCloudShadowComputeShader::StaticType()});
		const FGlobalShaderSetRegistration GCloudShadowFragmentShaderSet(
			"Renderer", "VolumetricCloudShadow.Fragment",
			EShaderRequestEligibility::GameAndEditor,
			{&FCloudShadowVertexShader::StaticType(),
			 &FCloudShadowFragmentShader::StaticType()});
		struct alignas(16) FCloudShadowUniform
		{
			float InverseViewProjection[16]{};
			float Layer[4]{};
			float Density[4]{};
			float BaseFrequency[4]{};
			float DetailFrequency[4]{};
			float WindOffset[4]{};
			float Weather[4]{};
			float LightDirection[4]{};
			float Viewport[4]{};
		};
		static_assert(sizeof(FCloudShadowUniform) == 192);

		auto MakeFailure(const char* Resource, const char* Key, std::string Message,
			ERenderResourceCreateErrorCategory Category,
			ERenderResourceCreateErrorReason Reason =
				ERenderResourceCreateErrorReason::Unspecified)
			-> FRenderResourceCreateError
		{
			return MakeRendererResourceCreateError(Category, Resource, Key,
				std::move(Message), ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device
					| ERenderResourceGenerationDependency::Manual, Reason);
		}
	}

	struct FVolumetricCloudShadowRenderer::FState
	{
		struct FFragmentPayload
		{
			FGlobalShaderSetRef ShaderSet;
			TShaderMapRef<FCloudShadowVertexShader> VertexShader;
			TShaderMapRef<FCloudShadowFragmentShader> FragmentShader;
			FGraphicsPipelineStateRHIRef PipelineState;
		};
		struct FComputePayload
		{
			FGlobalShaderSetRef ShaderSet;
			TShaderMapRef<FCloudShadowComputeShader> ComputeShader;
			FComputePipelineStateRHIRef PipelineState;
		};
		TRenderResourceCreationSlot<FFragmentPayload> FragmentResources{
			ERenderResourceGenerationDependency::Shader
			| ERenderResourceGenerationDependency::Device};
		TRenderResourceCreationSlot<FComputePayload> ComputeResources{
			ERenderResourceGenerationDependency::Shader
			| ERenderResourceGenerationDependency::Device};
	};

	FVolumetricCloudShadowRenderer::FVolumetricCloudShadowRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry)
		: Coordinator(InCoordinator), FullscreenGeometry(InFullscreenGeometry),
		  State(std::make_unique<FState>()) {}
	FVolumetricCloudShadowRenderer::~FVolumetricCloudShadowRenderer() = default;
	auto FVolumetricCloudShadowRenderer::SetTimingQuerySink(FTimingQuerySink Sink) -> void
	{
		GVolumetricCloudShadowTimingQuerySink.store(Sink, std::memory_order_release);
	}
	auto FVolumetricCloudShadowRenderer::SetCaptureSink(FCaptureSink Sink) -> void
	{
		GVolumetricCloudShadowCaptureSink.store(Sink, std::memory_order_release);
	}

	auto FVolumetricCloudShadowRenderer::DescribeFragmentTarget(
		uint32 Width, uint32 Height) -> FRHITextureCreateDesc
	{
		return FRHITextureCreateDesc::Create2D(
			"VolumetricCloudVisibility", Width, Height, EPixelFormat::R8_UNORM)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy
				| ETextureCreateFlags::CPUReadback)
			.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f));
	}

	auto FVolumetricCloudShadowRenderer::DescribeComputeTarget(
		uint32 Width, uint32 Height) -> FRHITextureCreateDesc
	{
		return FRHITextureCreateDesc::Create2D(
			"VolumetricCloudVisibilityCompute", Width, Height, EPixelFormat::R8_UNORM)
			.SetFlags(ETextureCreateFlags::Storage | ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback);
	}

	auto FVolumetricCloudShadowRenderer::SelectRoute(const FRouteInputs& Inputs)
		-> FRouteDecision
	{
		if (!Inputs.bRequested)
			return {ERoute::FactorOne, ERouteReason::DisabledOrUnneeded};
		if (!Inputs.bInputsValid)
			return {ERoute::FactorOne, ERouteReason::InvalidInputs};
		if (Inputs.bComputePayloadReady && Inputs.bComputeTargetReady
			&& Inputs.bComputeExtentSupported)
			return {ERoute::Compute, ERouteReason::Compute};
		ERouteReason Reason = ERouteReason::ComputePayloadUnavailable;
		if (Inputs.bComputePayloadReady && !Inputs.bComputeTargetReady)
			Reason = ERouteReason::ComputeTargetUnavailable;
		else if (Inputs.bComputePayloadReady && Inputs.bComputeTargetReady
			&& !Inputs.bComputeExtentSupported)
			Reason = ERouteReason::ComputeExtentUnsupported;
		if (Inputs.bFragmentReady)
			return {ERoute::Fragment, Reason};
		return {ERoute::FactorOne, ERouteReason::FragmentUnavailable};
	}

	auto FVolumetricCloudShadowRenderer::EnsureComputeResources_RenderThread()
		-> bool
	{
		using FPayload = FState::FComputePayload;
		using FResult = TRenderResourceCreateResult<FPayload>;
		return State->ComputeResources.Resolve(
			Coordinator.GetGeneration_RenderThread(), []() -> FResult {
				const std::array<const FGlobalShaderType*, 1> Types{
					&FCloudShadowComputeShader::StaticType()};
				FPayload Candidate;
				Candidate.ShaderSet = GetGlobalShaderMap().ResolveShaderSet(
					"VolumetricCloudShadow.Compute", Types, true,
					ReportRendererResourceCreateDiagnostic);
				if (!Candidate.ShaderSet)
					return FResult::Failure(MakeFailure("VolumetricCloudShadowCompute",
						"shader", "Global shader set is unavailable.",
						ERenderResourceCreateErrorCategory::ShaderCompile,
						ERenderResourceCreateErrorReason::GlobalShaderUnavailable));
				Candidate.ComputeShader =
					TShaderMapRef<FCloudShadowComputeShader>(Candidate.ShaderSet);
				FRHIShader* RHIShader = Candidate.ComputeShader.GetRHIShader(false);
				if (!RHIShader || !GDynamicRHI)
					return FResult::Failure(MakeFailure("VolumetricCloudShadowCompute",
						"pipeline", "Compute shader RHI is unavailable.",
						ERenderResourceCreateErrorCategory::RHIResource));
				FComputePipelineStateInitializer Initializer;
				Initializer.ComputeShader = RHIShader;
				Initializer.PipelineLayout = Candidate.ShaderSet.GetPipelineLayout();
				Candidate.PipelineState = GDynamicRHI->RHICreateComputePipelineState(
					"VolumetricCloudShadowComputePipeline", Initializer);
				if (!Candidate.PipelineState)
					return FResult::Failure(MakeFailure("VolumetricCloudShadowCompute",
						"pipeline", "Compute pipeline creation returned null.",
						ERenderResourceCreateErrorCategory::GraphicsPipeline));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable)
			!= nullptr;
	}

	auto FVolumetricCloudShadowRenderer::EnsureFragmentResources_RenderThread(
		FRHICommandListImmediate& CommandList) -> bool
	{
		using FPayload = FState::FFragmentPayload;
		using FResult = TRenderResourceCreateResult<FPayload>;
		return State->FragmentResources.Resolve(
			Coordinator.GetGeneration_RenderThread(), [this, &CommandList]() -> FResult {
				const std::array<const FGlobalShaderType*, 2> Types{
					&FCloudShadowVertexShader::StaticType(),
					&FCloudShadowFragmentShader::StaticType()};
				FPayload Candidate;
				Candidate.ShaderSet = GetGlobalShaderMap().ResolveShaderSet(
					"VolumetricCloudShadow.Fragment", Types, true,
					ReportRendererResourceCreateDiagnostic);
				if (!Candidate.ShaderSet)
					return FResult::Failure(MakeFailure("VolumetricCloudShadow",
						"shader", "Global shader set is unavailable.",
						ERenderResourceCreateErrorCategory::ShaderCompile,
						ERenderResourceCreateErrorReason::GlobalShaderUnavailable));
				if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList))
					return FResult::Failure(MakeFailure("VolumetricCloudShadow", "shader",
						"Typed shaders or fullscreen geometry are unavailable.",
						ERenderResourceCreateErrorCategory::ShaderBinding));
				Candidate.VertexShader =
					TShaderMapRef<FCloudShadowVertexShader>(Candidate.ShaderSet);
				Candidate.FragmentShader =
					TShaderMapRef<FCloudShadowFragmentShader>(Candidate.ShaderSet);
				FRHIShader* VertexRHI = Candidate.VertexShader.GetRHIShader(false);
				FRHIShader* FragmentRHI = Candidate.FragmentShader.GetRHIShader(false);
				if (!VertexRHI || !FragmentRHI || !GDynamicRHI)
					return FResult::Failure(MakeFailure("VolumetricCloudShadow", "pipeline",
						"Graphics shader RHI is unavailable.",
						ERenderResourceCreateErrorCategory::RHIResource));
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeVolumetricCloudShadowOutput();
				Initializer.BoundShaders.VertexShader = VertexRHI;
				Initializer.BoundShaders.FragmentShader = FragmentRHI;
				Initializer.VertexDeclaration =
					FullscreenGeometry.GetVertexDeclaration_RenderThread();
				Initializer.RasterizerState.CullMode = ERHICullMode::None;
				Initializer.PipelineLayout = Candidate.ShaderSet.GetPipelineLayout();
				Candidate.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState(
					"VolumetricCloudShadowPipeline", Initializer);
				if (!Candidate.PipelineState)
					return FResult::Failure(MakeFailure("VolumetricCloudShadow", "pipeline",
						"Graphics pipeline creation returned null.",
						ERenderResourceCreateErrorCategory::GraphicsPipeline));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable)
			!= nullptr;
	}

	auto FVolumetricCloudShadowRenderer::Render_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FTargets* FragmentTargets,
		const FComputeTargets* ComputeTargets,
		const FRenderInput& Input,
		const FRenderPolicy& Policy) -> FRenderResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		FRenderResult Result;
		if (!Input.bRequested) return Result;
		const FSceneView* View = Input.View;
		const double LightLengthSquared = Math::Dot(
			Input.Parameters.LightDirection, Input.Parameters.LightDirection);
		const bool bViewFits = View && View->ViewportX <= Input.Width
			&& View->ViewportY <= Input.Height
			&& View->ViewportWidth <= Input.Width - View->ViewportX
			&& View->ViewportHeight <= Input.Height - View->ViewportY;
		const bool bPhysicalInputsValid = View && Input.BaseDensity
			&& Input.DetailDensity && Input.Weather
			&& Input.SceneDepth && Input.DensitySampler;
		if (!View || !bPhysicalInputsValid || !bViewFits
			|| !Input.Parameters.IsValid() || !std::isfinite(LightLengthSquared)
			|| LightLengthSquared <= 1.0e-8)
		{
			Result.Reason = ERouteReason::InvalidInputs;
			return Result;
		}
		if (Input.Width == 0 || Input.Height == 0)
		{
			Result.Reason = ERouteReason::InvalidExtent;
			return Result;
		}

		if (ComputeTargets != nullptr)
			EnsureComputeResources_RenderThread();
		auto* ComputePayload = State->ComputeResources.GetPayload();

		const FRHICapabilities* Capabilities = GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr;
		const uint32 GroupsX = CalculateGroupCount(Input.Width);
		const uint32 GroupsY = CalculateGroupCount(Input.Height);
		const bool bComputeExtent = Capabilities && GroupsX <= Capabilities->MaxComputeWorkGroupCount[0]
			&& GroupsY <= Capabilities->MaxComputeWorkGroupCount[1];
		const bool bComputeTargetReady = ComputeTargets != nullptr;
		if (!(ComputePayload && bComputeTargetReady && bComputeExtent)
			&& FragmentTargets != nullptr)
			EnsureFragmentResources_RenderThread(CommandList);
		auto* FragmentPayload = State->FragmentResources.GetPayload();
		FRouteDecision Decision = SelectRoute({
			.bRequested = Input.bRequested,
			.bInputsValid = true,
			.bComputePayloadReady = ComputePayload != nullptr,
			.bComputeTargetReady = bComputeTargetReady,
			.bFragmentReady = FragmentTargets != nullptr && FragmentPayload != nullptr,
			.bComputeExtentSupported = bComputeExtent});
		if (Decision.Route == ERoute::FactorOne)
		{
			Result.Reason = Decision.Reason;
			return Result;
		}
		if (Policy.PreparedRoute)
		{
			if (Decision.Route != Policy.PreparedRoute->Route)
			{
				Result.Reason = Decision.Reason;
				return Result;
			}
			Decision = *Policy.PreparedRoute;
		}
		const bool bUseCompute = Decision.Route == ERoute::Compute;
		const ERouteReason FallbackReason = Decision.Reason;
		FMatrix InverseViewProjection;
		if (!Math::TryInverse(View->ViewProjectionMatrix, InverseViewProjection, 1.0e-8))
		{
			Result.Reason = ERouteReason::InvalidInputs;
			return Result;
		}
		FCloudShadowUniform Uniform;
		for (uint32 Row = 0; Row < 4; ++Row)
			for (uint32 Col = 0; Col < 4; ++Col)
				Uniform.InverseViewProjection[Row * 4 + Col] =
					static_cast<float>(InverseViewProjection[Col][Row]);
		Uniform.Layer[0] = static_cast<float>(Input.Parameters.MinimumZ);
		Uniform.Layer[1] = static_cast<float>(Input.Parameters.MaximumZ);
		Uniform.Layer[2] = Input.Parameters.LightExtinction;
		Uniform.Layer[3] = View->DepthConvention == ESceneDepthConvention::ReversedZ ? 1.0f : 0.0f;
		Uniform.Density[0] = Input.Parameters.Coverage;
		Uniform.Density[1] = Input.Parameters.DetailErosion;
		Uniform.Density[2] = static_cast<float>(ResolveSampleCount(Input.QualityTier));
		auto Copy3 = [](float* Out, const FVector3f& Value) {
			Out[0] = Value.x; Out[1] = Value.y; Out[2] = Value.z;
		};
		Copy3(Uniform.BaseFrequency, Input.Parameters.BaseFrequency);
		Copy3(Uniform.DetailFrequency, Input.Parameters.DetailFrequency);
		Copy3(Uniform.WindOffset, Input.Parameters.WindOffset);
		Uniform.Weather[0] = Input.Parameters.WeatherFrequency.x;
		Uniform.Weather[1] = Input.Parameters.WeatherFrequency.y;
		Uniform.Weather[2] = Input.Parameters.WeatherOffset.x;
		Uniform.Weather[3] = Input.Parameters.WeatherOffset.y;
		Copy3(Uniform.LightDirection, Math::Normalize(Input.Parameters.LightDirection));
		Uniform.Viewport[0] = 1.0f / static_cast<float>(View->ViewportWidth);
		Uniform.Viewport[1] = 1.0f / static_cast<float>(View->ViewportHeight);
		Uniform.Viewport[2] = static_cast<float>(View->ViewportX);
		Uniform.Viewport[3] = static_cast<float>(View->ViewportY);
		const FRHIUniformBufferRange Buffer = CommandList.AllocateDynamicUniformBuffer(
			&Uniform, sizeof(Uniform));
		if (!Buffer.Buffer || Buffer.Size != sizeof(Uniform))
		{
			Result.Reason = ERouteReason::InvalidInputs;
			return Result;
		}
		FGPUTimingQueryRHIRef TimingQuery;
		const FTimingQuerySink TimingSink =
			GVolumetricCloudShadowTimingQuerySink.load(std::memory_order_acquire);
		if (TimingSink && GDynamicRHI)
		{
			TimingQuery = GDynamicRHI->RHICreateGPUTimingQuery();
			if (TimingQuery) CommandList.BeginGPUTimingQuery(TimingQuery);
		}

		if (bUseCompute)
		{
			const std::array BufferTransition{FRHIBufferTransition{Buffer.Buffer, Buffer.Offset,
				Buffer.Size, ERHIAccess::Discard, ERHIAccess::ComputeUniformRead}};
			CommandList.TransitionBuffers(BufferTransition);
			const std::array Inputs{
				FRHITextureTransition::Whole(Input.BaseDensity, ERHIAccess::GraphicsShaderRead, ERHIAccess::ComputeShaderRead),
				FRHITextureTransition::Whole(Input.DetailDensity, ERHIAccess::GraphicsShaderRead, ERHIAccess::ComputeShaderRead),
				FRHITextureTransition::Whole(Input.Weather, ERHIAccess::GraphicsShaderRead, ERHIAccess::ComputeShaderRead),
				FRHITextureTransition::Whole(Input.SceneDepth, ERHIAccess::GraphicsShaderRead, ERHIAccess::ComputeShaderRead),
				FRHITextureTransition::Whole(ComputeTargets->Visibility, ERHIAccess::Discard, ERHIAccess::ComputeShaderReadWrite)};
			if (!Policy.bGraphManagedTextureAccess)
				CommandList.TransitionTextures(Inputs);
			CommandList.SwitchPipeline(ERHIPipeline::Compute);
			CommandList.SetComputePipelineState(*ComputePayload->PipelineState);
			FCloudShadowComputeShader::FParameters Params;
			Params.BaseDensity = Input.BaseDensity;
			Params.DetailDensity = Input.DetailDensity;
			Params.WeatherTexture = Input.Weather;
			Params.SceneDepth = Input.SceneDepth;
			Params.DensitySampler = Input.DensitySampler;
			Params.Params = Buffer;
			Params.CloudVisibilityOutput = ComputeTargets->Visibility;
			SetShaderParameters(CommandList, ComputePayload->ComputeShader, Params);
			CommandList.Dispatch(GroupsX, GroupsY, 1);
			const std::array Restore{
				FRHITextureTransition::Whole(Input.BaseDensity, ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Input.DetailDensity, ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Input.Weather, ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Input.SceneDepth, ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(ComputeTargets->Visibility, ERHIAccess::ComputeShaderReadWrite, ERHIAccess::GraphicsShaderRead)};
			if (!Policy.bGraphManagedTextureAccess)
				CommandList.TransitionTextures(Restore);
			CommandList.SwitchPipeline(ERHIPipeline::Graphics);
			Result.Visibility = ComputeTargets->Visibility;
			Result.Route = ERoute::Compute;
			Result.Reason = ERouteReason::Compute;
		}
		else
		{
			FRHIRenderPassInfo Pass{};
			Pass.RenderTargetLayout = RenderTargetLayouts::MakeVolumetricCloudShadowOutput();
			Pass.ColorRenderTargets[0] = FragmentTargets->Visibility;
			Pass.ColorClearValues[0] = FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f);
			CommandList.BeginRenderPass(Pass, "VolumetricCloudShadowRenderPass");
			CommandList.SetGraphicsPipelineState(*FragmentPayload->PipelineState);
			CommandList.SetViewport(static_cast<float>(View->ViewportX), static_cast<float>(View->ViewportY), 0.0f,
				static_cast<float>(View->ViewportX + View->ViewportWidth),
				static_cast<float>(View->ViewportY + View->ViewportHeight), 1.0f);
			CommandList.SetScissor(static_cast<float>(View->ViewportX), static_cast<float>(View->ViewportY),
				static_cast<float>(View->ViewportWidth), static_cast<float>(View->ViewportHeight));
			CommandList.BindVertexBuffer(0, FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
			CommandList.BindIndexBuffer(FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
			FCloudShadowFragmentShader::FParameters Params;
			Params.BaseDensity = Input.BaseDensity;
			Params.DetailDensity = Input.DetailDensity;
			Params.WeatherTexture = Input.Weather;
			Params.SceneDepth = Input.SceneDepth;
			Params.DensitySampler = Input.DensitySampler;
			Params.Params = Buffer;
			SetShaderParameters(CommandList, FragmentPayload->FragmentShader, Params);
			CommandList.DrawIndexed(3, 0, 0);
			CommandList.EndRenderPass();
			Result.Visibility = FragmentTargets->Visibility;
			Result.Route = ERoute::Fragment;
			Result.Reason = FallbackReason;
		}
		Result.SampleCount = ResolveSampleCount(Input.QualityTier);
		Result.TargetBytes = CalculateTargetBytes(Input.Width, Input.Height);
		if (TimingQuery)
		{
			CommandList.EndGPUTimingQuery(TimingQuery);
			TimingSink(TimingQuery, Result.Route);
		}
		const FCaptureSink CaptureSink =
			GVolumetricCloudShadowCaptureSink.load(std::memory_order_acquire);
		if (CaptureSink) CaptureSink(Result.Visibility, Result.Route);
		return Result;
	}

	auto FVolumetricCloudShadowRenderer::PrepareRoute_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FRenderInput& Input,
		bool bFragmentTargetExpected,
		bool bComputeTargetExpected) -> FRouteDecision
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		if (!Input.bRequested)
			return {ERoute::FactorOne, ERouteReason::DisabledOrUnneeded};
		const FSceneView* View = Input.View;
		const double LightLengthSquared = Math::Dot(
			Input.Parameters.LightDirection, Input.Parameters.LightDirection);
		const bool bViewFits = View && View->ViewportX <= Input.Width
			&& View->ViewportY <= Input.Height
			&& View->ViewportWidth <= Input.Width - View->ViewportX
			&& View->ViewportHeight <= Input.Height - View->ViewportY;
		const bool bInputsValid = View && bViewFits && Input.Parameters.IsValid()
			&& std::isfinite(LightLengthSquared) && LightLengthSquared > 1.0e-8;
		if (!bInputsValid)
			return {ERoute::FactorOne, ERouteReason::InvalidInputs};
		if (Input.Width == 0 || Input.Height == 0)
			return {ERoute::FactorOne, ERouteReason::InvalidExtent};
		if (bComputeTargetExpected)
			EnsureComputeResources_RenderThread();
		const FRHICapabilities* Capabilities =
			GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr;
		const uint32 GroupsX = CalculateGroupCount(Input.Width);
		const uint32 GroupsY = CalculateGroupCount(Input.Height);
		const bool bComputeExtent = Capabilities
			&& GroupsX <= Capabilities->MaxComputeWorkGroupCount[0]
			&& GroupsY <= Capabilities->MaxComputeWorkGroupCount[1];
		FRouteInputs Inputs{
			.bRequested = true,
			.bInputsValid = true,
			.bComputePayloadReady = State->ComputeResources.GetPayload() != nullptr,
			.bComputeTargetReady = bComputeTargetExpected,
			.bFragmentReady = false,
			.bComputeExtentSupported = bComputeExtent};
		FRouteDecision Decision = SelectRoute(Inputs);
		if (Decision.Route != ERoute::Compute && bFragmentTargetExpected)
			EnsureFragmentResources_RenderThread(CommandList);
		Inputs.bFragmentReady = bFragmentTargetExpected
			&& State->FragmentResources.GetPayload() != nullptr
			&& FullscreenGeometry.GetVertexBuffer_RenderThread() != nullptr
			&& FullscreenGeometry.GetIndexBuffer_RenderThread() != nullptr;
		return SelectRoute(Inputs);
	}

	auto FVolumetricCloudShadowRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->FragmentResources.Reset();
		State->ComputeResources.Reset();
	}
}
