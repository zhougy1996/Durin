#include "Renderers/ContactShadowRenderer.h"

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
		std::atomic<FContactShadowVisibilityRenderer::FTimingQuerySink>
			GContactVisibilityTimingQuerySink = nullptr;
		class FContactVisibilityVertexShader final : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FContactVisibilityVertexShader, FShader,
				"/Engine/ContactShadow", EShaderFrequency::Vertex, "VertexMain");
		};

		class FContactVisibilityFragmentShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FContactVisibilityFragmentShader)
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferMaterial);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferNormals);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferSurface);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferEmissive);
				DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Params);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FContactVisibilityFragmentShader, FShader,
				"/Engine/ContactShadow", EShaderFrequency::Fragment,
				"ContactVisibilityFragmentMain");
		};

		class FContactVisibilityComputeShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FContactVisibilityComputeShader)
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferMaterial);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferNormals);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferSurface);
				DURIN_SHADER_PARAMETER_TEXTURE(GBufferEmissive);
				DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER(Params);
				DURIN_SHADER_PARAMETER_STORAGE_IMAGE(ContactVisibilityOutput);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_SHADER(FContactVisibilityComputeShader, FShader,
				"/Engine/ContactShadow", EShaderFrequency::Compute,
				"ContactVisibilityComputeMain");
		};

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
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FContactVisibilityVertexShader> VertexShader;
			TShaderRef<FContactVisibilityFragmentShader> FragmentShader;
			FGraphicsPipelineStateRHIRef PipelineState;
		};
		struct FComputePayload
		{
			std::shared_ptr<FShaderMapBase> ShaderMap;
			TShaderRef<FContactVisibilityComputeShader> ComputeShader;
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

	FContactShadowVisibilityRenderer::FContactShadowVisibilityRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry)
		: Coordinator(InCoordinator), FullscreenGeometry(InFullscreenGeometry),
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
		uint32 Width, uint32 Height) -> FTargets*
	{
		check(IsInRenderingThread());
		if (Width == 0 || Height == 0) return nullptr;
		const uint64 Key = (static_cast<uint64>(Width) << 32) | Height;
		const auto Desc = FRHITextureCreateDesc::Create2D(
			"DirectionalContactVisibility", Width, Height, EPixelFormat::R8_UNORM)
			.SetFlags(ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ShaderResource | ETextureCreateFlags::SourceCopy)
			.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f));
		using FResult = TRenderResourceCreateResult<FTargets>;
		auto& Entry = State->TargetsBySize.FindOrAdd(Key);
		FTargets* Targets = Entry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(), [Key, &Desc]() -> FResult {
				FTargets Candidate;
				Candidate.Visibility = RHICreateTexture(Desc);
				if (Candidate.Visibility == nullptr)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"ContactVisibilityTarget", std::to_string(Key),
						"R8_UNORM visibility target creation returned null.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		const bool bResolved = Targets != nullptr;
		auto GetRetainedBytes = [this]() {
			return State->TargetsBySize.GetRetainedPayloadWeight(
				[](uint64 SizeKey, const FTargets&) {
					return CalculateTargetBytes(static_cast<uint32>(SizeKey >> 32),
						static_cast<uint32>(SizeKey));
				});
		};
		while (State->TargetsBySize.Num() > 1
			&& GetRetainedBytes() > MaximumRetainedBytesPerRoute)
			if (!State->TargetsBySize.EvictOldestExcept(Key)) break;
		if (!bResolved) return nullptr;
		auto* Retained = State->TargetsBySize.Find(Key);
		return Retained != nullptr ? Retained->Slot.GetPayload() : nullptr;
	}

	auto FContactShadowVisibilityRenderer::EnsureComputeTargets_RenderThread(
		uint32 Width, uint32 Height) -> FComputeTargets*
	{
		check(IsInRenderingThread());
		if (Width == 0 || Height == 0 || GDynamicRHI == nullptr) return nullptr;
		const uint64 Key = (static_cast<uint64>(Width) << 32) | Height;
		const auto Desc = FRHITextureCreateDesc::Create2D(
			"DirectionalContactVisibilityCompute", Width, Height,
			EPixelFormat::R8_UNORM)
			.SetFlags(ETextureCreateFlags::Storage
				| ETextureCreateFlags::ShaderResource
				| ETextureCreateFlags::SourceCopy);
		using FResult = TRenderResourceCreateResult<FComputeTargets>;
		auto& Entry = State->ComputeTargetsBySize.FindOrAdd(Key);
		FComputeTargets* Targets = Entry.Slot.Resolve(
			Coordinator.GetGeneration_RenderThread(), [Key, &Desc]() -> FResult {
				if (!GDynamicRHI->RHIIsTextureSupported(Desc))
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"ContactVisibilityComputeTarget", std::to_string(Key),
						"R8_UNORM sampled/storage target is unsupported.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				FComputeTargets Candidate;
				Candidate.Visibility = RHICreateTexture(Desc);
				if (Candidate.Visibility != nullptr)
				{
					Candidate.SampledView = GDynamicRHI->RHIGetOrCreateTextureView(
						Candidate.Visibility,
						MakeDefaultTextureViewDesc(*Candidate.Visibility,
							ERHITextureViewUsage::Sampled));
					Candidate.StorageView = GDynamicRHI->RHIGetOrCreateTextureView(
						Candidate.Visibility,
						MakeDefaultTextureViewDesc(*Candidate.Visibility,
							ERHITextureViewUsage::Storage));
				}
				if (Candidate.Visibility == nullptr || Candidate.SampledView == nullptr
					|| Candidate.StorageView == nullptr)
					return FResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::RHIResource,
						"ContactVisibilityComputeTarget", std::to_string(Key),
						"R8_UNORM sampled/storage target or canonical view creation returned null.",
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual));
				return FResult::Success(std::move(Candidate));
			}, ReportRendererResourceCreateDiagnostic);
		const bool bResolved = Targets != nullptr;
		auto GetRetainedBytes = [this]() {
			return State->ComputeTargetsBySize.GetRetainedPayloadWeight(
				[](uint64 SizeKey, const FComputeTargets&) {
					return CalculateTargetBytes(static_cast<uint32>(SizeKey >> 32),
						static_cast<uint32>(SizeKey));
				});
		};
		while (State->ComputeTargetsBySize.Num() > 1
			&& GetRetainedBytes() > MaximumRetainedBytesPerRoute)
			if (!State->ComputeTargetsBySize.EvictOldestExcept(Key)) break;
		if (!bResolved) return nullptr;
		auto* Retained = State->ComputeTargetsBySize.Find(Key);
		return Retained != nullptr ? Retained->Slot.GetPayload() : nullptr;
	}

	auto FContactShadowVisibilityRenderer::Render_RenderThread(
		FRHICommandListImmediate& CommandList, bool bRequested,
		FTargets* FragmentTargets, FComputeTargets* ComputeTargets,
		FRHITexture* Material, FRHITexture* Normals, FRHITexture* Surface,
		FRHITexture* Emissive, FRHITexture* SceneDepth, const FSceneView& View,
		const FVector3& LightDirection, uint32 Width, uint32 Height) -> FRenderResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const double LightLengthSquared = Math::Dot(LightDirection, LightDirection);
		const bool bViewFits = View.ViewportX <= Width && View.ViewportY <= Height
			&& View.ViewportWidth <= Width - View.ViewportX
			&& View.ViewportHeight <= Height - View.ViewportY;
		const bool bInputsValid = Material != nullptr && Normals != nullptr
			&& Surface != nullptr && Emissive != nullptr && SceneDepth != nullptr
			&& View.ViewportWidth != 0 && View.ViewportHeight != 0 && bViewFits
			&& std::isfinite(LightLengthSquared) && LightLengthSquared > 1.0e-8;

		using FComputePayload = FState::FComputePayload;
		using FComputeResult = TRenderResourceCreateResult<FComputePayload>;
		FComputePayload* ComputePayload = nullptr;
		if (bRequested && bInputsValid && Width != 0 && Height != 0)
		{
			ComputePayload = State->ComputeResources.Resolve(
				Coordinator.GetGeneration_RenderThread(), [this]() -> FComputeResult {
					FShaderCompileOptions Options;
					Options.bForceRecompile =
						Coordinator.ShouldForceShaderRecompile_RenderThread();
					FShaderType& ComputeType =
						FContactVisibilityComputeShader::StaticType();
					const std::array<const FShaderType*, 1> Types{&ComputeType};
					FComputePayload Candidate;
					Candidate.ShaderMap = std::make_shared<FShaderMapBase>();
					std::string Error;
					if (!Candidate.ShaderMap->InitializeFromShaderTypes(Types, Options, Error))
						return FComputeResult::Failure(MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderCompile,
							"ContactVisibilityCompute", "shader", std::move(Error),
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
					auto* Compute = static_cast<FContactVisibilityComputeShader*>(
						Candidate.ShaderMap->GetShader(&ComputeType));
					if (Compute == nullptr)
						return FComputeResult::Failure(MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::ShaderBinding,
							"ContactVisibilityCompute", "shader",
							"Compiled shader map is missing the typed compute shader.",
							ERenderResourceGenerationDependency::Shader
								| ERenderResourceGenerationDependency::Manual));
					Candidate.ComputeShader = {Compute, Candidate.ShaderMap.get()};
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
						Candidate.ShaderMap->GetMergedPipelineLayout();
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
				}, ReportRendererResourceCreateDiagnostic);
		}

		const FRHICapabilities* Capabilities =
			GDynamicRHI != nullptr ? GDynamicRHI->RHIGetCapabilities() : nullptr;
		FRouteInputs RouteInputs{
			.bRequested = bRequested,
			.bInputsValid = bInputsValid,
			.bComputePayloadReady = ComputePayload != nullptr,
			.bComputeTargetReady = ComputeTargets != nullptr
				&& ComputeTargets->Visibility != nullptr,
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
			&& Width != 0 && Height != 0 && FragmentTargets != nullptr
			&& FragmentTargets->Visibility != nullptr)
		{
			FragmentPayload = State->FragmentResources.Resolve(
			Coordinator.GetGeneration_RenderThread(), [this, &CommandList]() -> FFragmentResult {
				FShaderCompileOptions Options;
				Options.bForceRecompile = Coordinator.ShouldForceShaderRecompile_RenderThread();
				FShaderType& VertexType = FContactVisibilityVertexShader::StaticType();
				FShaderType& FragmentType = FContactVisibilityFragmentShader::StaticType();
				const std::array<const FShaderType*, 2> Types{&VertexType, &FragmentType};
				FFragmentPayload Candidate;
				Candidate.ShaderMap = std::make_shared<FShaderMapBase>();
				std::string Error;
				if (!Candidate.ShaderMap->InitializeFromShaderTypes(Types, Options, Error))
					return FFragmentResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderCompile,
						"ContactVisibility", "shader", std::move(Error),
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				auto* Vertex = static_cast<FContactVisibilityVertexShader*>(
					Candidate.ShaderMap->GetShader(&VertexType));
				auto* Fragment = static_cast<FContactVisibilityFragmentShader*>(
					Candidate.ShaderMap->GetShader(&FragmentType));
				if (Vertex == nullptr || Fragment == nullptr)
					return FFragmentResult::Failure(MakeRendererResourceCreateError(
						ERenderResourceCreateErrorCategory::ShaderBinding,
						"ContactVisibility", "shader",
						"Compiled shader map is missing a typed shader.",
						ERenderResourceGenerationDependency::Shader
							| ERenderResourceGenerationDependency::Manual));
				Candidate.VertexShader = {Vertex, Candidate.ShaderMap.get()};
				Candidate.FragmentShader = {Fragment, Candidate.ShaderMap.get()};
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
				Initializer.PipelineLayout = Candidate.ShaderMap->GetMergedPipelineLayout();
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
			}, ReportRendererResourceCreateDiagnostic);
		}
		RouteInputs.bFragmentReady = FragmentPayload != nullptr
			&& FullscreenGeometry.GetVertexBuffer_RenderThread() != nullptr
			&& FullscreenGeometry.GetIndexBuffer_RenderThread() != nullptr;
		Decision = SelectRoute(RouteInputs);
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
		FGPUTimingQueryRHIRef TimingQuery;
		const FTimingQuerySink TimingSink =
			GContactVisibilityTimingQuerySink.load(std::memory_order_acquire);
		if (TimingSink != nullptr && GDynamicRHI != nullptr)
		{
			TimingQuery = GDynamicRHI->RHICreateGPUTimingQuery();
			if (TimingQuery) CommandList.BeginGPUTimingQuery(TimingQuery);
		}

		if (Decision.Route == ERoute::Compute)
		{
			const std::array UniformTransition{FRHIBufferTransition{
				UniformBuffer.Buffer, UniformBuffer.Offset, UniformBuffer.Size,
				ERHIAccess::Discard, ERHIAccess::ComputeUniformRead}};
			CommandList.TransitionBuffers(UniformTransition);
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
					ERHIAccess::ComputeShaderRead)};
			CommandList.TransitionTextures(InputTransitions);
			const std::array OutputTransition{FRHITextureTransition::Whole(
				ComputeTargets->Visibility, ERHIAccess::Discard,
				ERHIAccess::ComputeShaderReadWrite)};
			CommandList.TransitionTextures(OutputTransition);
			CommandList.SwitchPipeline(ERHIPipeline::Compute);
			CommandList.SetComputePipelineState(*ComputePayload->PipelineState);
			FContactVisibilityComputeShader::FParameters Parameters;
			Parameters.GBufferMaterial = Material;
			Parameters.GBufferNormals = Normals;
			Parameters.GBufferSurface = Surface;
			Parameters.GBufferEmissive = Emissive;
			Parameters.SceneDepth = SceneDepth;
			Parameters.Params = UniformBuffer;
			Parameters.ContactVisibilityOutput = ComputeTargets->Visibility;
			SetShaderParameters(CommandList, ComputePayload->ComputeShader, Parameters);
			CommandList.Dispatch(CalculateGroupCount(Width), CalculateGroupCount(Height), 1);
			const std::array UniformRestore{FRHIBufferTransition{
				UniformBuffer.Buffer, UniformBuffer.Offset, UniformBuffer.Size,
				ERHIAccess::ComputeUniformRead, ERHIAccess::GraphicsUniformRead}};
			CommandList.TransitionBuffers(UniformRestore);
			const std::array RestoreTransitions{
				FRHITextureTransition::Whole(ComputeTargets->Visibility,
					ERHIAccess::ComputeShaderReadWrite, ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Material, ERHIAccess::ComputeShaderRead,
					ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Normals, ERHIAccess::ComputeShaderRead,
					ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Surface, ERHIAccess::ComputeShaderRead,
					ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(Emissive, ERHIAccess::ComputeShaderRead,
					ERHIAccess::GraphicsShaderRead),
				FRHITextureTransition::Whole(SceneDepth, ERHIAccess::ComputeShaderRead,
					ERHIAccess::GraphicsShaderRead)};
			CommandList.TransitionTextures(RestoreTransitions);
			CommandList.SwitchPipeline(ERHIPipeline::Graphics);
			if (TimingQuery)
			{
				CommandList.EndGPUTimingQuery(TimingQuery);
				TimingSink(TimingQuery, Decision.Route);
			}
			return {.Visibility = ComputeTargets->Visibility,
				.Route = Decision.Route, .Reason = Decision.Reason};
		}

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
			static_cast<float>(View.ViewportY), static_cast<float>(View.ViewportWidth),
			static_cast<float>(View.ViewportHeight));
		CommandList.BindVertexBuffer(0,
			FullscreenGeometry.GetVertexBuffer_RenderThread(), 0);
		CommandList.BindIndexBuffer(FullscreenGeometry.GetIndexBuffer_RenderThread(), 0);
		FContactVisibilityFragmentShader::FParameters Parameters;
		Parameters.GBufferMaterial = Material;
		Parameters.GBufferNormals = Normals;
		Parameters.GBufferSurface = Surface;
		Parameters.GBufferEmissive = Emissive;
		Parameters.SceneDepth = SceneDepth;
		Parameters.Params = UniformBuffer;
		SetShaderParameters(CommandList, FragmentPayload->FragmentShader, Parameters);
		CommandList.DrawIndexed(3, 0, 0);
		CommandList.EndRenderPass();
		if (TimingQuery)
		{
			CommandList.EndGPUTimingQuery(TimingQuery);
			TimingSink(TimingQuery, Decision.Route);
		}
		return {.Visibility = FragmentTargets->Visibility,
			.Route = Decision.Route, .Reason = Decision.Reason};
	}

	auto FContactShadowVisibilityRenderer::GetRetainedTargetBytes_RenderThread() const
		-> uint64
	{
		check(IsInRenderingThread());
		const uint64 FragmentBytes = State->TargetsBySize.GetRetainedPayloadWeight(
			[](uint64 Key, const FTargets&) {
				return CalculateTargetBytes(static_cast<uint32>(Key >> 32),
					static_cast<uint32>(Key));
			});
		const uint64 ComputeBytes =
			State->ComputeTargetsBySize.GetRetainedPayloadWeight(
				[](uint64 Key, const FComputeTargets&) {
					return CalculateTargetBytes(static_cast<uint32>(Key >> 32),
						static_cast<uint32>(Key));
				});
		return FragmentBytes + ComputeBytes;
	}

	auto FContactShadowVisibilityRenderer::ReleaseResources_RenderThread() -> void
	{
		State->TargetsBySize.Reset();
		State->ComputeTargetsBySize.Reset();
		State->FragmentResources.Reset();
		State->ComputeResources.Reset();
	}
} // namespace Durin
