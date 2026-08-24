#include "Renderers/ContactShadowRenderer.h"

#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/FullscreenGeometryResources.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Resources/RenderTargetLayouts.h"
#include "Renderers/RendererTransientTargetPool.h"
#include "Math/Operations.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderGraph.h"
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
		constexpr FRenderGraphBudget ContactComputeGraphBudget{
			.MaxPasses = 1, .MaxDependencies = 0,
			.MaxBufferTransitions = 2, .MaxTextureTransitions = 12,
			.MaxCompileMicroseconds = 2000,
			.MaxExecuteMicroseconds = 50000};
		constexpr FRenderGraphBudget ContactFragmentGraphBudget{
			.MaxPasses = 1, .MaxDependencies = 0,
			.MaxBufferTransitions = 0, .MaxTextureTransitions = 2,
			.MaxCompileMicroseconds = 2000,
			.MaxExecuteMicroseconds = 50000};

		auto GetWholeTextureRange(FRHITexture* Texture)
			-> FRHITextureSubresourceRange
		{
			return {GetTextureAspects(Texture->GetFormat()), 0,
				Texture->GetNumMips(), 0, Texture->GetArraySize()};
		}
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
		const FTimingQuerySink TimingSink =
			GContactVisibilityTimingQuerySink.load(std::memory_order_acquire);

		if (Decision.Route == ERoute::Compute)
		{
			FRenderGraphBuilder Graph;
			Graph.SetBudget(ContactComputeGraphBudget);
			const auto GraphMaterial = Graph.ImportTexture("Contact.Material", Material,
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			const auto GraphNormals = Graph.ImportTexture("Contact.Normals", Normals,
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			const auto GraphSurface = Graph.ImportTexture("Contact.Surface", Surface,
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			const auto GraphEmissive = Graph.ImportTexture("Contact.Emissive", Emissive,
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			const auto GraphDepth = Graph.ImportTexture("Contact.Depth", SceneDepth,
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			const auto GraphVisibility = Graph.CreateTexture("Contact.Visibility",
				ComputeTargets->Visibility, ERHIAccess::GraphicsShaderRead);
			const auto GraphUniform = Graph.ImportBuffer("Contact.Uniform",
				UniformBuffer.Buffer, ERHIAccess::Discard,
				ERHIAccess::GraphicsUniformRead);
			const auto Pass = Graph.AddPass("ContactVisibility.Compute",
				ERenderGraphPassType::Compute,
				[=](FRHICommandListImmediate& GraphCommands,
					const FRenderGraphPassResources& Resources) {
					FGPUTimingQueryRHIRef TimingQuery;
					if (TimingSink != nullptr && GDynamicRHI != nullptr)
					{
						TimingQuery = GDynamicRHI->RHICreateGPUTimingQuery();
						if (TimingQuery) GraphCommands.BeginGPUTimingQuery(TimingQuery);
					}
					GraphCommands.SwitchPipeline(ERHIPipeline::Compute);
					GraphCommands.SetComputePipelineState(*ComputePayload->PipelineState);
					FContactVisibilityComputeShader::FParameters Parameters;
					Parameters.GBufferMaterial = Resources.GetTexture(GraphMaterial);
					Parameters.GBufferNormals = Resources.GetTexture(GraphNormals);
					Parameters.GBufferSurface = Resources.GetTexture(GraphSurface);
					Parameters.GBufferEmissive = Resources.GetTexture(GraphEmissive);
					Parameters.SceneDepth = Resources.GetTexture(GraphDepth);
					Parameters.Params = {Resources.GetBuffer(GraphUniform),
						UniformBuffer.Offset, UniformBuffer.Size};
					Parameters.ContactVisibilityOutput =
						Resources.GetTexture(GraphVisibility);
					SetShaderParameters(GraphCommands, ComputePayload->ComputeShader,
						Parameters);
					GraphCommands.Dispatch(CalculateGroupCount(Width),
						CalculateGroupCount(Height), 1);
					GraphCommands.SwitchPipeline(ERHIPipeline::Graphics);
					if (TimingQuery)
					{
						GraphCommands.EndGPUTimingQuery(TimingQuery);
						TimingSink(TimingQuery, Decision.Route);
					}
				});
			Graph.UseTexture(Pass, GraphMaterial, GetWholeTextureRange(Material),
				ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead);
			Graph.UseTexture(Pass, GraphNormals, GetWholeTextureRange(Normals),
				ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead);
			Graph.UseTexture(Pass, GraphSurface, GetWholeTextureRange(Surface),
				ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead);
			Graph.UseTexture(Pass, GraphEmissive, GetWholeTextureRange(Emissive),
				ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead);
			Graph.UseTexture(Pass, GraphDepth, GetWholeTextureRange(SceneDepth),
				ERenderGraphUse::Read, ERHIAccess::ComputeShaderRead);
			Graph.UseTexture(Pass, GraphVisibility,
				GetWholeTextureRange(ComputeTargets->Visibility),
				ERenderGraphUse::ReadWrite, ERHIAccess::ComputeShaderReadWrite, true);
			Graph.UseBuffer(Pass, GraphUniform, UniformBuffer.Offset,
				UniformBuffer.Size, ERenderGraphUse::Read,
				ERHIAccess::ComputeUniformRead);
			auto Compiled = Graph.Compile();
			if (!Compiled.IsSuccess())
			{
				DURIN_WARN("Contact visibility graph compilation failed: {}",
					Compiled.Error);
				return {.Route = ERoute::FactorOne,
					.Reason = ERouteReason::InvalidInputs};
			}
			Compiled.Graph->Execute(CommandList);
			return {.Visibility = ComputeTargets->Visibility,
				.Route = Decision.Route, .Reason = Decision.Reason};
		}

		FRenderGraphBuilder Graph;
		Graph.SetBudget(ContactFragmentGraphBudget);
		const auto GraphMaterial = Graph.ImportTexture("Contact.Material", Material,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto GraphNormals = Graph.ImportTexture("Contact.Normals", Normals,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto GraphSurface = Graph.ImportTexture("Contact.Surface", Surface,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto GraphEmissive = Graph.ImportTexture("Contact.Emissive", Emissive,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto GraphDepth = Graph.ImportTexture("Contact.Depth", SceneDepth,
			ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		const auto GraphVisibility = Graph.CreateTexture("Contact.Visibility",
			FragmentTargets->Visibility, ERHIAccess::GraphicsShaderRead);
		const auto GraphUniform = Graph.ImportBuffer("Contact.Uniform",
			UniformBuffer.Buffer, ERHIAccess::GraphicsUniformRead,
			ERHIAccess::GraphicsUniformRead);
		FRHIBuffer* VertexBuffer = FullscreenGeometry.GetVertexBuffer_RenderThread();
		FRHIBuffer* IndexBuffer = FullscreenGeometry.GetIndexBuffer_RenderThread();
		const auto GraphVertices = Graph.ImportBuffer("Contact.FullscreenVertices",
			VertexBuffer, ERHIAccess::VertexBufferRead, ERHIAccess::VertexBufferRead);
		const auto GraphIndices = Graph.ImportBuffer("Contact.FullscreenIndices",
			IndexBuffer, ERHIAccess::IndexBufferRead, ERHIAccess::IndexBufferRead);
		const auto Pass = Graph.AddPass("ContactVisibility.Fragment",
			ERenderGraphPassType::Graphics,
			[=, &View](FRHICommandListImmediate& GraphCommands,
				const FRenderGraphPassResources& Resources) {
				FGPUTimingQueryRHIRef TimingQuery;
				if (TimingSink != nullptr && GDynamicRHI != nullptr)
				{
					TimingQuery = GDynamicRHI->RHICreateGPUTimingQuery();
					if (TimingQuery) GraphCommands.BeginGPUTimingQuery(TimingQuery);
				}
				FRHIRenderPassInfo PassInfo{};
				PassInfo.RenderTargetLayout =
					RenderTargetLayouts::MakeContactVisibilityOutput();
				PassInfo.ColorRenderTargets[0] = Resources.GetTexture(GraphVisibility);
				PassInfo.ColorClearValues[0] =
					FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f);
				GraphCommands.BeginRenderPass(PassInfo, "ContactVisibilityRenderPass");
				GraphCommands.SetGraphicsPipelineState(*FragmentPayload->PipelineState);
				GraphCommands.SetViewport(static_cast<float>(View.ViewportX),
					static_cast<float>(View.ViewportY), 0.0f,
					static_cast<float>(View.ViewportX + View.ViewportWidth),
					static_cast<float>(View.ViewportY + View.ViewportHeight), 1.0f);
				GraphCommands.SetScissor(static_cast<float>(View.ViewportX),
					static_cast<float>(View.ViewportY),
					static_cast<float>(View.ViewportWidth),
					static_cast<float>(View.ViewportHeight));
				GraphCommands.BindVertexBuffer(0,
					Resources.GetBuffer(GraphVertices), 0);
				GraphCommands.BindIndexBuffer(Resources.GetBuffer(GraphIndices), 0);
				FContactVisibilityFragmentShader::FParameters Parameters;
				Parameters.GBufferMaterial = Resources.GetTexture(GraphMaterial);
				Parameters.GBufferNormals = Resources.GetTexture(GraphNormals);
				Parameters.GBufferSurface = Resources.GetTexture(GraphSurface);
				Parameters.GBufferEmissive = Resources.GetTexture(GraphEmissive);
				Parameters.SceneDepth = Resources.GetTexture(GraphDepth);
				Parameters.Params = {Resources.GetBuffer(GraphUniform),
					UniformBuffer.Offset, UniformBuffer.Size};
				SetShaderParameters(GraphCommands, FragmentPayload->FragmentShader,
					Parameters);
				GraphCommands.DrawIndexed(3, 0, 0);
				GraphCommands.EndRenderPass();
				if (TimingQuery)
				{
					GraphCommands.EndGPUTimingQuery(TimingQuery);
					TimingSink(TimingQuery, Decision.Route);
				}
			});
		Graph.UseTexture(Pass, GraphMaterial, GetWholeTextureRange(Material),
			ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		Graph.UseTexture(Pass, GraphNormals, GetWholeTextureRange(Normals),
			ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		Graph.UseTexture(Pass, GraphSurface, GetWholeTextureRange(Surface),
			ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		Graph.UseTexture(Pass, GraphEmissive, GetWholeTextureRange(Emissive),
			ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		Graph.UseTexture(Pass, GraphDepth, GetWholeTextureRange(SceneDepth),
			ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		Graph.UseColorAttachment(Pass, GraphVisibility,
			GetWholeTextureRange(FragmentTargets->Visibility),
			ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::Store);
		Graph.UseBuffer(Pass, GraphUniform, UniformBuffer.Offset,
			UniformBuffer.Size, ERenderGraphUse::Read,
			ERHIAccess::GraphicsUniformRead);
		Graph.UseBuffer(Pass, GraphVertices, 0, VertexBuffer->GetSize(),
			ERenderGraphUse::Read, ERHIAccess::VertexBufferRead);
		Graph.UseBuffer(Pass, GraphIndices, 0, IndexBuffer->GetSize(),
			ERenderGraphUse::Read, ERHIAccess::IndexBufferRead);
		auto Compiled = Graph.Compile();
		if (!Compiled.IsSuccess())
		{
			DURIN_WARN("Contact visibility graph compilation failed: {}",
				Compiled.Error);
			return {.Route = ERoute::FactorOne,
				.Reason = ERouteReason::InvalidInputs};
		}
		Compiled.Graph->Execute(CommandList);
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
