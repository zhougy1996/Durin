#include "Materials/ObjectCacheContext.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialLoadedQueryDiagnostics.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"
#include <unordered_map>
#include <unordered_set>

namespace Durin
{
	struct FObjectCacheContext::FState
	{
		FGarbageCollectionDeferralScope Lifetime;
		FMaterialLoadedQueryDiagnostics Diagnostics;
		bool bBuilt = false;
		bool bDiscoveryOpen = true;
		std::unordered_set<FObjectKey> Admitted;
		std::unordered_map<FObjectKey, std::vector<DMaterialInterface*>> Children;

		auto CheckDiscovery() const -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			requiref(bDiscoveryOpen, "Object cache discovery cannot resume after external notifications.");
		}

		auto Build() -> void
		{
			if (bBuilt) return;
			const auto Objects = GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly);
			std::vector<DMaterialInterface*> Materials;
			for (auto* Object : Objects)
				if (auto* Material = Cast<DMaterialInterface>(Object); IsValid(Material))
				{
					Materials.push_back(Material);
					Admitted.emplace(Material);
				}
			std::unordered_set<FObjectKey> Indexed;
			for (auto* Material : Materials)
				for (auto* Node = Material; Node && Indexed.emplace(Node).second; Node = Node->GetParent())
					if (auto* Parent = Node->GetParent()) Children[FObjectKey(Parent)].push_back(Node);
			// Ancestors excluded from result admission may still connect live descendants.
			// Each canonical edge is inserted once, including malformed cycles.
			bBuilt = true;
			for (auto* Counts : {&Diagnostics, &Private::GetMutableMaterialLoadedQueryDiagnostics()})
			{
				++Counts->SnapshotCount;
				++Counts->ParentTableBuildCount;
				Counts->ScannedObjectCount += Objects.size();
				Counts->ScannedMaterialCount += Materials.size();
			}
		}

		auto Result(std::vector<DMaterialInterface*> Objects, EMaterialLoadedQueryOperation Operation)
			-> TObjectCacheIterator<DMaterialInterface>
		{
			std::ranges::sort(Objects, {}, [](auto* Object) { return FObjectKey(Object); });
			for (auto* Counts : {&Diagnostics, &Private::GetMutableMaterialLoadedQueryDiagnostics()})
			{
				++Counts->QueryCount;
				Counts->LastOperation = Operation;
				Counts->LastResultCount = Objects.size();
			}
			return TObjectCacheIterator<DMaterialInterface>(std::move(Objects));
		}
	};

	FObjectCacheContext::FObjectCacheContext() : State(std::make_unique<FState>()) {}
	FObjectCacheContext::~FObjectCacheContext() = default;
	auto FObjectCacheContext::GetDiagnostics() const -> const FMaterialLoadedQueryDiagnostics& { return State->Diagnostics; }
	auto FObjectCacheContext::EndDiscovery() -> void { if (GIsGameThreadIdInitialized) CheckGameThread(); State->bDiscoveryOpen = false; }

	auto FObjectCacheContext::GetDirectMaterialChildren(const DMaterialInterface* Parent)
		-> TObjectCacheIterator<DMaterialInterface>
	{
		State->CheckDiscovery();
		if (!IsValid(Parent)) return TObjectCacheIterator<DMaterialInterface>({});
		State->Build();
		std::vector<DMaterialInterface*> Result;
		const auto It = State->Children.find(FObjectKey(Parent));
		if (It != State->Children.end())
			for (auto* Child : It->second)
				if (IsValid(Child) && Cast<DMaterialInstance>(Child) && State->Admitted.contains(FObjectKey(Child)))
					Result.push_back(Child);
		return State->Result(std::move(Result), EMaterialLoadedQueryOperation::DirectChildren);
	}

	auto FObjectCacheContext::GetMaterialsAffectedByMaterial(const DMaterialInterface* Material)
		-> TObjectCacheIterator<DMaterialInterface>
	{
		auto* Root = const_cast<DMaterialInterface*>(Material);
		return GetMaterialsAffectedByMaterials({&Root, 1});
	}

	auto FObjectCacheContext::GetMaterialsAffectedByMaterials(std::span<DMaterialInterface* const> Materials)
		-> TObjectCacheIterator<DMaterialInterface>
	{
		State->CheckDiscovery();
		std::vector<DMaterialInterface*> Pending;
		for (auto* Material : Materials) if (IsValid(Material)) Pending.push_back(Material);
		if (Pending.empty()) return TObjectCacheIterator<DMaterialInterface>({});
		State->Build();
		std::unordered_set<FObjectKey> Visited;
		std::vector<DMaterialInterface*> Result;
		while (!Pending.empty())
		{
			auto* Material = Pending.back();
			Pending.pop_back();
			const FObjectKey Key(Material);
			if (!Visited.insert(Key).second) continue;
			if (State->Admitted.contains(Key) && IsValid(Material)) Result.push_back(Material);
			const auto It = State->Children.find(Key);
			if (It != State->Children.end()) Pending.insert(Pending.end(), It->second.begin(), It->second.end());
		}
		return State->Result(std::move(Result), EMaterialLoadedQueryOperation::Dependents);
	}
}
