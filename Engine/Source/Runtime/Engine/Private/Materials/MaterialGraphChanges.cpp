#include "Materials/MaterialGraphChanges.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "DObject/Archive.h"
#include "DObject/Property.h"
#include "Threading/RunnableThread.h"
#include <unordered_map>

namespace Durin
{
	using N = EMaterialGraphNodeChange;
	using G = EMaterialGraphChange;

	auto FMaterialGraphChangeSet::MarkNode(FGuid Id, N Change) -> void
	{
		if (Has(G::Reset) || Change == N::None) return;
		auto It = std::ranges::find(Nodes, Id, &FMaterialGraphNodeChange::NodeId);
		if (It == Nodes.end()) { Nodes.push_back({Id, Change}); return; }
		if ((Change & N::Removed) != N::None)
		{
			if ((It->Flags & N::Added) != N::None) Nodes.erase(It);
			else It->Flags = N::Removed;
		}
		else if ((Change & N::Added) != N::None)
		{
			It->Flags = (It->Flags & N::Removed) != N::None
				? N::Content | N::Interface | N::Inputs | N::Position : N::Added;
		}
		else if ((It->Flags & (N::Added | N::Removed)) == N::None) It->Flags |= Change;
	}
	auto FMaterialGraphChangeSet::MarkGraph(G Change) -> void
	{
		Flags |= Change;
		if (Has(G::Reset)) { Flags = G::Reset; Nodes.clear(); }
	}
	auto FMaterialGraphChangeSet::Merge(const FMaterialGraphChangeSet& Other) -> void
	{
		MarkGraph(Other.Flags);
		for (const auto& Node : Other.Nodes) MarkNode(Node.NodeId, Node.Flags);
	}

	namespace
	{
		std::vector<TWeakObjectPtr<DObject>> ObservedOwners;
		struct FNodeState
		{
			DClass* Class = nullptr;
			std::vector<FPropertyValueSnapshotPayload> Properties;
		};
		struct FGraphState
		{
			std::unordered_map<FGuid, FNodeState> Nodes;
			std::vector<FGuid> Order;
			FMaterialGraphPresentation Presentation;
			bool bValid = true;
		};
		auto Capture(DObject& Owner) -> FGraphState
		{
			FGraphState State;
			const FMaterialExpressionCollection* Collection;
			if (const auto* Material = Cast<DMaterial>(&Owner))
			{
				Collection = &Material->GetExpressionCollection();
				State.Presentation = Material->GetMaterialGraphPresentation();
			}
			else
			{
				const auto* Function = Cast<DMaterialFunction>(&Owner);
				check(Function);
				Collection = &Function->GetExpressionCollection();
				State.Presentation.Nodes = Function->GetFunctionPresentation().Nodes;
			}
			for (const auto& Expression : Collection->Expressions)
			{
				if (!Expression) { State.bValid = false; continue; }
				State.Order.push_back(Expression->Id);
				auto& Node = State.Nodes[Expression->Id];
				Node.Class = Expression->GetClass();
				Node.Class->ForEachProperty([&](FProperty* Property) {
					for (uint32 Index = 0; Index < Property->GetArrayDim(); ++Index)
					{
						FPropertyValueSnapshotPayload Payload;
						State.bValid &= CapturePropertyValuePayload(Property, Expression.Get(), Index, Payload);
						Node.Properties.push_back(std::move(Payload));
					}
				});
			}
			return State;
		}
		auto DifferencePresentation(const FMaterialGraphPresentation& Before,
			const FMaterialGraphPresentation& After) -> FMaterialGraphChangeSet
		{
			FMaterialGraphChangeSet Result;
			std::unordered_map<FGuid, FMaterialGraphNodePresentation> Positions;
			for (const auto& Position : Before.Nodes) Positions.emplace(Position.NodeId, Position);
			for (const auto& Position : After.Nodes)
			{
				const auto It = Positions.find(Position.NodeId);
				if (It == Positions.end() || It->second != Position)
				{
					Result.MarkNode(Position.NodeId, N::Position);
					if (It == Positions.end() ? !Position.DisplayName.empty() : It->second.DisplayName != Position.DisplayName)
						Result.MarkNode(Position.NodeId, N::Content);
				}
				Positions.erase(Position.NodeId);
			}
			for (const auto& [Id, Position] : Positions)
				Result.MarkNode(Id, Position.DisplayName.empty() ? N::Position : N::Position | N::Content);
			return Result;
		}
		auto Difference(const FGraphState& Before, const FGraphState& After) -> FMaterialGraphChangeSet
		{
			FMaterialGraphChangeSet Result;
			if (!Before.bValid || !After.bValid) { Result.MarkGraph(G::Reset); return Result; }
			for (const auto& [Id, Old] : Before.Nodes)
				if (!After.Nodes.contains(Id)) Result.MarkNode(Id, N::Removed);
			for (const auto& [Id, Node] : After.Nodes)
			{
				const auto It = Before.Nodes.find(Id);
				if (It == Before.Nodes.end()) { Result.MarkNode(Id, N::Added); continue; }
				const auto& Old = It->second;
				if (Old.Class != Node.Class || Old.Properties.size() != Node.Properties.size())
				{
					Result.MarkNode(Id, N::Content | N::Interface | N::Inputs);
					continue;
				}
				for (size_t Index = 0; Index < Node.Properties.size(); ++Index)
				{
					if (Old.Properties[Index] == Node.Properties[Index]) continue;
					const auto Name = Node.Properties[Index].GetProperty()->NamePrivate;
					// Known display/value fields cannot change pin identities or connections.
					// Unknown fields conservatively invalidate the node interface as well.
					const bool bContent = Name == FName("Value") || Name == FName("DefaultValue")
						|| Name == FName("Metadata") || Name == FName("bHasRange")
						|| Name == FName("MinimumValue") || Name == FName("MaximumValue");
					Result.MarkNode(Id, bContent ? N::Content : N::Content | N::Interface | N::Inputs);
				}
			}
			Result.Merge(DifferencePresentation(Before.Presentation, After.Presentation));
			// Unpositioned nodes use collection order for their deterministic fallback layout.
			if (Before.Order != After.Order)
				for (const auto Id : After.Order)
					if (After.Nodes.at(Id).Class != DMaterialExpressionMaterialOutput::StaticClass()
						&& std::ranges::find(After.Presentation.Nodes, Id, &FMaterialGraphNodePresentation::NodeId)
						== After.Presentation.Nodes.end()) Result.MarkNode(Id, N::Position);
			return Result;
		}
	}

	struct FMaterialGraphChangeSource::FImpl
	{
		TMulticastDelegate<void(const FMaterialGraphChangeSet&)> Changed;
		std::vector<std::pair<FDelegateHandle, std::shared_ptr<bool>>> Listeners;
		FGraphState Checkpoint;
		uint32 BatchDepth = 0;
		bool bPending = false;
		bool bGraphPending = false;
		TWeakObjectPtr<DObject> Owner;
		bool bDispatching = false;
	};
	FMaterialGraphChangeSource::FMaterialGraphChangeSource() = default;
	FMaterialGraphChangeSource::~FMaterialGraphChangeSource() = default;
	auto FMaterialGraphChangeSource::Subscribe(DObject& Owner,
		std::function<void(const FMaterialGraphChangeSet&)> Callback) -> FDelegateHandle
	{
		check(IsInGameThread());
		if (!Impl) Impl = std::make_unique<FImpl>();
		if (!Impl->Changed.IsBound())
		{
			Impl->Checkpoint = Capture(Owner);
			Impl->Owner = &Owner;
			std::erase_if(ObservedOwners, [](const auto& Entry) { return !Entry.IsValid(); });
			ObservedOwners.emplace_back(&Owner);
		}
		auto Active = std::make_shared<bool>(true);
		const auto Handle = Impl->Changed.AddLambda([Active, Callback = std::move(Callback)](const auto& Changes) {
			if (*Active) Callback(Changes);
		});
		Impl->Listeners.emplace_back(Handle, std::move(Active));
		return Handle;
	}
	auto FMaterialGraphChangeSource::Unsubscribe(FDelegateHandle Handle) -> void
	{
		check(IsInGameThread());
		if (!Impl) return;
		// The delegate takes a listener snapshot. Disable detached callbacks in that
		// snapshot too, so closing another observer during publication is safe.
		std::erase_if(Impl->Listeners, [&](const auto& Listener) {
			if (Listener.first != Handle) return false;
			*Listener.second = false;
			return true;
		});
		Impl->Changed.Remove(Handle);
		if (!Impl->Changed.IsBound())
		{
			Impl->Checkpoint = {};
			std::erase_if(ObservedOwners, [&](const auto& Entry) { return Entry.GetKey() == Impl->Owner.GetKey() || !Entry.IsValid(); });
		}
	}
	auto FMaterialGraphChangeSource::Publish(DObject& Owner) -> void
	{
		check(IsInGameThread());
		if (!Impl || !Impl->Changed.IsBound()) return;
		Impl->bGraphPending = true;
		Impl->bPending = true;
		Flush(Owner);
	}
	auto FMaterialGraphChangeSource::PublishPresentation(DObject& Owner) -> void
	{
		check(IsInGameThread());
		if (!Impl || !Impl->Changed.IsBound()) return;
		Impl->bPending = true;
		Flush(Owner);
	}
	auto FMaterialGraphChangeSource::Flush(DObject& Owner) -> void
	{
		if (Impl->BatchDepth || Impl->bDispatching) return;
		Impl->bDispatching = true;
		struct FDispatchGuard
		{
			bool& Dispatching;
			~FDispatchGuard() { Dispatching = false; }
		} Guard{Impl->bDispatching};
		while (Impl->bPending && Impl->Changed.IsBound())
		{
			Impl->bPending = false;
			FMaterialGraphChangeSet Changes;
			if (std::exchange(Impl->bGraphPending, false))
			{
				auto Current = Capture(Owner);
				Changes = Difference(Impl->Checkpoint, Current);
				Impl->Checkpoint = std::move(Current);
			}
			else
			{
				FMaterialGraphPresentation Current;
				if (const auto* Material = Cast<DMaterial>(&Owner)) Current = Material->GetMaterialGraphPresentation();
				else Current.Nodes = Cast<DMaterialFunction>(&Owner)->GetFunctionPresentation().Nodes;
				Changes = DifferencePresentation(Impl->Checkpoint.Presentation, Current);
				Impl->Checkpoint.Presentation = std::move(Current);
			}
			if (!Changes.IsEmpty()) Impl->Changed.Broadcast(Changes);
		}
	}
	auto FMaterialGraphChangeSource::BeginBatch() -> void
	{
		check(IsInGameThread());
		if (!Impl) Impl = std::make_unique<FImpl>();
		++Impl->BatchDepth;
	}
	auto FMaterialGraphChangeSource::EndBatch(DObject& Owner) -> void
	{
		check(Impl && Impl->BatchDepth);
		if (--Impl->BatchDepth == 0 && Impl->bPending) Flush(Owner);
	}
	auto RefreshMaterialGraphObservers() -> void
	{
		check(IsInGameThread());
		const auto Owners = ObservedOwners;
		for (const auto& Entry : Owners)
			if (auto* Owner = Entry.Get()) GetMaterialGraphChangeSource(*Owner).Publish(*Owner);
	}
	auto GetMaterialGraphChangeSource(DObject& Owner) -> FMaterialGraphChangeSource&
	{
		if (auto* Material = Cast<DMaterial>(&Owner)) return Material->GetGraphChanges();
		auto* Function = Cast<DMaterialFunction>(&Owner);
		check(Function);
		return Function->GetGraphChanges();
	}
	FScopedMaterialGraphChange::FScopedMaterialGraphChange(DObject& InOwner) : Owner(InOwner)
		{ GetMaterialGraphChangeSource(Owner).BeginBatch(); }
	FScopedMaterialGraphChange::~FScopedMaterialGraphChange()
		{ GetMaterialGraphChangeSource(Owner).EndBatch(Owner); }
}
