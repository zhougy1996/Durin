#include "Materials/MaterialRenderProxy.h"
#include "Materials/MaterialShaderMap.h"

#include "RenderingThread.h"
#include "Texture/Texture2D.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{

		struct FMaterialRenderProxyAtomicCounters
		{
			std::atomic<uint64> QueuedPublicationCount = 0;
			std::atomic<uint64> PublicationCount = 0;
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

		auto GetMaterialPayloadBytes(const FMaterialLocalRenderLayer& Layer) -> uint64
		{
			uint64 Bytes = 0;
			for (const auto& Parameter : Layer.Parameters)
				Bytes += std::visit([](const auto& Value) -> uint64 {
					return sizeof(Value);
				}, Parameter.Value);
			return Bytes;
		}

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
			delete this;
		}
		return Remaining;
	}

	auto FMaterialRenderProxy::QueuePublication_GameThread(
		FMaterialLocalRenderLayer Publication
	) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();

		GMaterialRenderProxyCounters.QueuedPublicationCount.fetch_add(1);
		GMaterialRenderProxyCounters.PendingPublicationCount.fetch_add(1);
		GMaterialRenderProxyCounters.CopiedMaterialPayloadBytes.fetch_add(GetMaterialPayloadBytes(Publication));
		EnqueueRenderCommand("PublishMaterialLayer",
			[Proxy = FMaterialRenderProxyRef(this), Layer = std::move(Publication)](FRHICommandListImmediate&) mutable {
				GMaterialRenderProxyCounters.PendingPublicationCount.fetch_sub(1);
				Proxy->ApplyPublication_RenderThread(std::move(Layer));
			});
	}

	auto FMaterialRenderProxy::ApplyPublication_RenderThread(
		FMaterialLocalRenderLayer Publication
	) -> void
	{
		CheckRenderingThread();

		check(std::ranges::is_sorted(
			Publication.Parameters,
			{},
			&FMaterialLocalRenderParameter::Id));
		LocalLayer = std::move(Publication);
		bHasResolvedData = false;
		++PublicationCount;
		GMaterialRenderProxyCounters.PublicationCount.fetch_add(1);
	}

	auto FMaterialRenderProxy::UpdateParameterBindings() -> void
	{
		bool bCurrent = BindingArtifact == (LocalLayer.CompiledProgram ? LocalLayer.CompiledProgram->ShaderMap : nullptr)
			&& ParameterBindings.size() == LocalLayer.Parameters.size();
		for (size_t Index = 0; bCurrent && Index < ParameterBindings.size(); ++Index)
			bCurrent = ParameterBindings[Index].Id == LocalLayer.Parameters[Index].Id
				&& ParameterBindings[Index].Type == LocalLayer.Parameters[Index].GetType();
		if (bCurrent) return;
		BindingArtifact = LocalLayer.CompiledProgram ? LocalLayer.CompiledProgram->ShaderMap : nullptr;
		ParameterBindings.clear();
		if (!BindingArtifact) return;
		std::unordered_map<FGuid, uint32> Fields;
		Fields.reserve(BindingArtifact->Layout.Fields.size());
		for (uint32 Index = 0; Index < BindingArtifact->Layout.Fields.size(); ++Index)
			Fields.emplace(BindingArtifact->Layout.Fields[Index].ParameterId, Index);
		std::unordered_map<FGuid, EMaterialParameterType> Active;
		Active.reserve(BindingArtifact->ActiveParameters.size());
		for (const auto& Parameter : BindingArtifact->ActiveParameters) Active.emplace(Parameter.Id, Parameter.Type);
		ParameterBindings.reserve(LocalLayer.Parameters.size());
		for (const auto& Parameter : LocalLayer.Parameters)
		{
			FParameterBinding Binding{Parameter.Id, Parameter.GetType()};
			const auto Declaration = Active.find(Parameter.Id);
			const auto Field = Fields.find(Parameter.Id);
			Binding.bActive = Declaration != Active.end() && Declaration->second == Binding.Type;
			if (Field != Fields.end()) Binding.FieldIndex = Field->second;
			ParameterBindings.push_back(Binding);
		}
		GMaterialRenderProxyCounters.ParameterBindingBuildCount.fetch_add(1);
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
		CachedResolvedData.Representation = FMaterialRenderRepresentation{};
		if (LocalLayer.CompiledProgram)
		{
			CachedResolvedData.CompiledProgram = LocalLayer.CompiledProgram;
			CachedResolvedData.PlanningPassIdentity.ShaderMap.ProgramIdentity =
				LocalLayer.CompiledProgram->GetShaderMap().ShaderMapId;
		}
		FMaterialRenderRepresentationBuilder RepresentationBuilder = CachedResolvedData.CompiledProgram
			&& CachedResolvedData.CompiledProgram->GetShaderMap().Layout.Identity.Version == CompiledMaterialRenderLayoutVersion
			&& CachedResolvedData.CompiledProgram->GetShaderMap().Layout.Identity != CachedResolvedData.Representation.GetLayout().Identity
			? FMaterialRenderRepresentationBuilder(CachedResolvedData.CompiledProgram->GetShaderMap().Layout)
			: FMaterialRenderRepresentationBuilder(CachedResolvedData.Representation);
		bool bRepresentationValid = true;
		if (CachedResolvedData.CompiledProgram)
			CachedResolvedData.PlanningPassIdentity.ShaderMap.RenderLayout = CachedResolvedData.CompiledProgram->GetShaderMap().Layout.Identity;
		UpdateParameterBindings();
		uint64 IndexedWrites = 0;
		for (size_t Index = 0; Index < LocalLayer.Parameters.size(); ++Index)
		{
			if (!BindingArtifact) break;
			if (!ParameterBindings[Index].bActive) continue;
			const auto FieldIndex = ParameterBindings[Index].FieldIndex;
			++IndexedWrites;
			if (!RepresentationBuilder.SetParameterAtIndex(FieldIndex, LocalLayer.Parameters[Index]))
			{
				bRepresentationValid = false;
			}
		}
		GMaterialRenderProxyCounters.IndexedParameterWriteCount.fetch_add(IndexedWrites);
		if (LocalLayer.StaticProperties)
		{
			ApplyStaticProperties(
				CachedResolvedData, *LocalLayer.StaticProperties);
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

	auto FMaterialLocalRenderParameter::GetType() const -> EMaterialParameterType
	{
		return std::visit([]<typename T>(const T&) {
			if constexpr (std::is_same_v<T, float>) return EMaterialParameterType::Scalar;
			else if constexpr (std::is_same_v<T, FVector2>) return EMaterialParameterType::Vector2;
			else if constexpr (std::is_same_v<T, FVector3>) return EMaterialParameterType::Vector;
			else if constexpr (std::is_same_v<T, FVector4>) return EMaterialParameterType::Vector4;
			else return EMaterialParameterType::Texture;
		}, Value);
	}

	auto BuildMaterialLocalRenderParameter(const FGuid& Id, const FMaterialParameterValue& Value)
		-> FMaterialLocalRenderParameter
	{
		FMaterialLocalRenderParameter Result{.Id = Id};
		switch (Value.GetType())
		{
		case EMaterialParameterType::Scalar: Result.Value = Value.GetScalar(); break;
		case EMaterialParameterType::Vector2: Result.Value = Value.GetVector2(); break;
		case EMaterialParameterType::Vector: Result.Value = Value.GetVector(); break;
		case EMaterialParameterType::Vector4: Result.Value = Value.GetVector4(); break;
		case EMaterialParameterType::Texture:
		{
			const auto& Texture = Value.GetTexture();
			FMaterialLocalTextureValue Local{.SamplerState = Texture.SamplerState, .TextureFallback = Texture.TextureFallback};
			if (Texture.Texture) Local.Texture = Texture.Texture->GetTextureReferenceRHI();
			Result.Value = std::move(Local);
			break;
		}
		}
		return Result;
	}

	auto ApplyMaterialLocalRenderParameter(FMaterialRenderRepresentationBuilder& RepresentationBuilder,
		const FMaterialLocalRenderParameter& Parameter) -> bool
	{
		return std::visit([&]<typename T>(const T& Value) {
			if constexpr (std::is_same_v<T, float>) return RepresentationBuilder.SetScalar(Parameter.Id, Value);
			else if constexpr (std::is_same_v<T, FVector2>) return RepresentationBuilder.SetVector2(Parameter.Id, Value);
			else if constexpr (std::is_same_v<T, FVector3>) return RepresentationBuilder.SetVector(Parameter.Id, Value);
			else if constexpr (std::is_same_v<T, FVector4>) return RepresentationBuilder.SetVector4(Parameter.Id, Value);
			else return RepresentationBuilder.SetTexture(Parameter.Id, Value.Texture, Value.SamplerState, Value.TextureFallback);
		}, Parameter.Value);
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
