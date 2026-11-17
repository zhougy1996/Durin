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
			FComputePipelineStateRef Pipeline;
		};
		TRenderResourceCreationSlot<FGPUCullingPayload> GPUCulling{
			ERenderResourceGenerationDependency::Shader
			| ERenderResourceGenerationDependency::Device
		};
	};
	FInstanceCullingManager::FInstanceCullingManager(FRendererResourceCoordinator& InCoordinator)
		: Coordinator(InCoordinator)
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
		if (Failure == EGPUCullingFailure::ShaderUnavailable || Failure == EGPUCullingFailure::PipelineUnavailable) return false;
		using FPayload = FState::FGPUCullingPayload;
		using FResult = TRenderResourceCreateResult<FPayload>;
		return State->GPUCulling.Resolve(
				   Coordinator.GetGeneration_RenderThread(), []() -> FResult {
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
		CommandList.SetComputePipelineState(Payload->Pipeline);
		FInstanceCullingComputeShader::FParameters Parameters;
		Parameters.Params = UniformBuffer;
		SetShaderParameters(CommandList, Payload->Shader, GraphShaderParameters, Parameters);
		CommandList.Dispatch((CandidateCount + 63u) / 64u, 1, 1);
		CommandList.SwitchPipeline(ERHIPipeline::Graphics);
		return true;
	}

	FInstanceCullingContext::FInstanceCullingContext(std::shared_ptr<const FInstanceCullingView> InView, RendererPrivate::FMeshPassKey InPass, FInstanceCullingManager& InManager)
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
		auto Context = std::make_shared<FInstanceCullingContext>(Views[ViewId], Pass, *this);
		return Context;
	}
	auto FInstanceCullingContext::SetupDrawCommands(RendererPrivate::FMeshCommandOneFrameArray& VisibleMeshDrawCommands,
		std::span<const RendererPrivate::FMeshDrawCommandCullingPayload> Sources, size_t* OutOverflowCandidates, size_t MaximumGeneratedBytes,
		FInstanceCullingAdmissionDiagnostics* OutDiagnostics) -> bool
	{
		require(!bBuilt);
		Candidates.clear(); Arguments.clear(); InstanceSceneData = {}; DynamicContexts.clear();
		MeshDrawCommandInfos.assign(VisibleMeshDrawCommands.size(), {});
		FallbackCommands.clear(); bDispatched = false;
		SceneCapacityBytes = StandaloneSceneCapacityBytes = 0;
		if (OutOverflowCandidates) *OutOverflowCandidates = 0;
		FInstanceCullingAdmissionDiagnostics Diagnostics;
		if (OutDiagnostics) *OutDiagnostics = {};

		std::vector<bool> Admitted(VisibleMeshDrawCommands.size());
		for (uint32 Index = 0; Index < VisibleMeshDrawCommands.size(); ++Index)
		{
			const auto& Visible = VisibleMeshDrawCommands[Index];
			const size_t NumSourceInstances = Visible.NumSourceDraws;
			if (Visible.CachedInfo && Visible.CachedInfo->CommandIndex == INDEX_NONE)
			{ Diagnostics.PreparationUnavailableCandidates += NumSourceInstances; continue; }
			const auto* Command = Visible.Command;
			if (!NumSourceInstances || Visible.SourceDrawIndex > Sources.size() || NumSourceInstances > Sources.size() - Visible.SourceDrawIndex)
			{ Diagnostics.PreparationUnavailableCandidates += NumSourceInstances; continue; }
			const auto DrawSources = Sources.subspan(Visible.SourceDrawIndex, NumSourceInstances);
			if (!Command || !Command->IndexBuffer || !std::holds_alternative<FRHIDrawIndexedArguments>(Command->DrawParameters)
				|| std::ranges::any_of(DrawSources, [](const auto& Source) { return !Source.bSupportsInstanceCulling; }))
			{ Diagnostics.IncompatibleCandidates += NumSourceInstances; continue; }
			if (!Visible.Pass || !Command->Pipeline || !Command->FragmentBindings
				|| std::ranges::any_of(DrawSources, [](const auto& Payload) {
					const auto& Source = Payload.SceneData;
					const auto Buffer = Source.Buffers ? Source.Buffers->SceneData[1] : FBufferRHIRef{};
					return !Buffer || Source.InstanceId >= Buffer->GetSize() / sizeof(FInstanceSceneShaderData);
				}))
			{ Diagnostics.PreparationUnavailableCandidates += NumSourceInstances; continue; }
			const auto& Source = DrawSources.front().SceneData;
			const auto Buffer = Source.Buffers->SceneData[1];
			if ((InstanceSceneData && InstanceSceneData != Buffer)
				|| std::ranges::any_of(DrawSources, [&](const auto& Payload) { return Payload.SceneData.Buffers->SceneData[1] != Buffer; }))
			{ Diagnostics.IncompatibleCandidates += NumSourceInstances; continue; }
			if (!InstanceSceneData)
			{
				InstanceSceneData = Buffer;
				auto& Capacity = Source.Buffers->bStandaloneScene ? StandaloneSceneCapacityBytes : SceneCapacityBytes;
				for (const auto& Resource : Source.Buffers->SceneData) if (Resource) Capacity += Resource->GetSize();
			}
			for (const auto& Payload : DrawSources)
				if (const auto& Owner = Payload.SceneData.Buffers->DynamicContext; Owner
					&& std::ranges::find(DynamicContexts, Owner) == DynamicContexts.end()) DynamicContexts.push_back(Owner);
			Admitted[Index] = true;
		}
		constexpr size_t PerCandidateBytes = sizeof(FInstanceCullingCandidate) + sizeof(uint32);
		const auto Runs = RendererPrivate::BuildMeshDrawCommandRuns(VisibleMeshDrawCommands, Sources);
		for (const auto& Run : Runs)
		{
			const auto Group = Run.GetDrawIndices();
			size_t NumInstances = 0;
			for (uint32 Index : Group) NumInstances += VisibleMeshDrawCommands[Index].NumSourceDraws;
			if (std::ranges::any_of(Group, [&](uint32 Index) { return !Admitted[Index]; })) continue;
			if (NumInstances < MinimumGroupCandidates) { Diagnostics.BelowThresholdCandidates += NumInstances; continue; }
			const size_t RequiredBytes = NumInstances * PerCandidateBytes + sizeof(FRHIDrawIndexedIndirectArguments);
			const size_t CurrentBytes = Candidates.size() * PerCandidateBytes + Arguments.size() * sizeof(FRHIDrawIndexedIndirectArguments);
			if (NumInstances > MaximumCandidates - Candidates.size()
				|| RequiredBytes > std::min(MaximumGeneratedBytes, FInstanceCullingContext::MaximumGeneratedBytes) - CurrentBytes)
			{ if (OutOverflowCandidates) *OutOverflowCandidates += NumInstances; continue; }
			const uint32 ArgumentIndex = static_cast<uint32>(Arguments.size());
			MeshDrawCommandInfos[Run.FirstCommand].ArgumentIndex = ArgumentIndex;
			const auto Direct = std::get<FRHIDrawIndexedArguments>(VisibleMeshDrawCommands[Group.front()].Command->DrawParameters);
			Arguments.push_back({.IndexCount = Direct.IndexCount, .InstanceCount = 0, .FirstIndex = Direct.FirstIndex,
				.VertexOffset = Direct.VertexOffset, .FirstInstance = static_cast<uint32>(Candidates.size())});
			for (size_t Index = 0; Index < Group.size(); ++Index)
			{
				const auto& Item = Group[Index];
				const auto& Visible = VisibleMeshDrawCommands[Item];
				for (const auto& Source : Sources.subspan(Visible.SourceDrawIndex, Visible.NumSourceDraws))
					Candidates.push_back({Source.SceneData.InstanceId, ArgumentIndex});
			}
		}
		// Compact admitted GPU groups in the visible array. Submission addresses final commands,
		// never group members. Preserve originals solely for failed-dispatch direct fallback.
		size_t Output = 0, RunIndex = 0;
		for (uint32 First = 0; First < VisibleMeshDrawCommands.size();)
		{
			while (RunIndex < Runs.size() && Runs[RunIndex].FirstCommand < First) ++RunIndex;
			const uint32 Count = MeshDrawCommandInfos[First].ArgumentIndex != InvalidArgument
				? Runs[RunIndex].NumCommands : 1;
			auto Info = MeshDrawCommandInfos[First];
			auto Leader = VisibleMeshDrawCommands[First];
			if (Count > 1)
			{
				Info.Fallback = {static_cast<uint32>(FallbackCommands.size()), Count};
				Leader.NumSourceDraws = 0;
				for (uint32 Index = First; Index < First + Count; ++Index)
				{
					FallbackCommands.push_back(VisibleMeshDrawCommands[Index]);
					Leader.NumSourceDraws += VisibleMeshDrawCommands[Index].NumSourceDraws;
					Leader.bSourceReady &= VisibleMeshDrawCommands[Index].bSourceReady;
				}
			}
			VisibleMeshDrawCommands[Output] = Leader;
			MeshDrawCommandInfos[Output++] = Info;
			First += Count;
		}
		VisibleMeshDrawCommands.resize(Output); MeshDrawCommandInfos.resize(Output);
		require(VisibleMeshDrawCommands.size() == MeshDrawCommandInfos.size());
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


	auto FInstanceCullingDrawParams::GetRDGParametersMetadata() -> const FRDGParametersMetadata*
	{
		using P = FInstanceCullingDrawParams;
		static const std::array Members{
			MakeRDGNestedParameterMemberMetadata<P, decltype(Scene)>("Scene", offsetof(P, Scene), FGPUSceneResourceParameters::GetRDGParametersMetadata()),
			MakeRDGResourceParameterMemberMetadata<P, decltype(InstanceIdsBuffer), FRDGBufferParameter>("InstanceIdsBuffer", offsetof(P, InstanceIdsBuffer), ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer, ERDGParameterRangeKind::BufferBytes, ERDGUse::Read, ERHIAccess::GraphicsShaderRead),
			MakeRDGResourceParameterMemberMetadata<P, decltype(DynamicInstanceIdsBuffer), FRDGBufferParameter>("DynamicInstanceIdsBuffer", offsetof(P, DynamicInstanceIdsBuffer), ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer, ERDGParameterRangeKind::BufferBytes, ERDGUse::Read, ERHIAccess::GraphicsShaderRead),
			MakeRDGResourceParameterMemberMetadata<P, decltype(InstanceIdOffsetBuffer), FRDGBufferParameter>("InstanceIdOffsetBuffer", offsetof(P, InstanceIdOffsetBuffer), ERDGParameterMemberKind::Buffer, ERDGResourceKind::Buffer, ERDGParameterRangeKind::BufferBytes, ERDGUse::Read, ERHIAccess::GraphicsShaderRead),
			MakeRDGIndirectArgumentMetadata<P, decltype(DrawIndirectArgsBuffer)>("DrawIndirectArgsBuffer", offsetof(P, DrawIndirectArgsBuffer))};
		static const auto Metadata = MakeInlineRDGParametersMetadata<P>("FInstanceCullingDrawParams", Members);
		return &Metadata;
	}
	auto GetMeshDrawCommandOverrideArgs(const FInstanceCullingDrawParams& P, const FRDGParameterResolver& Resolver) -> RendererPrivate::FMeshDrawCommandOverrideArgs
	{
		require(!P.InstanceIdOffsetBuffer || P.InstanceDataByteOffset <= P.InstanceIdOffsetBuffer->Size);
		require(!P.DrawIndirectArgsBuffer || P.IndirectArgsByteOffset <= P.DrawIndirectArgsBuffer->Size);
		const uint64 InstanceOffset = P.InstanceDataByteOffset + (P.InstanceIdOffsetBuffer ? P.InstanceIdOffsetBuffer->Offset : uint64(0));
		require(InstanceOffset <= UINT32_MAX && InstanceOffset % sizeof(uint32) == 0);
		const uint64 ArgumentOffset = P.DrawIndirectArgsBuffer ? P.DrawIndirectArgsBuffer->Offset : 0;
		require(P.IndirectArgsByteOffset <= UINT64_MAX - ArgumentOffset);
		return {.InstanceBuffer = P.InstanceIdOffsetBuffer ? Resolver.GetBuffer(*P.InstanceIdOffsetBuffer) : nullptr,
			.IndirectArgsBuffer = P.DrawIndirectArgsBuffer ? Resolver.GetBuffer(*P.DrawIndirectArgsBuffer) : nullptr,
			.InstanceDataByteOffset = static_cast<uint32>(InstanceOffset), .IndirectArgsByteOffset = P.IndirectArgsByteOffset + ArgumentOffset};
	}

	auto FInstanceCullingContext::BuildRenderingCommands(FRDGBuilder& Graph, FInstanceCullingTelemetry& Telemetry, bool bAsyncCompute, bool bObserveVisibility, EGPUCullingFailure Failure) -> FInstanceCullingDrawParams
	{
		require(!bBuilt);
		bBuilt = true;
		FInstanceCullingDrawParams Result;
		Telemetry.GPUCullingSceneCapacityBytes += SceneCapacityBytes;
		Telemetry.GPUCullingStandaloneSceneCapacityBytes += StandaloneSceneCapacityBytes;
		if (Candidates.empty()) return Result;
		auto& CullingManager = Manager;
		auto GPUCullingPlan = shared_from_this();
		static constexpr std::array PassNames{"Forward", "HybridRetainedForward", "SortedTranslucency", "ShadowDepth", "GBuffer", "HitProxy"};
		const auto PassName = std::format("Scene.{}.GPUCulling", PassNames[static_cast<size_t>(Pass.Pass)]);
		FRDGBufferHandle VisibleInstances, IndirectArguments;
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
		Graph.QueueBufferUploadOwned(VisibleInstances, 0, FByteBuffer(VisibleBytes));
		Graph.QueueBufferUpload(IndirectArguments, 0, std::as_bytes(std::span(GPUCullingPlan->Arguments)));
		{
			auto CullParameters = Graph.AllocParameters<
				FInstanceCullingPassParameters>();
			CullParameters->Candidates = {CandidateBuffer, 0, CandidateBytes};
			CullParameters->VisibleInstances = {VisibleInstances, 0, VisibleBytes};
			CullParameters->Arguments = {IndirectArguments, 0, ArgumentBytes};
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
					++Telemetry.GPUCullingDispatchFailureViews;
				}
			});
			if (bAsyncCompute) Graph.SetPassAsyncComputeEligible(CullPass);
		}
		Result.InstanceIdOffsetBuffer = FRDGBufferParameter{VisibleInstances, 0, VisibleBytes};
		Result.DrawIndirectArgsBuffer = FRDGBufferParameter{IndirectArguments, 0, ArgumentBytes};
		return Result;
	}

	auto FInstanceCullingContext::SubmitDrawCommands(std::span<const RendererPrivate::FVisibleMeshDrawCommand> VisibleMeshDrawCommands,
		const RendererPrivate::FMeshDrawCommandOverrideArgs& OverrideArgs, uint32 StartIndex, uint32 NumMeshDrawCommands,
		FRHICommandList& Commands, FMeshDrawCommandStateCache* StateCache, RendererPrivate::FMeshDrawCommandPassStats* Statistics) const -> RendererPrivate::FMeshDrawCommandPassResult
	{
		RendererPrivate::FMeshDrawCommandPassResult Result;
		if (StartIndex > VisibleMeshDrawCommands.size() || NumMeshDrawCommands > VisibleMeshDrawCommands.size() - StartIndex
			|| MeshDrawCommandInfos.size() != VisibleMeshDrawCommands.size()
			|| OverrideArgs.InstanceDataByteOffset % sizeof(uint32) != 0)
		{ Result.bComplete = false; return Result; }
		FMeshDrawCommandStateCache LocalCache;
		auto& Cache = StateCache ? *StateCache : LocalCache;
		for (uint32 DrawIndex = StartIndex; DrawIndex < StartIndex + NumMeshDrawCommands; ++DrawIndex)
		{
			const auto Index = GetArgumentIndex(DrawIndex);
			const auto& Visible = VisibleMeshDrawCommands[DrawIndex];
			if (Index == InvalidArgument && MeshDrawCommandInfos[DrawIndex].Fallback.NumCommands)
			{
				for (uint32 Source : MeshDrawCommandInfos[DrawIndex].Fallback.GetDrawIndices())
				{
					const auto& Original = FallbackCommands[Source];
					const uint32 Before = Cache.DrawCalls;
					const bool Submitted = Original.SubmitDraw(Commands, &Cache);
					Result.Record(Original.NumSourceDraws, Submitted, Cache.DrawCalls - Before, false);
					if (Statistics) Statistics->Record(Original.SourceDrawIndex, Original.NumSourceDraws, Submitted, Cache.DrawCalls - Before, false);
				}
				continue;
			}
			const uint32 Before = Cache.DrawCalls;
			auto Submit = [&]() -> bool {
				for (uint32 Source : MeshDrawCommandInfos[DrawIndex].Fallback.GetDrawIndices())
					if (const auto& Original = FallbackCommands[Source]; !Original.bSourceReady
						|| (Original.CachedInfo && Original.CachedInfo->CommandIndex == INDEX_NONE)) return false;
				if (Index == InvalidArgument) return Visible.SubmitDraw(Commands, &Cache);
				if (!OverrideArgs.IndirectArgsBuffer || !OverrideArgs.InstanceBuffer
					|| !EnumHasAnyFlags(OverrideArgs.IndirectArgsBuffer->GetUsage(), EBufferUsageFlags::DrawIndirect)) return false;
				const uint64 Size = OverrideArgs.IndirectArgsBuffer->GetSize();
				const uint64 RelativeOffset = uint64(Index) * sizeof(FRHIDrawIndexedIndirectArguments);
				if (OverrideArgs.IndirectArgsByteOffset > Size || RelativeOffset > Size - OverrideArgs.IndirectArgsByteOffset
					|| sizeof(FRHIDrawIndexedIndirectArguments) > Size - OverrideArgs.IndirectArgsByteOffset - RelativeOffset) return false;
				return Visible.SubmitDraw(Commands, &Cache,
					{.InstanceBuffer = OverrideArgs.InstanceBuffer, .IndirectArgsBuffer = OverrideArgs.IndirectArgsBuffer,
						.InstanceDataByteOffset = OverrideArgs.InstanceDataByteOffset,
						.IndirectArgsByteOffset = OverrideArgs.IndirectArgsByteOffset + RelativeOffset});
			};
			const bool Submitted = Submit();
			Result.Record(Visible.NumSourceDraws, Submitted, Cache.DrawCalls - Before, Index != InvalidArgument);
			if (Statistics) Statistics->Record(Visible.SourceDrawIndex, Visible.NumSourceDraws, Submitted, Cache.DrawCalls - Before, Index != InvalidArgument);
		}
		return Result;
	}
}
