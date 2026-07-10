#include "Components/PrimitiveComponent.h"

#include "Engine/Engine.h"
#include "IScene.h"

namespace Durin
{
	auto DPrimitiveComponent::OnRegister() -> void
	{
		Super::OnRegister();
		if (GEngine != nullptr)
		{
			if (IScene* Scene = GEngine->GetMainScene())
			{
				Scene->AddPrimitive(this);
			}
		}
	}

	auto DPrimitiveComponent::OnUnregister() -> void
	{
		if (GEngine != nullptr)
		{
			if (IScene* Scene = GEngine->GetMainScene())
			{
				Scene->RemovePrimitive(this);
			}
		}
		Super::OnUnregister();
	}

	auto DPrimitiveComponent::CreateSceneProxy() -> std::unique_ptr<PrimitiveSceneProxy>
	{
		return nullptr;
	}

	auto DPrimitiveComponent::GetRenderMatrix() const -> FMatrix
	{
		return GetComponentToWorldMatrix();
	}

	auto DPrimitiveComponent::OnUpdateTransform() -> void
	{
		Super::OnUpdateTransform();
		if (IsRegistered() && GEngine != nullptr)
		{
			if (IScene* Scene = GEngine->GetMainScene())
			{
				Scene->UpdatePrimitiveTransform(this);
			}
		}
	}
}
