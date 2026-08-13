#include "Components/PrimitiveComponent.h"

#include "Engine/Actor.h"
#include "DObject/Property.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "IScene.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GNextPrimitiveSceneId = 1;
	}

	DPrimitiveComponent::DPrimitiveComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DPrimitiveComponent::OnRegister() -> void
	{
		Super::OnRegister();
		EnsurePrimitiveSceneId();
		MarkRenderStateDirty();
		RecreatePhysicsState();
	}

	auto DPrimitiveComponent::OnUnregister() -> void
	{
		DestroyPhysicsState();
		DestroyRenderState();
#if DURIN_WITH_EDITOR
		NotifyEditorPickingMutation(true);
#endif
		Super::OnUnregister();
	}

	auto DPrimitiveComponent::SetCollisionProfileName(FName ProfileName) -> bool
	{
		CollisionProfile::FProfile Profile;
		if (!CollisionProfile::Resolve(ProfileName, Profile)) return false;
		BodyInstance.ProfileName = ProfileName;
		BodyInstance.CollisionEnabled = Profile.Enabled;
		BodyInstance.ObjectChannel = Profile.ObjectChannel;
		BodyInstance.Responses = Profile.Responses;
		MarkPackageDirty();
		RecreatePhysicsState();
		return true;
	}

	auto DPrimitiveComponent::SetCollisionEnabled(ECollisionEnabled Enabled) -> void
	{
		if (BodyInstance.CollisionEnabled == Enabled) return;
		BodyInstance.CollisionEnabled = Enabled;
		BodyInstance.ProfileName = FName();
		MarkPackageDirty();
		RecreatePhysicsState();
	}

	auto DPrimitiveComponent::SetCollisionObjectType(ECollisionChannel Channel) -> void
	{
		BodyInstance.ObjectChannel = Channel;
		BodyInstance.ProfileName = FName();
		MarkPackageDirty();
		RecreatePhysicsState();
	}

	auto DPrimitiveComponent::SetCollisionResponseToChannel(
		ECollisionChannel Channel, ECollisionResponse Response) -> void
	{
		BodyInstance.Responses.SetResponse(Channel, Response);
		BodyInstance.ProfileName = FName();
		MarkPackageDirty();
		RecreatePhysicsState();
	}

	auto DPrimitiveComponent::SetPhysicsBodyMotionType(EPhysicsBodyMotionType MotionType) -> void
	{
		if (BodyInstance.MotionType == MotionType) return;
		BodyInstance.MotionType = MotionType;
		UpdatePhysicsState();
	}

	auto DPrimitiveComponent::BuildCollisionShape(FCollisionShape&, FTransform&) const -> bool
	{
		return false;
	}

	auto DPrimitiveComponent::BuildCollisionGeometry(
		FCollisionGeometryRef& OutGeometry, FTransform& OutWorldTransform) const -> bool
	{
		FCollisionShape Shape;
		if (!BuildCollisionShape(Shape, OutWorldTransform)) return false;
		const FCollisionGeometryChild* Existing = CachedCollisionGeometry.GetChild(0);
		bool bSameShape = Existing && Existing->Shape.GetType() == Shape.GetType();
		if (bSameShape)
		{
			switch (Shape.GetType())
			{
			case ECollisionShapeType::Box:
				bSameShape = Existing->Shape.GetBoxHalfExtent() == Shape.GetBoxHalfExtent();
				break;
			case ECollisionShapeType::Sphere:
				bSameShape = Existing->Shape.GetSphereRadius() == Shape.GetSphereRadius();
				break;
			case ECollisionShapeType::Capsule:
				bSameShape = Existing->Shape.GetCapsuleRadius() == Shape.GetCapsuleRadius()
					&& Existing->Shape.GetCapsuleHalfHeight() == Shape.GetCapsuleHalfHeight();
				break;
			}
		}
		if (!bSameShape) CachedCollisionGeometry = FCollisionGeometryRef::MakePrimitive(Shape);
		OutGeometry = CachedCollisionGeometry;
		return OutGeometry.IsValid();
	}

	auto DPrimitiveComponent::RecreatePhysicsState() -> void
	{
		DestroyPhysicsState();
		if (!IsRegistered() || BodyInstance.CollisionEnabled == ECollisionEnabled::NoCollision) return;
		DWorld* World = GetPhysicsWorld();
		FCollisionGeometryRef Geometry;
		FTransform Transform;
		if (!World || !BuildCollisionGeometry(Geometry, Transform)) return;
		const FPhysicsBodyDesc Desc = MakePhysicsBodyDesc(Geometry, Transform);
		BodyInstance.ActorHandle = World->GetPhysicsScene().AddBody(Desc);
		BodyInstance.PublishedBodySetupRevision = BodyInstance.ActorHandle.IsValid()
			? GetCollisionStateRevision() : 0;
	}

	auto DPrimitiveComponent::DestroyPhysicsState() -> void
	{
		if (!BodyInstance.ActorHandle.IsValid()) return;
		if (DWorld* World = GetPhysicsWorld()) World->GetPhysicsScene().RemoveBody(BodyInstance.ActorHandle);
		BodyInstance.ActorHandle = {};
		BodyInstance.PublishedBodySetupRevision = 0;
	}

	auto DPrimitiveComponent::UpdatePhysicsState() -> void
	{
		if (!BodyInstance.ActorHandle.IsValid())
		{
			RecreatePhysicsState();
			return;
		}
		DWorld* World = GetPhysicsWorld();
		FCollisionGeometryRef Geometry;
		FTransform Transform;
		if (!World || !BuildCollisionGeometry(Geometry, Transform))
		{
			DestroyPhysicsState();
			return;
		}
		const FPhysicsBodyDesc Desc = MakePhysicsBodyDesc(Geometry, Transform);
		if (!World->GetPhysicsScene().UpdateBody(BodyInstance.ActorHandle, Desc))
			RecreatePhysicsState();
		else
			BodyInstance.PublishedBodySetupRevision = GetCollisionStateRevision();
	}

	auto DPrimitiveComponent::MakePhysicsBodyDesc(
		const FCollisionGeometryRef& Geometry,
		const FTransform& Transform) const -> FPhysicsBodyDesc
	{
		FPhysicsBodyDesc Desc;
		Desc.Geometry = Geometry;
		Desc.Transform = Transform;
		Desc.Filter.ObjectChannel = static_cast<uint8>(BodyInstance.ObjectChannel);
		for (uint8 Index = 0; Index < MaximumPhysicsChannels; ++Index)
			Desc.Filter.Responses[Index] =
				ToPhysicsResponse(BodyInstance.Responses.Responses[Index]);
		Desc.UserToken = reinterpret_cast<uint64>(this);
		Desc.MotionType = BodyInstance.MotionType;
		return Desc;
	}

	auto DPrimitiveComponent::GetPhysicsWorld() const -> DWorld*
	{
		const AActor* Owner = GetOwner();
		auto* Level = Owner ? Cast<DLevel>(Owner->GetOuter()) : nullptr;
		return Level ? Level->GetWorld() : nullptr;
	}

	auto DPrimitiveComponent::OnOwnerVisibilityChanged() -> void
	{
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::Visibility);
	}

	auto DPrimitiveComponent::SetVisible(bool bInVisible) -> void
	{
		if (bVisible == bInVisible) return;
		bVisible = bInVisible;
		MarkPackageDirty();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::Visibility);
	}

	auto DPrimitiveComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty) return;
		if (Event.MemberProperty->NamePrivate == FName("bVisible"))
		{
			MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::Visibility);
			return;
		}
		if (Event.MemberProperty->NamePrivate != FName("BodyInstance")) return;
		if (!BodyInstance.ProfileName.IsNone())
		{
			CollisionProfile::FProfile Profile;
			if (CollisionProfile::Resolve(BodyInstance.ProfileName, Profile))
			{
				BodyInstance.CollisionEnabled = Profile.Enabled;
				BodyInstance.ObjectChannel = Profile.ObjectChannel;
				BodyInstance.Responses = Profile.Responses;
			}
		}
		RecreatePhysicsState();
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
#if DURIN_WITH_EDITOR
		if (EnumHasAnyFlags(DirtyFlags, EPrimitiveRenderStateDirtyFlags::Proxy)
			|| EnumHasAnyFlags(DirtyFlags, EPrimitiveRenderStateDirtyFlags::Transform)
			|| EnumHasAnyFlags(DirtyFlags, EPrimitiveRenderStateDirtyFlags::Visibility))
			NotifyEditorPickingMutation();
#endif
		IScene* Scene = GetRenderScene();
		if (Scene == nullptr) return;

		const FPrimitiveSceneId SceneId = EnsurePrimitiveSceneId();
		const AActor* Owner = GetOwner();
		const bool bPrimitiveVisible = bVisible && (Owner == nullptr || !Owner->IsHidden());
		if (EnumHasAnyFlags(DirtyFlags, EPrimitiveRenderStateDirtyFlags::Proxy))
		{
			std::unique_ptr<FPrimitiveSceneProxy> Proxy = CreateSceneProxy();
			if (Proxy != nullptr)
			{
				Scene->AddOrReplacePrimitive(
					SceneId, std::move(Proxy), GetRenderMatrix(), bPrimitiveVisible);
			}
			else
			{
				Scene->RemovePrimitive(SceneId);
			}
			return;
		}
		if (EnumHasAnyFlags(DirtyFlags, EPrimitiveRenderStateDirtyFlags::Visibility))
		{
			Scene->UpdatePrimitiveVisibility(SceneId, bPrimitiveVisible);
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
		UpdatePhysicsState();
	}

#if DURIN_WITH_EDITOR
	auto DPrimitiveComponent::GetEditorPickingLocalBounds(
		FBox&, EEditorPickingPrimitiveFamily&) const -> bool
	{
		return false;
	}

	auto DPrimitiveComponent::NotifyEditorPickingMutation(bool bRetired) -> void
	{
		AActor* Owner = GetOwner();
		if (auto* Level = Owner ? Cast<DLevel>(Owner->GetOuter()) : nullptr)
			Level->NotifyEditorPickingPrimitiveChanged(this, bRetired);
	}
#endif
}
