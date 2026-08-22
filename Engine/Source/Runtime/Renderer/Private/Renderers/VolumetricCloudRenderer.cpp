#include "Renderers/VolumetricCloudRenderer.h"

#include "RendererResourceSlotCache.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Math/Operations.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SceneView.h"
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
			DURIN_DECLARE_SHADER(FCloudVertexShader, FShader,
				"/Engine/VolumetricCloud", EShaderFrequency::Vertex, "VertexMain");
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
			DURIN_DECLARE_SHADER(FCloudFragmentShader, FShader,
				"/Engine/VolumetricCloud", EShaderFrequency::Fragment,
				"CloudFragmentMain");
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
			DURIN_DECLARE_SHADER(FCloudComputeShader, FShader,
				"/Engine/VolumetricCloud", EShaderFrequency::Compute,
				"CloudComputeMain");
		};

		class FCloudCompositeVertexShader final : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FCloudCompositeVertexShader, FShader,
				"/Engine/VolumetricCloudComposite", EShaderFrequency::Vertex,
				"VertexMain");
		};

		class FCloudCompositeFragmentShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FCloudCompositeFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(SceneColorTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(CloudTexture);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FCloudCompositeFragmentShader, FShader,
				"/Engine/VolumetricCloudComposite", EShaderFrequency::Fragment,
				"FragmentMain");
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
			float CameraPosition[4]{};
			float Viewport[4]{};
		};
		static_assert(sizeof(FCloudUniform) == 240);

		auto MakeFailure(const char* Resource, const char* Key,
			std::string Message, ERenderResourceCreateErrorCategory Category)
			-> FRenderResourceCreateError
		{
			return MakeRendererResourceCreateError(Category, Resource, Key,
				std::move(Message), ERenderResourceGenerationDependency::Shader
					| ERenderResourceGenerationDependency::Device
					| ERenderResourceGenerationDependency::Manual);
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
		struct FFallbackPayload { FTextureRHIRef WhiteWeather; };
		struct FDensitySamplerPayload { FSamplerRHIRef Sampler; };

		TRenderResourceCreationSlot<FFragmentPayload> FragmentResources{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
		TRenderResourceCreationSlot<FComputePayload> ComputeResources{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
		TRenderResourceCreationSlot<FCompositePayload> CompositeResources{
			ERenderResourceGenerationDependency::Shader
				| ERenderResourceGenerationDependency::Device};
		TRenderResourceCreationSlot<FFallbackPayload> FallbackResources{
			ERenderResourceGenerationDependency::Device};
		TRenderResourceCreationSlot<FDensitySamplerPayload> DensitySamplerResources{
			ERenderResourceGenerationDependency::Device};
		TRendererResourceSlotCache<uint64, FTargets> TargetsBySize{
			ERenderResourceGenerationDependency::Device};
		TRendererResourceSlotCache<uint64, FComputeTargets> ComputeTargetsBySize{
			ERenderResourceGenerationDependency::Device};
		TRendererResourceSlotCache<uint64, FTargets> CompositeTargetsBySize{
			ERenderResourceGenerationDependency::Device};
	};

	FVolumetricCloudRenderer::FVolumetricCloudRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry)
		: Coordinator(InCoordinator), FullscreenGeometry(InFullscreenGeometry),
		  State(std::make_unique<FState>()) {}

	FVolumetricCloudRenderer::~FVolumetricCloudRenderer() = default;

	auto FVolumetricCloudRenderer::EnsureDensitySampler_RenderThread()
		-> FRHISampler*
	{
		check(IsInRenderingThread());
		using FResult = TRenderResourceCreateResult<FState::FDensitySamplerPayload>;
		auto* Payload = State->DensitySamplerResources.Resolve(
			Coordinator.GetGeneration_RenderThread(), []() -> FResult {
				FState::FDensitySamplerPayload Candidate{
					.Sampler = RHICreateSampler(FRHISamplerDesc::LinearClamp())};
				if (!Candidate.Sampler)
					return FResult::Failure(MakeFailure(
						"VolumetricCloud", "density-sampler",
						"Density sampler creation returned null.",
						ERenderResourceCreateErrorCategory::RHIResource));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
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
		uint32 Width, uint32 Height) -> FTargets*
	{
		check(IsInRenderingThread());
		const uint64 Bytes = FSpatial::CalculateTargetBytes(Width, Height);
		if (Width == 0 || Height == 0
			|| Bytes > FSpatial::MaximumRetainedTargetBytes / 3) return nullptr;
		const uint64 Key = (static_cast<uint64>(Width) << 32) | Height;
		const auto Desc = FRHITextureCreateDesc::Create2D(
			"VolumetricCloudFragment", Width, Height, EPixelFormat::RGBA16_FLOAT)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy
				| ETextureCreateFlags::CPUReadback)
			.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f));
		using FResult = TRenderResourceCreateResult<FTargets>;
		auto& Entry = State->TargetsBySize.FindOrAdd(Key);
		FTargets* Result = Entry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(), [Key, &Desc]() -> FResult {
				FTargets Candidate{.Cloud = RHICreateTexture(Desc)};
				if (!Candidate.Cloud)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"VolumetricCloudFragmentTarget", std::to_string(Key),
						"RGBA16_FLOAT target creation returned null.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		while (State->TargetsBySize.Num() > 1
			&& State->TargetsBySize.GetRetainedPayloadWeight(
				[](uint64 K, const FTargets&) { return FSpatial::CalculateTargetBytes(
					static_cast<uint32>(K >> 32), static_cast<uint32>(K)); })
				> FSpatial::MaximumRetainedTargetBytes / 3)
			if (!State->TargetsBySize.EvictOldestExcept(Key)) break;
		if (!Result) return nullptr;
		auto* Retained = State->TargetsBySize.Find(Key);
		return Retained ? Retained->Slot.GetPayload() : nullptr;
	}

	auto FVolumetricCloudRenderer::EnsureComputeTargets_RenderThread(
		uint32 Width, uint32 Height) -> FComputeTargets*
	{
		check(IsInRenderingThread());
		const uint64 Bytes = FSpatial::CalculateTargetBytes(Width, Height);
		if (Width == 0 || Height == 0 || GDynamicRHI == nullptr
			|| Bytes > FSpatial::MaximumRetainedTargetBytes / 3) return nullptr;
		const uint64 Key = (static_cast<uint64>(Width) << 32) | Height;
		const auto Desc = FRHITextureCreateDesc::Create2D(
			"VolumetricCloudCompute", Width, Height, EPixelFormat::RGBA16_FLOAT)
			.SetFlags(ETextureCreateFlags::Storage | ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback);
		using FResult = TRenderResourceCreateResult<FComputeTargets>;
		auto& Entry = State->ComputeTargetsBySize.FindOrAdd(Key);
		FComputeTargets* Result = Entry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(), [Key, &Desc]() -> FResult {
				if (!GDynamicRHI->RHIIsTextureSupported(Desc))
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"VolumetricCloudComputeTarget", std::to_string(Key),
						"RGBA16_FLOAT sampled/storage target is unsupported.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				FComputeTargets Candidate;
				Candidate.Cloud = RHICreateTexture(Desc);
				if (Candidate.Cloud)
				{
					Candidate.SampledView = GDynamicRHI->RHIGetOrCreateTextureView(
						Candidate.Cloud, MakeDefaultTextureViewDesc(*Candidate.Cloud,
							ERHITextureViewUsage::Sampled));
					Candidate.StorageView = GDynamicRHI->RHIGetOrCreateTextureView(
						Candidate.Cloud, MakeDefaultTextureViewDesc(*Candidate.Cloud,
							ERHITextureViewUsage::Storage));
				}
				if (!Candidate.Cloud || !Candidate.SampledView || !Candidate.StorageView)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"VolumetricCloudComputeTarget", std::to_string(Key),
						"Target or canonical view creation returned null.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		while (State->ComputeTargetsBySize.Num() > 1
			&& State->ComputeTargetsBySize.GetRetainedPayloadWeight(
				[](uint64 K, const FComputeTargets&) { return FSpatial::CalculateTargetBytes(
					static_cast<uint32>(K >> 32), static_cast<uint32>(K)); })
				> FSpatial::MaximumRetainedTargetBytes / 3)
			if (!State->ComputeTargetsBySize.EvictOldestExcept(Key)) break;
		if (!Result) return nullptr;
		auto* Retained = State->ComputeTargetsBySize.Find(Key);
		return Retained ? Retained->Slot.GetPayload() : nullptr;
	}

	auto FVolumetricCloudRenderer::EnsureCompositeTargets_RenderThread(
		uint32 Width, uint32 Height) -> FTargets*
	{
		check(IsInRenderingThread());
		const uint64 Bytes = FSpatial::CalculateTargetBytes(Width, Height);
		if (Width == 0 || Height == 0
			|| Bytes > FSpatial::MaximumRetainedTargetBytes / 3) return nullptr;
		const uint64 Key = (static_cast<uint64>(Width) << 32) | Height;
		const auto Desc = FRHITextureCreateDesc::Create2D(
			"VolumetricCloudComposite", Width, Height, EPixelFormat::RGBA16_FLOAT)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::SourceCopy
				| ETextureCreateFlags::CPUReadback)
			.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f));
		using FResult = TRenderResourceCreateResult<FTargets>;
		auto& Entry = State->CompositeTargetsBySize.FindOrAdd(Key);
		FTargets* Result = Entry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(), [Key, &Desc]() -> FResult {
				FTargets Candidate{.Cloud = RHICreateTexture(Desc)};
				if (!Candidate.Cloud)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"VolumetricCloudCompositeTarget", std::to_string(Key),
						"RGBA16_FLOAT target creation returned null.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		while (State->CompositeTargetsBySize.Num() > 1
			&& State->CompositeTargetsBySize.GetRetainedPayloadWeight(
				[](uint64 K, const FTargets&) {
					return FSpatial::CalculateTargetBytes(
						static_cast<uint32>(K >> 32), static_cast<uint32>(K));
				}) > FSpatial::MaximumRetainedTargetBytes / 3)
			if (!State->CompositeTargetsBySize.EvictOldestExcept(Key)) break;
		if (!Result) return nullptr;
		auto* Retained = State->CompositeTargetsBySize.Find(Key);
		return Retained ? Retained->Slot.GetPayload() : nullptr;
	}

	// Resource resolution, binding, and execution continue below to keep target
	// creation independently testable.
	auto FVolumetricCloudRenderer::Render_RenderThread(
		FRHICommandListImmediate& CommandList, FTargets* FragmentTargets,
		FComputeTargets* ComputeTargets, const FRenderInput& Input) -> FRenderResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const FSceneView* View = Input.View;
		const bool bViewFits = View != nullptr && View->ViewportX <= Input.Width
			&& View->ViewportY <= Input.Height
			&& View->ViewportWidth <= Input.Width - View->ViewportX
			&& View->ViewportHeight <= Input.Height - View->ViewportY;
		const bool bBaseInputsValid = Input.Textures.HasRequiredInputs()
			&& Input.Parameters.IsValid() && bViewFits
			&& View->ViewportWidth != 0 && View->ViewportHeight != 0;

		using FFallbackResult = TRenderResourceCreateResult<FState::FFallbackPayload>;
		FState::FFallbackPayload* Fallback = nullptr;
		if (Input.bRequested && bBaseInputsValid && Input.Textures.Weather == nullptr)
		{
			Fallback = State->FallbackResources.Resolve(
				Coordinator.GetGeneration_RenderThread(), [&CommandList]() -> FFallbackResult {
					FState::FFallbackPayload Candidate;
					const auto Desc = FRHITextureCreateDesc::Create2D(
						"VolumetricCloudWhiteWeather", 1, 1, EPixelFormat::R8_UNORM)
						.SetFlags(ETextureCreateFlags::ShaderResource);
					Candidate.WhiteWeather = GDynamicRHI != nullptr
						? GDynamicRHI->RHICreateTexture(CommandList, Desc) : nullptr;
					if (Candidate.WhiteWeather)
					{
						const uint8 White = 255;
						GDynamicRHI->RHIUpdateTexture2D(CommandList, Candidate.WhiteWeather,
							0, 0, FUpdateTextureRegion2D(0, 0, 0, 0, 1, 1), 1, &White);
					}
					if (!Candidate.WhiteWeather)
						return FFallbackResult::Failure(MakeFailure(
							"VolumetricCloud", "white-weather",
							"Optional white weather texture creation returned null.",
							ERenderResourceCreateErrorCategory::RHIResource));
					return FFallbackResult::Success(std::move(Candidate));
				}, ReportRendererResourceCreateDiagnostic);
		}
		FRHITexture* Weather = Input.Textures.Weather != nullptr
			? Input.Textures.Weather
			: (Fallback != nullptr ? Fallback->WhiteWeather.GetReference() : nullptr);
		const bool bInputsValid = bBaseInputsValid && Weather != nullptr;

		using FComputePayload = FState::FComputePayload;
		using FComputeResult = TRenderResourceCreateResult<FComputePayload>;
		FComputePayload* ComputePayload = nullptr;
		if (Input.bRequested && bInputsValid && Input.Width != 0 && Input.Height != 0)
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
						return FComputeResult::Failure(MakeFailure("VolumetricCloudCompute",
							"shader", std::move(Error), ERenderResourceCreateErrorCategory::ShaderCompile));
					auto* Shader = static_cast<FCloudComputeShader*>(
						Candidate.ShaderMap->GetShader(&Type));
					if (Shader == nullptr)
						return FComputeResult::Failure(MakeFailure("VolumetricCloudCompute",
							"shader", "Compiled shader map is missing the typed compute shader.",
							ERenderResourceCreateErrorCategory::ShaderBinding));
					Candidate.ComputeShader = {Shader, Candidate.ShaderMap.get()};
					FRHIShader* RHIShader = Candidate.ComputeShader.GetRHIShader(false);
					if (RHIShader == nullptr || GDynamicRHI == nullptr)
						return FComputeResult::Failure(MakeFailure("VolumetricCloudCompute",
							"pipeline", "Compute shader RHI creation returned null.",
							ERenderResourceCreateErrorCategory::RHIResource));
					FComputePipelineStateInitializer Initializer;
					Initializer.ComputeShader = RHIShader;
					Initializer.PipelineLayout = Candidate.ShaderMap->GetMergedPipelineLayout();
					Candidate.PipelineState = GDynamicRHI->RHICreateComputePipelineState(
						"VolumetricCloudComputePipeline", Initializer);
					if (!Candidate.PipelineState)
						return FComputeResult::Failure(MakeFailure("VolumetricCloudCompute",
							"pipeline", "Compute pipeline creation returned null.",
							ERenderResourceCreateErrorCategory::GraphicsPipeline));
					return FComputeResult::Success(std::move(Candidate));
				}, ReportRendererResourceCreateDiagnostic);
		}
		const FRHICapabilities* Capabilities = GDynamicRHI != nullptr
			? GDynamicRHI->RHIGetCapabilities() : nullptr;
		FSpatial::FRouteInputs RouteInputs{
			.bRequested = Input.bRequested,
			.bRequiredInputsValid = bInputsValid,
			.bComputePayloadReady = ComputePayload != nullptr,
			.bComputeTargetReady = ComputeTargets != nullptr && ComputeTargets->Cloud != nullptr,
			.bFragmentPayloadReady = false,
			.bFragmentTargetReady = FragmentTargets != nullptr && FragmentTargets->Cloud != nullptr,
			.Width = Input.Width, .Height = Input.Height,
			.MaxGroupCountX = Capabilities ? Capabilities->MaxComputeWorkGroupCount[0] : 0,
			.MaxGroupCountY = Capabilities ? Capabilities->MaxComputeWorkGroupCount[1] : 0};
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
						return FFragmentResult::Failure(MakeFailure("VolumetricCloudFragment",
							"shader", std::move(Error), ERenderResourceCreateErrorCategory::ShaderCompile));
					auto* Vertex = static_cast<FCloudVertexShader*>(Candidate.ShaderMap->GetShader(&VertexType));
					auto* Fragment = static_cast<FCloudFragmentShader*>(Candidate.ShaderMap->GetShader(&FragmentType));
					if (Vertex == nullptr || Fragment == nullptr)
						return FFragmentResult::Failure(MakeFailure("VolumetricCloudFragment",
							"shader", "Compiled shader map is missing a typed shader.",
							ERenderResourceCreateErrorCategory::ShaderBinding));
					Candidate.VertexShader = {Vertex, Candidate.ShaderMap.get()};
					Candidate.FragmentShader = {Fragment, Candidate.ShaderMap.get()};
					if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList))
						return FFragmentResult::Failure(MakeFailure("VolumetricCloudFragment",
							"fullscreen-geometry", "Shared fullscreen geometry is unavailable.",
							ERenderResourceCreateErrorCategory::RHIResource));
					FRHIShader* VertexRHI = Candidate.VertexShader.GetRHIShader(false);
					FRHIShader* FragmentRHI = Candidate.FragmentShader.GetRHIShader(false);
					if (VertexRHI == nullptr || FragmentRHI == nullptr || GDynamicRHI == nullptr)
						return FFragmentResult::Failure(MakeFailure("VolumetricCloudFragment",
							"pipeline", "RHI shader creation returned null.",
							ERenderResourceCreateErrorCategory::RHIResource));
					FGraphicsPipelineStateInitializer Initializer;
					Initializer.RenderTargetLayout = RenderTargetLayouts::MakeVolumetricCloudOutput();
					Initializer.BoundShaders.VertexShader = VertexRHI;
					Initializer.BoundShaders.FragmentShader = FragmentRHI;
					Initializer.VertexDeclaration = FullscreenGeometry.GetVertexDeclaration_RenderThread();
					Initializer.RasterizerState.CullMode = ERHICullMode::None;
					Initializer.PipelineLayout = Candidate.ShaderMap->GetMergedPipelineLayout();
					Candidate.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState(
						"VolumetricCloudFragmentPipeline", Initializer);
					if (!Candidate.PipelineState)
						return FFragmentResult::Failure(MakeFailure("VolumetricCloudFragment",
							"pipeline", "Graphics pipeline creation returned null.",
							ERenderResourceCreateErrorCategory::GraphicsPipeline));
					return FFragmentResult::Success(std::move(Candidate));
				}, ReportRendererResourceCreateDiagnostic);
		}
		RouteInputs.bFragmentPayloadReady = FragmentPayload != nullptr
			&& FullscreenGeometry.GetVertexBuffer_RenderThread() != nullptr
			&& FullscreenGeometry.GetIndexBuffer_RenderThread() != nullptr;
		Decision = FSpatial::SelectRoute(RouteInputs);
		const uint64 Pixels = bViewFits
			? static_cast<uint64>(View->ViewportWidth) * View->ViewportHeight
			: 0;
		FExecutionCounters Counters = FSpatial::MakeExecutionCounters(RouteInputs, Decision,
			Pixels * Input.Parameters.PrimarySampleCount,
			Pixels * Input.Parameters.PrimarySampleCount * Input.Parameters.LightSampleCount);
		if (Decision.Route == ERoute::Disabled)
			return {.Counters = Counters};

		FMatrix InverseViewProjection;
		if (!Math::TryInverse(View->ViewProjectionMatrix, InverseViewProjection, 1.0e-8))
		{
			RouteInputs.bRequiredInputsValid = false;
			Decision = FSpatial::SelectRoute(RouteInputs);
			return {.Counters = FSpatial::MakeExecutionCounters(RouteInputs, Decision, 0, 0)};
		}
		FCloudUniform Uniform;
		for (uint32 Row = 0; Row < 4; ++Row)
			for (uint32 Col = 0; Col < 4; ++Col)
				Uniform.InverseViewProjection[Row * 4 + Col] =
					static_cast<float>(InverseViewProjection[Col][Row]);
		auto Copy3 = [](float* Out, const FVector3f& V) { Out[0] = V.x; Out[1] = V.y; Out[2] = V.z; };
		Uniform.Layer[0] = static_cast<float>(Input.Parameters.MinimumZ);
		Uniform.Layer[1] = static_cast<float>(Input.Parameters.MaximumZ);
		Uniform.Layer[2] = static_cast<float>(Input.Parameters.MaximumDistance);
		Uniform.Layer[3] = Input.Parameters.Extinction;
		Uniform.Density[0] = Input.Parameters.Coverage;
		Uniform.Density[1] = Input.Parameters.DetailErosion;
		Uniform.Density[2] = Input.Parameters.LightExtinction;
		Uniform.Density[3] = Input.Parameters.Ambient;
		Uniform.Sampling[0] = static_cast<float>(Input.Parameters.PrimarySampleCount);
		Uniform.Sampling[1] = static_cast<float>(Input.Parameters.LightSampleCount);
		Uniform.Sampling[2] = Input.Parameters.TransmittanceCutoff;
		Uniform.Sampling[3] = View->DepthConvention == ESceneDepthConvention::ReversedZ ? 1.0f : 0.0f;
		Copy3(Uniform.BaseFrequency, Input.Parameters.BaseFrequency);
		Copy3(Uniform.DetailFrequency, Input.Parameters.DetailFrequency);
		Copy3(Uniform.WindOffset, Input.Parameters.WindOffset);
		Uniform.Weather[0] = Input.Parameters.WeatherFrequency.x;
		Uniform.Weather[1] = Input.Parameters.WeatherFrequency.y;
		Uniform.Weather[2] = Input.Parameters.WeatherOffset.x;
		Uniform.Weather[3] = Input.Parameters.WeatherOffset.y;
		Copy3(Uniform.LightDirection, glm::normalize(Input.Parameters.LightDirection));
		Copy3(Uniform.LightColor, Input.Parameters.LightColor);
		Uniform.CameraPosition[0] = static_cast<float>(View->ViewLocation.x);
		Uniform.CameraPosition[1] = static_cast<float>(View->ViewLocation.y);
		Uniform.CameraPosition[2] = static_cast<float>(View->ViewLocation.z);
		Uniform.Viewport[0] = 1.0f / static_cast<float>(View->ViewportWidth);
		Uniform.Viewport[1] = 1.0f / static_cast<float>(View->ViewportHeight);
		Uniform.Viewport[2] = static_cast<float>(View->ViewportX);
		Uniform.Viewport[3] = static_cast<float>(View->ViewportY);
		const FRHIUniformBufferRange UniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&Uniform, sizeof(Uniform));
		if (UniformBuffer.Buffer == nullptr || UniformBuffer.Size != sizeof(Uniform))
		{
			RouteInputs.bRequiredInputsValid = false;
			Decision = FSpatial::SelectRoute(RouteInputs);
			return {.Counters = FSpatial::MakeExecutionCounters(RouteInputs, Decision, 0, 0)};
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
			const std::array BufferToCompute{FRHIBufferTransition{UniformBuffer.Buffer,
				UniformBuffer.Offset, UniformBuffer.Size, ERHIAccess::Discard,
				ERHIAccess::ComputeUniformRead}};
			CommandList.TransitionBuffers(BufferToCompute);
			const std::array InputsToCompute{
				FRHITextureTransition::Whole(Input.Textures.BaseDensity, ERHIAccess::GraphicsShaderRead, ERHIAccess::ComputeShaderRead),
				FRHITextureTransition::Whole(Input.Textures.DetailDensity, ERHIAccess::GraphicsShaderRead, ERHIAccess::ComputeShaderRead),
				FRHITextureTransition::Whole(Weather, ERHIAccess::GraphicsShaderRead, ERHIAccess::ComputeShaderRead),
				FRHITextureTransition::Whole(Input.Textures.SceneDepth, ERHIAccess::GraphicsShaderRead, ERHIAccess::ComputeShaderRead)};
			CommandList.TransitionTextures(InputsToCompute);
			const std::array OutputToCompute{FRHITextureTransition::Whole(ComputeTargets->Cloud,
				ERHIAccess::Discard, ERHIAccess::ComputeShaderReadWrite)};
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
			const std::array BufferRestore{FRHIBufferTransition{UniformBuffer.Buffer,
				UniformBuffer.Offset, UniformBuffer.Size, ERHIAccess::ComputeUniformRead,
				ERHIAccess::GraphicsUniformRead}};
			CommandList.TransitionBuffers(BufferRestore);
			const std::array TextureRestore{
				FRHITextureTransition::Whole(ComputeTargets->Cloud, ERHIAccess::ComputeShaderReadWrite, ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Input.Textures.BaseDensity, ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Input.Textures.DetailDensity, ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Weather, ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Input.Textures.SceneDepth, ERHIAccess::ComputeShaderRead, ERHIAccess::GraphicsShaderRead)};
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
			CommandList.SetViewport(static_cast<float>(View->ViewportX), static_cast<float>(View->ViewportY), 0.0f,
				static_cast<float>(View->ViewportX + View->ViewportWidth), static_cast<float>(View->ViewportY + View->ViewportHeight), 1.0f);
			CommandList.SetScissor(static_cast<float>(View->ViewportX), static_cast<float>(View->ViewportY),
				static_cast<float>(View->ViewportWidth), static_cast<float>(View->ViewportHeight));
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

	auto FVolumetricCloudRenderer::Composite_RenderThread(
		FRHICommandListImmediate& CommandList, FRHITexture* SceneColor,
		FRHITexture* Cloud, const FSceneView& View) -> FRHITexture*
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		if (SceneColor == nullptr || Cloud == nullptr || View.ViewportWidth == 0
			|| View.ViewportHeight == 0) return nullptr;
		FTargets* CompositeTargets = EnsureCompositeTargets_RenderThread(
			SceneColor->GetSizeX(), SceneColor->GetSizeY());
		if (CompositeTargets == nullptr || !CompositeTargets->Cloud) return nullptr;
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
					return FResult::Failure(MakeFailure("VolumetricCloudComposite", "shader",
						std::move(Error), ERenderResourceCreateErrorCategory::ShaderCompile));
				auto* Vertex = static_cast<FCloudCompositeVertexShader*>(Candidate.ShaderMap->GetShader(&VertexType));
				auto* Fragment = static_cast<FCloudCompositeFragmentShader*>(Candidate.ShaderMap->GetShader(&FragmentType));
				if (Vertex == nullptr || Fragment == nullptr)
					return FResult::Failure(MakeFailure("VolumetricCloudComposite", "shader",
						"Compiled shader map is missing a typed shader.",
						ERenderResourceCreateErrorCategory::ShaderBinding));
				Candidate.VertexShader = {Vertex, Candidate.ShaderMap.get()};
				Candidate.FragmentShader = {Fragment, Candidate.ShaderMap.get()};
				if (!FullscreenGeometry.EnsureResources_RenderThread(CommandList))
					return FResult::Failure(MakeFailure("VolumetricCloudComposite", "fullscreen-geometry",
						"Shared fullscreen geometry is unavailable.",
						ERenderResourceCreateErrorCategory::RHIResource));
				FRHIShader* VertexRHI = Candidate.VertexShader.GetRHIShader(false);
				FRHIShader* FragmentRHI = Candidate.FragmentShader.GetRHIShader(false);
				if (VertexRHI == nullptr || FragmentRHI == nullptr || GDynamicRHI == nullptr)
					return FResult::Failure(MakeFailure("VolumetricCloudComposite", "pipeline",
						"RHI shader creation returned null.", ERenderResourceCreateErrorCategory::RHIResource));
				FGraphicsPipelineStateInitializer Initializer;
				Initializer.RenderTargetLayout =
					RenderTargetLayouts::MakeVolumetricCloudComposite();
				Initializer.BoundShaders.VertexShader = VertexRHI;
				Initializer.BoundShaders.FragmentShader = FragmentRHI;
				Initializer.VertexDeclaration = FullscreenGeometry.GetVertexDeclaration_RenderThread();
				Initializer.RasterizerState.CullMode = ERHICullMode::None;
				Initializer.PipelineLayout = Candidate.ShaderMap->GetMergedPipelineLayout();
				Candidate.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState(
					"VolumetricCloudCompositePipeline", Initializer);
				if (!Candidate.PipelineState)
					return FResult::Failure(MakeFailure("VolumetricCloudComposite", "pipeline",
						"Graphics pipeline creation returned null.",
						ERenderResourceCreateErrorCategory::GraphicsPipeline));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		if (Payload == nullptr) return nullptr;
		FRHIRenderPassInfo PassInfo{};
		PassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeVolumetricCloudComposite();
		PassInfo.ColorRenderTargets[0] = CompositeTargets->Cloud;
		PassInfo.ColorClearValues[0] = FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f);
		CommandList.BeginRenderPass(PassInfo, "VolumetricCloudCompositeRenderPass");
		CommandList.SetGraphicsPipelineState(*Payload->PipelineState);
		CommandList.SetViewport(0.0f, 0.0f, 0.0f,
			static_cast<float>(CompositeTargets->Cloud->GetSizeX()),
			static_cast<float>(CompositeTargets->Cloud->GetSizeY()), 1.0f);
		CommandList.SetScissor(0.0f, 0.0f,
			static_cast<float>(CompositeTargets->Cloud->GetSizeX()),
			static_cast<float>(CompositeTargets->Cloud->GetSizeY()));
		CommandList.BindVertexBuffer(0, FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
		CommandList.BindIndexBuffer(FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
		FCloudCompositeFragmentShader::FParameters Params;
		Params.SceneColorTexture = SceneColor;
		Params.CloudTexture = Cloud;
		SetShaderParameters(CommandList, Payload->FragmentShader, Params);
		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();
		return CompositeTargets->Cloud;
	}

	auto FVolumetricCloudRenderer::GetRetainedTargetBytes_RenderThread() const -> uint64
	{
		check(IsInRenderingThread());
		auto Weight = [](uint64 Key, const auto&) { return FSpatial::CalculateTargetBytes(
			static_cast<uint32>(Key >> 32), static_cast<uint32>(Key)); };
		const uint64 Fragment = State->TargetsBySize.GetRetainedPayloadWeight(Weight);
		const uint64 Compute = State->ComputeTargetsBySize.GetRetainedPayloadWeight(Weight);
		const uint64 Composite =
			State->CompositeTargetsBySize.GetRetainedPayloadWeight(Weight);
		const uint64 RouteBytes = Compute > std::numeric_limits<uint64>::max() - Fragment
			? std::numeric_limits<uint64>::max() : Fragment + Compute;
		return Composite > std::numeric_limits<uint64>::max() - RouteBytes
			? std::numeric_limits<uint64>::max() : RouteBytes + Composite;
	}

	auto FVolumetricCloudRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->TargetsBySize.Reset();
		State->ComputeTargetsBySize.Reset();
		State->CompositeTargetsBySize.Reset();
		State->FragmentResources.Reset();
		State->ComputeResources.Reset();
		State->CompositeResources.Reset();
		State->FallbackResources.Reset();
		State->DensitySamplerResources.Reset();
	}
} // namespace Durin
