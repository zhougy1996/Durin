#include "Components/PrimitiveComponent.h"
#include "Components/ScopedTransformOverlapUpdate.h"

#include "Engine/Actor.h"
#include "DObject/Property.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Physics/PhysicsObjectID.h"
#include "Rendering/SceneInterface.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GNextPrimitiveComponentId = 1;
	}

	thread_local FScopedTransformOverlapUpdate* FScopedTransformOverlapUpdate::Active = nullptr;

	FScopedTransformOverlapUpdate::FScopedTransformOverlapUpdate()
	{
		if (!Active) Active = this;
	}

	FScopedTransformOverlapUpdate::~FScopedTransformOverlapUpdate()
	{
		if (Active != this) return;
		Active = nullptr;
		for (const auto& Key : Pending)
			if (auto* Component = Key.ResolveObjectPtr()) Component->RefreshOverlaps();
	}

	auto FScopedTransformOverlapUpdate::Defer(DPrimitiveComponent& Component) -> bool
	{
		if (!Active) return false;
		Active->Pending.emplace_back(&Component);
		return true;
	}

	auto DPrimitiveComponent::SetCustomPrimitiveData(uint32 First, std::span<const float> Values) -> bool
	{
		auto Candidate = CustomPrimitiveData;
		if (!Candidate.Set(First, Values)) return false;
		if (Candidate == CustomPrimitiveData) return true;
		CustomPrimitiveData = Candidate;
		if (SceneProxy && GetRenderScene())
			GetRenderScene()->UpdatePrimitiveCustomData(PrimitiveComponentId, Candidate);
		return true;
	}

	DPrimitiveComponent::DPrimitiveComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DPrimitiveComponent::OnRegister() -> void
	{
		Super::OnRegister();
		EnsurePrimitiveComponentId();
		CreateRenderState();
		ApplyPhysicsStateCreationPolicy();
	}

	auto DPrimitiveComponent::OnUnregister() -> void
	{
		PSOPrecacheComponentData.Reset();
		DestroyPhysicsState();
		DestroyRenderState();
		Super::OnUnregister();
	}

	auto DPrimitiveComponent::SetCollisionProfileName(FName ProfileName) -> bool
	{
		const FName PreviousName = BodyInstance.CollisionProfileName;
		BodyInstance.CollisionProfileName = ProfileName;
		if (ProfileName.IsNone() || !BodyInstance.LoadProfileData())
		{
			BodyInstance.CollisionProfileName = PreviousName;
			return false;
		}
		MarkPackageDirty();
		OnCollisionSettingsChanged();
		return true;
	}

	auto DPrimitiveComponent::SetCollisionEnabled(ECollisionEnabled Enabled) -> void
	{
		if (BodyInstance.CollisionEnabled == Enabled) return;
		BodyInstance.CollisionEnabled = Enabled;
		BodyInstance.CollisionProfileName = FName();
		MarkPackageDirty();
		OnCollisionSettingsChanged();
	}

	auto DPrimitiveComponent::SetCollisionObjectType(ECollisionChannel Channel) -> void
	{
		if (BodyInstance.CollisionProfileName.IsNone() && BodyInstance.ObjectChannel == Channel) return;
		BodyInstance.ObjectChannel = Channel;
		BodyInstance.CollisionProfileName = FName();
		MarkPackageDirty();
		OnCollisionSettingsChanged();
	}

	auto DPrimitiveComponent::SetCollisionResponseToChannel(
		ECollisionChannel Channel, ECollisionResponse Response
	) -> void
	{
		if (BodyInstance.CollisionProfileName.IsNone() && BodyInstance.Responses.GetResponse(Channel) == Response) return;
		BodyInstance.Responses.SetResponse(Channel, Response);
		BodyInstance.CollisionProfileName = FName();
		MarkPackageDirty();
		OnCollisionSettingsChanged();
	}

	auto DPrimitiveComponent::SetPhysicsSourceObject(DObject* SourceObject) -> void
	{
		const TWeakObjectPtr<DObject> NewSource(SourceObject);
		if (BodyInstance.SourceObject.GetKey() == NewSource.GetKey()) return;
		BodyInstance.SourceObject = NewSource;
		if (BodyInstance.ActorHandle.IsValid()) UpdatePhysicsState();
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

	auto DPrimitiveComponent::PopulateCollisionGeometry(FPhysicsBodyDesc& Desc) const -> bool
	{
		FCollisionShape Shape;
		if (!BuildCollisionShape(Shape, Desc.SimpleTransform)) return false;
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
		Desc.SimpleGeometry = CachedCollisionGeometry;
		return Desc.SimpleGeometry.IsValid();
	}

	auto DPrimitiveComponent::BeginPlay() -> void
	{
		Super::BeginPlay();
		RefreshOverlaps();
	}

	auto DPrimitiveComponent::EndPlay() -> void
	{
		RefreshOverlaps();
		Super::EndPlay();
	}

	auto DPrimitiveComponent::SetGenerateOverlapEvents(bool bEnabled) -> void
	{
		if (bGenerateOverlapEvents == bEnabled) return;
		bGenerateOverlapEvents = bEnabled;
		MarkPackageDirty();
		if (BodyInstance.ActorHandle.IsValid()) UpdatePhysicsState();
	}

	auto DPrimitiveComponent::SweepForBlockingHit(const FVector3& StartOffset, const FVector3& Delta, FHitResult& OutHit) const -> bool
	{
		OutHit.Reset();
		const auto* World = GetPhysicsWorld();
		FPhysicsQueryHit Block;
		return World && World->GetPhysicsScene().SweepBody(GetPhysicsActorHandle(), Delta, PhysicsPrivate::MakeObjectID(GetOwner()),
			nullptr, Block, StartOffset) && Block.IsHit() && OutHit.SetFromPhysicsHit(Block);
	}

	auto DPrimitiveComponent::MoveComponentImpl(const FVector3& Delta, const FQuat& NewRotation,
		bool bSweep, FHitResult* OutHit) -> bool
	{
		auto* World = GetPhysicsWorld();
		if (!bSweep || !World || !GetPhysicsActorHandle().IsValid() || Delta == FVector3(0.0))
			return Super::MoveComponentImpl(Delta, NewRotation, false, nullptr);
		std::vector<FPhysicsQueryHit> Touches;
		FPhysicsQueryHit Block;
		if (!World->GetPhysicsScene().SweepBody(GetPhysicsActorHandle(), Delta, PhysicsPrivate::MakeObjectID(GetOwner()), &Touches, Block))
			return false;
		if (Block.IsHit() && OutHit) OutHit->SetFromPhysicsHit(Block);
		if (World->HasBegunPlay() && CanGenerateOverlaps())
		{
			for (const auto& Touch : Touches)
			{
				auto* Other = Cast<DPrimitiveComponent>(PhysicsPrivate::ResolveObjectID(Touch.ComponentID));
				if (Other && Other->CanGenerateOverlaps() && Other->GetPhysicsWorld() == World)
					BeginOverlap(*World, *Other);
			}
		}
		// Publishing the final pose reconciles all pending touches against actual end overlaps.
		const bool bMoved = Super::MoveComponentImpl(Delta * Block.Time, NewRotation, false, nullptr);
		if (!bMoved && !Touches.empty()) RefreshOverlaps();
		return bMoved;
	}

	auto DPrimitiveComponent::QueueOverlapTransition(DWorld& World, TObjectKey<DPrimitiveComponent> Peer,
		DPrimitiveComponent* Other, bool bBegin) -> void
	{
		DWorld::FComponentOverlapNotification Event{TObjectKey<DPrimitiveComponent>(this), Peer, bBegin, {}, {}};
		AActor* Owner = GetOwner();
		AActor* PeerOwner = Other ? Other->GetOwner() : nullptr;
		if (Owner && PeerOwner && Owner != PeerOwner)
		{
			// Called before linking a new pair and after unlinking a retired pair.
			const bool bHasOtherPair = Owner->GetComponents().size() <= PeerOwner->GetComponents().size()
				? Owner->IsOverlappingActor(PeerOwner) : PeerOwner->IsOverlappingActor(Owner);
			if (!bHasOtherPair)
			{
				Event.FirstActor = TObjectKey<AActor>(Owner);
				Event.SecondActor = TObjectKey<AActor>(PeerOwner);
			}
		}
		World.PendingOverlapNotifications.push_back(Event);
	}

	auto DPrimitiveComponent::BeginOverlap(DWorld& World, DPrimitiveComponent& Other) -> void
	{
		const TObjectKey<DPrimitiveComponent> Peer(&Other);
		if (OverlappingComponents.contains(Peer)) return;
		QueueOverlapTransition(World, Peer, &Other, true);
		OverlappingComponents.insert(Peer);
		Other.OverlappingComponents.emplace(this);
	}

	auto DPrimitiveComponent::CanGenerateOverlaps() const -> bool
	{
		return bGenerateOverlapEvents && IsRegistered() && HasBegunPlay() && !IsEndingPlay()
			&& !IsBeingDestroyed() && !IsPendingKill() && BodyInstance.ActorHandle.IsValid();
	}

	auto DPrimitiveComponent::RefreshOverlaps() -> void
	{
		const bool bParticipating = CanGenerateOverlaps();
		if (!bParticipating && OverlappingComponents.empty()) return;
		DWorld* World = GetPhysicsWorld();
		const bool bWorldPlaying = World && World->HasBegunPlay();
		if (bWorldPlaying && bParticipating && FScopedTransformOverlapUpdate::Defer(*this)) return;
		std::set<TObjectKey<DPrimitiveComponent>> Current;
		if (bWorldPlaying && bParticipating)
		{
			for (const uint64 ComponentID : World->GetPhysicsScene().FindBodyOverlaps(BodyInstance.ActorHandle))
			{
				auto* Other = Cast<DPrimitiveComponent>(PhysicsPrivate::ResolveObjectID(ComponentID));
				if (Other && Other != this && Other->CanGenerateOverlaps() && Other->GetPhysicsWorld() == World)
					Current.emplace(Other);
			}
		}
		if (Current == OverlappingComponents) return;
		const TObjectKey<DPrimitiveComponent> Self(this);

		for (auto It = OverlappingComponents.begin(); It != OverlappingComponents.end();)
		{
			if (Current.contains(*It)) { ++It; continue; }
			const auto Old = *It;
			It = OverlappingComponents.erase(It);
			auto* Other = Old.ResolveObjectPtr();
			if (Other) Other->OverlappingComponents.erase(Self);
			if (bWorldPlaying) QueueOverlapTransition(*World, Old, Other, false);
		}
		for (const auto& New : Current)
		{
			if (!OverlappingComponents.contains(New)) BeginOverlap(*World, *New.ResolveObjectPtr());
		}
	}

	auto DPrimitiveComponent::RecreatePhysicsState() -> void
	{
		RemovePublishedBody();
		DWorld* World = GetPhysicsWorld();
		FPhysicsBodyDesc Desc;
		if (World && IsRegistered() && BodyInstance.CollisionEnabled != ECollisionEnabled::NoCollision && BuildPhysicsBodyDesc(Desc))
		{
			BodyInstance.ActorHandle = World->GetPhysicsScene().AddBody(Desc);
			BodyInstance.PublishedBodySetupRevision = BodyInstance.ActorHandle.IsValid() ? GetCollisionStateRevision() : 0;
		}
		RefreshOverlaps();
	}

	auto DPrimitiveComponent::RequestPhysicsStateCreation(bool) -> bool
	{
		RecreatePhysicsState();
		return BodyInstance.CollisionEnabled == ECollisionEnabled::NoCollision
			   || BodyInstance.ActorHandle.IsValid();
	}

	auto DPrimitiveComponent::GetPhysicsStateCreationPolicy() const -> EPhysicsStateCreationPolicy
	{
		return EPhysicsStateCreationPolicy::Eager;
	}

	auto DPrimitiveComponent::OnCollisionSettingsChanged() -> void
	{
		ApplyPhysicsStateCreationPolicy();
	}

	auto DPrimitiveComponent::ApplyPhysicsStateCreationPolicy() -> void
	{
		switch (GetPhysicsStateCreationPolicy())
		{
		case EPhysicsStateCreationPolicy::Eager:
			RecreatePhysicsState();
			break;
		case EPhysicsStateCreationPolicy::OnDemand:
			DestroyPhysicsState();
			break;
		case EPhysicsStateCreationPolicy::DeferredRequired:
			(void)RequestPhysicsStateCreation(false);
			break;
		}
	}

	auto DPrimitiveComponent::DestroyPhysicsState() -> void
	{
		RemovePublishedBody();
		RefreshOverlaps();
	}

	auto DPrimitiveComponent::RemovePublishedBody() -> void
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
			ApplyPhysicsStateCreationPolicy();
			return;
		}
		DWorld* World = GetPhysicsWorld();
		FPhysicsBodyDesc Desc;
		if (!World || !BuildPhysicsBodyDesc(Desc))
		{
			DestroyPhysicsState();
			return;
		}
		if (!World->GetPhysicsScene().UpdateBody(BodyInstance.ActorHandle, Desc))
			RecreatePhysicsState();
		else
		{
			BodyInstance.PublishedBodySetupRevision = GetCollisionStateRevision();
			RefreshOverlaps();
		}
	}

	auto DPrimitiveComponent::BuildPhysicsBodyDesc(FPhysicsBodyDesc& Desc) const -> bool
	{
		Desc = {};
		if (!PopulateCollisionGeometry(Desc)) return false;
		if ((Desc.SimpleGeometry && !IsValidPhysicsTransform(Desc.SimpleTransform))
			|| (Desc.ComplexGeometry && !IsValidPhysicsTransform(Desc.ComplexTransform))) return false;
		Desc.Filter.ObjectChannel = static_cast<uint8>(BodyInstance.ObjectChannel);
		for (uint8 Index = 0; Index < MaximumPhysicsChannels; ++Index)
			Desc.Filter.Responses[Index] =
				ToPhysicsResponse(BodyInstance.Responses.Responses[Index]);
		Desc.ComponentID = PhysicsPrivate::MakeObjectID(this);
		Desc.bGenerateOverlapEvents = bGenerateOverlapEvents;
		const DObject* SourceObject = BodyInstance.SourceObject.Get();
		Desc.OwnerID = PhysicsPrivate::MakeObjectID(SourceObject ? SourceObject : GetOwner());
		Desc.MotionType = BodyInstance.MotionType;
		return true;
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

	auto DPrimitiveComponent::SetSelectable(bool bInSelectable) -> void
	{
		if (bSelectable == bInSelectable) return;
		bSelectable = bInSelectable;
		MarkPackageDirty();
	}

	auto DPrimitiveComponent::SetVisible(bool bInVisible) -> void
	{
		if (bVisible == bInVisible) return;
		bVisible = bInVisible;
		MarkPackageDirty();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::Visibility);
	}

	auto DPrimitiveComponent::SetCastShadow(bool bValue) -> void
	{
		if (bCastShadow == bValue) return;
		bCastShadow = bValue;
		MarkPackageDirty();
		MarkRenderStateDirty();
	}

	auto DPrimitiveComponent::SetRenderInMainPass(bool bValue) -> void
	{
		if (bRenderInMainPass == bValue) return;
		bRenderInMainPass = bValue;
		MarkPackageDirty();
		MarkRenderStateDirty();
	}

	auto DPrimitiveComponent::SetupPrecachePSOParams(FPSOPrecacheParams& Params) const -> void
	{
		Params.Participation = GetRenderParticipation();
	}

	auto DPrimitiveComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (!Event.MemberProperty) return;
		if (Event.MemberProperty->NamePrivate == FName("bCastShadow")
			|| Event.MemberProperty->NamePrivate == FName("bRenderInMainPass"))
		{
			MarkRenderStateDirty();
			return;
		}
		if (Event.MemberProperty->NamePrivate == FName("bVisible"))
		{
			MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::Visibility);
			return;
		}
		if (Event.MemberProperty->NamePrivate == FName("bGenerateOverlapEvents"))
		{
			if (BodyInstance.ActorHandle.IsValid()) UpdatePhysicsState();
			return;
		}
		if (Event.MemberProperty->NamePrivate != FName("BodyInstance")) return;
		const bool bProfileNameChanged = Event.LeafProperty
										 && Event.LeafProperty->NamePrivate == FName("CollisionProfileName");
		const bool bRestoredSettings = Event.Origin != EPropertyChangeOrigin::Edit
			|| !Event.LeafProperty || Event.LeafProperty == Event.MemberProperty;
		if (bProfileNameChanged || bRestoredSettings)
		{
			if (!BodyInstance.LoadProfileData())
				BodyInstance.CollisionProfileName = FName();
		}
		else
		{
			// Directly edited filter fields are custom settings. Keeping the old
			// preset name would immediately overwrite the user's value.
			BodyInstance.CollisionProfileName = FName();
		}
		OnCollisionSettingsChanged();
	}

	auto DPrimitiveComponent::CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy>
	{
		return nullptr;
	}

	auto DPrimitiveComponent::GetRenderMatrix() const -> FMatrix
	{
		return GetComponentToWorldMatrix();
	}

	auto DPrimitiveComponent::CreateRenderState() -> void
	{
		if (!IsRegistered()) return;
		if (FSceneInterface* Scene = GetRenderScene(); Scene && IsPSOPrecacheReadyForProxyCreation()) Scene->AddPrimitive(this);
	}

	auto DPrimitiveComponent::DestroyRenderState() -> void
	{
		UntrackPSOPrecache();
		PSOPrecacheComponentData.Reset();
		bPSOPrecacheSubmissionPending = false;
		if (!IsRegistered()) return;
		if (FSceneInterface* Scene = GetRenderScene()) Scene->RemovePrimitive(this);
	}

	auto DPrimitiveComponent::RecreateRenderState() -> void
	{
		if (!IsRegistered()) return;
		DestroyRenderState();
		CreateRenderState();
	}

	auto DPrimitiveComponent::MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags DirtyFlags) -> void
	{
		const bool bPrecacheChanged = EnumHasAnyFlags(DirtyFlags, EPrimitiveRenderStateDirtyFlags::Proxy | EPrimitiveRenderStateDirtyFlags::MaterialBinding);
		if (bPrecacheChanged) { PSOPrecacheComponentData.Reset(); PrecachePSOs(); }
		if (!IsRegistered()) return;
		FSceneInterface* Scene = GetRenderScene();
		if (Scene == nullptr) return;
		if (EnumHasAnyFlags(DirtyFlags, EPrimitiveRenderStateDirtyFlags::Proxy))
		{
			RecreateRenderState();
			return;
		}
		if (bPrecacheChanged && !IsPSOPrecacheReadyForProxyCreation())
		{
			Scene->RemovePrimitive(this);
			return;
		}
		if (SceneProxy == nullptr && bPrecacheChanged) { CreateRenderState(); return; }
		if (SceneProxy == nullptr) return;

		const FPrimitiveComponentId SceneId = EnsurePrimitiveComponentId();
		const AActor* Owner = GetOwner();
		const bool bPrimitiveVisible = bVisible && (Owner == nullptr || !Owner->IsHidden());
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

	auto DPrimitiveComponent::OnComponentPendingKill() -> void
	{
		UntrackPSOPrecache();
		PSOPrecacheComponentData.Reset();
		bPSOPrecacheSubmissionPending = false;
		Super::OnComponentPendingKill();
	}

	auto DPrimitiveComponent::BuildMaterialRenderProxyBindingUpdate(
		FMaterialRenderProxyBindingUpdate& OutUpdate
	) -> bool
	{
		return false;
	}

	auto DPrimitiveComponent::EnsurePrimitiveComponentId() -> FPrimitiveComponentId
	{
		if (!PrimitiveComponentId.IsValid())
		{
			PrimitiveComponentId = FPrimitiveComponentId(GNextPrimitiveComponentId.fetch_add(1, std::memory_order_relaxed));
		}
		return PrimitiveComponentId;
	}

	auto DPrimitiveComponent::OnUpdateTransform() -> void
	{
		Super::OnUpdateTransform();
		MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags::Transform);
		UpdatePhysicsState();
	}

} // namespace Durin
