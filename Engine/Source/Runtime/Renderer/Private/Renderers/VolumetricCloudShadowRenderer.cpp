#include "Renderers/VolumetricCloudShadowRenderer.h"

#include "Math/Operations.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Renderers/RendererTransientTargetPool.h"
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
			ERenderResourceCreateErrorCategory Category) -> FRenderResourceCreateError
		{
			return MakeRendererResourceCreateError(Category, Resource, Key,
				std::move(Message), ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device
				| ERenderResourceGenerationDependency::Manual);
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
		FFullscreenGeometryResources& InFullscreenGeometry,
		FRendererTransientTargetPool& InTransientTargets)
		: Coordinator(InCoordinator), FullscreenGeometry(InFullscreenGeometry),
		  TransientTargets(InTransientTargets),
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

	auto FVolumetricCloudShadowRenderer::EnsureTargets_RenderThread(
		uint32 Width, uint32 Height) -> std::optional<FTargets>
	{
		check(IsInRenderingThread());
		if (Width == 0 || Height == 0
			|| CalculateTargetBytes(Width, Height) > MaximumRetainedBytesPerRoute)
			return std::nullopt;
		const auto Desc = FRHITextureCreateDesc::Create2D(
			"VolumetricCloudVisibility", Width, Height, EPixelFormat::R8_UNORM)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy
				| ETextureCreateFlags::CPUReadback)
			.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f));
		const auto Lease = TransientTargets.AcquireBundle_RenderThread(
			ERendererTransientTargetGroup::VolumetricCloudShadowFragment,
			std::span(&Desc, 1),
			MaximumRetainedBytesPerRoute);
		if (!Lease) return std::nullopt;
		return FTargets{.Visibility = Lease->Textures[0]};
	}

	auto FVolumetricCloudShadowRenderer::EnsureComputeTargets_RenderThread(
		uint32 Width, uint32 Height) -> std::optional<FComputeTargets>
	{
		check(IsInRenderingThread());
		if (Width == 0 || Height == 0 || GDynamicRHI == nullptr
			|| CalculateTargetBytes(Width, Height) > MaximumRetainedBytesPerRoute)
			return std::nullopt;
		const auto Desc = FRHITextureCreateDesc::Create2D(
			"VolumetricCloudVisibilityCompute", Width, Height, EPixelFormat::R8_UNORM)
			.SetFlags(ETextureCreateFlags::Storage | ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback);
		if (!GDynamicRHI->RHIIsTextureSupported(Desc)) return std::nullopt;
		const auto Lease = TransientTargets.AcquireBundle_RenderThread(
			ERendererTransientTargetGroup::VolumetricCloudShadowCompute,
			std::span(&Desc, 1),
			MaximumRetainedBytesPerRoute);
		if (!Lease) return std::nullopt;
		FComputeTargets Targets{.Visibility = Lease->Textures[0]};
		Targets.SampledView =
			GDynamicRHI->RHIGetOrCreateTextureView(Lease->Textures[0],
				MakeDefaultTextureViewDesc(*Lease->Textures[0],
					ERHITextureViewUsage::Sampled));
		Targets.StorageView =
			GDynamicRHI->RHIGetOrCreateTextureView(Lease->Textures[0],
				MakeDefaultTextureViewDesc(*Lease->Textures[0],
					ERHITextureViewUsage::Storage));
		return Targets.SampledView && Targets.StorageView
			? std::optional<FComputeTargets>{std::move(Targets)} : std::nullopt;
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
		if (!View || !(Policy.bPreparationOnly
			? Policy.bInputsExpected : bPhysicalInputsValid)
			|| !bViewFits
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

		using FComputePayload = FState::FComputePayload;
		using FComputeResult = TRenderResourceCreateResult<FComputePayload>;
		FComputePayload* ComputePayload = nullptr;
		if (Policy.bPreparationOnly
			? Policy.bComputeTargetExpected : ComputeTargets != nullptr)
			ComputePayload = State->ComputeResources.Resolve(
				Coordinator.GetGeneration_RenderThread(), [this]() -> FComputeResult {
					const std::array<const FGlobalShaderType*, 1> Types{
						&FCloudShadowComputeShader::StaticType()};
					FComputePayload Candidate;
					Candidate.ShaderSet = GetGlobalShaderMap().ResolveShaderSet(
						"VolumetricCloudShadow.Compute", Types, true,
						ReportRendererResourceCreateDiagnostic);
					if (!Candidate.ShaderSet)
						return FComputeResult::Failure(MakeFailure("VolumetricCloudShadowCompute",
							"shader", "Global shader set is unavailable.", ERenderResourceCreateErrorCategory::ShaderCompile));
					Candidate.ComputeShader = TShaderMapRef<FCloudShadowComputeShader>(Candidate.ShaderSet);
					FRHIShader* RHIShader = Candidate.ComputeShader.GetRHIShader(false);
					if (!RHIShader || !GDynamicRHI) return FComputeResult::Failure(MakeFailure(
						"VolumetricCloudShadowCompute", "pipeline", "Compute shader RHI is unavailable.",
						ERenderResourceCreateErrorCategory::RHIResource));
					FComputePipelineStateInitializer Initializer;
					Initializer.ComputeShader = RHIShader;
					Initializer.PipelineLayout = Candidate.ShaderSet.GetPipelineLayout();
					Candidate.PipelineState = GDynamicRHI->RHICreateComputePipelineState(
						"VolumetricCloudShadowComputePipeline", Initializer);
					if (!Candidate.PipelineState) return FComputeResult::Failure(MakeFailure(
						"VolumetricCloudShadowCompute", "pipeline", "Compute pipeline creation returned null.",
						ERenderResourceCreateErrorCategory::GraphicsPipeline));
					return FComputeResult::Success(std::move(Candidate));
				}, ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable);

		const FRHICapabilities* Capabilities = GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr;
		const uint32 GroupsX = CalculateGroupCount(Input.Width);
		const uint32 GroupsY = CalculateGroupCount(Input.Height);
		const bool bComputeExtent = Capabilities && GroupsX <= Capabilities->MaxComputeWorkGroupCount[0]
			&& GroupsY <= Capabilities->MaxComputeWorkGroupCount[1];
		const bool bComputeTargetReady = Policy.bPreparationOnly
			? Policy.bComputeTargetExpected : ComputeTargets != nullptr;
		ERouteReason FallbackReason = !ComputePayload ? ERouteReason::ComputePayloadUnavailable
			: !bComputeTargetReady ? ERouteReason::ComputeTargetUnavailable
			: ERouteReason::ComputeExtentUnsupported;

		using FFragmentPayload = FState::FFragmentPayload;
		using FFragmentResult = TRenderResourceCreateResult<FFragmentPayload>;
		FFragmentPayload* FragmentPayload = nullptr;
		const bool bFragmentTargetReady = Policy.bPreparationOnly
			? Policy.bFragmentTargetExpected : FragmentTargets != nullptr;
		if (!(ComputePayload && bComputeTargetReady && bComputeExtent)
			&& bFragmentTargetReady)
			FragmentPayload = State->FragmentResources.Resolve(
				Coordinator.GetGeneration_RenderThread(), [this, &CommandList]() -> FFragmentResult {
					const std::array<const FGlobalShaderType*, 2> Types{
						&FCloudShadowVertexShader::StaticType(),
						&FCloudShadowFragmentShader::StaticType()};
					FFragmentPayload Candidate;
					Candidate.ShaderSet = GetGlobalShaderMap().ResolveShaderSet(
						"VolumetricCloudShadow.Fragment", Types, true,
						ReportRendererResourceCreateDiagnostic);
					if (!Candidate.ShaderSet)
						return FFragmentResult::Failure(MakeFailure("VolumetricCloudShadow",
							"shader", "Global shader set is unavailable.", ERenderResourceCreateErrorCategory::ShaderCompile));
					if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList))
						return FFragmentResult::Failure(MakeFailure("VolumetricCloudShadow", "shader",
							"Typed shaders or fullscreen geometry are unavailable.",
							ERenderResourceCreateErrorCategory::ShaderBinding));
					Candidate.VertexShader = TShaderMapRef<FCloudShadowVertexShader>(Candidate.ShaderSet);
					Candidate.FragmentShader = TShaderMapRef<FCloudShadowFragmentShader>(Candidate.ShaderSet);
					FRHIShader* VertexRHI = Candidate.VertexShader.GetRHIShader(false);
					FRHIShader* FragmentRHI = Candidate.FragmentShader.GetRHIShader(false);
					if (!VertexRHI || !FragmentRHI || !GDynamicRHI)
						return FFragmentResult::Failure(MakeFailure("VolumetricCloudShadow", "pipeline",
							"Graphics shader RHI is unavailable.", ERenderResourceCreateErrorCategory::RHIResource));
					FGraphicsPipelineStateInitializer Initializer;
					Initializer.RenderTargetLayout = RenderTargetLayouts::MakeVolumetricCloudShadowOutput();
					Initializer.BoundShaders.VertexShader = VertexRHI;
					Initializer.BoundShaders.FragmentShader = FragmentRHI;
					Initializer.VertexDeclaration = FullscreenGeometry.GetVertexDeclaration_RenderThread();
					Initializer.RasterizerState.CullMode = ERHICullMode::None;
					Initializer.PipelineLayout = Candidate.ShaderSet.GetPipelineLayout();
					Candidate.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState(
						"VolumetricCloudShadowPipeline", Initializer);
					if (!Candidate.PipelineState) return FFragmentResult::Failure(MakeFailure(
						"VolumetricCloudShadow", "pipeline", "Graphics pipeline creation returned null.",
						ERenderResourceCreateErrorCategory::GraphicsPipeline));
					return FFragmentResult::Success(std::move(Candidate));
				}, ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable);

		const bool bUseCompute = ComputePayload && bComputeTargetReady && bComputeExtent;
		if (!bUseCompute && !FragmentPayload)
		{
			Result.Reason = ERouteReason::FragmentUnavailable;
			return Result;
		}
		if (Policy.bPreparationOnly)
		{
			Result.Route = bUseCompute ? ERoute::Compute : ERoute::Fragment;
			Result.Reason = bUseCompute ? ERouteReason::Compute : FallbackReason;
			return Result;
		}
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

	auto FVolumetricCloudShadowRenderer::GetRetainedTargetBytes_RenderThread() const -> uint64
	{
		check(IsInRenderingThread());
		const uint64 Fragment = TransientTargets.GetRetainedBytes_RenderThread(
			ERendererTransientTargetGroup::VolumetricCloudShadowFragment);
		const uint64 Compute = TransientTargets.GetRetainedBytes_RenderThread(
			ERendererTransientTargetGroup::VolumetricCloudShadowCompute);
		return Fragment + Compute;
	}

	auto FVolumetricCloudShadowRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->FragmentResources.Reset();
		State->ComputeResources.Reset();
	}
}
