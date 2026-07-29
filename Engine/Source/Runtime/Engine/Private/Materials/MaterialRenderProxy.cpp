#include "Materials/MaterialRenderProxy.h"

#include "RenderingThread.h"
#include "Texture/Texture2D.h"
#include "Threading/RunnableThread.h"

#include <limits>

namespace Durin
{
	namespace
	{
		auto ApplyLocalParameter(
			FMaterialRenderData& RenderData,
			const FMaterialLocalRenderParameter& Parameter
		) -> void
		{
			if (Parameter.Id == MaterialParameters::BaseColorId
				&& Parameter.Type == EMaterialParameterType::Vector)
			{
				RenderData.BaseColor.r = static_cast<float>(
					std::clamp(Parameter.VectorValue.x, 0.0, 1.0));
				RenderData.BaseColor.g = static_cast<float>(
					std::clamp(Parameter.VectorValue.y, 0.0, 1.0));
				RenderData.BaseColor.b = static_cast<float>(
					std::clamp(Parameter.VectorValue.z, 0.0, 1.0));
			}
			else if (Parameter.Id == MaterialParameters::BaseColorTextureId
				&& Parameter.Type == EMaterialParameterType::Texture)
			{
				RenderData.BaseColorTexture = Parameter.TextureValue;
			}
			else if (Parameter.Id == MaterialParameters::OpacityId
				&& Parameter.Type == EMaterialParameterType::Scalar)
			{
				RenderData.BaseColor.a =
					std::clamp(Parameter.ScalarValue, 0.0f, 1.0f);
			}
			else if (Parameter.Id == MaterialParameters::SpecularStrengthId
				&& Parameter.Type == EMaterialParameterType::Scalar)
			{
				RenderData.SpecularStrength =
					std::clamp(Parameter.ScalarValue, 0.0f, 1.0f);
			}
			else if (Parameter.Id == MaterialParameters::ShininessId
				&& Parameter.Type == EMaterialParameterType::Scalar)
			{
				RenderData.Shininess =
					std::clamp(Parameter.ScalarValue, 1.0f, 256.0f);
			}
		}

		auto ApplyStaticProperties(
			FMaterialRenderData& RenderData,
			const FMaterialStaticProperties& StaticProperties
		) -> void
		{
			RenderData.PipelineIdentity.ShaderMap.BlendMode =
				StaticProperties.BlendMode;
			RenderData.PipelineIdentity.ShaderMap.ShadingModel =
				StaticProperties.ShadingModel;
			RenderData.PipelineIdentity.ShaderMap.OpacityMaskThreshold =
				StaticProperties.OpacityMaskThreshold;
			RenderData.PipelineIdentity.bTwoSided =
				StaticProperties.bTwoSided;
			RenderData.PipelineIdentity.DepthWritePolicy =
				StaticProperties.DepthWritePolicy;
		}
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

	auto FMaterialRenderProxy::ApplyPublication_RenderThread(
		FMaterialRenderProxyPublication Publication
	) -> bool
	{
		CheckRenderingThread();
		if (Publication.LocalVersion == 0
			|| Publication.LocalVersion <= LocalVersion)
		{
			++StalePublicationCount;
			return false;
		}

		check(std::ranges::is_sorted(
			Publication.LocalLayer.Parameters,
			{},
			&FMaterialLocalRenderParameter::Id));
		LocalLayer = std::move(Publication.LocalLayer);
		ParentProxy = std::move(Publication.ParentProxy);
		LocalVersion = Publication.LocalVersion;
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
			return CachedResolvedData;
		}

		CachedResolvedData =
			ParentData ? *ParentData : FMaterialRenderData{};
		for (const FMaterialLocalRenderParameter& Parameter
			: LocalLayer.Parameters)
		{
			ApplyLocalParameter(CachedResolvedData, Parameter);
		}
		if (LocalLayer.StaticProperties)
		{
			ApplyStaticProperties(
				CachedResolvedData, *LocalLayer.StaticProperties);
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
		switch (Type)
		{
		case EMaterialParameterType::Scalar:
			Result.ScalarValue = Value.ScalarValue;
			break;
		case EMaterialParameterType::Vector:
			Result.VectorValue = Value.VectorValue;
			break;
		case EMaterialParameterType::Texture:
			if (Value.TextureValue != nullptr)
			{
				Result.TextureValue =
					Value.TextureValue->GetTextureReferenceRHI();
			}
			break;
		}
		return Result;
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
