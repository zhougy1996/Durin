#include "Tickable.h"

#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"
#include "Engine/World.h"

namespace Durin
{
	namespace
	{
		struct FTickableEntry
		{
			FTickableGameObject* Object;
			ETickableTickType Type;
		};
		struct FTickableStatics
		{
			std::vector<FTickableEntry> Objects;
			std::vector<FTickableEntry> Pending;
			bool bTicking = false;
		};
		auto GetTickableStatics() -> FTickableStatics&
		{
			// Objects can unregister during static destruction.
			static auto* Statics = new FTickableStatics;
			return *Statics;
		}
		auto CheckTickThread() -> void { require(!GIsGameThreadIdInitialized || IsInGameThread()); }
	}

	FTickableGameObject::FTickableGameObject(ETickableTickType StartingTickType)
	{
		SetTickableTickType(StartingTickType);
	}
	FTickableGameObject::~FTickableGameObject()
	{
		SetTickableTickType(ETickableTickType::Never);
	}
	auto FTickableGameObject::SetTickableTickType(ETickableTickType NewTickType) -> void
	{
		if (!bRegistered && NewTickType == ETickableTickType::Never) return;
		CheckTickThread();
		require(NewTickType <= ETickableTickType::NewObject);
		auto& Statics = GetTickableStatics();
		if (NewTickType == ETickableTickType::Never)
		{
			for (auto& Entry : Statics.Objects) if (Entry.Object == this) Entry.Object = nullptr;
			std::erase_if(Statics.Pending, [this](const auto& Entry) { return Entry.Object == this; });
			if (!Statics.bTicking) std::erase_if(Statics.Objects, [](const auto& Entry) { return !Entry.Object; });
			bRegistered = false;
			return;
		}
		for (auto& Entry : Statics.Objects)
			if (Entry.Object == this)
			{
				requiref(NewTickType != ETickableTickType::NewObject, "An active Tickable cannot return to NewObject.");
				Entry.Type = NewTickType;
				return;
			}
		for (auto& Entry : Statics.Pending)
			if (Entry.Object == this) { Entry.Type = NewTickType; return; }
		Statics.Pending.push_back({this, NewTickType});
		bRegistered = true;
	}
	auto FTickableGameObject::TickObjects(DWorld* World, ELevelTick TickType, bool bIsPaused, float DeltaSeconds) -> void
	{
		CheckTickThread();
		if (IsEngineExitRequested()) return;
		auto& Statics = GetTickableStatics();
		requiref(!Statics.bTicking, "Tickable dispatch cannot be nested.");
		Statics.bTicking = true;
		struct FFinishTick
		{
			FTickableStatics& Statics;
			~FFinishTick()
			{
				std::erase_if(Statics.Objects, [](const auto& Entry) { return !Entry.Object; });
				Statics.bTicking = false;
			}
		} Finish{Statics};
		// Resolve every pending registration before any object ticks.
		// Keep pending membership intact if a query throws.
		for (auto& Entry : Statics.Pending)
		{
			if (Entry.Type == ETickableTickType::NewObject) Entry.Type = Entry.Object->GetTickableTickType();
			requiref(Entry.Type <= ETickableTickType::Never, "GetTickableTickType must resolve NewObject.");
		}
		Statics.Objects.reserve(Statics.Objects.size() + Statics.Pending.size());
		for (const auto& Entry : Statics.Pending)
		{
			if (Entry.Type == ETickableTickType::Never) Entry.Object->bRegistered = false;
			else Statics.Objects.push_back(Entry);
		}
		Statics.Pending.clear();
		for (auto& Entry : Statics.Objects)
		{
			auto* Object = Entry.Object;
			if (!Object) continue;
			if (!Object->IsAllowedToTick()) continue;
			if (Entry.Type == ETickableTickType::Conditional && !Object->IsTickable()) continue;
			if (Object->GetTickableGameObjectWorld() != World) continue;
			const bool bIsGameWorld = TickType == LEVELTICK_All
				|| (World && (World->GetWorldType() == EWorldType::Game || World->GetWorldType() == EWorldType::PIE));
			if (!((GIsEditor && Object->IsTickableInEditor())
				|| (bIsGameWorld && ((!bIsPaused && TickType != LEVELTICK_TimeOnly)
					|| (bIsPaused && Object->IsTickableWhenPaused()))))) continue;
			Object->Tick(DeltaSeconds);
		}
	}
}
