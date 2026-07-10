#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"

#include "Level.gen.h"

namespace Durin
{
	class AActor;
	class ACameraActor;
	class DWorld;

	DCLASS()
	class ENGINE_API DLevel : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DLevel(const FObjectInitializer& ObjectInitializer);
		~DLevel() override = default;

		template<typename T>
		auto SpawnActor(FName InName = FName()) -> T*
		{
			static_assert(std::is_base_of_v<AActor, T>, "T must derive from AActor");
			const FName UniqueName = MakeUniqueActorName(InName.IsNone() ? FName(T::StaticClass()->GetName()) : InName);
			T* Actor = NewObject<T>(this, UniqueName);
			Actors.emplace_back(Actor);
			OnActorAdded(Actor);
			MarkPackageDirty();
			return Actor;
		}

		auto DestroyActor(AActor* Actor) -> bool;
		auto DestroyAllActors() -> void;
		auto ContainsActor(const AActor* Actor) const -> bool;
		auto FindActorByName(FName Name) const -> AActor*;
		auto GetActors() const -> const std::vector<TObjectPtr<AActor>>& { return Actors; }

		auto SetPrimaryCameraActor(ACameraActor* Actor) -> bool;
		auto GetPrimaryCameraActor() const -> ACameraActor* { return PrimaryCameraActor.Get(); }
		auto GetWorld() const -> DWorld* { return OwningWorld; }
		auto PostLoad(std::string& OutError) -> bool override;

	private:
		auto MakeUniqueActorName(FName RequestedName) const -> FName;
		auto OnActorAdded(AActor* Actor) -> void;
		auto SetOwningWorld(DWorld* World) -> void { OwningWorld = World; }

		DPROPERTY()
		std::vector<TObjectPtr<AActor>> Actors;

		DPROPERTY()
		TObjectPtr<ACameraActor> PrimaryCameraActor;

		DWorld* OwningWorld = nullptr;

		friend class DWorld;
	};
}
