#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"

namespace Durin
{
	class AActor;

	class ENGINE_API DWorld
	{
	public:
		DWorld() = default;
		~DWorld();

		DWorld(const DWorld&) = delete;
		DWorld& operator=(const DWorld&) = delete;

		template<typename T>
		auto SpawnActor(FName InName = FName()) -> T*
		{
			static_assert(std::is_base_of_v<AActor, T>, "T must derive from AActor");
			const FName UniqueName = MakeUniqueActorName(InName.IsNone() ? FName(T::StaticClass()->GetName()) : InName);
			T* Actor = NewObject<T>(nullptr, UniqueName);
			Actors.emplace_back(Actor);
			return Actor;
		}

		auto DestroyActor(AActor* Actor) -> bool;
		auto ContainsActor(const AActor* Actor) const -> bool;
		auto FindActorByName(FName Name) const -> AActor*;
		auto GetActors() const -> const std::vector<TObjectPtr<AActor>>& { return Actors; }

	private:
		auto MakeUniqueActorName(FName RequestedName) const -> FName;

		std::vector<TObjectPtr<AActor>> Actors;
	};
} // namespace Durin
