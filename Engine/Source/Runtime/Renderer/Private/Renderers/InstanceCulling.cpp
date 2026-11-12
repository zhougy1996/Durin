#include "Renderers/InstanceCulling.h"
#include "RendererResourceSlotCache.h"
#include "Resources/RendererResourceCoordinator.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Renderers/MeshRendererShared.h"
#include "Shader/GlobalShader.h"
#include "RenderResourcePreparation.h"
#include "Renderers/MeshPassProcessor.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/RendererRDGAllocator.h"
#include "Renderers/ViewRenderTelemetry.h"
#include "InstanceUniformShaderParameters.h"
#include "SceneView.h"

namespace Durin
{
	namespace
	{
		class FInstanceCullingComputeShader final : public FGlobalShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FInstanceCullingComputeShader)
				DURIN_SHADER_PARAMETER_GRAPH_STORAGE_BUFFER(Candidates);
				DURIN_SHADER_PARAMETER_GRAPH_STORAGE_BUFFER(VisibleInstances);
				DURIN_SHADER_PARAMETER_GRAPH_STORAGE_BUFFER(IndirectArguments);
				DURIN_SHADER_PARAMETER_GRAPH_STORAGE_BUFFER(InstanceSceneData);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER(Params);
			DURIN_END_SHADER_PARAMETERS();
			DURIN_DECLARE_GLOBAL_SHADER(FInstanceCullingComputeShader, FGlobalShader, "/Engine/InstanceCulling", EShaderFrequency::Compute, "CullMain");
		};
		DURIN_IMPLEMENT_GLOBAL_SHADER(FInstanceCullingComputeShader);
		const FGlobalShaderSetRegistration GInstanceCullingShaderSet(
			"Renderer", "InstanceCulling", EShaderRequestEligibility::GameAndEditor, {&FInstanceCullingComputeShader::StaticType()}
		);

	} // namespace
	struct FInstanceCullingManager::FState
	{
		struct FGPUCullingPayload
		{
			FGlobalShaderSetRef ShaderSet;
			TShaderMapRef<FInstanceCullingComputeShader> Shader;
			FComputePipelineStateRHIRef Pipeline;
		};
		TRenderResourceCreationSlot<FGPUCullingPayload> GPUCulling{
			ERenderResourceGenerationDependency::Shader
			| ERenderResourceGenerationDependency::Device
		};
	};
	FInstanceCullingManager::FInstanceCullingManager()
		: State(std::make_unique<FState>())
	{
	}
	FInstanceCullingManager::FInstanceCullingManager(FRendererResourceCoordinator& InCoordinator)
		: Coordinator(&InCoordinator)
		, State(std::make_unique<FState>())
	{
	}
	FInstanceCullingManager::~FInstanceCullingManager() = default;
	auto FInstanceCullingManager::ResetViews() -> void
	{
		Views.clear();
	}
	auto FInstanceCullingManager::ReleaseResources_RenderThread() -> void
	{
		ResetViews();
		State->GPUCulling.Reset();
	}
	auto FInstanceCullingManager::EnsureResources_RenderThread(EGPUCullingFailure Failure) -> bool
	{
		require(Coordinator);
		if (Failure == EGPUCullingFailure::ShaderUnavailable) return false;
		using FPayload = FState::FGPUCullingPayload;
		using FResult = TRenderResourceCreateResult<FPayload>;
		return State->GPUCulling.Resolve(
				   Coordinator->GetGeneration_RenderThread(), []() -> FResult {
					   const std::array<const FGlobalShaderType*, 1> Types{
						   &FInstanceCullingComputeShader::StaticType()
					   };
					   FPayload Candidate;
					   const auto ShaderSetCompletion = GetGlobalShaderMap().ResolveShaderSet(
						   "InstanceCulling", Types, true,
						   ReportRendererResourceCreateDiagnostic
					   );
					   if (ShaderSetCompletion.Status == EShaderBuildStatus::Pending) return FResult::Pending();
					   if (ShaderSetCompletion.Status == EShaderBuildStatus::Canceled) return FResult::Canceled();
					   if (!ShaderSetCompletion)
						   return FResult::Failure(MakeRendererResourceCreateError(
							   ERenderResourceCreateErrorCategory::ShaderCompile,
							   "InstanceCulling", "shader",
							   ERenderResourceCreateErrorReason::GlobalShaderUnavailable,
							   ERenderResourceGenerationDependency::Shader
								   | ERenderResourceGenerationDependency::Manual
						   ));
					   Candidate.ShaderSet = *ShaderSetCompletion;
					   Candidate.Shader = TShaderMapRef<FInstanceCullingComputeShader>(
						   Candidate.ShaderSet
					   );
					   FRHIShader* ComputeRHI = Candidate.Shader.GetRHIShader(false);
					   if (ComputeRHI == nullptr)
						   return FResult::Pending();
					   FComputePipelineStateInitializer Initializer;
					   Initializer.ComputeShader = ComputeRHI;
					   Initializer.PipelineLayout = Candidate.ShaderSet.GetPipelineLayout();
					   Candidate.Pipeline = FRenderPipelineRequestScope::Compute(
						   "InstanceCullingPipeline", Initializer
					   );
					   if (Candidate.Pipeline == nullptr)
						   return FResult::Failure(MakeRendererResourceCreateError(
							   ERenderResourceCreateErrorCategory::GraphicsPipeline,
							   "InstanceCulling", "pipeline",
							   ERenderResourceCreateErrorReason::PipelinePreparationRejected,
							   ERenderResourceGenerationDependency::Shader
								   | ERenderResourceGenerationDependency::Device
								   | ERenderResourceGenerationDependency::Manual
						   ));
					   return FResult::Success(std::move(Candidate));
				   },
				   ReportRendererResourceCreateDiagnosticUnlessGlobalShaderUnavailable
			   )
			   != nullptr;
	}

	auto FInstanceCullingManager::Dispatch_RenderThread(
		FRHICommandListImmediate& CommandList, const FVector3& ViewLocation, const std::optional<FViewFrustum>& Frustum, uint32 CandidateCount, const FRDGShaderParameterScope& GraphShaderParameters, EGPUCullingFailure Failure
	) -> bool
	{
		check(!CommandList.IsInsideRenderPass());
		if (Failure == EGPUCullingFailure::ShaderUnavailable
			|| Failure == EGPUCullingFailure::PipelineUnavailable
			|| Failure == EGPUCullingFailure::CommandAdmission) return false;
		auto* Payload = State->GPUCulling.GetPayload();
		if (Payload == nullptr || CandidateCount == 0) return false;
		FInstanceCullingUniform Uniform;
		const FVector3f OriginHigh(ViewLocation);
		Uniform.WorldOriginAndRelativeTolerance = FVector4f(OriginHigh, PlaneTouchRelativeEpsilon);
		Uniform.ViewOriginLow = FVector4f(FVector3f(ViewLocation - FVector3(OriginHigh)), 0.f);
		if (!Math::IsFinite(Uniform.WorldOriginAndRelativeTolerance) || !Math::IsFinite(Uniform.ViewOriginLow)) return false;
		if (Frustum)
		{
			if (!Math::IsFinite(Uniform.WorldOriginAndRelativeTolerance)) return false;
			for (const FVector4& Plane : Frustum->Planes)
			{
				const FVector3 Normal(Plane);
				const FVector4f TranslatedPlane(FVector4(Normal, Plane.w + Math::Dot(Normal, ViewLocation)));
				if (!Math::IsFinite(TranslatedPlane)) return false;
				Uniform.Planes[Uniform.PlaneCount++] = TranslatedPlane;
			}
		}
		Uniform.CandidateCount = CandidateCount;
		const auto UniformBuffer = CommandList.CreateUniformBufferRange(
			&Uniform, sizeof(Uniform)
		);
		if (!UniformBuffer.Buffer) return false;
		CommandList.SwitchPipeline(ERHIPipeline::Compute);
		CommandList.SetComputePipelineState(*Payload->Pipeline);
		FInstanceCullingComputeShader::FParameters Parameters;
		Parameters.Params = UniformBuffer;
		SetShaderParameters(CommandList, Payload->Shader, GraphShaderParameters, Parameters);
		CommandList.Dispatch((CandidateCount + 63u) / 64u, 1, 1);
		CommandList.SwitchPipeline(ERHIPipeline::Graphics);
		return true;
	}

	FInstanceCullingContext::FInstanceCullingContext(std::shared_ptr<const FInstanceCullingView> InView, RendererPrivate::FMeshPassKey InPass, FInstanceCullingManager* InManager)
		: Manager(InManager)
		, View(std::move(InView))
		, Pass(InPass)
	{
		require(View);
	}
	auto FInstanceCullingManager::RegisterView(const FSceneView& View, std::optional<FViewFrustum> Frustum) -> uint32
	{
		require(Views.size() < UINT32_MAX);
		Views.push_back(std::make_shared<const FInstanceCullingView>(FInstanceCullingView{View.ViewLocation, std::move(Frustum)}));
		return static_cast<uint32>(Views.size() - 1);
	}
	auto FInstanceCullingManager::CreateContext(uint32 ViewId, RendererPrivate::FMeshPassKey Pass) -> std::shared_ptr<FInstanceCullingContext>
	{
		require(ViewId < Views.size());
		auto Context = std::make_shared<FInstanceCullingContext>(Views[ViewId], Pass, this);
		return Context;
	}
	auto FInstanceCullingContext::SetupDrawCommands(std::span<const RendererPrivate::FVisibleMeshDrawCommand> VisibleMeshDrawCommands,
		bool bRequested, bool bIndirectDrawSupported, size_t* OutOverflowCandidates, size_t MaximumGeneratedBytes,
		FInstanceCullingAdmissionDiagnostics* OutDiagnostics) -> bool
	{
		require(!bBuilt);
		Candidates.clear(); Arguments.clear(); InstanceSceneData = {}; DynamicContexts.clear(); bDispatched = false;
		ArgumentByResolvedDraw.assign(VisibleMeshDrawCommands.size(), InvalidArgument);
		SceneCapacityBytes = StandaloneSceneCapacityBytes = 0;
		if (OutOverflowCandidates) *OutOverflowCandidates = 0;
		FInstanceCullingAdmissionDiagnostics Diagnostics;
		if (OutDiagnostics) *OutDiagnostics = {};
		if (!bRequested || !bIndirectDrawSupported) return false;
		struct FPendingCandidate { uint32 DrawIndex; };
		std::vector<std::vector<FPendingCandidate>> Groups;
		for (uint32 Index = 0; Index < VisibleMeshDrawCommands.size(); ++Index)
		{
			const auto& Visible = VisibleMeshDrawCommands[Index];
			const auto* Command = Visible.Command;
			if (!Visible.bSupportsInstanceCulling || !Command || !Command->IndexBuffer
				|| !std::holds_alternative<FRHIDrawIndexedArguments>(Command->DrawParameters))
			{ ++Diagnostics.IncompatibleCandidates; continue; }
			const auto& Source = Visible.SceneData;
			const auto Buffer = Source.Buffers ? Source.Buffers->SceneData[1] : FBufferRHIRef{};
			if (!Visible.Pass || !Command->Pipeline || !Command->FragmentBindings || !Buffer
				|| Source.InstanceId >= Buffer->GetSize() / sizeof(FInstanceSceneShaderData))
			{ ++Diagnostics.PreparationUnavailableCandidates; continue; }
			// A context consumes one scene allocation domain. Cross-scene draws
			// require separate contexts, never a shader-side owner selector.
			if (InstanceSceneData && InstanceSceneData != Buffer) { ++Diagnostics.IncompatibleCandidates; continue; }
			if (!InstanceSceneData)
			{
				InstanceSceneData = Buffer;
				auto& Capacity = Source.Buffers->bStandaloneScene ? StandaloneSceneCapacityBytes : SceneCapacityBytes;
				for (const auto& Resource : Source.Buffers->SceneData) if (Resource) Capacity += Resource->GetSize();
			}
			if (Source.Buffers->DynamicContext && std::ranges::find(DynamicContexts, Source.Buffers->DynamicContext) == DynamicContexts.end())
				DynamicContexts.push_back(Source.Buffers->DynamicContext);
			bool bContinue = false;
			if (!Groups.empty() && Groups.back().back().DrawIndex + 1 == Index)
			{
				const auto& Previous = VisibleMeshDrawCommands[Groups.back().back().DrawIndex];
				bContinue = Command->MatchesForDynamicInstancing(*Previous.Command)
					&& Source.Buffers->SceneData == Previous.SceneData.Buffers->SceneData
					&& Visible.DepthBias == Previous.DepthBias && Visible.Pass->Vertex == Previous.Pass->Vertex
					&& Visible.Pass->Fragment == Previous.Pass->Fragment;
			}
			if (!bContinue) Groups.emplace_back();
			Groups.back().push_back({Index});
		}
		constexpr size_t PerCandidateBytes = sizeof(FInstanceCullingCandidate) + sizeof(uint32);
		for (const auto& Group : Groups)
		{
			if (Group.size() < MinimumGroupCandidates) { Diagnostics.BelowThresholdCandidates += Group.size(); continue; }
			const size_t RequiredBytes = Group.size() * PerCandidateBytes + sizeof(FRHIDrawIndexedIndirectArguments);
			const size_t CurrentBytes = Candidates.size() * PerCandidateBytes + Arguments.size() * sizeof(FRHIDrawIndexedIndirectArguments);
			if (Group.size() > MaximumCandidates - Candidates.size()
				|| RequiredBytes > std::min(MaximumGeneratedBytes, FInstanceCullingContext::MaximumGeneratedBytes) - CurrentBytes)
			{ if (OutOverflowCandidates) *OutOverflowCandidates += Group.size(); continue; }
			const uint32 ArgumentIndex = static_cast<uint32>(Arguments.size());
			const auto Direct = std::get<FRHIDrawIndexedArguments>(VisibleMeshDrawCommands[Group.front().DrawIndex].Command->DrawParameters);
			Arguments.push_back({.IndexCount = Direct.IndexCount, .InstanceCount = 0, .FirstIndex = Direct.FirstIndex,
				.VertexOffset = Direct.VertexOffset, .FirstInstance = static_cast<uint32>(Candidates.size())});
			for (size_t Index = 0; Index < Group.size(); ++Index)
			{
				const auto& Item = Group[Index];
				ArgumentByResolvedDraw[Item.DrawIndex] = Index == 0 ? ArgumentIndex : GroupedMember;
				Candidates.push_back({VisibleMeshDrawCommands[Item.DrawIndex].SceneData.InstanceId, ArgumentIndex});
			}
		}
		if (OutDiagnostics) *OutDiagnostics = Diagnostics;
		return !Candidates.empty();
	}

	auto FInstanceCullingPassParameters::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FInstanceCullingPassParameters;
		static const std::array Members{
			WithRDGShaderBinding(MakeRDGResourceParameterMemberMetadata<FParameters, decltype(Candidates), FRDGBufferParameter>("Candidates", offsetof(FParameters, Candidates), ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer, ERDGParameterRangeKind::BufferBytes, ERDGUse::Read, ERHIAccess::ComputeShaderRead), ERHIBindingType::StorageBuffer),
			WithRDGShaderBinding(MakeRDGResourceParameterMemberMetadata<FParameters, decltype(VisibleInstances), FRDGBufferParameter>("VisibleInstances", offsetof(FParameters, VisibleInstances), ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer, ERDGParameterRangeKind::BufferBytes, ERDGUse::ReadWrite, ERHIAccess::ComputeShaderReadWrite), ERHIBindingType::StorageBuffer),
			WithRDGShaderBinding(MakeRDGResourceParameterMemberMetadata<FParameters, decltype(Arguments), FRDGBufferParameter>("IndirectArguments", offsetof(FParameters, Arguments), ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer, ERDGParameterRangeKind::BufferBytes, ERDGUse::ReadWrite, ERHIAccess::ComputeShaderReadWrite), ERHIBindingType::StorageBuffer),
			WithRDGShaderBinding(MakeRDGResourceParameterMemberMetadata<FParameters, decltype(InstanceSceneData), FRDGBufferParameter>("InstanceSceneData", offsetof(FParameters, InstanceSceneData), ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer, ERDGParameterRangeKind::BufferBytes, ERDGUse::Read, ERHIAccess::ComputeShaderRead), ERHIBindingType::StorageBuffer),

		};
		static const auto Metadata = MakeInlineRDGParametersMetadata<FParameters>(
			"FInstanceCullingPassParameters", Members
		);
		return &Metadata;
	}


	auto FInstanceCullingContext::BuildRenderingCommands(FRDGBuilder& Graph, FInstanceCullingTelemetry& Telemetry, bool bAsyncCompute, bool bObserveVisibility, EGPUCullingFailure Failure) -> FInstanceCullingDrawParams
	{
		require(!bBuilt);
		bBuilt = true;
		FInstanceCullingDrawParams Result;
		Telemetry.GPUCullingSceneCapacityBytes += SceneCapacityBytes;
		Telemetry.GPUCullingStandaloneSceneCapacityBytes += StandaloneSceneCapacityBytes;
		if (Candidates.empty()) return Result;
		require(Manager);
		auto& CullingManager = *Manager;
		auto GPUCullingPlan = shared_from_this();
		static constexpr std::array PassNames{"Forward", "HybridRetainedForward", "SortedTranslucency", "ShadowDepth", "GBuffer", "HitProxy"};
		const auto PassName = std::format("Scene.{}.GPUCulling", PassNames[static_cast<size_t>(Pass.Pass)]);
		auto& VisibleInstances = Result.InstanceIdOffsetBuffer;
		auto& IndirectArguments = Result.DrawIndirectArgsBuffer;
		const uint32 CandidateBytes = static_cast<uint32>(
			GPUCullingPlan->Candidates.size()
			* sizeof(FInstanceCullingCandidate)
		);
		const uint32 VisibleBytes = static_cast<uint32>(
			GPUCullingPlan->Candidates.size() * sizeof(uint32)
		);
		const uint32 ArgumentBytes = static_cast<uint32>(
			GPUCullingPlan->Arguments.size()
			* sizeof(FRHIDrawIndexedIndirectArguments)
		);
		Telemetry.GPUCullingCandidates +=
			GPUCullingPlan->Candidates.size();
		Telemetry.GPUCullingGroups +=
			GPUCullingPlan->Arguments.size();
		Telemetry.GPUCullingCommands +=
			GPUCullingPlan->Arguments.size();
		Telemetry.GPUCullingBufferBytes +=
			CandidateBytes + VisibleBytes + ArgumentBytes;
		Telemetry.GPUCullingAsyncComputeEligibleViews +=
			bAsyncCompute ? 1u : 0u;
		const auto CandidateBuffer = Graph.CreateBuffer({.Buffer = FRHIBufferDesc(CandidateBytes, sizeof(FInstanceCullingCandidate), EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::DestinationCopy), .ObservationTag = static_cast<uint32>(ERDGAllocationObservation::GPUCullingCandidates)}, "Scene.GBuffer.GPUCulling.Candidates");
		VisibleInstances = Graph.CreateBuffer({.Buffer = FRHIBufferDesc(VisibleBytes, sizeof(uint32), EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::DestinationCopy), .ObservationTag = static_cast<uint32>(ERDGAllocationObservation::GPUCullingVisible)}, "Scene.GBuffer.GPUCulling.VisibleInstances");
		IndirectArguments = Graph.CreateBuffer({.Buffer = FRHIBufferDesc(ArgumentBytes, sizeof(uint32), EBufferUsageFlags::UnorderedAccess | EBufferUsageFlags::DrawIndirect | (bObserveVisibility ? EBufferUsageFlags::SourceCopy : EBufferUsageFlags::None) | EBufferUsageFlags::DestinationCopy), .ObservationTag = static_cast<uint32>(ERDGAllocationObservation::GPUCullingArguments)}, "Scene.GBuffer.GPUCulling.Arguments");
		Graph.QueueBufferUpload(CandidateBuffer, 0, std::as_bytes(std::span(GPUCullingPlan->Candidates)));
		Graph.QueueBufferUploadOwned(*VisibleInstances, 0, FByteBuffer(VisibleBytes));
		Graph.QueueBufferUpload(*IndirectArguments, 0, std::as_bytes(std::span(GPUCullingPlan->Arguments)));
		{
			auto CullParameters = Graph.AllocParameters<
				FInstanceCullingPassParameters>();
			CullParameters->Candidates = {CandidateBuffer, 0, CandidateBytes};
			CullParameters->VisibleInstances = {*VisibleInstances, 0, VisibleBytes};
			CullParameters->Arguments = {*IndirectArguments, 0, ArgumentBytes};
			auto Import = [&](const FBufferRHIRef& Resource) {
				const auto Handle = Graph.RegisterExternalBuffer(Resource, std::format("GPUScene.Read.{}", reinterpret_cast<uintptr_t>(Resource.GetReference())), ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
				return FRDGBufferParameter{Handle, 0, Resource->GetSize()};
			};
			CullParameters->InstanceSceneData = Import(InstanceSceneData);
			const auto CullPass = Graph.AddPass(PassName, ERDGPassType::Compute, std::move(CullParameters), [Plan = GPUCullingPlan, &CullingManager, &Telemetry, View = View, Failure](FRHICommandListImmediate& Commands, const FInstanceCullingPassParameters& Parameters, const FRDGParameterResolver& Resolver) {
				const auto ShaderParameters = Resolver.GetShaderParameters(Parameters);
				TScopedRendererGPUTimingQuery GPUCullingTiming(
					Commands, GetGPUCullingTimingQuerySink()
				);
				const bool Dispatched = CullingManager.Dispatch_RenderThread(
					Commands, View->Location, View->Frustum, static_cast<uint32>(Plan->Candidates.size()),
					ShaderParameters, Failure
				);
				if (Dispatched) GPUCullingTiming.Commit();
				Plan->bDispatched = Dispatched;
				if (Plan->bDispatched)
				{
					++Telemetry.GPUCullingDispatchedViews;
				}
				else
				{
					++Telemetry.GPUCullingFallbackViews;
					++Telemetry.GPUCullingDispatchFailureViews;
				}
			});
			if (bAsyncCompute) Graph.SetPassAsyncComputeEligible(CullPass);
		}
		return Result;
	}

	auto FInstanceCullingContext::PrepareForSubmission(FResolvedStaticMeshView& Resolved) const -> void
	{
		for (auto& Group : Resolved.DynamicInstancingGroups)
			if (Group.Pass == Pass)
				Group.bEnabled = !bDispatched || std::ranges::none_of(Group.Draws, [&](uint32 Index) {
					return GetArgumentIndex(Index) != InvalidArgument;
				});
	}

	auto FInstanceCullingContext::SubmitDrawCommands(std::span<const RendererPrivate::FVisibleMeshDrawCommand> VisibleMeshDrawCommands,
		const RendererPrivate::FMeshDrawCommandOverrideArgs& OverrideArgs, uint32 StartIndex, uint32 NumMeshDrawCommands,
		FRHICommandList& Commands, FMeshDrawCommandStateCache* StateCache) const -> bool
	{
		if (StartIndex > VisibleMeshDrawCommands.size() || NumMeshDrawCommands > VisibleMeshDrawCommands.size() - StartIndex
			|| OverrideArgs.InstanceDataByteOffset % sizeof(uint32) != 0) return false;
		FMeshDrawCommandStateCache LocalCache;
		auto& Cache = StateCache ? *StateCache : LocalCache;
		for (uint32 DrawIndex = StartIndex; DrawIndex < StartIndex + NumMeshDrawCommands; ++DrawIndex)
		{
			const auto Index = GetArgumentIndex(DrawIndex);
			if (Index == GroupedMember) continue;
			const auto& Visible = VisibleMeshDrawCommands[DrawIndex];
			if (Index == InvalidArgument) { if (!Visible.SubmitDraw(Commands, &Cache)) return false; continue; }
			const auto* Command = Visible.Command;
			if (!Command || !Visible.Pass || !OverrideArgs.IndirectArgsBuffer || !OverrideArgs.InstanceBuffer || !Command->IndexBuffer
				|| !EnumHasAnyFlags(OverrideArgs.IndirectArgsBuffer->GetUsage(), EBufferUsageFlags::DrawIndirect)) return false;
			const uint64 Size = OverrideArgs.IndirectArgsBuffer->GetSize();
			const uint64 RelativeOffset = uint64(Index) * sizeof(FRHIDrawIndexedIndirectArguments);
			if (OverrideArgs.IndirectArgsByteOffset > Size || RelativeOffset > Size - OverrideArgs.IndirectArgsByteOffset
				|| sizeof(FRHIDrawIndexedIndirectArguments) > Size - OverrideArgs.IndirectArgsByteOffset - RelativeOffset) return false;
			const RendererPrivate::FMeshDrawCommandSceneArgs SceneArgs{
				.PrimitiveIdsBuffer = OverrideArgs.InstanceBuffer, .InstanceDataByteOffset = OverrideArgs.InstanceDataByteOffset,
				.IndirectArgsBuffer = OverrideArgs.IndirectArgsBuffer,
				.IndirectArgsByteOffset = OverrideArgs.IndirectArgsByteOffset + RelativeOffset};
			if (!Command->SubmitVisible(Commands, Visible.Pass->Vertex, Visible.Pass->Fragment, Visible.DepthBias, &Cache, SceneArgs)) return false;
		}
		return true;
	}
} // namespace Durin
