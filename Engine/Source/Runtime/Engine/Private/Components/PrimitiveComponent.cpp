#include "Components/PrimitiveComponent.h"

#include "Engine/Actor.h"
#include "IScene.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GNextPrimitiveSceneId = 1;
	}

	auto DPrimitiveComponent::OnRegister() -> void
	{
		Super::OnRegister();
		EnsurePrimitiveSceneId();
		MarkRenderStateDirty();
	}

	auto DPrimitiveComponent::OnUnregister() -> void
	{
		DestroyRenderState();
		Super::OnUnregister();
	}

	auto DPrimitiveComponent::OnOwnerVisibilityChanged() -> void
	{
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::Visibility);
	}

	auto DPrimitiveComponent::CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy>
	{
		return nullptr;
	}

	auto DPrimitiveComponent::GetRenderMatrix() const -> FMatrix
	{
		return GetComponentToWorldMatrix();
	}

	auto DPrimitiveComponent::DestroyRenderState() -> void
	{
		if (!IsRegistered()) return;
		if (IScene* Scene = GetRenderScene()) Scene->RemovePrimitive(PrimitiveSceneId);
	}

	auto DPrimitiveComponent::RecreateRenderState() -> void
	{
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::Proxy);
	}

	auto DPrimitiveComponent::MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags DirtyFlags) -> void
	{
		if (!IsRegistered()) return;
		IScene* Scene = GetRenderScene();
		if (Scene == nullptr) return;

		const FPrimitiveSceneId SceneId = EnsurePrimitiveSceneId();
		const AActor* Owner = GetOwner();
		const bool bVisible = Owner == nullptr || !Owner->IsHidden();
		if (EnumHasAnyFlags(DirtyFlags, EPrimitiveRenderStateDirtyFlags::Proxy))
		{
			std::unique_ptr<FPrimitiveSceneProxy> Proxy = CreateSceneProxy();
			if (Proxy != nullptr)
			{
				Scene->AddOrReplacePrimitive(
					SceneId, std::move(Proxy), GetRenderMatrix(), bVisible);
			}
			else
			{
				Scene->RemovePrimitive(SceneId);
			}
			return;
		}
		if (EnumHasAnyFlags(DirtyFlags, EPrimitiveRenderStateDirtyFlags::Visibility))
		{
			Scene->UpdatePrimitiveVisibility(SceneId, bVisible);
		}

		if (EnumHasAnyFlags(DirtyFlags, EPrimitiveRenderStateDirtyFlags::Transform))
		{
			Scene->UpdatePrimitiveTransform(SceneId, GetRenderMatrix());
		}
		if (EnumHasAnyFlags(DirtyFlags, EPrimitiveRenderStateDirtyFlags::MaterialBinding))
		{
			FMaterialRenderProxyBindingUpdate Update;
			if (BuildMaterialRenderProxyBindingUpdate(Update))
			{
				Scene->UpdatePrimitiveMaterialBinding(SceneId, Update);
			}
		}
	}

	auto DPrimitiveComponent::BuildMaterialRenderProxyBindingUpdate(
		FMaterialRenderProxyBindingUpdate& OutUpdate) -> bool
	{
		return false;
	}

	auto DPrimitiveComponent::EnsurePrimitiveSceneId() -> FPrimitiveSceneId
	{
		if (PrimitiveSceneId == InvalidPrimitiveSceneId)
		{
			PrimitiveSceneId = FPrimitiveSceneId(GNextPrimitiveSceneId.fetch_add(1, std::memory_order_relaxed));
		}
		return PrimitiveSceneId;
	}

	auto DPrimitiveComponent::OnUpdateTransform() -> void
	{
		Super::OnUpdateTransform();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::Transform);
	}
}
