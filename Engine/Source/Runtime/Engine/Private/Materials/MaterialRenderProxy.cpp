#include "RHICommandList.h"
#include "Materials/MaterialRenderProxy.h"
#include "Materials/MaterialShaderMap.h"

#include "RenderingThread.h"
#include "Rendering/SceneInterface.h"
#include "Texture/Texture2D.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	auto FMaterialRenderProxy::ReleaseRHIForAllResources() -> void
	{
		std::vector<FMaterialRenderProxy*> Proxies;
		VisitInitializedRenderResources([&](FRenderResource& Resource) {
			if (auto* Proxy = dynamic_cast<FMaterialRenderProxy*>(&Resource)) Proxies.push_back(Proxy);
		});
		for (auto* Proxy : Proxies) Proxy->ReleaseResource();
	}

	namespace
	{

		struct FMaterialRenderProxyAtomicCounters
		{
			std::atomic<uint64> QueuedPublicationCount = 0;
			std::atomic<uint64> PublicationCount = 0;
			std::atomic<uint64> UniformCacheInvalidationCount = 0;
			std::atomic<uint64> PropertyUpdateCount = 0;
			std::atomic<uint64> ParameterUpdateCount = 0;
			std::atomic<uint64> ProgramUpdateCount = 0;
			std::atomic<uint64> PendingPublicationCount = 0;
			std::atomic<uint64> CopiedMaterialPayloadBytes = 0;
			std::atomic<uint64> ResolutionCacheHitCount = 0;
			std::atomic<uint64> ResolutionCacheMissCount = 0;
			std::atomic<uint64> BindingUpdateCount = 0;
			std::atomic<uint64> RepresentationValidationFailureCount = 0;
			std::atomic<uint64> ParameterBindingBuildCount = 0;
			std::atomic<uint64> IndexedParameterWriteCount = 0;
		};

		FMaterialRenderProxyAtomicCounters GMaterialRenderProxyCounters;

		auto ApplyStaticProperties(
			FMaterialRenderData& RenderData,
			const FMaterialStaticProperties& StaticProperties
		) -> void
		{
			RenderData.PlanningPassIdentity.ShaderMap.BlendMode =
				StaticProperties.BlendMode;
			RenderData.PlanningPassIdentity.ShaderMap.ShadingModel =
				StaticProperties.ShadingModel;
			RenderData.PlanningPassIdentity.ShaderMap.OpacityMaskThreshold =
				StaticProperties.OpacityMaskThreshold;
			RenderData.PlanningPassIdentity.bTwoSided =
				StaticProperties.bTwoSided;
			RenderData.PlanningPassIdentity.DepthWritePolicy =
				StaticProperties.DepthWritePolicy;
		}
	}


	auto GetMaterialRenderProxyCounters() -> FMaterialRenderProxyCounters
	{
		return {
			.QueuedPublicationCount = GMaterialRenderProxyCounters.QueuedPublicationCount.load(),
			.PublicationCount = GMaterialRenderProxyCounters.PublicationCount.load(),
			.ProgramUpdateCount = GMaterialRenderProxyCounters.ProgramUpdateCount.load(),
			.ParameterUpdateCount = GMaterialRenderProxyCounters.ParameterUpdateCount.load(),
			.PropertyUpdateCount = GMaterialRenderProxyCounters.PropertyUpdateCount.load(),
			.UniformCacheInvalidationCount = GMaterialRenderProxyCounters.UniformCacheInvalidationCount.load(),
			.PendingPublicationCount = GMaterialRenderProxyCounters.PendingPublicationCount.load(),
			.CopiedMaterialPayloadBytes = GMaterialRenderProxyCounters.CopiedMaterialPayloadBytes.load(),
			.ResolutionCacheHitCount = GMaterialRenderProxyCounters.ResolutionCacheHitCount.load(),
			.ResolutionCacheMissCount = GMaterialRenderProxyCounters.ResolutionCacheMissCount.load(),
			.BindingUpdateCount = GMaterialRenderProxyCounters.BindingUpdateCount.load(),
			.RepresentationValidationFailureCount = GMaterialRenderProxyCounters.RepresentationValidationFailureCount.load(),
			.ParameterBindingBuildCount = GMaterialRenderProxyCounters.ParameterBindingBuildCount.load(),
			.IndexedParameterWriteCount = GMaterialRenderProxyCounters.IndexedParameterWriteCount.load(),
		};
	}

	auto ResetMaterialRenderProxyCounters() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		GMaterialRenderProxyCounters.QueuedPublicationCount.store(0);
		GMaterialRenderProxyCounters.PublicationCount.store(0);
		GMaterialRenderProxyCounters.UniformCacheInvalidationCount.store(0);
		GMaterialRenderProxyCounters.PropertyUpdateCount.store(0);
		GMaterialRenderProxyCounters.ParameterUpdateCount.store(0);
		GMaterialRenderProxyCounters.ProgramUpdateCount.store(0);
		GMaterialRenderProxyCounters.PendingPublicationCount.store(0);
		GMaterialRenderProxyCounters.CopiedMaterialPayloadBytes.store(0);
		GMaterialRenderProxyCounters.ResolutionCacheHitCount.store(0);
		GMaterialRenderProxyCounters.ResolutionCacheMissCount.store(0);
		GMaterialRenderProxyCounters.BindingUpdateCount.store(0);
		GMaterialRenderProxyCounters.RepresentationValidationFailureCount.store(0);
		GMaterialRenderProxyCounters.ParameterBindingBuildCount.store(0);
		GMaterialRenderProxyCounters.IndexedParameterWriteCount.store(0);
	}

	auto RecordMaterialBindingUpdate() -> void
	{
		GMaterialRenderProxyCounters.BindingUpdateCount.fetch_add(1);
	}

	auto FMaterialRenderProxy::AddSceneReference(FSceneInterface& Scene) -> void
	{ CheckRenderingThread(); ++SceneReferences[&Scene]; }
	auto FMaterialRenderProxy::RemoveSceneReference(FSceneInterface& Scene) -> void
	{
		CheckRenderingThread();
		const auto Found = SceneReferences.find(&Scene); require(Found != SceneReferences.end() && Found->second);
		if (!--Found->second) SceneReferences.erase(Found);
	}
	auto FMaterialRenderProxy::ReleaseRHI() -> void
	{
		FMaterialRenderProxy* Material = this;
		for (const auto& [Scene, References] : SceneReferences) Scene->UpdateStaticDrawListsForMaterials(std::span(&Material, 1));
		UniformExpressionCache->ReleaseRHI();
	}
	auto FMaterialRenderProxy::AddRef() const -> uint32
	{
		const uint32 Previous =
			ReferenceCount.fetch_add(1, std::memory_order_acquire);
		check(Previous != std::numeric_limits<uint32>::max());
		return Previous + 1;
	}

	auto FMaterialRenderProxy::Release() const -> uint32
	{
		const uint32 Previous =
			ReferenceCount.fetch_sub(1, std::memory_order_release);
		check(Previous != 0);
		const uint32 Remaining = Previous - 1;
		if (Remaining == 0)
		{
			std::atomic_thread_fence(std::memory_order_acquire);
			DeleteRenderResource(const_cast<FMaterialRenderProxy*>(this));
		}
		return Remaining;
	}

	namespace
	{
		template<class F> auto QueueMaterialUpdate(FMaterialRenderProxy& Proxy, F Work, uint64 Bytes = 0) -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			GMaterialRenderProxyCounters.QueuedPublicationCount.fetch_add(1);
			GMaterialRenderProxyCounters.PendingPublicationCount.fetch_add(1);
			GMaterialRenderProxyCounters.CopiedMaterialPayloadBytes.fetch_add(Bytes);
			EnqueueRenderCommand("UpdateMaterialResource", [Proxy = FMaterialRenderProxyRef(&Proxy), Work = std::move(Work)](FRHICommandListImmediate&) mutable {
				GMaterialRenderProxyCounters.PendingPublicationCount.fetch_sub(1);
				Work(*Proxy);
			});
		}
	}
	auto FMaterialRenderProxy::RecordUpdate_RenderThread() -> void
	{
		CheckRenderingThread();
		++PublicationCount;
		GMaterialRenderProxyCounters.PublicationCount.fetch_add(1);
	}
	auto FMaterialRenderProxy::InvalidateStaticDrawLists_RenderThread() -> void
	{
		FMaterialRenderProxy* Material = this;
		for (const auto& [Scene, References] : SceneReferences) Scene->UpdateStaticDrawListsForMaterials(std::span(&Material, 1));
	}
	auto FMaterialRenderProxy::SetProgram_GameThread(std::shared_ptr<const FMaterialProgram> Program) -> void
	{ QueueMaterialUpdate(*this, [Program = std::move(Program)](auto& Proxy) mutable { Proxy.SetProgram_RenderThread(std::move(Program)); }); }
	auto FMaterialRenderProxy::SetProgram_RenderThread(std::shared_ptr<const FMaterialProgram> Program) -> void
	{
		RecordUpdate_RenderThread();
		if (RenderingThreadProgram == Program) return;
		const bool LayoutChanged = !RenderingThreadProgram || !Program
			|| RenderingThreadProgram->GetShaderMap().Layout.Identity != Program->GetShaderMap().Layout.Identity;
		const bool ShaderMapChanged = !RenderingThreadProgram || !Program || RenderingThreadProgram->ShaderMap != Program->ShaderMap;
		if (ShaderMapChanged) InvalidateStaticDrawLists_RenderThread();
		const bool CollectionsChanged = !RenderingThreadProgram || !Program || RenderingThreadProgram->ActiveCollections != Program->ActiveCollections;
		RenderingThreadProgram = std::move(Program);
		if (ShaderMapChanged || CollectionsChanged) UniformExpressionCache->InvalidateUniformExpressionCache();
		InvalidateUniformExpressionCache_RenderThread(LayoutChanged);
		GMaterialRenderProxyCounters.ProgramUpdateCount.fetch_add(1);
	}
	auto FMaterialRenderProxy::InitParameters_GameThread(FMaterialInstanceParameterSet Parameters) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		Parameters.ResolveTextureReferences();
		const auto Bytes = Parameters.GetPayloadBytes();
		QueueMaterialUpdate(*this, [Parameters = std::move(Parameters)](auto& Proxy) mutable { Proxy.InitParameters_RenderThread(std::move(Parameters)); }, Bytes);
	}
	auto FMaterialRenderProxy::InitParameters_RenderThread(FMaterialInstanceParameterSet Parameters) -> void
	{
		RecordUpdate_RenderThread();
		check(Parameters.IsRenderThreadSet());
		std::ranges::sort(Parameters.ScalarParameters, {}, &FMaterialScalarParameterValue::ParameterId);
		std::ranges::sort(Parameters.Vector2Parameters, {}, &std::pair<FGuid, FVector2>::first);
		std::ranges::sort(Parameters.Vector3Parameters, {}, &std::pair<FGuid, FVector3>::first);
		std::ranges::sort(Parameters.VectorParameters, {}, &FMaterialVectorParameterValue::ParameterId);
		std::ranges::sort(Parameters.TextureParameters, {}, &FMaterialTextureParameterValue::ParameterId);
		if (ParameterValues == Parameters) return;
		ParameterValues = std::move(Parameters);
		InvalidateUniformExpressionCache_RenderThread(false);
		GMaterialRenderProxyCounters.ParameterUpdateCount.fetch_add(1);
	}
	auto FMaterialRenderProxy::UpdateParameter_GameThread(const FGuid& Id, const FMaterialParameterValue& Value) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		auto RenderValue = Value;
		RenderValue.ResolveTextureReference();
		const auto Bytes = RenderValue.GetPayloadBytes();
		QueueMaterialUpdate(*this, [Id, Value = std::move(RenderValue)](auto& Proxy) mutable { Proxy.UpdateParameter_RenderThread(Id, std::move(Value)); }, Bytes);
	}
	auto FMaterialRenderProxy::UpdateParameter_RenderThread(const FGuid& Id, FMaterialParameterValue Value) -> void
	{
		RecordUpdate_RenderThread();
		check(Value.IsRenderThreadValue());
		FMaterialParameterValue Current;
		if (ParameterValues.GetParameterValue(Id, Current) && Current == Value) return;
		ParameterValues.SetParameter(Id, Value);
		InvalidateUniformExpressionCache_RenderThread(false);
		GMaterialRenderProxyCounters.ParameterUpdateCount.fetch_add(1);
	}

	auto FMaterialRenderProxy::SetProperties_GameThread(std::optional<FMaterialStaticProperties> NewProperties) -> void
	{ QueueMaterialUpdate(*this, [NewProperties](auto& Proxy) { Proxy.SetProperties_RenderThread(NewProperties); }); }
	auto FMaterialRenderProxy::SetProperties_RenderThread(std::optional<FMaterialStaticProperties> NewProperties) -> void
	{
		RecordUpdate_RenderThread();
		if (Properties == NewProperties) return;
		InvalidateStaticDrawLists_RenderThread();
		Properties = std::move(NewProperties);
		bHasResolvedData = false;
		GMaterialRenderProxyCounters.PropertyUpdateCount.fetch_add(1);
	}
	auto FMaterialRenderProxy::SetCompiling_GameThread(bool Compiling) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (bCompilingInitialized && bGameThreadCompiling == Compiling) return;
		bCompilingInitialized = true;
		bGameThreadCompiling = Compiling;
		QueueMaterialUpdate(*this, [Compiling](auto& Proxy) { Proxy.SetCompiling_RenderThread(Compiling); });
	}
	auto FMaterialRenderProxy::SetCompiling_RenderThread(bool Compiling) -> void
	{
		RecordUpdate_RenderThread();
		if (bCompiling == Compiling) return;
		bCompiling = Compiling;
		bHasResolvedData = false;
	}
	auto FMaterialRenderProxy::InvalidateUniformExpressionCache_GameThread(bool RecreateUniformBuffer) -> void
	{ QueueMaterialUpdate(*this, [RecreateUniformBuffer](auto& Proxy) { Proxy.RecordUpdate_RenderThread(); Proxy.InvalidateUniformExpressionCache_RenderThread(RecreateUniformBuffer); }); }
	auto FMaterialRenderProxy::InvalidateUniformExpressionCache_RenderThread(bool RecreateUniformBuffer) -> void
	{
		CheckRenderingThread();
		if (RecreateUniformBuffer) UniformExpressionCache->ReleaseRHI();
		else UniformExpressionCache->InvalidateUniformValues();
		bHasResolvedData = false;
		GMaterialRenderProxyCounters.UniformCacheInvalidationCount.fetch_add(1);
	}

	auto FMaterialRenderProxy::UpdateParameterBindings() -> void
	{
		bool bCurrent = BindingArtifact == (RenderingThreadProgram ? RenderingThreadProgram->ShaderMap : nullptr)
			&& ParameterBindings.size() == ParameterValues.GetParameterCount();
		size_t ParameterIndex = 0;
		ParameterValues.VisitParameters([&](const FGuid& Id, const FMaterialParameterValue& Value) {
			if (bCurrent) bCurrent = ParameterBindings[ParameterIndex].Id == Id && ParameterBindings[ParameterIndex].Type == Value.GetType();
			++ParameterIndex;
		});
		if (bCurrent) return;
		BindingArtifact = RenderingThreadProgram ? RenderingThreadProgram->ShaderMap : nullptr;
		ParameterBindings.clear();
		if (!BindingArtifact) return;
		std::unordered_map<FGuid, uint32> Fields;
		Fields.reserve(BindingArtifact->Layout.Fields.size());
		for (uint32 Index = 0; Index < BindingArtifact->Layout.Fields.size(); ++Index)
			Fields.emplace(BindingArtifact->Layout.Fields[Index].ParameterId, Index);
		std::unordered_map<FGuid, EMaterialParameterType> Active;
		Active.reserve(BindingArtifact->ActiveParameters.size());
		for (const auto& Parameter : BindingArtifact->ActiveParameters) Active.emplace(Parameter.Id, Parameter.Type);
		ParameterBindings.reserve(ParameterValues.GetParameterCount());
		ParameterValues.VisitParameters([&](const FGuid& Id, const FMaterialParameterValue& Value) {
			FParameterBinding Binding{Id, Value.GetType()};
			const auto Declaration = Active.find(Id);
			const auto Field = Fields.find(Id);
			Binding.bActive = Declaration != Active.end() && Declaration->second == Binding.Type;
			if (Field != Fields.end()) Binding.FieldIndex = Field->second;
			ParameterBindings.push_back(Binding);
		});
		GMaterialRenderProxyCounters.ParameterBindingBuildCount.fetch_add(1);
	}

	auto FMaterialRenderProxy::IsCompiling() const -> bool
	{
		CheckRenderingThread();
		return bCompiling;
	}

	auto FMaterialRenderProxy::GetMaterialNoFallback() -> const FMaterialRenderData*
	{
		CheckRenderingThread();
		if (!RenderingThreadProgram) return nullptr;
		const auto& Material = Resolve_RenderThread();
		return Material.CompiledProgram && !Material.Representation.IsError() ? &Material : nullptr;
	}

	auto FMaterialRenderProxy::GetMaterialWithFallback() -> const FMaterialRenderData&
	{
		return Resolve_RenderThread();
	}

	auto FMaterialRenderProxy::Resolve_RenderThread()
		-> const FMaterialRenderData&
	{
		CheckRenderingThread();
		if (bHasResolvedData)
		{
			GMaterialRenderProxyCounters.ResolutionCacheHitCount.fetch_add(1);
			return CachedResolvedData;
		}
		GMaterialRenderProxyCounters.ResolutionCacheMissCount.fetch_add(1);

		CachedResolvedData = {};
		CachedResolvedData.UniformExpressionCache = UniformExpressionCache;
		if (!RenderingThreadProgram && bCompiling)
		{
			++ResolutionCount;
			bHasResolvedData = true;
			return CachedResolvedData;
		}
		CachedResolvedData.Representation = FMaterialRenderRepresentation{};
		if (RenderingThreadProgram)
		{
			CachedResolvedData.CompiledProgram = RenderingThreadProgram;
			CachedResolvedData.PlanningPassIdentity.ShaderMap.ProgramIdentity =
				RenderingThreadProgram->GetShaderMap().ShaderMapId;
		}
		FMaterialRenderRepresentationBuilder RepresentationBuilder = CachedResolvedData.CompiledProgram
			&& CachedResolvedData.CompiledProgram->GetShaderMap().Layout.Identity.Version == CompiledMaterialRenderLayoutVersion
			&& CachedResolvedData.CompiledProgram->GetShaderMap().Layout.Identity != CachedResolvedData.Representation.GetLayout().Identity
			? FMaterialRenderRepresentationBuilder(CachedResolvedData.CompiledProgram->GetShaderMap().Layout)
			: FMaterialRenderRepresentationBuilder(CachedResolvedData.Representation);
		bool bRepresentationValid = true;
		if (CachedResolvedData.CompiledProgram)
			CachedResolvedData.PlanningPassIdentity.ShaderMap.RenderLayout = CachedResolvedData.CompiledProgram->GetShaderMap().Layout.Identity;
		if (RenderingThreadProgram)
			RenderingThreadProgram->DefaultParameters.VisitParameters([&](const FGuid& Id, const FMaterialParameterValue& Value) {
				bRepresentationValid &= RepresentationBuilder.SetParameter(Id, Value);
			});
		UpdateParameterBindings();
		uint64 IndexedWrites = 0;
		size_t ParameterIndex = 0;
		ParameterValues.VisitParameters([&](const FGuid& Id, const FMaterialParameterValue& Value) {
			if (!BindingArtifact) return;
			const auto& Binding = ParameterBindings[ParameterIndex++];
			if (!Binding.bActive) return;
			++IndexedWrites;
			if (!RepresentationBuilder.SetParameterAtIndex(Binding.FieldIndex, Id, Value)) bRepresentationValid = false;
		});
		GMaterialRenderProxyCounters.IndexedParameterWriteCount.fetch_add(IndexedWrites);
		if (Properties)
		{
			ApplyStaticProperties(
				CachedResolvedData, *Properties);
		}
		FMaterialRenderRepresentation CompiledRepresentation;
		FMaterialRenderValidationDiagnostic ValidationDiagnostic;
		if (!CachedResolvedData.CompiledProgram
			|| !bRepresentationValid
			|| !RepresentationBuilder.Build(
				CompiledRepresentation, ValidationDiagnostic))
		{
			RecordMaterialFallbackReason(
				EMaterialFallbackReason::MaterialDataInvalid);
			CachedResolvedData = GetErrorMaterialRenderData();
			CachedResolvedData.UniformExpressionCache = UniformExpressionCache;
			GMaterialRenderProxyCounters.RepresentationValidationFailureCount
				.fetch_add(1);
		}
		else
		{
			CachedResolvedData.Representation = std::move(CompiledRepresentation);
		}

		++ResolutionCount;
		bHasResolvedData = true;
		return CachedResolvedData;
	}

	auto FMaterialRenderProxy::GetPublicationCount_RenderThread() const
		-> uint64
	{
		CheckRenderingThread();
		return PublicationCount;
	}

	auto FMaterialRenderProxy::GetResolutionCount_RenderThread() const
		-> uint64
	{
		CheckRenderingThread();
		return ResolutionCount;
	}

	auto ReleaseMaterialRenderProxy_GameThread(
		FMaterialRenderProxyRef Proxy
	) -> void
	{
		if (!Proxy) return;
		if (GIsGameThreadIdInitialized) CheckGameThread();
		const auto Admission = GetRenderCommandAdmissionState();
		if (Admission == ERenderCommandAdmissionState::Stopped) return;
		checkf(Admission == ERenderCommandAdmissionState::Running,
			"Material proxy retirement must finish before render-command shutdown.");

		struct FReleaseMaterialRenderProxyCommand
		{
			static constexpr auto GetName() -> const char*
			{
				return "ReleaseMaterialRenderProxy";
			}
		};
		FRenderThreadCommandPipe::Enqueue<FReleaseMaterialRenderProxyCommand>(
			[Proxy = std::move(Proxy)](
				FRHICommandListImmediate&) mutable {
				Proxy = {};
			});
	}
}
