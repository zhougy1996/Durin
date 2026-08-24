#include "Renderers/VolumetricCloudRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Renderers/RendererTransientTargetPool.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Math/Operations.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "Renderers/SceneViewState.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin
{
	namespace
	{
		std::atomic<FVolumetricCloudRenderer::FTimingQuerySink>
			GVolumetricCloudTimingQuerySink = nullptr;
		std::atomic<FVolumetricCloudRenderer::FCaptureSink>
			GVolumetricCloudCaptureSink = nullptr;

		class FCloudVertexShader final : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FCloudVertexShader, FShader, "/Engine/VolumetricCloud", EShaderFrequency::Vertex, "VertexMain");
		};

		class FCloudFragmentShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FCloudFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(BaseDensity);
				DURIN_SHADER_PARAMETER_TEXTURE(DetailDensity);
				DURIN_SHADER_PARAMETER_TEXTURE(WeatherTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);
				DURIN_SHADER_PARAMETER_SAMPLER(DensitySampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Params);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FCloudFragmentShader, FShader, "/Engine/VolumetricCloud", EShaderFrequency::Fragment, "CloudFragmentMain");
		};

		class FCloudComputeShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FCloudComputeShader)
				DURIN_SHADER_PARAMETER_TEXTURE(BaseDensity);
				DURIN_SHADER_PARAMETER_TEXTURE(DetailDensity);
				DURIN_SHADER_PARAMETER_TEXTURE(WeatherTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);
				DURIN_SHADER_PARAMETER_SAMPLER(DensitySampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER(Params);
				DURIN_SHADER_PARAMETER_STORAGE_IMAGE(CloudOutput);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FCloudComputeShader, FShader, "/Engine/VolumetricCloud", EShaderFrequency::Compute, "CloudComputeMain");
		};

		class FCloudCompositeVertexShader final : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FCloudCompositeVertexShader, FShader, "/Engine/VolumetricCloudComposite", EShaderFrequency::Vertex, "VertexMain");
		};

		class FCloudTemporalVertexShader final : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FCloudTemporalVertexShader, FShader, "/Engine/VolumetricCloudTemporal", EShaderFrequency::Vertex, "VertexMain");
		};

		class FCloudTemporalFragmentShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FCloudTemporalFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(CurrentCloud);
				DURIN_SHADER_PARAMETER_TEXTURE(PreviousCloud);
				DURIN_SHADER_PARAMETER_SAMPLER(HistorySampler);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Params);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FCloudTemporalFragmentShader, FShader, "/Engine/VolumetricCloudTemporal", EShaderFrequency::Fragment, "FragmentMain");
		};

		class FCloudCompositeFragmentShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FCloudCompositeFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SceneColorTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(CloudTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(SceneDepthTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(DebugTexture);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Params);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FCloudCompositeFragmentShader, FShader, "/Engine/VolumetricCloudComposite", EShaderFrequency::Fragment, "FragmentMain");
		};

		struct alignas(16) FCloudUniform
		{
			float InverseViewProjection[16]{};
			float Layer[4]{};
			float Density[4]{};
			float Sampling[4]{};
			float BaseFrequency[4]{};
			float DetailFrequency[4]{};
			float WindOffset[4]{};
			float Weather[4]{};
			float LightDirection[4]{};
			float LightColor[4]{};
			float AmbientColor[4]{};
			float CameraPosition[4]{};
			float Viewport[4]{};
			float OutputViewport[4]{};
			float Target[4]{};
			float Jitter[4]{};
		};
		static_assert(sizeof(FCloudUniform) == 304);

		struct alignas(16) FCloudCompositeUniform
		{
			float Extent[4]{};
			float Viewport[4]{};
			float Debug[4]{};
		};
		static_assert(sizeof(FCloudCompositeUniform) == 48);

		struct alignas(16) FCloudTemporalUniform
		{
			float InverseViewProjection[16]{};
			float PreviousViewProjection[16]{};
			float Layer[4]{};
			float CameraPosition[4]{};
			float Viewport[4]{};
			float Target[4]{};
		};
		static_assert(sizeof(FCloudTemporalUniform) == 192);

		auto MakeFailure(const char* Resource, const char* Key, std::string Message, ERenderResourceCreateErrorCategory Category)
			-> FRenderResourceCreateError
		{
			return MakeRendererResourceCreateError(Category, Resource, Key, std::move(Message), ERenderResourceGenerationDependency::Shader | ERenderResourceGenerationDependency::Device | ERenderResourceGenerationDependency::Manual);
		}
	} // namespace

	struct FVolumetricCloudRenderer::FState
	{
		struct FFragmentPayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FCloudVertexShader> VertexShader;
			TShaderRef<FCloudFragmentShader> FragmentShader;
			FGraphicsPipelineStateRHIRef PipelineState;
		};
		struct FComputePayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FCloudComputeShader> ComputeShader;
			FComputePipelineStateRHIRef PipelineState;
		};
		struct FCompositePayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FCloudCompositeVertexShader> VertexShader;
			TShaderRef<FCloudCompositeFragmentShader> FragmentShader;
			FGraphicsPipelineStateRHIRef PipelineState;
		};
		struct FTemporalPayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FCloudTemporalVertexShader> VertexShader;
			TShaderRef<FCloudTemporalFragmentShader> FragmentShader;
			FGraphicsPipelineStateRHIRef PipelineState;
		};
		struct FFallbackPayload
		{
			FTextureRHIRef WhiteWeather;
		};
		struct FDensitySamplerPayload
		{
			FSamplerRHIRef Sampler;
		};

		TRenderResourceCreationSlot<FFragmentPayload> FragmentResources{
			ERenderResourceGenerationDependency::Shader
			| ERenderResourceGenerationDependency::Device
		};
		TRenderResourceCreationSlot<FComputePayload> ComputeResources{
			ERenderResourceGenerationDependency::Shader
			| ERenderResourceGenerationDependency::Device
		};
		TRenderResourceCreationSlot<FCompositePayload> CompositeResources{
			ERenderResourceGenerationDependency::Shader
			| ERenderResourceGenerationDependency::Device
		};
		TRenderResourceCreationSlot<FTemporalPayload> TemporalResources{
			ERenderResourceGenerationDependency::Shader
			| ERenderResourceGenerationDependency::Device
		};
		TRenderResourceCreationSlot<FFallbackPayload> FallbackResources{
			ERenderResourceGenerationDependency::Device
		};
		TRenderResourceCreationSlot<FDensitySamplerPayload> DensitySamplerResources{
			ERenderResourceGenerationDependency::Device
		};
	};

	FVolumetricCloudRenderer::FVolumetricCloudRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry,
		FRendererTransientTargetPool& InTransientTargets
	)
		: Coordinator(InCoordinator)
		, FullscreenGeometry(InFullscreenGeometry)
		, TransientTargets(InTransientTargets)
		, State(std::make_unique<FState>())
	{
	}

	FVolumetricCloudRenderer::~FVolumetricCloudRenderer() = default;

	auto FVolumetricCloudRenderer::EnsureDensitySampler_RenderThread()
		-> FRHISampler*
	{
		check(IsInRenderingThread());
		using FResult = TRenderResourceCreateResult<FState::FDensitySamplerPayload>;
		auto* Payload = State->DensitySamplerResources.Resolve(
			Coordinator.GetGeneration_RenderThread(), []() -> FResult {
				FState::FDensitySamplerPayload Candidate{
					.Sampler = RHICreateSampler(FRHISamplerDesc::LinearClamp())
				};
				if (!Candidate.Sampler)
					return FResult::Failure(MakeFailure(
						"VolumetricCloud", "density-sampler",
						"Density sampler creation returned null.",
						ERenderResourceCreateErrorCategory::RHIResource
					));
				return FResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic
		);
		return Payload != nullptr ? Payload->Sampler.GetReference() : nullptr;
	}

	auto FVolumetricCloudRenderer::SetTimingQuerySink(FTimingQuerySink Sink) -> void
	{
		GVolumetricCloudTimingQuerySink.store(Sink, std::memory_order_release);
	}

	auto FVolumetricCloudRenderer::SetCaptureSink(FCaptureSink Sink) -> void
	{
		GVolumetricCloudCaptureSink.store(Sink, std::memory_order_release);
	}

	auto FVolumetricCloudRenderer::EnsureTargets_RenderThread(
		uint32 Width, uint32 Height
	) -> std::optional<FTargets>
	{
		check(IsInRenderingThread());
		const uint64 Bytes = FSpatial::CalculateTargetBytes(Width, Height);
		if (Width == 0 || Height == 0
			|| Bytes > FSpatial::MaximumRetainedTargetBytesPerFamily)
			return std::nullopt;
		const auto Desc = FRHITextureCreateDesc::Create2D(
							  "VolumetricCloudFragment", Width, Height, EPixelFormat::RGBA16_FLOAT
		)
							  .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback)
							  .SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f));
		const auto Lease = TransientTargets.AcquireBundle_RenderThread(
			ERendererTransientTargetGroup::VolumetricCloudFragment,
			std::span(&Desc, 1),
			FSpatial::MaximumRetainedTargetBytesPerFamily);
		if (!Lease) return std::nullopt;
		return FTargets{.Cloud = Lease->Textures[0]};
	}

	auto FVolumetricCloudRenderer::EnsureComputeTargets_RenderThread(
		uint32 Width, uint32 Height
	) -> std::optional<FComputeTargets>
	{
		check(IsInRenderingThread());
		const uint64 Bytes = FSpatial::CalculateTargetBytes(Width, Height);
		if (Width == 0 || Height == 0 || GDynamicRHI == nullptr
			|| Bytes > FSpatial::MaximumRetainedTargetBytesPerFamily)
			return std::nullopt;
		const auto Desc = FRHITextureCreateDesc::Create2D(
							  "VolumetricCloudCompute", Width, Height, EPixelFormat::RGBA16_FLOAT
		)
							  .SetFlags(ETextureCreateFlags::Storage | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback);
		if (!GDynamicRHI->RHIIsTextureSupported(Desc)) return std::nullopt;
		const auto Lease = TransientTargets.AcquireBundle_RenderThread(
			ERendererTransientTargetGroup::VolumetricCloudCompute,
			std::span(&Desc, 1),
			FSpatial::MaximumRetainedTargetBytesPerFamily);
		if (!Lease) return std::nullopt;
		FComputeTargets Targets{.Cloud = Lease->Textures[0]};
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

	auto FVolumetricCloudRenderer::EnsureCompositeTargets_RenderThread(
		uint32 Width, uint32 Height
	) -> std::optional<FTargets>
	{
		check(IsInRenderingThread());
		const uint64 Bytes = FSpatial::CalculateTargetBytes(Width, Height);
		if (Width == 0 || Height == 0
			|| Bytes > FSpatial::MaximumRetainedTargetBytesPerFamily)
			return std::nullopt;
		const auto Desc = FRHITextureCreateDesc::Create2D(
							  "VolumetricCloudComposite", Width, Height, EPixelFormat::RGBA16_FLOAT
		)
							  .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback)
							  .SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f));
		const auto Lease = TransientTargets.AcquireBundle_RenderThread(
			ERendererTransientTargetGroup::VolumetricCloudComposite,
			std::span(&Desc, 1),
			FSpatial::MaximumRetainedTargetBytesPerFamily);
		if (!Lease) return std::nullopt;
		return FTargets{.Cloud = Lease->Textures[0]};
	}

	// Resource resolution, binding, and execution continue below to keep target
	// creation independently testable.
	auto FVolumetricCloudRenderer::Render_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FTargets* FragmentTargets,
		const FComputeTargets* ComputeTargets,
		const FRenderInput& Input,
		const FRenderPolicy& Policy
	) -> FRenderResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const FSceneView* View = Input.View;
		const uint32 OutputWidth = Input.OutputWidth != 0 ? Input.OutputWidth : Input.Width;
		const uint32 OutputHeight = Input.OutputHeight != 0 ? Input.OutputHeight : Input.Height;
		const FSpatial::FQualityPolicy Quality =
			FSpatial::ResolveQualityPolicy(Input.QualityTier);
		const FSpatial::FExtent ExpectedExtent = FSpatial::CalculateScaledExtent(
			OutputWidth, OutputHeight, Quality
		);
		const bool bTargetMatchesPolicy = ExpectedExtent.Width == Input.Width
										  && ExpectedExtent.Height == Input.Height;
		const bool bViewFits = View != nullptr && View->ViewportX <= OutputWidth
							   && View->ViewportY <= OutputHeight
							   && View->ViewportWidth <= OutputWidth - View->ViewportX
							   && View->ViewportHeight <= OutputHeight - View->ViewportY;
		FSceneView CloudView = View != nullptr ? *View : FSceneView{};
		if (bViewFits && bTargetMatchesPolicy)
		{
			const auto Scaled = FSpatial::CalculateScaledViewport(
				{View->ViewportX, View->ViewportY, View->ViewportWidth,
				 View->ViewportHeight},
				{OutputWidth, OutputHeight}, {Input.Width, Input.Height}
			);
			CloudView.ViewportX = Scaled.X;
			CloudView.ViewportY = Scaled.Y;
			CloudView.ViewportWidth = Scaled.Width;
			CloudView.ViewportHeight = Scaled.Height;
		}
		FParameters Parameters = Input.Parameters;
		Parameters.PrimarySampleCount = Quality.PrimarySampleCount;
		Parameters.LightSampleCount = Quality.LightSampleCount;
		const bool bBaseInputsValid = (Policy.bPreparationOnly
			? Policy.bInputsExpected : Input.Textures.HasRequiredInputs())
									  && Parameters.IsValid() && bViewFits && bTargetMatchesPolicy
									  && CloudView.ViewportWidth != 0 && CloudView.ViewportHeight != 0;

		using FFallbackResult = TRenderResourceCreateResult<FState::FFallbackPayload>;
		FState::FFallbackPayload* Fallback = nullptr;
		if (Input.bRequested && bBaseInputsValid && Input.Textures.Weather == nullptr)
		{
			Fallback = State->FallbackResources.Resolve(
				Coordinator.GetGeneration_RenderThread(), [&CommandList]() -> FFallbackResult {
					FState::FFallbackPayload Candidate;
					const auto Desc = FRHITextureCreateDesc::Create2D(
										  "VolumetricCloudWhiteWeather", 1, 1, EPixelFormat::R8_UNORM
					)
										  .SetFlags(ETextureCreateFlags::ShaderResource);
					Candidate.WhiteWeather = GDynamicRHI != nullptr ? GDynamicRHI->RHICreateTexture(CommandList, Desc) : nullptr;
					if (Candidate.WhiteWeather)
					{
						const std::byte White{255};
						GDynamicRHI->RHIUpdateTexture2D(CommandList, Candidate.WhiteWeather,
							0, 0, FUpdateTextureRegion2D(0, 0, 0, 0, 1, 1), 1,
							std::span{&White, 1});
					}
					if (!Candidate.WhiteWeather)
						return FFallbackResult::Failure(MakeFailure(
							"VolumetricCloud", "white-weather",
							"Optional white weather texture creation returned null.",
							ERenderResourceCreateErrorCategory::RHIResource
						));
					return FFallbackResult::Success(std::move(Candidate));
				},
				ReportRendererResourceCreateDiagnostic
			);
		}
		FRHITexture* Weather = Input.Textures.Weather != nullptr ? Input.Textures.Weather : (Fallback != nullptr ? Fallback->WhiteWeather.GetReference() : nullptr);
		const bool bInputsValid = bBaseInputsValid && Weather != nullptr;

		using FComputePayload = FState::FComputePayload;
		using FComputeResult = TRenderResourceCreateResult<FComputePayload>;
		FComputePayload* ComputePayload = nullptr;
		if (Input.bRequested && bInputsValid && Input.Width != 0 && Input.Height != 0
			&& (Policy.bPreparationOnly
				? Policy.bComputeTargetExpected : ComputeTargets != nullptr))
		{
			ComputePayload = State->ComputeResources.Resolve(
				Coordinator.GetGeneration_RenderThread(), [this]() -> FComputeResult {
					FShaderCompileOptions Options;
					Options.bForceRecompile = Coordinator.ShouldForceShaderRecompile_RenderThread();
					FShaderType& Type = FCloudComputeShader::StaticType();
					const std::array<const FShaderType*, 1> Types{&Type};
					FComputePayload Candidate;
					Candidate.ShaderMap = std::make_shared<FShaderMapBase>();
					std::string Error;
					if (!Candidate.ShaderMap->InitializeFromShaderTypes(Types, Options, Error))
						return FComputeResult::Failure(MakeFailure("VolumetricCloudCompute", "shader", std::move(Error), ERenderResourceCreateErrorCategory::ShaderCompile));
					auto* Shader = static_cast<FCloudComputeShader*>(
						Candidate.ShaderMap->GetShader(&Type)
					);
					if (Shader == nullptr)
						return FComputeResult::Failure(MakeFailure("VolumetricCloudCompute", "shader", "Compiled shader map is missing the typed compute shader.", ERenderResourceCreateErrorCategory::ShaderBinding));
					Candidate.ComputeShader = {Shader, Candidate.ShaderMap.get()};
					FRHIShader* RHIShader = Candidate.ComputeShader.GetRHIShader(false);
					if (RHIShader == nullptr || GDynamicRHI == nullptr)
						return FComputeResult::Failure(MakeFailure("VolumetricCloudCompute", "pipeline", "Compute shader RHI creation returned null.", ERenderResourceCreateErrorCategory::RHIResource));
					FComputePipelineStateInitializer Initializer;
					Initializer.ComputeShader = RHIShader;
					Initializer.PipelineLayout = Candidate.ShaderMap->GetMergedPipelineLayout();
					Candidate.PipelineState = GDynamicRHI->RHICreateComputePipelineState(
						"VolumetricCloudComputePipeline", Initializer
					);
					if (!Candidate.PipelineState)
						return FComputeResult::Failure(MakeFailure("VolumetricCloudCompute", "pipeline", "Compute pipeline creation returned null.", ERenderResourceCreateErrorCategory::GraphicsPipeline));
					return FComputeResult::Success(std::move(Candidate));
				},
				ReportRendererResourceCreateDiagnostic
			);
		}
		const FRHICapabilities* Capabilities = GDynamicRHI != nullptr ? GDynamicRHI->RHIGetCapabilities() : nullptr;
		FSpatial::FRouteInputs RouteInputs{
			.bRequested = Input.bRequested,
			.bRequiredInputsValid = bInputsValid,
			.bComputePayloadReady = ComputePayload != nullptr,
			.bComputeTargetReady = Policy.bPreparationOnly
				? Policy.bComputeTargetExpected
				: ComputeTargets != nullptr && ComputeTargets->Cloud != nullptr,
			.bFragmentPayloadReady = false,
			.bFragmentTargetReady = Policy.bPreparationOnly
				? Policy.bFragmentTargetExpected
				: FragmentTargets != nullptr && FragmentTargets->Cloud != nullptr,
			.Width = Input.Width,
			.Height = Input.Height,
			.MaxGroupCountX = Capabilities ? Capabilities->MaxComputeWorkGroupCount[0] : 0,
			.MaxGroupCountY = Capabilities ? Capabilities->MaxComputeWorkGroupCount[1] : 0
		};
		FSpatial::FRouteDecision Decision = FSpatial::SelectRoute(RouteInputs);

		using FFragmentPayload = FState::FFragmentPayload;
		using FFragmentResult = TRenderResourceCreateResult<FFragmentPayload>;
		FFragmentPayload* FragmentPayload = nullptr;
		if (Decision.Route != ERoute::Compute && Input.bRequested && bInputsValid
			&& RouteInputs.bFragmentTargetReady)
		{
			FragmentPayload = State->FragmentResources.Resolve(
				Coordinator.GetGeneration_RenderThread(), [this, &CommandList]() -> FFragmentResult {
					FShaderCompileOptions Options;
					Options.bForceRecompile = Coordinator.ShouldForceShaderRecompile_RenderThread();
					FShaderType& VertexType = FCloudVertexShader::StaticType();
					FShaderType& FragmentType = FCloudFragmentShader::StaticType();
					const std::array<const FShaderType*, 2> Types{&VertexType, &FragmentType};
					FFragmentPayload Candidate;
					Candidate.ShaderMap = std::make_shared<FShaderMapBase>();
					std::string Error;
					if (!Candidate.ShaderMap->InitializeFromShaderTypes(Types, Options, Error))
						return FFragmentResult::Failure(MakeFailure("VolumetricCloudFragment", "shader", std::move(Error), ERenderResourceCreateErrorCategory::ShaderCompile));
					auto* Vertex = static_cast<FCloudVertexShader*>(Candidate.ShaderMap->GetShader(&VertexType));
					auto* Fragment = static_cast<FCloudFragmentShader*>(Candidate.ShaderMap->GetShader(&FragmentType));
					if (Vertex == nullptr || Fragment == nullptr)
						return FFragmentResult::Failure(MakeFailure("VolumetricCloudFragment", "shader", "Compiled shader map is missing a typed shader.", ERenderResourceCreateErrorCategory::ShaderBinding));
					Candidate.VertexShader = {Vertex, Candidate.ShaderMap.get()};
					Candidate.FragmentShader = {Fragment, Candidate.ShaderMap.get()};
					if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList))
						return FFragmentResult::Failure(MakeFailure("VolumetricCloudFragment", "fullscreen-geometry", "Shared fullscreen geometry is unavailable.", ERenderResourceCreateErrorCategory::RHIResource));
					FRHIShader* VertexRHI = Candidate.VertexShader.GetRHIShader(false);
					FRHIShader* FragmentRHI = Candidate.FragmentShader.GetRHIShader(false);
					if (VertexRHI == nullptr || FragmentRHI == nullptr || GDynamicRHI == nullptr)
						return FFragmentResult::Failure(MakeFailure("VolumetricCloudFragment", "pipeline", "RHI shader creation returned null.", ERenderResourceCreateErrorCategory::RHIResource));
					FGraphicsPipelineStateInitializer Initializer;
					Initializer.RenderTargetLayout = RenderTargetLayouts::MakeVolumetricCloudOutput();
					Initializer.BoundShaders.VertexShader = VertexRHI;
					Initializer.BoundShaders.FragmentShader = FragmentRHI;
					Initializer.VertexDeclaration = FullscreenGeometry.GetVertexDeclaration_RenderThread();
					Initializer.RasterizerState.CullMode = ERHICullMode::None;
					Initializer.PipelineLayout = Candidate.ShaderMap->GetMergedPipelineLayout();
					Candidate.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState(
						"VolumetricCloudFragmentPipeline", Initializer
					);
					if (!Candidate.PipelineState)
						return FFragmentResult::Failure(MakeFailure("VolumetricCloudFragment", "pipeline", "Graphics pipeline creation returned null.", ERenderResourceCreateErrorCategory::GraphicsPipeline));
					return FFragmentResult::Success(std::move(Candidate));
				},
				ReportRendererResourceCreateDiagnostic
			);
		}
		RouteInputs.bFragmentPayloadReady = FragmentPayload != nullptr
											&& FullscreenGeometry.GetVertexBuffer_RenderThread() != nullptr
											&& FullscreenGeometry.GetIndexBuffer_RenderThread() != nullptr;
		Decision = FSpatial::SelectRoute(RouteInputs);
		const uint64 Pixels = bBaseInputsValid ? static_cast<uint64>(CloudView.ViewportWidth) * CloudView.ViewportHeight : 0;
		FExecutionCounters Counters = FSpatial::MakeExecutionCounters(RouteInputs, Decision, Pixels * Parameters.PrimarySampleCount, Pixels * Parameters.PrimarySampleCount * Parameters.LightSampleCount);
		auto DescribeRoute = [&](FExecutionCounters& Value) {
			Value.TargetWidth = Input.Width;
			Value.TargetHeight = Input.Height;
			Value.OutputWidth = OutputWidth;
			Value.OutputHeight = OutputHeight;
			Value.QualityTier = Input.QualityTier;
		};
		DescribeRoute(Counters);
		if (Decision.Route == ERoute::Disabled)
			return {.Counters = Counters};
		if (Policy.bPreparationOnly)
			return {.Counters = Counters};

		FMatrix InverseViewProjection;
		if (!Math::TryInverse(View->ViewProjectionMatrix, InverseViewProjection, 1.0e-8))
		{
			RouteInputs.bRequiredInputsValid = false;
			Decision = FSpatial::SelectRoute(RouteInputs);
			Counters = FSpatial::MakeExecutionCounters(RouteInputs, Decision, 0, 0);
			DescribeRoute(Counters);
			return {.Counters = Counters};
		}
		FCloudUniform Uniform;
		for (uint32 Row = 0; Row < 4; ++Row)
			for (uint32 Col = 0; Col < 4; ++Col)
				Uniform.InverseViewProjection[Row * 4 + Col] =
					static_cast<float>(InverseViewProjection[Col][Row]);
		auto Copy3 = [](float* Out, const FVector3f& V) { Out[0] = V.x; Out[1] = V.y; Out[2] = V.z; };
		Uniform.Layer[0] = static_cast<float>(Parameters.MinimumZ);
		Uniform.Layer[1] = static_cast<float>(Parameters.MaximumZ);
		Uniform.Layer[2] = static_cast<float>(Parameters.MaximumDistance);
		Uniform.Layer[3] = Parameters.Extinction;
		Uniform.Density[0] = Parameters.Coverage;
		Uniform.Density[1] = Parameters.DetailErosion;
		Uniform.Density[2] = Parameters.LightExtinction;
		Uniform.Density[3] = Parameters.Ambient;
		Uniform.Sampling[0] = static_cast<float>(Parameters.PrimarySampleCount);
		Uniform.Sampling[1] = static_cast<float>(Parameters.LightSampleCount);
		Uniform.Sampling[2] = Parameters.TransmittanceCutoff;
		Uniform.Sampling[3] = View->DepthConvention == ESceneDepthConvention::ReversedZ ? 1.0f : 0.0f;
		Copy3(Uniform.BaseFrequency, Parameters.BaseFrequency);
		Copy3(Uniform.DetailFrequency, Parameters.DetailFrequency);
		Copy3(Uniform.WindOffset, Parameters.WindOffset);
		Uniform.Weather[0] = Parameters.WeatherFrequency.x;
		Uniform.Weather[1] = Parameters.WeatherFrequency.y;
		Uniform.Weather[2] = Parameters.WeatherOffset.x;
		Uniform.Weather[3] = Parameters.WeatherOffset.y;
		Copy3(Uniform.LightDirection, Math::Normalize(Parameters.LightDirection));
		Copy3(Uniform.LightColor, Parameters.LightColor);
		Copy3(Uniform.AmbientColor, Parameters.AmbientColor);
		Uniform.CameraPosition[0] = static_cast<float>(View->ViewLocation.x);
		Uniform.CameraPosition[1] = static_cast<float>(View->ViewLocation.y);
		Uniform.CameraPosition[2] = static_cast<float>(View->ViewLocation.z);
		Uniform.Viewport[0] = 1.0f / static_cast<float>(CloudView.ViewportWidth);
		Uniform.Viewport[1] = 1.0f / static_cast<float>(CloudView.ViewportHeight);
		Uniform.Viewport[2] = static_cast<float>(CloudView.ViewportX);
		Uniform.Viewport[3] = static_cast<float>(CloudView.ViewportY);
		Uniform.OutputViewport[0] = static_cast<float>(View->ViewportX);
		Uniform.OutputViewport[1] = static_cast<float>(View->ViewportY);
		Uniform.OutputViewport[2] = static_cast<float>(View->ViewportWidth);
		Uniform.OutputViewport[3] = static_cast<float>(View->ViewportHeight);
		Uniform.Target[0] = static_cast<float>(Input.Width);
		Uniform.Target[1] = static_cast<float>(Input.Height);
		Uniform.Target[2] = static_cast<float>(OutputWidth);
		Uniform.Target[3] = static_cast<float>(OutputHeight);
		const FVector2f Jitter = FSpatial::CalculateJitter(
			Input.SuccessfulSequence, Quality
		);
		Uniform.Jitter[0] = Jitter.x;
		Uniform.Jitter[1] = Jitter.y;
		const FRHIUniformBufferRange UniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
		if (UniformBuffer.Buffer == nullptr || UniformBuffer.Size != sizeof(Uniform))
		{
			RouteInputs.bRequiredInputsValid = false;
			Decision = FSpatial::SelectRoute(RouteInputs);
			Counters = FSpatial::MakeExecutionCounters(RouteInputs, Decision, 0, 0);
			DescribeRoute(Counters);
			return {.Counters = Counters};
		}
		FGPUTimingQueryRHIRef TimingQuery;
		const FTimingQuerySink TimingSink = GVolumetricCloudTimingQuerySink.load(std::memory_order_acquire);
		if (TimingSink != nullptr && GDynamicRHI != nullptr)
		{
			TimingQuery = GDynamicRHI->RHICreateGPUTimingQuery();
			if (TimingQuery) CommandList.BeginGPUTimingQuery(TimingQuery);
		}
		FRHITexture* Cloud = nullptr;
		if (Decision.Route == ERoute::Compute)
		{
			const std::array BufferToCompute{FRHIBufferTransition{UniformBuffer.Buffer, UniformBuffer.Offset, UniformBuffer.Size, ERHIAccess::Discard, ERHIAccess::ComputeUniformRead}};
			CommandList.TransitionBuffers(BufferToCompute);
			const std::array InputsToCompute{
				FRHITextureTransition::Whole(Input.Textures.BaseDensity, ERHIAccess::GraphicsShaderRead, ERHIAccess::ComputeShaderRead),
				FRHITextureTransition::Whole(Input.Textures.DetailDensity, ERHIAccess::GraphicsShaderRead, ERHIAccess::ComputeShaderRead),
				FRHITextureTransition::Whole(Weather, ERHIAccess::GraphicsShaderRead, ERHIAccess::ComputeShaderRead),
				FRHITextureTransition::Whole(Input.Textures.SceneDepth, ERHIAccess::GraphicsShaderRead, ERHIAccess::ComputeShaderRead)
			};
			if (!Policy.bGraphManagedTextureAccess)
				CommandList.TransitionTextures(InputsToCompute);
			const std::array OutputToCompute{FRHITextureTransition::Whole(ComputeTargets->Cloud, ERHIAccess::Discard, ERHIAccess::ComputeShaderReadWrite)};
			if (!Policy.bGraphManagedTextureAccess)
				CommandList.TransitionTextures(OutputToCompute);
			CommandList.SwitchPipeline(ERHIPipeline::Compute);
			CommandList.SetComputePipelineState(*ComputePayload->PipelineState);
			FCloudComputeShader::FParameters Params;
			Params.BaseDensity = Input.Textures.BaseDensity;
			Params.DetailDensity = Input.Textures.DetailDensity;
			Params.WeatherTexture = Weather;
			Params.SceneDepth = Input.Textures.SceneDepth;
			Params.DensitySampler = Input.Textures.DensitySampler;
			Params.Params = UniformBuffer;
			Params.CloudOutput = ComputeTargets->Cloud;
			SetShaderParameters(CommandList, ComputePayload->ComputeShader, Params);
			CommandList.Dispatch(Counters.GroupCountX, Counters.GroupCountY, 1);
			const std::array BufferRestore{FRHIBufferTransition{UniformBuffer.Buffer, UniformBuffer.Offset, UniformBuffer.Size, ERHIAccess::ComputeUniformRead, ERHIAccess::GraphicsUniformRead}};
			CommandList.TransitionBuffers(BufferRestore);
			const std::array TextureRestore{
				FRHITextureTransition::Whole(ComputeTargets->Cloud, ERHIAccess::ComputeShaderReadWrite, ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Input.Textures.BaseDensity, ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Input.Textures.DetailDensity, ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Weather, ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Input.Textures.SceneDepth, ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead)
			};
			if (!Policy.bGraphManagedTextureAccess)
				CommandList.TransitionTextures(TextureRestore);
			CommandList.SwitchPipeline(ERHIPipeline::Graphics);
			Cloud = ComputeTargets->Cloud;
		}
		else
		{
			FRHIRenderPassInfo PassInfo{};
			PassInfo.RenderTargetLayout = RenderTargetLayouts::MakeVolumetricCloudOutput();
			PassInfo.ColorRenderTargets[0] = FragmentTargets->Cloud;
			PassInfo.ColorClearValues[0] = FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
			CommandList.BeginRenderPass(PassInfo, "VolumetricCloudRenderPass");
			CommandList.SetGraphicsPipelineState(*FragmentPayload->PipelineState);
			CommandList.SetViewport(static_cast<float>(CloudView.ViewportX), static_cast<float>(CloudView.ViewportY), 0.0f, static_cast<float>(CloudView.ViewportX + CloudView.ViewportWidth), static_cast<float>(CloudView.ViewportY + CloudView.ViewportHeight), 1.0f);
			CommandList.SetScissor(static_cast<float>(CloudView.ViewportX), static_cast<float>(CloudView.ViewportY), static_cast<float>(CloudView.ViewportWidth), static_cast<float>(CloudView.ViewportHeight));
			CommandList.BindVertexBuffer(0, FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
			CommandList.BindIndexBuffer(FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
			FCloudFragmentShader::FParameters Params;
			Params.BaseDensity = Input.Textures.BaseDensity;
			Params.DetailDensity = Input.Textures.DetailDensity;
			Params.WeatherTexture = Weather;
			Params.SceneDepth = Input.Textures.SceneDepth;
			Params.DensitySampler = Input.Textures.DensitySampler;
			Params.Params = UniformBuffer;
			SetShaderParameters(CommandList, FragmentPayload->FragmentShader, Params);
			CommandList.DrawIndexed(3, 0, 0);
			CommandList.EndRenderPass();
			Cloud = FragmentTargets->Cloud;
		}
		if (TimingQuery)
		{
			CommandList.EndGPUTimingQuery(TimingQuery);
			TimingSink(TimingQuery, Decision.Route);
		}
		const FCaptureSink CaptureSink = GVolumetricCloudCaptureSink.load(std::memory_order_acquire);
		if (CaptureSink != nullptr) CaptureSink(Cloud, Decision.Route);
		return {.Cloud = Cloud, .Counters = Counters};
	}

	auto FVolumetricCloudRenderer::ReconstructTemporal_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FTemporalReconstructionInput& Input
	)
		-> FTemporalReconstructionResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		FTemporalReconstructionResult Result{.Cloud = Input.CurrentCloud};
		if (Input.CurrentCloud == nullptr || Input.View == nullptr
			|| Input.TemporalContext == nullptr || Input.ViewState == nullptr)
		{
			return Result;
		}
		auto& History = Input.ViewState->GetVolumetricCloudHistory();
		const FSpatial::FQualityPolicy Quality =
			FSpatial::ResolveQualityPolicy(Input.QualityTier);
		const uint64 PolicyKey = FSpatial::CalculatePolicyKey(Input.QualityTier);
		if (Quality.IsFullResolution() || Quality.HistoryWeight <= 0.0f)
		{
			History.SetPendingClear(PolicyKey, Input.CloudHistoryKey);
			Result.bCandidatePublished = true;
			Result.HistoryBytes = History.GetRetainedBytes();
			return Result;
		}

		const uint32 Width = Input.CurrentCloud->GetSizeX();
		const uint32 Height = Input.CurrentCloud->GetSizeY();
		const bool bHistoryAccepted = Input.TemporalContext->bHistoryValid
									  && History.CanReproject(
										  PolicyKey, Input.CloudHistoryKey, Width, Height
									  );
		FTextureRHIRef Candidate = History.TakeReusable(Width, Height);
		if (Candidate == nullptr)
		{
			Candidate = RHICreateTexture(FRHITextureCreateDesc::Create2D(
											 "VolumetricCloudTemporalHistory", Width, Height,
											 EPixelFormat::RGBA16_FLOAT
			)
											 .SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource)
											 .SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f)));
		}
		if (Candidate == nullptr)
			return Result;

		using FPayload = FState::FTemporalPayload;
		using FCreateResult = TRenderResourceCreateResult<FPayload>;
		FPayload* Payload = State->TemporalResources.Resolve(
			Coordinator.GetGeneration_RenderThread(),
			[this, &CommandList]() -> FCreateResult {
				FShaderCompileOptions Options;
				Options.bForceRecompile =
					Coordinator.ShouldForceShaderRecompile_RenderThread();
				FShaderType& VertexType = FCloudTemporalVertexShader::StaticType();
				FShaderType& FragmentType = FCloudTemporalFragmentShader::StaticType();
				const std::array<const FShaderType*, 2> Types{
					&VertexType, &FragmentType
				};
				FPayload CandidatePayload;
				CandidatePayload.ShaderMap = std::make_shared<FShaderMapBase>();
				std::string Error;
				if (!CandidatePayload.ShaderMap->InitializeFromShaderTypes(
						Types, Options, Error
					))
				{
					return FCreateResult::Failure(MakeFailure(
						"VolumetricCloudTemporal", "shader", std::move(Error),
						ERenderResourceCreateErrorCategory::ShaderCompile
					));
				}
				auto* Vertex = static_cast<FCloudTemporalVertexShader*>(
					CandidatePayload.ShaderMap->GetShader(&VertexType)
				);
				auto* Fragment = static_cast<FCloudTemporalFragmentShader*>(
					CandidatePayload.ShaderMap->GetShader(&FragmentType)
				);
				if (Vertex == nullptr || Fragment == nullptr)
				{
					return FCreateResult::Failure(MakeFailure(
						"VolumetricCloudTemporal", "shader",
						"Compiled shader map is missing a typed shader.",
						ERenderResourceCreateErrorCategory::ShaderBinding
					));
				}
				CandidatePayload.VertexShader = {
					Vertex, CandidatePayload.ShaderMap.get()
				};
				CandidatePayload.FragmentShader = {
					Fragment, CandidatePayload.ShaderMap.get()
				};
				if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList))
				{
					return FCreateResult::Failure(MakeFailure(
						"VolumetricCloudTemporal", "fullscreen-geometry",
						"Shared fullscreen geometry is unavailable.",
						ERenderResourceCreateErrorCategory::RHIResource
					));
				}
				FRHIShader* VertexRHI =
					CandidatePayload.VertexShader.GetRHIShader(false);
				FRHIShader* FragmentRHI =
					CandidatePayload.FragmentShader.GetRHIShader(false);
				if (VertexRHI == nullptr || FragmentRHI == nullptr
					|| GDynamicRHI == nullptr)
				{
					return FCreateResult::Failure(MakeFailure(
						"VolumetricCloudTemporal", "pipeline",
						"RHI shader creation returned null.",
						ERenderResourceCreateErrorCategory::RHIResource
					));
				}
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeVolumetricCloudOutput();
				Initializer.BoundShaders.VertexShader = VertexRHI;
				Initializer.BoundShaders.FragmentShader = FragmentRHI;
				Initializer.VertexDeclaration =
					FullscreenGeometry.GetVertexDeclaration_RenderThread();
				Initializer.RasterizerState.CullMode = ERHICullMode::None;
				Initializer.PipelineLayout =
					CandidatePayload.ShaderMap->GetMergedPipelineLayout();
				CandidatePayload.PipelineState =
					GDynamicRHI->RHICreateGraphicsPipelineState(
						"VolumetricCloudTemporalPipeline", Initializer
					);
				if (!CandidatePayload.PipelineState)
				{
					return FCreateResult::Failure(MakeFailure(
						"VolumetricCloudTemporal", "pipeline",
						"Graphics pipeline creation returned null.",
						ERenderResourceCreateErrorCategory::GraphicsPipeline
					));
				}
				return FCreateResult::Success(std::move(CandidatePayload));
			},
			ReportRendererResourceCreateDiagnostic
		);
		FRHISampler* Sampler = EnsureDensitySampler_RenderThread();
		if (Payload == nullptr || Sampler == nullptr)
			return Result;

		FMatrix InverseViewProjection;
		if (!Math::TryInverse(Input.View->ViewProjectionMatrix, InverseViewProjection, 1.0e-8))
		{
			return Result;
		}
		FCloudTemporalUniform Uniform;
		auto CopyMatrix = [](float* Out, const FMatrix& Matrix) {
			for (uint32 Row = 0; Row < 4; ++Row)
				for (uint32 Column = 0; Column < 4; ++Column)
					Out[Row * 4 + Column] =
						static_cast<float>(Matrix[Column][Row]);
		};
		CopyMatrix(Uniform.InverseViewProjection, InverseViewProjection);
		CopyMatrix(Uniform.PreviousViewProjection, Input.TemporalContext->Previous.ViewProjectionMatrix);
		Uniform.Layer[0] = static_cast<float>(Input.Parameters.MinimumZ);
		Uniform.Layer[1] = static_cast<float>(Input.Parameters.MaximumZ);
		Uniform.Layer[2] = static_cast<float>(Input.Parameters.MaximumDistance);
		Uniform.Layer[3] = Input.View->DepthConvention
								   == ESceneDepthConvention::ReversedZ ?
							   1.0f :
							   0.0f;
		Uniform.CameraPosition[0] = static_cast<float>(Input.View->ViewLocation.x);
		Uniform.CameraPosition[1] = static_cast<float>(Input.View->ViewLocation.y);
		Uniform.CameraPosition[2] = static_cast<float>(Input.View->ViewLocation.z);
		const auto Viewport = FSpatial::CalculateScaledViewport(
			{Input.View->ViewportX, Input.View->ViewportY,
			 Input.View->ViewportWidth, Input.View->ViewportHeight},
			{Input.TemporalContext->Current.OutputWidth,
			 Input.TemporalContext->Current.OutputHeight},
			{Width, Height}
		);
		if (Viewport.Width == 0 || Viewport.Height == 0)
			return Result;
		Uniform.Viewport[0] = static_cast<float>(Viewport.X);
		Uniform.Viewport[1] = static_cast<float>(Viewport.Y);
		Uniform.Viewport[2] = static_cast<float>(Viewport.Width);
		Uniform.Viewport[3] = static_cast<float>(Viewport.Height);
		Uniform.Target[0] = static_cast<float>(Width);
		Uniform.Target[1] = static_cast<float>(Height);
		Uniform.Target[2] = Quality.HistoryWeight;
		Uniform.Target[3] = bHistoryAccepted ? 1.0f : 0.0f;
		const FRHIUniformBufferRange UniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
		if (UniformBuffer.Buffer == nullptr
			|| UniformBuffer.Size != sizeof(Uniform))
		{
			return Result;
		}

		FRHIRenderPassInfo PassInfo{};
		PassInfo.RenderTargetLayout = RenderTargetLayouts::MakeVolumetricCloudOutput();
		PassInfo.ColorRenderTargets[0] = Candidate;
		PassInfo.ColorClearValues[0] =
			FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
		CommandList.BeginRenderPass(PassInfo, "VolumetricCloudTemporalRenderPass");
		CommandList.SetGraphicsPipelineState(*Payload->PipelineState);
		CommandList.SetViewport(0.0f, 0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height), 1.0f);
		CommandList.SetScissor(0.0f, 0.0f, static_cast<float>(Width), static_cast<float>(Height));
		CommandList.BindVertexBuffer(
			0, FullscreenGeometry.GetVertexBuffer_RenderThread(), 0
		);
		CommandList.BindIndexBuffer(
			FullscreenGeometry.GetIndexBuffer_RenderThread(), 0
		);
		FCloudTemporalFragmentShader::FParameters Params;
		Params.CurrentCloud = Input.CurrentCloud;
		Params.PreviousCloud = bHistoryAccepted ? History.CommittedTexture.GetReference() : Input.CurrentCloud;
		Params.HistorySampler = Sampler;
		Params.Params = UniformBuffer;
		SetShaderParameters(CommandList, Payload->FragmentShader, Params);
		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();

		History.SetPending(Candidate, PolicyKey, Input.CloudHistoryKey);
		Result.Cloud = Candidate;
		Result.bHistoryAccepted = bHistoryAccepted;
		Result.bCandidatePublished = true;
		Result.HistoryBytes = History.GetRetainedBytes();
		return Result;
	}

	auto FVolumetricCloudRenderer::Composite_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FTargets& CompositeTargets, FRHITexture* SceneColor,
		FRHITexture* Cloud, FRHITexture* SceneDepth,
		FRHITexture* ShadowVisibility, bool bHistoryAvailable,
		bool bHistoryAccepted, const FSceneView& View
	)
		-> FRHITexture*
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		if (SceneColor == nullptr || Cloud == nullptr || SceneDepth == nullptr
			|| View.ViewportWidth == 0
			|| View.ViewportHeight == 0) return nullptr;
		if (!CompositeTargets.Cloud) return nullptr;
		using FPayload = FState::FCompositePayload;
		using FResult = TRenderResourceCreateResult<FPayload>;
		FPayload* Payload = State->CompositeResources.Resolve(
			Coordinator.GetGeneration_RenderThread(), [this, &CommandList]() -> FResult {
				FShaderCompileOptions Options;
				Options.bForceRecompile = Coordinator.ShouldForceShaderRecompile_RenderThread();
				FShaderType& VertexType = FCloudCompositeVertexShader::StaticType();
				FShaderType& FragmentType = FCloudCompositeFragmentShader::StaticType();
				const std::array<const FShaderType*, 2> Types{&VertexType, &FragmentType};
				FPayload Candidate;
				Candidate.ShaderMap = std::make_shared<FShaderMapBase>();
				std::string Error;
				if (!Candidate.ShaderMap->InitializeFromShaderTypes(Types, Options, Error))
					return FResult::Failure(MakeFailure("VolumetricCloudComposite", "shader", std::move(Error), ERenderResourceCreateErrorCategory::ShaderCompile));
				auto* Vertex = static_cast<FCloudCompositeVertexShader*>(Candidate.ShaderMap->GetShader(&VertexType));
				auto* Fragment = static_cast<FCloudCompositeFragmentShader*>(Candidate.ShaderMap->GetShader(&FragmentType));
				if (Vertex == nullptr || Fragment == nullptr)
					return FResult::Failure(MakeFailure("VolumetricCloudComposite", "shader", "Compiled shader map is missing a typed shader.", ERenderResourceCreateErrorCategory::ShaderBinding));
				Candidate.VertexShader = {Vertex, Candidate.ShaderMap.get()};
				Candidate.FragmentShader = {Fragment, Candidate.ShaderMap.get()};
				if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList))
					return FResult::Failure(MakeFailure("VolumetricCloudComposite", "fullscreen-geometry", "Shared fullscreen geometry is unavailable.", ERenderResourceCreateErrorCategory::RHIResource));
				FRHIShader* VertexRHI = Candidate.VertexShader.GetRHIShader(false);
				FRHIShader* FragmentRHI = Candidate.FragmentShader.GetRHIShader(false);
				if (VertexRHI == nullptr || FragmentRHI == nullptr || GDynamicRHI == nullptr)
					return FResult::Failure(MakeFailure("VolumetricCloudComposite", "pipeline", "RHI shader creation returned null.", ERenderResourceCreateErrorCategory::RHIResource));
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeVolumetricCloudComposite();
				Initializer.BoundShaders.VertexShader = VertexRHI;
				Initializer.BoundShaders.FragmentShader = FragmentRHI;
				Initializer.VertexDeclaration = FullscreenGeometry.GetVertexDeclaration_RenderThread();
				Initializer.RasterizerState.CullMode = ERHICullMode::None;
				Initializer.PipelineLayout = Candidate.ShaderMap->GetMergedPipelineLayout();
				Candidate.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState(
					"VolumetricCloudCompositePipeline", Initializer
				);
				if (!Candidate.PipelineState)
					return FResult::Failure(MakeFailure("VolumetricCloudComposite", "pipeline", "Graphics pipeline creation returned null.", ERenderResourceCreateErrorCategory::GraphicsPipeline));
				return FResult::Success(std::move(Candidate));
			},
			ReportRendererResourceCreateDiagnostic
		);
		if (Payload == nullptr) return nullptr;
		FCloudCompositeUniform Uniform;
		Uniform.Extent[0] = static_cast<float>(SceneColor->GetSizeX());
		Uniform.Extent[1] = static_cast<float>(SceneColor->GetSizeY());
		Uniform.Extent[2] = static_cast<float>(Cloud->GetSizeX());
		Uniform.Extent[3] = static_cast<float>(Cloud->GetSizeY());
		Uniform.Viewport[0] = static_cast<float>(View.ViewportX);
		Uniform.Viewport[1] = static_cast<float>(View.ViewportY);
		Uniform.Viewport[2] = static_cast<float>(View.ViewportWidth);
		Uniform.Viewport[3] = static_cast<float>(View.ViewportHeight);
		const EVolumetricCloudDebugMode DebugMode =
			View.Settings.VolumetricCloud.DebugMode < EVolumetricCloudDebugMode::Count
				? View.Settings.VolumetricCloud.DebugMode
				: EVolumetricCloudDebugMode::Lit;
		Uniform.Debug[0] = static_cast<float>(DebugMode);
		Uniform.Debug[1] = !bHistoryAvailable ? -1.0f
			: bHistoryAccepted ? 1.0f : 0.0f;
		Uniform.Debug[2] = ShadowVisibility != nullptr ? 1.0f : 0.0f;
		const FRHIUniformBufferRange UniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
		if (UniformBuffer.Buffer == nullptr || UniformBuffer.Size != sizeof(Uniform))
			return nullptr;
		FRHIRenderPassInfo PassInfo{};
		PassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeVolumetricCloudComposite();
		PassInfo.ColorRenderTargets[0] = CompositeTargets.Cloud;
		PassInfo.ColorClearValues[0] = FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
		CommandList.BeginRenderPass(PassInfo, "VolumetricCloudCompositeRenderPass");
		CommandList.SetGraphicsPipelineState(*Payload->PipelineState);
		CommandList.SetViewport(0.0f, 0.0f, 0.0f, static_cast<float>(CompositeTargets.Cloud->GetSizeX()), static_cast<float>(CompositeTargets.Cloud->GetSizeY()), 1.0f);
		CommandList.SetScissor(0.0f, 0.0f, static_cast<float>(CompositeTargets.Cloud->GetSizeX()), static_cast<float>(CompositeTargets.Cloud->GetSizeY()));
		CommandList.BindVertexBuffer(0, FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
		CommandList.BindIndexBuffer(FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
		FCloudCompositeFragmentShader::FParameters Params;
		Params.SceneColorTexture = SceneColor;
		Params.CloudTexture = Cloud;
		Params.SceneDepthTexture = SceneDepth;
		Params.DebugTexture = ShadowVisibility != nullptr ? ShadowVisibility : SceneDepth;
		Params.Params = UniformBuffer;
		SetShaderParameters(CommandList, Payload->FragmentShader, Params);
		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();
		return CompositeTargets.Cloud;
	}

	auto FVolumetricCloudRenderer::GetRetainedTargetBytes_RenderThread() const -> uint64
	{
		check(IsInRenderingThread());
		const uint64 Fragment = TransientTargets.GetRetainedBytes_RenderThread(
			ERendererTransientTargetGroup::VolumetricCloudFragment);
		const uint64 Compute = TransientTargets.GetRetainedBytes_RenderThread(
			ERendererTransientTargetGroup::VolumetricCloudCompute);
		const uint64 Composite = TransientTargets.GetRetainedBytes_RenderThread(
			ERendererTransientTargetGroup::VolumetricCloudComposite);
		const uint64 RouteBytes = Compute > std::numeric_limits<uint64>::max() - Fragment ? std::numeric_limits<uint64>::max() : Fragment + Compute;
		return Composite > std::numeric_limits<uint64>::max() - RouteBytes ? std::numeric_limits<uint64>::max() : RouteBytes + Composite;
	}

	auto FVolumetricCloudRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->FragmentResources.Reset();
		State->ComputeResources.Reset();
		State->CompositeResources.Reset();
		State->TemporalResources.Reset();
		State->FallbackResources.Reset();
		State->DensitySamplerResources.Reset();
	}
} // namespace Durin
