#include "Materials/ObjectCacheContext.h"
#include "DObject/Package.h"
#include "DObject/ObjectGraphReplacement.h"
#include "MaterialDependencyIndex.h"
#include "DObject/ObjectLifecycle.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialLoadedQueryDiagnostics.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"
#include <unordered_map>
#include <unordered_set>

namespace Durin
{
	namespace Private
	{
		auto GetMaterialDependencyIndex() -> FMaterialDependencyIndex&
		{
			static FMaterialDependencyIndex Index;
			return Index;
		}
		static auto SetEdge(FMaterialDependencyIndex& Index, FObjectKey Child, FObjectKey Parent) -> void
		{
			const auto Existing = Index.Parents.find(Child);
			if (Existing != Index.Parents.end())
			{
				if (Existing->second == Parent) return;
				const auto Children = Index.Children.find(Existing->second);
				if (Children != Index.Children.end())
				{
					Children->second.erase(Child);
					if (Children->second.empty()) Index.Children.erase(Children);
				}
				Index.Parents.erase(Existing);
			}
			else if (Parent.IsNull()) return;
			if (!Parent.IsNull())
			{
				Index.Parents.emplace(Child, Parent);
				Index.Children[Parent].insert(Child);
			}
			if (++Index.Revision == 0) ++Index.Revision;
		}
		auto RefreshMaterialDependency(DMaterialInterface& Material) -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			const FObjectKey Key(&Material);
			if (!Key.IsNull()) SetEdge(GetMaterialDependencyIndex(), Key, FObjectKey(Material.GetParent()));
		}
		auto RemoveMaterialDependency(DMaterialInterface& Material) -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			SetEdge(GetMaterialDependencyIndex(), FObjectKey(&Material), {});
		}
		auto PrepareMaterialDependencyReplacement(const FObjectReplacementMap& Map) -> FMaterialDependencyIndex
		{
			FMaterialDependencyIndex Candidate;
			Candidate.Revision = GetMaterialDependencyIndex().Revision + 1;
			if (Candidate.Revision == 0) ++Candidate.Revision;
			for (const auto& [Child, Parent] : GetMaterialDependencyIndex().Parents)
			{
				auto* Object = Child.ResolveObjectPtr();
				if (!Object || Map.Find(Object)) continue;
				auto ParentKey = Parent;
				if (const auto* Entry = Map.Find(Parent.ResolveObjectPtr())) ParentKey = FObjectKey(Entry->Replacement);
				SetEdge(Candidate, Child, ParentKey);
			}
			for (auto* Object : Map.GetPreparedObjects())
				if (auto* Material = Cast<DMaterialInterface>(Object))
				{
					auto* Parent = Material->GetParent();
					if (const auto* Entry = Map.Find(Parent)) Parent = Cast<DMaterialInterface>(Entry->Replacement);
					SetEdge(Candidate, FObjectKey(Material), FObjectKey(Parent));
				}
			return Candidate;
		}
	}

	struct FObjectCacheContext::FState
	{
		FGarbageCollectionDeferralScope Lifetime;
		FMaterialLoadedQueryDiagnostics Diagnostics;
		bool bBuilt = false;
		bool bDiscoveryOpen = true;
		std::unordered_set<FObjectKey> Admitted;
		std::unordered_map<FObjectKey, std::vector<FObjectKey>> Children;

		auto CheckDiscovery() const -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			requiref(bDiscoveryOpen, "Object cache discovery cannot resume after external notifications.");
		}

		auto Build() -> void
		{
			if (bBuilt) return;
			bBuilt = true;
			for (auto* Counts : {&Diagnostics, &Private::GetMutableMaterialLoadedQueryDiagnostics()})
				++Counts->SnapshotCount;
		}
		auto Capture(FObjectKey Key) -> const std::vector<FObjectKey>&
		{
			const auto [It, Inserted] = Children.try_emplace(Key);
			if (!Inserted) return It->second;
			const auto& Index = Private::GetMaterialDependencyIndex();
			if (const auto Found = Index.Children.find(Key); Found != Index.Children.end())
				It->second.assign(Found->second.begin(), Found->second.end());
			auto* Material = Cast<DMaterialInterface>(Key.ResolveObjectPtr());
			if (Material)
			{
				for (auto* Counts : {&Diagnostics, &Private::GetMutableMaterialLoadedQueryDiagnostics()})
					++Counts->ScannedMaterialCount;
				const auto* Package = Material->GetPackage();
				if (IsValid(Material) && !Material->IsTemplateObject() && (!Package || !Package->IsGraphPrivate()))
					Admitted.emplace(Key);
			}
			return It->second;
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
		for (const auto Key : State->Capture(FObjectKey(Parent)))
		{
			State->Capture(Key);
			if (State->Admitted.contains(Key))
				if (auto* Child = Cast<DMaterialInstance>(Key.ResolveObjectPtr()); IsValid(Child)) Result.push_back(Child);
		}

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
		std::vector<FObjectKey> Pending;
		for (auto* Material : Materials) if (Material) Pending.emplace_back(Material);
		if (Pending.empty()) return TObjectCacheIterator<DMaterialInterface>({});
		State->Build();
		std::unordered_set<FObjectKey> Visited;
		std::vector<DMaterialInterface*> Result;
		while (!Pending.empty())
		{
			const auto Key = Pending.back();
			Pending.pop_back();
			if (!Visited.insert(Key).second) continue;
			const auto& Children = State->Capture(Key);
			if (State->Admitted.contains(Key))
				if (auto* Material = Cast<DMaterialInterface>(Key.ResolveObjectPtr()); IsValid(Material)) Result.push_back(Material);
			Pending.insert(Pending.end(), Children.begin(), Children.end());
		}
		return State->Result(std::move(Result), EMaterialLoadedQueryOperation::Dependents);
	}
}
