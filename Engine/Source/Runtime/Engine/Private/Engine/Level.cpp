#include "Asset/RegistryOperations.h"
#include "Logging/LogMacros.h"
#include "Engine/Level.h"

#include "Actors/CameraActor.h"
#include "AssetRegistry/Catalog.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Actor.h"
#include "Engine/World.h"

namespace Durin
{
	namespace
	{
		auto MakeLevelPackageResolutionError(
			const FPackagePath& PackagePath,
			EAssetPathResolveState State) -> FAssetReadResult
		{
			using enum EAssetPathResolveState;
			switch (State)
			{
			case ProjectionPending:
				return {EAssetReadError::ProjectionPending,
					std::format("Registry projection for the default-level package '{}' is pending synchronization.",
						PackagePath.ToString())};
			case NotFound:
				return {EAssetReadError::NotFound,
					std::format("The default-level package '{}' is not registered.",
						PackagePath.ToString())};
			case MissingRedirectTarget:
				return {EAssetReadError::MissingDependency,
					std::format("The default-level package '{}' redirects to a missing package.",
						PackagePath.ToString())};
			case UnknownTargetClass:
				return {EAssetReadError::UnknownClass,
					std::format("The default-level package '{}' has an unknown registered class.",
						PackagePath.ToString())};
			case RedirectTypeMismatch:
				return {EAssetReadError::TypeMismatch,
					std::format("The default-level package '{}' has an incompatible redirect target.",
						PackagePath.ToString())};
			case RedirectCycle:
			case RedirectDepthExceeded:
			case CorruptRedirector:
				return {EAssetReadError::CorruptFile,
					std::format("The default-level package '{}' has an invalid redirect chain.",
						PackagePath.ToString())};
			case Resolved:
				break;
			}
			return {EAssetReadError::CorruptFile,
				"The default-level package resolution returned an invalid result."};
		}
	}

	auto ResolveLevelPackage(
		const FPackagePath& PackagePath,
		FObjectPath& OutLevelPath) -> FAssetReadResult
	{
		if (!PackagePath.IsValid())
			return {EAssetReadError::InvalidPath,
				"The default level must be a valid package path."};

		const FAssetPathResolveResult Resolution = ResolveAssetPathForOperation(PackagePath);
		if (!Resolution)
			return MakeLevelPackageResolutionError(PackagePath, Resolution.State);
		if (!Resolution.FinalAssetData)
			return {EAssetReadError::CorruptFile,
				"The resolved default-level package has no registry metadata."};

		FObjectPath LevelPath;
		uint32 LevelCount = 0;
		for (const FTopLevelAssetData& Asset :
			Resolution.FinalAssetData->TopLevelAssets)
		{
			DClass* AssetClass = FindClassByQualifiedName(
				FName(Asset.AssetClassName));
			if (!AssetClass)
				return {EAssetReadError::UnknownClass,
					std::format("The default-level package '{}' contains the unknown top-level class '{}'.",
						Resolution.FinalPath.ToString(), Asset.AssetClassName)};
			if (!AssetClass->IsChildOf(DLevel::StaticClass())) continue;
			++LevelCount;
			if (!FObjectPath::TryCreate(
					Asset.AssetPath, std::span<const std::string>{}, LevelPath))
				return {EAssetReadError::CorruptFile,
					"The default-level package contains an invalid Level object path."};
		}

		if (LevelCount == 0)
			return {EAssetReadError::TypeMismatch,
				std::format("The package '{}' does not contain a top-level Level asset.",
					Resolution.FinalPath.ToString())};
		if (LevelCount > 1)
			return {EAssetReadError::InvalidPackageType,
				std::format("The package '{}' contains multiple top-level Level assets; a default-level package must contain exactly one.",
					Resolution.FinalPath.ToString())};

		OutLevelPath = std::move(LevelPath);
		return {};
	}


	DLevel::DLevel(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer), TickRegistry(this)
	{
	}

	DLevel::~DLevel()
	{
		TickRegistry.Reset();
	}

	auto DLevel::SpawnActor(DClass* ActorClass, FName InName, ESpawnActorNameMode NameMode) -> AActor*
	{
		return SpawnActorInternal(ActorClass, InName, true, NameMode);
	}

	auto DLevel::SpawnActorDeferredPlay(DClass* ActorClass, FName InName) -> AActor*
	{
		return SpawnActorInternal(ActorClass, InName, false);
	}

	auto DLevel::SpawnActorInternal(DClass* ActorClass, FName InName, bool bDispatchBeginPlay, ESpawnActorNameMode NameMode) -> AActor*
	{
		if (OwningWorld && OwningWorld->IsEndingPlay()) return nullptr;
		if (!CanConstructObjectOfClass(ActorClass, AActor::StaticClass())) return nullptr;
		DObjectForceRegistration(ActorClass);
		const FName RequestedName = InName.IsNone()
			? FName(ActorClass->GetDefaultObjectName().empty() ? ActorClass->GetName() : ActorClass->GetDefaultObjectName()) : InName;
		const FName Name = ResolveActorName(RequestedName, InName.IsNone() ? ESpawnActorNameMode::Requested : NameMode);
		if (Name.IsNone()) return nullptr;
		EObjectFlags Flags = EObjectFlags::NoFlags;
#if DURIN_WITH_EDITOR
		if (!OwningWorld || OwningWorld->GetWorldType() == EWorldType::Editor) Flags |= EObjectFlags::Transactional;
#endif
		AActor* Actor = NewObject<AActor>(ActorClass, this, Name, Flags);
		if (!Actor) return nullptr;
		Actor->SetActorLabel(MakeUniqueActorLabel(RequestedName.ToString()));
		Actors.emplace_back(Actor);
		if (!Actor->RequestNativeReconstruction())
		{
			Actors.pop_back();
			MarkObjectHierarchyAsGarbage(Actor);
			return nullptr;
		}
#if DURIN_WITH_EDITOR
		NotifyEditorActorHierarchyChanged();
#endif
		OnActorAdded(Actor);
		if (bDispatchBeginPlay && OwningWorld && OwningWorld->HasBegunPlay()) Actor->DispatchBeginPlay();
		if (!PrimaryCameraActor)
		{
			if (auto* Camera = Cast<ACameraActor>(Actor)) PrimaryCameraActor = Camera;
		}
		MarkPackageDirty();
		return Actor;
	}

	auto DLevel::DestroyActor(AActor* Actor) -> bool
	{
		const auto It = std::find_if(Actors.begin(), Actors.end(), [Actor](const TObjectPtr<AActor>& Entry) { return Entry.Get() == Actor; });
		if (It == Actors.end())
		{
			return Actor
				&& Actor->GetOuter() == this
				&& Actor->DestructionState != EActorDestructionState::Alive;
		}
		if (Actor->DestructionState == EActorDestructionState::Destroying) return true;
		if (Actor->PlayState == EActorPlayState::BeginningPlay
			|| Actor->PlayState == EActorPlayState::EndingPlay)
		{
			Actor->DestructionState = EActorDestructionState::Requested;
			return true;
		}
		Actor->DestructionState = EActorDestructionState::Destroying;
		Actor->UnregisterTickFunction();
		Actor->OnActorDestroyed();
		if (OwningWorld) OwningWorld->OnActorDestroyed(Actor);
		Actor->RouteEndPlay();
		const bool bWasPrimaryCamera = PrimaryCameraActor.Get() == Actor;
		const std::vector<TObjectPtr<DActorComponent>> Components = Actor->GetComponentsSnapshot();
		for (const TObjectPtr<DActorComponent>& Component : Components)
		{
			if (!Component) continue;
			if (Component->IsRegistered()) Component->UnregisterComponent();
			Component->DestroyComponent();
		}
		const auto RemovalIt = std::find_if(Actors.begin(), Actors.end(), [Actor](const TObjectPtr<AActor>& Entry) { return Entry.Get() == Actor; });
		if (RemovalIt != Actors.end()) Actors.erase(RemovalIt);
#if DURIN_WITH_EDITOR
		NotifyEditorActorHierarchyChanged();
#endif
		if (bWasPrimaryCamera)
		{
			PrimaryCameraActor = nullptr;
			for (const TObjectPtr<AActor>& RemainingActor : Actors)
			{
				if (auto* Camera = Cast<ACameraActor>(RemainingActor.Get())) { PrimaryCameraActor = Camera; break; }
			}
		}
		MarkObjectHierarchyAsGarbage(Actor);
		MarkPackageDirty();
		return true;
	}

	auto DLevel::SetActorPresence(AActor* Actor, bool bPresent, size_t ActorIndex, bool bRegisterComponents) -> bool
	{
		if (!IsValid(Actor) || Actor->IsTemplateObject() || Actor->GetOuter() != this || Actor->IsBeingDestroyed()
			|| Actor->HasBegunPlay() || (OwningWorld && (OwningWorld->HasBegunPlay() || OwningWorld->IsEndingPlay()))
			|| ContainsActor(Actor) == bPresent) return false;
		if (bPresent)
		{
			if (!IsActorNameAvailable(Actor->GetFName(), Actor)) return false;
			Actors.insert(Actors.begin() + std::min(ActorIndex, Actors.size()), Actor);
			if (OwningWorld) Actor->RegisterTickFunction(this);
			if (bRegisterComponents) OnActorAdded(Actor);
			if (!PrimaryCameraActor) PrimaryCameraActor = Cast<ACameraActor>(Actor);
		}
		else
		{
			Actor->UnregisterTickFunction();
			for (const auto& Component : Actor->GetComponentsSnapshot())
				if (Component) Component->UnregisterComponent();
			std::erase_if(Actors, [Actor](const auto& Entry) { return Entry.Get() == Actor; });
			if (PrimaryCameraActor.Get() == Actor)
			{
				PrimaryCameraActor = nullptr;
				for (const auto& Remaining : Actors)
					if (auto* Camera = Cast<ACameraActor>(Remaining.Get())) { PrimaryCameraActor = Camera; break; }
			}
		}
#if DURIN_WITH_EDITOR
		NotifyEditorActorHierarchyChanged();
#endif
		MarkPackageDirty();
		return true;
	}

	auto DLevel::DestroyAllActors() -> void
	{
		const std::vector<TObjectPtr<AActor>> Snapshot = Actors;
		for (auto It = Snapshot.rbegin(); It != Snapshot.rend(); ++It)
		{
			if (*It) DestroyActor(It->Get());
		}
	}

	auto DLevel::ContainsActor(const AActor* Actor) const -> bool
	{
		return Actor && std::ranges::any_of(Actors, [Actor](const TObjectPtr<AActor>& Entry) { return Entry.Get() == Actor; });
	}

	auto DLevel::FindActorByName(FName Name) const -> AActor*
	{
		const auto It = std::find_if(Actors.begin(), Actors.end(), [Name](const TObjectPtr<AActor>& Entry) { return Entry && Entry->GetFName() == Name; });
		return It == Actors.end() ? nullptr : It->Get();
	}

	auto DLevel::RenameActor(AActor* Actor, FName RequestedName, ESpawnActorNameMode NameMode) -> bool
	{
		if (!ContainsActor(Actor) || RequestedName.IsNone()) return false;
		const FName Name = ResolveActorName(RequestedName, NameMode, Actor);
		if (Name.IsNone()) return false;
		if (Actor->GetFName() == Name) return true;
		// Freeze the fallback label of a legacy object before changing its internal name.
		Actor->SetActorLabel(Actor->GetActorLabel());
		Actor->Rename(Name);
#if DURIN_WITH_EDITOR
		NotifyEditorActorHierarchyChanged();
#endif
		MarkPackageDirty();
		return true;
	}

	auto DLevel::SetPrimaryCameraActor(ACameraActor* Actor) -> bool
	{
		if (Actor && !ContainsActor(Actor)) return false;
		PrimaryCameraActor = Actor;
		MarkPackageDirty();
		return true;
	}

	auto DLevel::MakeUniqueActorName(FName RequestedName, const AActor* IgnoredActor) const -> FName
	{
		if (RequestedName.IsNone()) return {};
		if (IsActorNameAvailable(RequestedName, IgnoredActor)) return RequestedName;
		const std::string BaseName = RequestedName.ToString();
		for (uint32 Suffix = 2;; ++Suffix)
		{
			FName Candidate(std::format("{}_{}", BaseName, Suffix));
			if (IsActorNameAvailable(Candidate, IgnoredActor)) return Candidate;
		}
	}

	auto DLevel::IsActorNameAvailable(FName Name, const AActor* IgnoredActor) const -> bool
	{
		if (Name.IsNone()) return false;
		const auto Objects = GDObjectArray.GetObjectsWithOuter(this, EObjectQueryScope::LiveOnly);
		return std::ranges::none_of(Objects, [&](DObject* Object) { return Object != IgnoredActor && Object->GetFName() == Name; });
	}

	auto DLevel::ResolveActorName(FName RequestedName, ESpawnActorNameMode NameMode, const AActor* IgnoredActor) const -> FName
	{
		switch (NameMode)
		{
		case ESpawnActorNameMode::Requested: return MakeUniqueActorName(RequestedName, IgnoredActor);
		case ESpawnActorNameMode::RequiredErrorAndReturnNull:
			if (IsActorNameAvailable(RequestedName, IgnoredActor)) return RequestedName;
			DURIN_ERROR("Actor object name '{}' is unavailable in '{}'.", RequestedName, GetObjectPath());
			return {};
		case ESpawnActorNameMode::RequiredReturnNull:
			return IsActorNameAvailable(RequestedName, IgnoredActor) ? RequestedName : FName{};
		}
		return {};
	}

	auto DLevel::MakeUniqueActorLabel(const std::string& RequestedLabel, const AActor* IgnoredActor,
		std::span<const std::string> ReservedLabels) const -> std::string
	{
		const auto Occupied = [&](const std::string& Label) {
			return std::ranges::find(ReservedLabels, Label) != ReservedLabels.end()
				|| std::ranges::any_of(Actors, [&](const auto& Actor) { return Actor && Actor.Get() != IgnoredActor && Actor->GetActorLabel() == Label; });
		};
		std::string Label = RequestedLabel;
		for (uint32 Suffix = 2; Occupied(Label); ++Suffix) Label = std::format("{}_{}", RequestedLabel, Suffix);
		return Label;
	}

	auto DLevel::OnActorAdded(AActor* Actor) -> void
	{
		if (!Actor || !OwningWorld) return;
		Actor->RegisterTickFunction(this);
		const std::vector<TObjectPtr<DActorComponent>> Components = Actor->GetComponentsSnapshot();
		for (const TObjectPtr<DActorComponent>& Component : Components)
		{
			if (Component
				&& !Component->IsPendingKill()
				&& Component->GetOwner() == Actor
				&& Actor->OwnsComponent(Component.Get())
				&& !Component->IsBeingDestroyed()
				&& !Component->IsRegistered())
			{
				Component->RegisterComponent();
			}
		}
	}

	auto DLevel::ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError>
	{
		if (auto Result = Super::ValidateLoadedObjectGraph(Context); !Result) return Result;
		for (const auto& Actor : Actors)
		{
			if (!Actor || Actor->GetOuter() != this)
				return RejectLoadedObjectGraph(GetObjectPath(), "Level contains an actor outside its owned graph.");
			for (const auto& Component : Actor->GetComponents())
			{
				if (!Component || Component->GetOuter() != Actor.Get())
					return RejectLoadedObjectGraph(GetObjectPath(), "Actor contains a component outside its owned graph.");
				const auto* Scene = Cast<DSceneComponent>(Component.Get());
				if (!Scene) continue;
				std::unordered_set<const DSceneComponent*> Visited;
				for (const DSceneComponent* Parent = Scene->AttachParent.Get(); Parent; Parent = Parent->AttachParent.Get())
				{
					if (Parent == Scene || !Visited.insert(Parent).second)
						return RejectLoadedObjectGraph(GetObjectPath(), "Component attachment graph contains a cycle.");
					if (!Parent->GetOwner() || Parent->GetOwner()->GetOuter() != this)
						return RejectLoadedObjectGraph(GetObjectPath(), "Component attachment crosses level ownership.");
				}
			}
		}
		return {};
	}

	auto DLevel::PostLoad() -> void
	{
		Super::PostLoad();
		std::erase_if(Actors, [&](const TObjectPtr<AActor>& Actor) {
			if (Actor && Actor->GetOuter() == this) return false;
			DURIN_ERROR("PostLoad '{}': removing an actor outside its object graph.", GetObjectPath());
			return true;
		});
		std::vector<DSceneComponent*> SceneComponents;
		for (const TObjectPtr<AActor>& ActorPtr : Actors)
		{
			AActor* Actor = ActorPtr.Get();
			for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetComponents())
			{
				DActorComponent* Component = ComponentPtr.Get();
				if (!Component || Component->GetOuter() != Actor)
				{
					DURIN_ERROR("PostLoad '{}': skipping a component outside its actor graph.", GetObjectPath());
					continue;
				}
				Component->SetOwnedByActor(true);
				if (auto* SceneComponent = Cast<DSceneComponent>(Component)) SceneComponents.push_back(SceneComponent);
			}
		}

		for (DSceneComponent* Component : SceneComponents)
		{
			Component->AttachChildren.clear();
			std::unordered_set<DSceneComponent*> Visited;
			for (DSceneComponent* Parent = Component->AttachParent.Get(); Parent; Parent = Parent->AttachParent.Get())
			{
				if (!Visited.insert(Parent).second || Parent == Component)
				{
					DURIN_ERROR("PostLoad '{}': breaking a component attachment cycle.", GetObjectPath());
					Component->AttachParent = nullptr;
					break;
				}
				if (!Parent->GetOwner() || Parent->GetOwner()->GetOuter() != this)
				{
					DURIN_ERROR("PostLoad '{}': clearing a cross-level component attachment.", GetObjectPath());
					Component->AttachParent = nullptr;
					break;
				}
			}
		}
		for (const TObjectPtr<AActor>& Actor : Actors)
		{
			if (Actor && !Actor->RequestNativeReconstruction())
			{
				DURIN_ERROR("PostLoad '{}': {}", Actor->GetObjectPath(), Actor->GetNativeConstructionError());
			}
		}
		for (DSceneComponent* Component : SceneComponents)
		{
			if (DSceneComponent* Parent = Component->AttachParent.Get()) Parent->AttachChildren.emplace_back(Component);
		}
		for (DSceneComponent* Component : SceneComponents)
		{
			if (!Component->AttachParent) Component->UpdateComponentToWorld();
		}

		if (!PrimaryCameraActor || !ContainsActor(PrimaryCameraActor.Get()))
		{
			PrimaryCameraActor = nullptr;
			for (const TObjectPtr<AActor>& Actor : Actors)
			{
				if (auto* Camera = Cast<ACameraActor>(Actor.Get())) { PrimaryCameraActor = Camera; break; }
			}
		}
#if DURIN_WITH_EDITOR
		NotifyEditorActorHierarchyChanged();
#endif
	}

}
