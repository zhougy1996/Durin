#include "Materials/MaterialRenderProxy.h"

#include "Math/Operations.h"
#include "RenderingThread.h"
#include "Texture/Texture2D.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		struct FMaterialRenderProxyAtomicCounters
		{
			std::atomic<uint64> PublicationCount = 0;
			std::atomic<uint64> CoalescedPublicationCount = 0;
			std::atomic<uint64> ResolutionCacheHitCount = 0;
			std::atomic<uint64> ResolutionCacheMissCount = 0;
			std::atomic<uint64> StalePublicationCount = 0;
			std::atomic<uint64> BindingUpdateCount = 0;
			std::atomic<uint64> RepresentationValidationFailureCount = 0;
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
			.PublicationCount = GMaterialRenderProxyCounters.PublicationCount.load(),
			.CoalescedPublicationCount = GMaterialRenderProxyCounters.CoalescedPublicationCount.load(),
			.ResolutionCacheHitCount = GMaterialRenderProxyCounters.ResolutionCacheHitCount.load(),
			.ResolutionCacheMissCount = GMaterialRenderProxyCounters.ResolutionCacheMissCount.load(),
			.StalePublicationCount = GMaterialRenderProxyCounters.StalePublicationCount.load(),
			.BindingUpdateCount = GMaterialRenderProxyCounters.BindingUpdateCount.load(),
			.RepresentationValidationFailureCount = GMaterialRenderProxyCounters.RepresentationValidationFailureCount.load(),
		};
	}

	auto ResetMaterialRenderProxyCounters() -> void
	{
		GMaterialRenderProxyCounters.PublicationCount.store(0);
		GMaterialRenderProxyCounters.CoalescedPublicationCount.store(0);
		GMaterialRenderProxyCounters.ResolutionCacheHitCount.store(0);
		GMaterialRenderProxyCounters.ResolutionCacheMissCount.store(0);
		GMaterialRenderProxyCounters.StalePublicationCount.store(0);
		GMaterialRenderProxyCounters.BindingUpdateCount.store(0);
		GMaterialRenderProxyCounters.RepresentationValidationFailureCount.store(0);
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
		FMaterialRenderProxyPublication Publication
	) -> bool
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (Publication.LocalVersion == 0) return false;

		bool bNeedsRenderCommand = false;
		{
			std::lock_guard Lock(PublicationMutex);
			if (PendingPublication.has_value()
				&& Publication.LocalVersion <= PendingPublication->LocalVersion)
			{
				// A publication can remain pending when render-command admission was
				// stopped. Retry that retained wave once the rendering thread starts
				// again instead of treating it as an already-queued stale update.
				if (bPublicationCommandQueued) return false;
				bPublicationCommandQueued = true;
				bNeedsRenderCommand = true;
			}
			else
			{
				if (PendingPublication.has_value())
				{
					GMaterialRenderProxyCounters.CoalescedPublicationCount.fetch_add(1);
				}
				PendingPublication = std::move(Publication);
				if (!bPublicationCommandQueued)
				{
					bPublicationCommandQueued = true;
					bNeedsRenderCommand = true;
				}
			}
		}

		if (!bNeedsRenderCommand) return true;

		struct FApplyPendingMaterialRenderProxyCommand
		{
			static constexpr auto GetName() -> const char*
			{
				return "ApplyPendingMaterialRenderProxy";
			}
		};
		FMaterialRenderProxyRef Proxy(this);
		const bool bAccepted =
			FRenderThreadCommandPipe::TryEnqueue<
				FApplyPendingMaterialRenderProxyCommand>(
				[Proxy = std::move(Proxy)](
					FRHICommandListImmediate&) mutable {
					Proxy->ApplyPendingPublication_RenderThread();
				});
		if (bAccepted) return true;

		std::lock_guard Lock(PublicationMutex);
		bPublicationCommandQueued = false;
		return false;
	}

	auto FMaterialRenderProxy::ApplyPendingPublication_RenderThread() -> bool
	{
		CheckRenderingThread();
		FMaterialRenderProxyPublication Publication;
		{
			std::lock_guard Lock(PublicationMutex);
			if (!PendingPublication.has_value())
			{
				bPublicationCommandQueued = false;
				return false;
			}
			Publication = std::move(*PendingPublication);
			PendingPublication.reset();
			bPublicationCommandQueued = false;
		}
		return ApplyPublication_RenderThread(std::move(Publication));
	}

	auto FMaterialRenderProxy::ApplyPublication_RenderThread(
		FMaterialRenderProxyPublication Publication
	) -> bool
	{
		CheckRenderingThread();
		if (Publication.LocalVersion == 0
			|| Publication.LocalVersion <= LocalVersion)
		{
			++StalePublicationCount;
			GMaterialRenderProxyCounters.StalePublicationCount.fetch_add(1);
			return false;
		}

		check(std::ranges::is_sorted(
			Publication.LocalLayer.Parameters,
			{},
			&FMaterialLocalRenderParameter::Id));
		LocalLayer = std::move(Publication.LocalLayer);
		ParentProxy = std::move(Publication.ParentProxy);
		LocalVersion = Publication.LocalVersion;
		GMaterialRenderProxyCounters.PublicationCount.fetch_add(1);
		return true;
	}

	auto FMaterialRenderProxy::Resolve_RenderThread()
		-> const FMaterialRenderData&
	{
		CheckRenderingThread();
		checkf(
			!bIsResolving,
			"Material render proxy parent cycle reached render-thread resolution.");
		struct FScopedMaterialProxyResolution
		{
			explicit FScopedMaterialProxyResolution(bool& InIsResolving)
				: bIsResolving(InIsResolving)
			{
				bIsResolving = true;
			}
			~FScopedMaterialProxyResolution()
			{
				bIsResolving = false;
			}
			bool& bIsResolving;
		} ResolutionScope(bIsResolving);

		const FMaterialRenderProxy* ParentIdentity =
			ParentProxy.GetReference();
		const FMaterialRenderData* ParentData = nullptr;
		uint64 ParentResolvedVersion = 0;
		if (ParentProxy)
		{
			ParentData = &ParentProxy->Resolve_RenderThread();
			ParentResolvedVersion =
				ParentProxy->GetResolvedVersion_RenderThread();
		}

		if (bHasResolvedData
			&& CachedLocalVersion == LocalVersion
			&& CachedParentIdentity == ParentIdentity
			&& ObservedParentResolvedVersion == ParentResolvedVersion)
		{
			GMaterialRenderProxyCounters.ResolutionCacheHitCount.fetch_add(1);
			return CachedResolvedData;
		}
		GMaterialRenderProxyCounters.ResolutionCacheMissCount.fetch_add(1);

		CachedResolvedData =
			ParentData ? *ParentData : GetErrorMaterialRenderData();
		FMaterialRenderRepresentationBuilder RepresentationBuilder(
			CachedResolvedData.Representation);
		bool bRepresentationValid = true;
		for (const FMaterialLocalRenderParameter& Parameter
			: LocalLayer.Parameters)
		{
			if (!ApplyMaterialLocalRenderParameter(
					RepresentationBuilder, Parameter))
			{
				bRepresentationValid = false;
			}
		}
		if (LocalLayer.StaticProperties)
		{
			ApplyStaticProperties(
				CachedResolvedData, *LocalLayer.StaticProperties);
		}

		FMaterialRenderRepresentation CompiledRepresentation;
		FMaterialRenderValidationDiagnostic ValidationDiagnostic;
		if (!bRepresentationValid
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

		CachedLocalVersion = LocalVersion;
		CachedParentIdentity = ParentIdentity;
		ObservedParentResolvedVersion = ParentResolvedVersion;
		++ResolvedVersion;
		if (ResolvedVersion == 0) ++ResolvedVersion;
		bHasResolvedData = true;
		return CachedResolvedData;
	}

	auto FMaterialRenderProxy::GetLocalVersion_RenderThread() const
		-> uint64
	{
		CheckRenderingThread();
		return LocalVersion;
	}

	auto FMaterialRenderProxy::GetResolvedVersion_RenderThread() const
		-> uint64
	{
		CheckRenderingThread();
		return ResolvedVersion;
	}

	auto FMaterialRenderProxy::GetObservedParentResolvedVersion_RenderThread() const
		-> uint64
	{
		CheckRenderingThread();
		return ObservedParentResolvedVersion;
	}

	auto FMaterialRenderProxy::GetParentProxyIdentity_RenderThread() const
		-> const FMaterialRenderProxy*
	{
		CheckRenderingThread();
		return ParentProxy.GetReference();
	}

	auto FMaterialRenderProxy::GetStalePublicationCount_RenderThread() const
		-> uint64
	{
		CheckRenderingThread();
		return StalePublicationCount;
	}

	auto BuildMaterialLocalRenderParameter(
		const FGuid& Id,
		EMaterialParameterType Type,
		const FMaterialParameterValue& Value
	) -> FMaterialLocalRenderParameter
	{
		FMaterialLocalRenderParameter Result{
			.Id = Id,
			.Type = Type,
		};
		const std::span Definitions =
			GetCanonicalMaterialParameterDefinitions();
		const auto Definition = std::ranges::find(
			Definitions, Id, &FMaterialParameterDefinition::Id);
		if (Definition == Definitions.end() || Definition->Type != Type)
		{
			return Result;
		}
		switch (Type)
		{
		case EMaterialParameterType::Scalar:
			Result.ScalarValue = std::isfinite(Value.ScalarValue)
				? Value.ScalarValue
				: Definition->Value.ScalarValue;
			Result.ScalarValue = std::clamp(
				Result.ScalarValue,
				Definition->MinimumValue,
				Definition->MaximumValue);
			if (Definition->Presentation
				== EMaterialParameterPresentation::Integer)
			{
				Result.ScalarValue = std::floor(Result.ScalarValue + 0.5f);
			}
			break;
		case EMaterialParameterType::Vector2:
			Result.Vector2Value = Value.Vector2Value;
			if (!std::isfinite(Result.Vector2Value.x)
				|| !std::isfinite(Result.Vector2Value.y))
			{
				Result.Vector2Value = Definition->Value.Vector2Value;
			}
			Result.Vector2Value.x = std::clamp(
				Result.Vector2Value.x,
				static_cast<double>(Definition->MinimumValue),
				static_cast<double>(Definition->MaximumValue));
			Result.Vector2Value.y = std::clamp(
				Result.Vector2Value.y,
				static_cast<double>(Definition->MinimumValue),
				static_cast<double>(Definition->MaximumValue));
			break;
		case EMaterialParameterType::Vector:
			Result.VectorValue = Value.VectorValue;
			if (!std::isfinite(Result.VectorValue.x)
				|| !std::isfinite(Result.VectorValue.y)
				|| !std::isfinite(Result.VectorValue.z))
			{
				Result.VectorValue = Definition->Value.VectorValue;
			}
			Result.VectorValue.x = std::clamp(
				Result.VectorValue.x,
				static_cast<double>(Definition->MinimumValue),
				static_cast<double>(Definition->MaximumValue));
			Result.VectorValue.y = std::clamp(
				Result.VectorValue.y,
				static_cast<double>(Definition->MinimumValue),
				static_cast<double>(Definition->MaximumValue));
			Result.VectorValue.z = std::clamp(
				Result.VectorValue.z,
				static_cast<double>(Definition->MinimumValue),
				static_cast<double>(Definition->MaximumValue));
			if (Id == MaterialParameters::NormalId)
			{
				const double LengthSquared =
					Math::LengthSquared(Result.VectorValue);
				Result.VectorValue = LengthSquared < 1.0e-8
					? Definition->Value.VectorValue
					: Result.VectorValue / std::sqrt(LengthSquared);
			}
			break;
		case EMaterialParameterType::Texture:
			if (DTexture2D* Texture = Value.TextureValue.Get();
				Texture != nullptr
				&& Texture->GetUsage() == Definition->TextureUsage
				&& Texture->IsSRGB()
					== (Definition->TextureUsage == ETextureUsage::Color))
			{
				Result.TextureValue =
					Texture->GetTextureReferenceRHI();
			}
			break;
		}
		return Result;
	}

	auto ApplyMaterialLocalRenderParameter(
		FMaterialRenderRepresentationBuilder& RepresentationBuilder,
		const FMaterialLocalRenderParameter& Parameter
	) -> bool
	{
		const std::span Definitions =
			GetCanonicalMaterialParameterDefinitions();
		const auto Definition = std::ranges::find(
			Definitions, Parameter.Id, &FMaterialParameterDefinition::Id);
		if (Definition == Definitions.end()
			|| Definition->Type != Parameter.Type)
		{
			return false;
		}

		switch (Parameter.Type)
		{
		case EMaterialParameterType::Scalar:
			return RepresentationBuilder.SetScalar(
				Parameter.Id, Parameter.ScalarValue);
		case EMaterialParameterType::Vector2:
			return RepresentationBuilder.SetVector2(
				Parameter.Id, Parameter.Vector2Value);
		case EMaterialParameterType::Vector:
			return RepresentationBuilder.SetVector(
				Parameter.Id, Parameter.VectorValue);
		case EMaterialParameterType::Texture:
			return RepresentationBuilder.SetTexture(
				Parameter.Id, Parameter.TextureValue);
		}
		return false;
	}

	auto ReleaseMaterialRenderProxy_GameThread(
		FMaterialRenderProxyRef Proxy
	) -> void
	{
		if (!Proxy) return;
		if (GIsGameThreadIdInitialized) CheckGameThread();

		struct FReleaseMaterialRenderProxyCommand
		{
			static constexpr auto GetName() -> const char*
			{
				return "ReleaseMaterialRenderProxy";
			}
		};
		FRenderThreadCommandPipe::TryEnqueue<FReleaseMaterialRenderProxyCommand>(
			[Proxy = std::move(Proxy)](
				FRHICommandListImmediate&) mutable {
				Proxy = {};
			});
	}
}
