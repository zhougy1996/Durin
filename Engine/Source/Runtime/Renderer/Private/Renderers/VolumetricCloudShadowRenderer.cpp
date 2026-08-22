#include "Renderers/VolumetricCloudShadowRenderer.h"

#include "Math/Operations.h"
#include "RendererResourceSlotCache.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
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
		std::atomic<FVolumetricCloudShadowRenderer::FTimingQuerySink>
			GVolumetricCloudShadowTimingQuerySink = nullptr;
		std::atomic<FVolumetricCloudShadowRenderer::FCaptureSink>
			GVolumetricCloudShadowCaptureSink = nullptr;
		class FCloudShadowVertexShader final : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FCloudShadowVertexShader, FShader,
				"/Engine/VolumetricCloudShadow", EShaderFrequency::Vertex, "VertexMain");
		};
		class FCloudShadowFragmentShader final : public FShader
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
			DURIN_DECLARE_SHADER(FCloudShadowFragmentShader, FShader,
				"/Engine/VolumetricCloudShadow", EShaderFrequency::Fragment,
				"CloudVisibilityFragmentMain");
		};
		class FCloudShadowComputeShader final : public FShader
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
			DURIN_DECLARE_SHADER(FCloudShadowComputeShader, FShader,
				"/Engine/VolumetricCloudShadow", EShaderFrequency::Compute,
				"CloudVisibilityComputeMain");
		};
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
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FCloudShadowVertexShader> VertexShader;
			TShaderRef<FCloudShadowFragmentShader> FragmentShader;
			FGraphicsPipelineStateRHIRef PipelineState;
		};
		struct FComputePayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FCloudShadowComputeShader> ComputeShader;
			FComputePipelineStateRHIRef PipelineState;
		};
		TRenderResourceCreationSlot<FFragmentPayload> FragmentResources{
			ERenderResourceGenerationDependency::Shader
			| ERenderResourceGenerationDependency::Device};
		TRenderResourceCreationSlot<FComputePayload> ComputeResources{
			ERenderResourceGenerationDependency::Shader
			| ERenderResourceGenerationDependency::Device};
		TRendererResourceSlotCache<uint64, FTargets> TargetsBySize{
			ERenderResourceGenerationDependency::Device};
		TRendererResourceSlotCache<uint64, FComputeTargets> ComputeTargetsBySize{
			ERenderResourceGenerationDependency::Device};
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

	auto FVolumetricCloudShadowRenderer::EnsureTargets_RenderThread(
		uint32 Width, uint32 Height) -> FTargets*
	{
		check(IsInRenderingThread());
		if (Width == 0 || Height == 0
			|| CalculateTargetBytes(Width, Height) > MaximumRetainedBytesPerRoute)
			return nullptr;
		const uint64 Key = (static_cast<uint64>(Width) << 32) | Height;
		const auto Desc = FRHITextureCreateDesc::Create2D(
			"VolumetricCloudVisibility", Width, Height, EPixelFormat::R8_UNORM)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy
				| ETextureCreateFlags::CPUReadback)
			.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f));
		using FResult = TRenderResourceCreateResult<FTargets>;
		auto& Entry = State->TargetsBySize.FindOrAdd(Key);
		FTargets* Result = Entry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(), [Key, &Desc]() -> FResult {
				FTargets Candidate{.Visibility = RHICreateTexture(Desc)};
				if (!Candidate.Visibility)
					return FResult::Failure(MakeFailure("VolumetricCloudShadowTarget",
						std::to_string(Key).c_str(), "R8_UNORM target creation returned null.",
						ERenderResourceCreateErrorCategory::RHIResource));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		while (State->TargetsBySize.Num() > 1
			&& State->TargetsBySize.GetRetainedPayloadWeight(
				[](uint64 K, const FTargets&) { return CalculateTargetBytes(
					static_cast<uint32>(K >> 32), static_cast<uint32>(K)); })
				> MaximumRetainedBytesPerRoute)
			if (!State->TargetsBySize.EvictOldestExcept(Key)) break;
		if (!Result) return nullptr;
		auto* Retained = State->TargetsBySize.Find(Key);
		return Retained ? Retained->Slot.GetPayload() : nullptr;
	}

	auto FVolumetricCloudShadowRenderer::EnsureComputeTargets_RenderThread(
		uint32 Width, uint32 Height) -> FComputeTargets*
	{
		check(IsInRenderingThread());
		if (Width == 0 || Height == 0 || GDynamicRHI == nullptr
			|| CalculateTargetBytes(Width, Height) > MaximumRetainedBytesPerRoute)
			return nullptr;
		const uint64 Key = (static_cast<uint64>(Width) << 32) | Height;
		const auto Desc = FRHITextureCreateDesc::Create2D(
			"VolumetricCloudVisibilityCompute", Width, Height, EPixelFormat::R8_UNORM)
			.SetFlags(ETextureCreateFlags::Storage | ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback);
		using FResult = TRenderResourceCreateResult<FComputeTargets>;
		auto& Entry = State->ComputeTargetsBySize.FindOrAdd(Key);
		FComputeTargets* Result = Entry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(), [Key, &Desc]() -> FResult {
				if (!GDynamicRHI->RHIIsTextureSupported(Desc))
					return FResult::Failure(MakeFailure("VolumetricCloudShadowComputeTarget",
						std::to_string(Key).c_str(), "R8_UNORM storage target is unsupported.",
						ERenderResourceCreateErrorCategory::RHIResource));
				FComputeTargets Candidate;
				Candidate.Visibility = RHICreateTexture(Desc);
				if (Candidate.Visibility)
				{
					Candidate.SampledView = GDynamicRHI->RHIGetOrCreateTextureView(
						Candidate.Visibility, MakeDefaultTextureViewDesc(*Candidate.Visibility,
							ERHITextureViewUsage::Sampled));
					Candidate.StorageView = GDynamicRHI->RHIGetOrCreateTextureView(
						Candidate.Visibility, MakeDefaultTextureViewDesc(*Candidate.Visibility,
							ERHITextureViewUsage::Storage));
				}
				if (!Candidate.Visibility || !Candidate.SampledView || !Candidate.StorageView)
					return FResult::Failure(MakeFailure("VolumetricCloudShadowComputeTarget",
						std::to_string(Key).c_str(), "Target or canonical view creation returned null.",
						ERenderResourceCreateErrorCategory::RHIResource));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		while (State->ComputeTargetsBySize.Num() > 1
			&& State->ComputeTargetsBySize.GetRetainedPayloadWeight(
				[](uint64 K, const FComputeTargets&) { return CalculateTargetBytes(
					static_cast<uint32>(K >> 32), static_cast<uint32>(K)); })
				> MaximumRetainedBytesPerRoute)
			if (!State->ComputeTargetsBySize.EvictOldestExcept(Key)) break;
		if (!Result) return nullptr;
		auto* Retained = State->ComputeTargetsBySize.Find(Key);
		return Retained ? Retained->Slot.GetPayload() : nullptr;
	}

	auto FVolumetricCloudShadowRenderer::Render_RenderThread(
		FRHICommandListImmediate& CommandList, FTargets* FragmentTargets,
		FComputeTargets* ComputeTargets, const FRenderInput& Input) -> FRenderResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		FRenderResult Result;
		if (!Input.bRequested) return Result;
		const FSceneView* View = Input.View;
		const double LightLengthSquared = glm::dot(
			Input.Parameters.LightDirection, Input.Parameters.LightDirection);
		const bool bViewFits = View && View->ViewportX <= Input.Width
			&& View->ViewportY <= Input.Height
			&& View->ViewportWidth <= Input.Width - View->ViewportX
			&& View->ViewportHeight <= Input.Height - View->ViewportY;
		if (!View || !Input.BaseDensity || !Input.DetailDensity || !Input.Weather
			|| !Input.SceneDepth || !Input.DensitySampler || !bViewFits
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
		if (ComputeTargets)
			ComputePayload = State->ComputeResources.Resolve(
				Coordinator.GetGeneration_RenderThread(), [this]() -> FComputeResult {
					FShaderCompileOptions Options;
					Options.bForceRecompile = Coordinator.ShouldForceShaderRecompile_RenderThread();
					FShaderType& Type = FCloudShadowComputeShader::StaticType();
					const std::array<const FShaderType*, 1> Types{&Type};
					FComputePayload Candidate;
					Candidate.ShaderMap = std::make_shared<FShaderMapBase>();
					std::string Error;
					if (!Candidate.ShaderMap->InitializeFromShaderTypes(Types, Options, Error))
						return FComputeResult::Failure(MakeFailure("VolumetricCloudShadowCompute",
							"shader", std::move(Error), ERenderResourceCreateErrorCategory::ShaderCompile));
					auto* Shader = static_cast<FCloudShadowComputeShader*>(Candidate.ShaderMap->GetShader(&Type));
					if (!Shader) return FComputeResult::Failure(MakeFailure("VolumetricCloudShadowCompute",
						"shader", "Typed compute shader is missing.", ERenderResourceCreateErrorCategory::ShaderBinding));
					Candidate.ComputeShader = {Shader, Candidate.ShaderMap.get()};
					FRHIShader* RHIShader = Candidate.ComputeShader.GetRHIShader(false);
					if (!RHIShader || !GDynamicRHI) return FComputeResult::Failure(MakeFailure(
						"VolumetricCloudShadowCompute", "pipeline", "Compute shader RHI is unavailable.",
						ERenderResourceCreateErrorCategory::RHIResource));
					FComputePipelineStateInitializer Initializer;
					Initializer.ComputeShader = RHIShader;
					Initializer.PipelineLayout = Candidate.ShaderMap->GetMergedPipelineLayout();
					Candidate.PipelineState = GDynamicRHI->RHICreateComputePipelineState(
						"VolumetricCloudShadowComputePipeline", Initializer);
					if (!Candidate.PipelineState) return FComputeResult::Failure(MakeFailure(
						"VolumetricCloudShadowCompute", "pipeline", "Compute pipeline creation returned null.",
						ERenderResourceCreateErrorCategory::GraphicsPipeline));
					return FComputeResult::Success(std::move(Candidate));
				}, ReportRendererResourceCreateDiagnostic);

		const FRHICapabilities* Capabilities = GDynamicRHI ? GDynamicRHI->RHIGetCapabilities() : nullptr;
		const uint32 GroupsX = CalculateGroupCount(Input.Width);
		const uint32 GroupsY = CalculateGroupCount(Input.Height);
		const bool bComputeExtent = Capabilities && GroupsX <= Capabilities->MaxComputeWorkGroupCount[0]
			&& GroupsY <= Capabilities->MaxComputeWorkGroupCount[1];
		ERouteReason FallbackReason = !ComputePayload ? ERouteReason::ComputePayloadUnavailable
			: !ComputeTargets ? ERouteReason::ComputeTargetUnavailable
			: ERouteReason::ComputeExtentUnsupported;

		using FFragmentPayload = FState::FFragmentPayload;
		using FFragmentResult = TRenderResourceCreateResult<FFragmentPayload>;
		FFragmentPayload* FragmentPayload = nullptr;
		if (!(ComputePayload && ComputeTargets && bComputeExtent) && FragmentTargets)
			FragmentPayload = State->FragmentResources.Resolve(
				Coordinator.GetGeneration_RenderThread(), [this, &CommandList]() -> FFragmentResult {
					FShaderCompileOptions Options;
					Options.bForceRecompile = Coordinator.ShouldForceShaderRecompile_RenderThread();
					FShaderType& VertexType = FCloudShadowVertexShader::StaticType();
					FShaderType& FragmentType = FCloudShadowFragmentShader::StaticType();
					const std::array<const FShaderType*, 2> Types{&VertexType, &FragmentType};
					FFragmentPayload Candidate;
					Candidate.ShaderMap = std::make_shared<FShaderMapBase>();
					std::string Error;
					if (!Candidate.ShaderMap->InitializeFromShaderTypes(Types, Options, Error))
						return FFragmentResult::Failure(MakeFailure("VolumetricCloudShadow",
							"shader", std::move(Error), ERenderResourceCreateErrorCategory::ShaderCompile));
					auto* Vertex = static_cast<FCloudShadowVertexShader*>(Candidate.ShaderMap->GetShader(&VertexType));
					auto* Fragment = static_cast<FCloudShadowFragmentShader*>(Candidate.ShaderMap->GetShader(&FragmentType));
					if (!Vertex || !Fragment || !FullscreenGeometry.EnsureResources_RenderThread(CommandList))
						return FFragmentResult::Failure(MakeFailure("VolumetricCloudShadow", "shader",
							"Typed shaders or fullscreen geometry are unavailable.",
							ERenderResourceCreateErrorCategory::ShaderBinding));
					Candidate.VertexShader = {Vertex, Candidate.ShaderMap.get()};
					Candidate.FragmentShader = {Fragment, Candidate.ShaderMap.get()};
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
					Initializer.PipelineLayout = Candidate.ShaderMap->GetMergedPipelineLayout();
					Candidate.PipelineState = GDynamicRHI->RHICreateGraphicsPipelineState(
						"VolumetricCloudShadowPipeline", Initializer);
					if (!Candidate.PipelineState) return FFragmentResult::Failure(MakeFailure(
						"VolumetricCloudShadow", "pipeline", "Graphics pipeline creation returned null.",
						ERenderResourceCreateErrorCategory::GraphicsPipeline));
					return FFragmentResult::Success(std::move(Candidate));
				}, ReportRendererResourceCreateDiagnostic);

		const bool bUseCompute = ComputePayload && ComputeTargets && bComputeExtent;
		if (!bUseCompute && !FragmentPayload)
		{
			Result.Reason = ERouteReason::FragmentUnavailable;
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
		Copy3(Uniform.LightDirection, glm::normalize(Input.Parameters.LightDirection));
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
		const uint64 Fragment = State->TargetsBySize.GetRetainedPayloadWeight(
			[](uint64 K, const FTargets&) { return CalculateTargetBytes(
				static_cast<uint32>(K >> 32), static_cast<uint32>(K)); });
		const uint64 Compute = State->ComputeTargetsBySize.GetRetainedPayloadWeight(
			[](uint64 K, const FComputeTargets&) { return CalculateTargetBytes(
				static_cast<uint32>(K >> 32), static_cast<uint32>(K)); });
		return Fragment + Compute;
	}

	auto FVolumetricCloudShadowRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->TargetsBySize.Reset();
		State->ComputeTargetsBySize.Reset();
		State->FragmentResources.Reset();
		State->ComputeResources.Reset();
	}
}
