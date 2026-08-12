#pragma once

#include "EngineAPI.h"
#include "Misc/Guid.h"

namespace Durin
{
	class AActor;
	class DActorComponent;
	class DClass;

	// Identifies one derived component independently of object name or construction generation.
	struct FActorGeneratedComponentKey
	{
		FName Namespace;
		FGuid Id;

		auto IsValid() const -> bool { return !Namespace.IsNone() && Id.IsValid(); }
		auto operator==(const FActorGeneratedComponentKey&) const -> bool = default;
	};

	// Stages one complete keyed desired set and commits it atomically to an actor.
	class FActorConstructionContext final
	{
	public:
		ENGINE_API ~FActorConstructionContext();
		FActorConstructionContext(const FActorConstructionContext&) = delete;
		auto operator=(const FActorConstructionContext&) -> FActorConstructionContext& = delete;

		ENGINE_API auto AcquireGeneratedComponent(const FActorGeneratedComponentKey& Key,
			DClass* ExactClass, FName RequestedName = FName()) -> DActorComponent*;
		auto HasFailed() const -> bool { return !Error.empty(); }
		auto GetError() const -> const std::string& { return Error; }
		auto GetGeneration() const -> uint64 { return Generation; }

	private:
		friend class AActor;
		struct FDesiredEntry
		{
			FActorGeneratedComponentKey Key;
			DActorComponent* Component = nullptr;
			bool bCandidate = false;
		};

		FActorConstructionContext(AActor& InActor, uint64 InGeneration);
		auto Commit(std::string& OutError) -> bool;
		auto RollbackCandidates() -> void;
		auto Fail(std::string Message) -> DActorComponent*;

		AActor& Actor;
		uint64 Generation = 0;
		std::vector<FDesiredEntry> Desired;
		std::string Error;
		bool bCommitted = false;
	};
} // namespace Durin
