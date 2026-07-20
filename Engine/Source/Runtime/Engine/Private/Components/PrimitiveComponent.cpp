#include "Components/PrimitiveComponent.h"

#include "Engine/Engine.h"
#include "Engine/Actor.h"
#include "IScene.h"

namespace Durin
{
	namespace
	{
		std::atomic<FPrimitiveSceneId> GNextPrimitiveSceneId = 1;
	}

	auto DPrimitiveComponent::OnRegister() -> void
	{
		Super::OnRegister();
		EnsurePrimitiveSceneId();
		MarkRenderStateDirty();
	}

	auto DPrimitiveComponent::OnUnregister() -> void
	{
		if (GEngine != nullptr)
		{
			if (IScene* Scene = GEngine->GetMainScene())
			{
				Scene->RemovePrimitive(PrimitiveSceneId);
			}
		}
		Super::OnUnregister();
	}

	auto DPrimitiveComponent::OnOwnerVisibilityChanged() -> void
	{
		MarkRenderStateDirty();
	}

	auto DPrimitiveComponent::CreateSceneProxy() -> std::unique_ptr<PrimitiveSceneProxy>
	{
		return nullptr;
	}

	auto DPrimitiveComponent::GetRenderMatrix() const -> FMatrix
	{
		return GetComponentToWorldMatrix();
	}

	auto DPrimitiveComponent::MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags DirtyFlags) -> void
	{
		if (!IsRegistered() || GEngine == nullptr) return;
		IScene* Scene = GEngine->GetMainScene();
		if (Scene == nullptr) return;

		const FPrimitiveSceneId SceneId = EnsurePrimitiveSceneId();
		// Hidden actors keep their component registration and scene identity so showing
		// them again only needs to recreate the render proxy.
		if (const AActor* Owner = GetOwner(); Owner && Owner->IsHidden())
		{
			Scene->RemovePrimitive(SceneId);
			return;
		}
		if (EnumHasAnyFlags(DirtyFlags, EPrimitiveRenderStateDirtyFlags::Proxy))
		{
			std::unique_ptr<PrimitiveSceneProxy> Proxy = CreateSceneProxy();
			if (Proxy != nullptr)
			{
				Scene->AddOrReplacePrimitive(SceneId, std::move(Proxy), GetRenderMatrix());
			}
			else
			{
				Scene->RemovePrimitive(SceneId);
			}
			return;
		}

		if (EnumHasAnyFlags(DirtyFlags, EPrimitiveRenderStateDirtyFlags::Transform))
		{
			Scene->UpdatePrimitiveTransform(SceneId, GetRenderMatrix());
		}
		if (EnumHasAnyFlags(DirtyFlags, EPrimitiveRenderStateDirtyFlags::MaterialData))
		{
			FMaterialRenderUpdate Update;
			if (BuildMaterialRenderUpdate(EMaterialRenderDirtyFlags::ParameterValues, Update))
			{
				Scene->UpdatePrimitiveMaterial(SceneId, Update);
			}
		}
	}

	auto DPrimitiveComponent::BuildMaterialRenderUpdate(EMaterialRenderDirtyFlags DirtyFlags, FMaterialRenderUpdate& OutUpdate) -> bool
	{
		return false;
	}

	auto DPrimitiveComponent::EnsurePrimitiveSceneId() -> FPrimitiveSceneId
	{
		if (PrimitiveSceneId == InvalidPrimitiveSceneId)
		{
			PrimitiveSceneId = GNextPrimitiveSceneId.fetch_add(1, std::memory_order_relaxed);
		}
		return PrimitiveSceneId;
	}

	auto DPrimitiveComponent::OnUpdateTransform() -> void
	{
		Super::OnUpdateTransform();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::Transform);
	}
}
