#include "Engine/TransactionAnnotations.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/ObjectGraphReplacement.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"

namespace Durin
{
	namespace
	{
		// UE's FDiffableObjectDataWriter writes references as path FNames. Keep
		// that observation representation separate from exact-key undo payloads.
		auto ObservationPath(FName Path) -> uint64
		{
			return (Path.IsNone() ? 0 : Path.GetComparisonNameEntry()->GetComparisonId().GetValue())
				| (uint64(Path.GetNumber()) << 32);
		}
		auto ObservationPath(DObject* Object) -> uint64
		{ return ObservationPath(Object ? FName(Object->GetObjectPath()) : FName()); }
		auto ReplaceObservationPath(uint64 Path, const FObjectReplacementMap& Map) -> uint64
		{
			if (!Path) return Path;
			const auto* Entry = FName::ResolveEntry(FNameEntryId(Path & 0xffffffffu));
			if (!Entry) throw std::runtime_error("Component observation path identity is invalid.");
			std::string Text(Entry->MakeView());
			if (const auto Number = static_cast<uint32>(Path >> 32)) Text += "_" + std::to_string(Number - 1);
			// A captured generated reference can outlive its generation and be
			// absent from the graph map. Its longest mapped ancestor still gives
			// the package/Actor path translation without resolving a retired object.
			size_t Prefix = 0; std::string Replacement;
			for (const auto& Pair : Map.GetEntries())
			{
				if (!Pair.Replacement) continue;
				const auto Before = Pair.Previous->GetObjectPath();
				if (Before.size() <= Prefix || Before.size() > Text.size()) continue;
				if (Before.size() != Text.size() && Text[Before.size()] != '.' && Text[Before.size()] != ':') continue;
				if (FName(std::string_view(Text).substr(0, Before.size())) != FName(Before)) continue;
				Prefix = Before.size(); Replacement = Pair.Replacement->GetObjectPath();
			}
			return Prefix ? ObservationPath(FName(Replacement + Text.substr(Prefix))) : Path;
		}
		template<typename Transform>
		auto TransformObservationReferences(FDiffableObject& Observation, Transform&& Convert) -> void
		{
			if (!Observation.SerializedObject || !Observation.SerializationContext)
				throw std::runtime_error("Component observation is incomplete.");
			auto Payload = std::make_shared<FSerializedObject>(*Observation.SerializedObject);
			for (const auto& Slot : Observation.SerializationContext->ReferenceSlots)
			{
				constexpr size_t Width = sizeof(uint64);
				if (Slot.Offset > Payload->Bytes.size() || Payload->Bytes.size() - Slot.Offset < Width)
					throw std::runtime_error("Component observation reference bounds are invalid.");
				const auto Value = Convert(FByteView(Payload->Bytes).subspan(Slot.Offset, Width));
				FByteBuffer Encoded; FObjectMemoryWriter Writer(Encoded, EArchivePurpose::Transaction);
				auto Token = Value; Writer << Token;
				if (Writer.IsError() || Encoded.size() != Width) throw std::runtime_error("Component observation path encoding failed.");
				std::copy(Encoded.begin(), Encoded.end(), Payload->Bytes.begin() + Slot.Offset);
			}
			Payload->HardReferences.clear();
			Observation.SerializedObject = std::move(Payload);
			Observation.ObjectInfo.Target = {};
		}
	}

	auto FComponentInstanceLifecycleState::FindAndReplaceInstances(const FObjectReplacementMap& Map) -> FTransactionCaptureResult
	{
		auto Result = *this;
		if (auto Valid = Result.Target.FindAndReplaceInstances(Map); !Valid) return Valid;
		if (auto Valid = Result.ValidateReferences(); !Valid) return Valid;
		*this = std::move(Result);
		return {};
	}

	auto FComponentTransactionAnnotation::CreateReplacement(const FObjectReplacementMap& Map) const
		-> std::expected<std::shared_ptr<ITransactionObjectAnnotation>, std::string>
	{
		auto Result = std::make_shared<FComponentTransactionAnnotation>(*this);
		if (auto Valid = Result->Data.FindAndReplaceInstances(Map); !Valid) return std::unexpected(Valid.error());
		return Result;
	}

	auto FActorTransactionAnnotation::CreateReplacement(const FObjectReplacementMap& Map) const
		-> std::expected<std::shared_ptr<ITransactionObjectAnnotation>, std::string>
	{
		auto Result = std::make_shared<FActorTransactionAnnotation>(*this);
		auto Key = Map.RemapObjectKey(Result->Data.Target.GetKey());
		if (!Key) return std::unexpected(ToString(Key.error()));
		Result->Data.Target = FPersistentObjectRef::FromKey(*Key);
		if (auto Valid = Result->Data.ComponentInstanceData.FindAndReplaceInstances(Map); !Valid) return std::unexpected(Valid.error());
		if (auto Valid = Result->Data.RootData.FindAndReplaceInstances(Map); !Valid) return std::unexpected(Valid.error());
		for (auto& State : Result->Data.ComponentLifecycle)
			if (auto Valid = State.FindAndReplaceInstances(Map); !Valid) return std::unexpected(Valid.error());
		for (auto& Info : Result->DiffableComponentInfos)
		{
			if (auto Valid = Info.ComponentSourceInfo.FindAndReplaceInstances(Map); !Valid) return std::unexpected(Valid.error());
			// Observation paths have no replay target, including when a generated
			// key is currently absent. Remap through the checked graph, without
			// serializing live components or loading historical bytes.
			auto& Observation = Info.DiffableComponent;
			for (auto* Ref : {&Observation.ObjectInfo.Class, &Observation.ObjectInfo.Outer, &Observation.ObjectInfo.Package})
			{
				auto Key = Map.RemapObjectKey(Ref->GetKey());
				if (!Key) return std::unexpected(ToString(Key.error()));
				*Ref = FPersistentObjectRef::FromKey(*Key);
			}
			TransformObservationReferences(Observation, [&](FByteView Bytes) {
				FObjectMemoryReader Reader(Bytes, EArchivePurpose::Transaction); uint64 Path = 0; Reader << Path;
				if (Reader.IsError()) throw std::runtime_error("Component observation path decoding failed.");
				return ReplaceObservationPath(Path, Map);
			});
		}
		if (auto Valid = Result->Validate(); !Valid) return std::unexpected(Valid.error());
		return Result;
	}

	namespace
	{
		auto SerializeReference(FArchive& Archive, FPersistentObjectRef& Reference) -> void
		{
			auto Key = Reference.GetKey(); Key.SerializeForSnapshot(Archive);
			if (Archive.IsLoading() && !Archive.IsError()) Reference = FPersistentObjectRef::FromKey(Key);
		}
		auto GetDefaultActorTickState(const AActor& Actor) -> FTickFunctionState
		{
			FTickFunctionState State;
			auto* Level = Cast<DLevel>(Actor.GetOuter());
			auto* World = Level ? Level->GetWorld() : nullptr;
			State.bRegistered = World && World->GetCurrentLevel() == Level && Level->ContainsActor(&Actor);
			return State;
		}

	}

	FComponentInstanceLifecycleState::FComponentInstanceLifecycleState(const DActorComponent& Component)
		: Target(&Component), State(Component.CaptureLifecycleState()) {}
	auto FComponentInstanceLifecycleState::IsDefault() const -> bool { return State == FActorComponentLifecycleState{}; }
	auto FComponentInstanceLifecycleState::DefaultFor(const DActorComponent& Component) -> FComponentInstanceLifecycleState
	{
		auto Data = FComponentInstanceLifecycleState(Component);
		Data.State = {};
		return Data;
	}
	auto FComponentInstanceLifecycleState::Validate() const -> FTransactionCaptureResult
	{
		if (auto Result = ValidateReferences(); !Result) return Result;
		auto* Component = Target.Resolve();
		if (!Component) return std::unexpected("Component lifecycle participant is unavailable.");
		return Component->ValidateLifecycleState(State);
	}
	auto FComponentInstanceLifecycleState::ValidateReferences() const -> FTransactionCaptureResult
	{
		if (Target.IsNull()) return std::unexpected("Component lifecycle owner or target expired.");
		return Target.Validate();
	}
	auto FComponentInstanceLifecycleState::Restore() const -> FCommandChangeResult
	{
		if (auto Valid = Validate(); !Valid) return std::unexpected(FCommandChangeError{Valid.error()});
		if (auto Result = Target.Resolve()->ApplyLifecycleState(State); !Result)
			return std::unexpected(FCommandChangeError{Result.error(), true});
		return {};
	}
	auto FComponentInstanceLifecycleState::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{ Target.AddReferencedObjects(Collector); }
	auto FComponentInstanceLifecycleState::Serialize(FArchive& Ar) -> void
	{ Target.Serialize(Ar); DActorComponent::SerializeLifecycleState(Ar, State); }

	FActorTransactionAnnotation::FActorTransactionAnnotation(const AActor& Actor, const FActorTransactionAnnotation* Previous)
		: Data{FPersistentObjectRef(const_cast<AActor*>(&Actor)), Actor.GetPrimaryActorTick().CaptureState(),
			FComponentInstanceDataCache(Actor), {}, FActorRootComponentReconstructionData(Actor, Previous ? &Previous->Data.RootData : nullptr)}
	{
		for (const auto& Component : Actor.GetComponentsSnapshot())
			if (Component)
			{
				Data.ComponentLifecycle.emplace_back(*Component);
				if (Component->GetCreationMethod() == EComponentCreationMethod::Generated)
				{
					auto Observation = DiffUtil::GetDiffableObject(*Component, {DiffUtil::EGetDiffableObjectMode::SerializeProperties});
					if (!Observation) throw std::runtime_error(Observation.error());
					TransformObservationReferences(*Observation, [](FByteView Bytes) {
						FObjectMemoryReader Reader(Bytes, EArchivePurpose::Transaction); FObjectKey Key; Key.SerializeForSnapshot(Reader);
						if (Reader.IsError()) throw std::runtime_error("Component observation reference decoding failed.");
						return ObservationPath(ResolveObjectKey(Key));
					});
					DiffableComponentInfos.push_back({FActorComponentInstanceSourceInfo(Component.Get()), std::move(*Observation)});
				}
			}
	}

	auto FActorTransactionAnnotation::CreateIfRequired(const AActor& Actor) -> std::shared_ptr<FActorTransactionAnnotation>
	{
		auto Annotation = std::make_shared<FActorTransactionAnnotation>(Actor);
		return Annotation->HasInstanceData() ? Annotation : nullptr;
	}
	auto FActorTransactionAnnotation::HasInstanceData() const -> bool
	{
		auto* Actor = Cast<AActor>(Data.Target.Resolve());
		if (!Actor) return true;
		const auto DefaultTick = GetDefaultActorTickState(*Actor);
		return !DiffableComponentInfos.empty() || Data.ComponentInstanceData.HasInstanceData() || Data.RootData.HasInstanceData()
			|| std::ranges::any_of(Data.ComponentLifecycle, [](const auto& State) { return !State.IsDefault(); })
			|| Data.Tick.Group != DefaultTick.Group || Data.Tick.bEnabled != DefaultTick.bEnabled
			|| Data.Tick.bRegistered != DefaultTick.bRegistered;
	}
	auto FActorTransactionAnnotation::RestoreDefault(AActor& Actor) -> FCommandChangeResult
	{
		if (auto Result = Actor.GetPrimaryActorTick().RestoreState(GetDefaultActorTickState(Actor), Cast<DLevel>(Actor.GetOuter())); !Result)
			return std::unexpected(FCommandChangeError{Result.error(), true});
		return {};
	}

	auto FActorTransactionAnnotation::CreateSnapshot(const DObject& Object) const -> std::shared_ptr<ITransactionObjectAnnotation>
	{
		auto* Actor = Cast<AActor>(const_cast<DObject*>(&Object));
		if (!Actor || Data.Target.Resolve() != Actor) throw std::runtime_error("Actor annotation snapshot refresh target is incompatible.");
		return std::make_shared<FActorTransactionAnnotation>(*Actor, this);
	}
	auto FActorTransactionAnnotation::Validate() const -> FTransactionCaptureResult
	{
		auto* Actor = Cast<AActor>(Data.Target.Resolve());
		if (!Actor || Actor->IsBeingDestroyed()) return std::unexpected("Actor annotation target expired.");
		if (auto Result = FTickFunction::ValidateState(Data.Tick, Cast<DLevel>(Actor->GetOuter())); !Result) return Result;
		if (auto Result = Data.ComponentInstanceData.Validate(); !Result) return Result;
		for (const auto& State : Data.ComponentLifecycle) if (auto Result = State.ValidateReferences(); !Result) return Result;
		for (const auto& Info : DiffableComponentInfos)
			if (auto Result = Info.ComponentSourceInfo.Validate(); !Result) return Result;
		return Data.RootData.Validate();
	}
	auto FActorTransactionAnnotation::Restore(AActor& Actor, bool bApplyRootData) const -> FCommandChangeResult
	{
		if (auto Result = Validate(); !Result) return std::unexpected(FCommandChangeError{Result.error()});
		if (Data.Target.Resolve() != &Actor) return std::unexpected(FCommandChangeError{"Actor annotation target mismatch."});
		if (auto Result = Actor.GetPrimaryActorTick().RestoreState(Data.Tick, Cast<DLevel>(Actor.GetOuter())); !Result)
			return std::unexpected(FCommandChangeError{Result.error(), true});
		if (bApplyRootData) if (auto Result = Data.RootData.ApplyToActor(Actor); !Result) return Result;
		for (const auto& State : Data.ComponentLifecycle) if (auto Result = State.Validate(); !Result) return std::unexpected(FCommandChangeError{Result.error()});
		for (const auto& State : Data.ComponentLifecycle) if (auto Result = State.Restore(); !Result) return Result;
		return {};
	}
	auto FActorTransactionAnnotation::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		Data.Target.AddReferencedObjects(Collector);
		Data.ComponentInstanceData.AddReferencedObjects(Collector);
		Data.RootData.AddReferencedObjects(Collector);
		for (const auto& State : Data.ComponentLifecycle) State.AddReferencedObjects(Collector);
		for (const auto& Info : DiffableComponentInfos)
		{
			Info.ComponentSourceInfo.AddReferencedObjects(Collector);
			const auto& Observation = Info.DiffableComponent;
			// Do not retain ObjectInfo.Target: it is a historical generated instance.
			Observation.ObjectInfo.Class.AddReferencedObjects(Collector);
			Observation.ObjectInfo.Outer.AddReferencedObjects(Collector);
			Observation.ObjectInfo.Package.AddReferencedObjects(Collector);
			if (Observation.SerializedObject) Observation.SerializedObject->AddReferencedObjects(Collector);
		}
	}
	auto FActorTransactionAnnotation::Serialize(FArchive& Archive) -> void
	{
		if (Archive.IsLoading()) DiffableComponentInfos.clear();
		SerializeReference(Archive, Data.Target);
		FTickFunction::SerializeState(Archive, Data.Tick);
		Data.ComponentInstanceData.Serialize(Archive);
		Data.RootData.Serialize(Archive);
		uint64 Count = Data.ComponentLifecycle.size(); Archive << Count;
		if (Count > 1000000) { Archive.SetError("Component lifecycle annotation count exceeds the limit."); return; }
		if (Archive.IsLoading() && !Archive.IsError()) Data.ComponentLifecycle.resize(static_cast<size_t>(Count));
		for (auto& State : Data.ComponentLifecycle) { if (Archive.IsError()) break; State.Serialize(Archive); }
	}
	auto FActorTransactionAnnotation::GetAllocatedSize() const -> size_t
	{
		constexpr auto Limit = std::numeric_limits<size_t>::max();
		const auto CacheSize = Data.ComponentInstanceData.GetAllocatedSize(), RootSize = Data.RootData.GetAllocatedSize();
		if (CacheSize == Limit || RootSize == Limit) return Limit;
		// Cache/root objects are inline in Data; count only their dynamic storage again.
		const auto CacheBytes = CacheSize - sizeof(Data.ComponentInstanceData);
		const auto RootBytes = RootSize - sizeof(Data.RootData);
		if (CacheBytes > Limit - sizeof(*this) || RootBytes > Limit - sizeof(*this) - CacheBytes) return Limit;
		const auto BaseBytes = sizeof(*this) + CacheBytes + RootBytes;
		if (Data.ComponentLifecycle.capacity() > (Limit - BaseBytes) / sizeof(FComponentInstanceLifecycleState)) return Limit;
		auto Total = BaseBytes + Data.ComponentLifecycle.capacity() * sizeof(FComponentInstanceLifecycleState);
		if (DiffableComponentInfos.capacity() > (Limit - Total) / sizeof(FDiffableComponentInfo)) return Limit;
		Total += DiffableComponentInfos.capacity() * sizeof(FDiffableComponentInfo);
		for (const auto& Info : DiffableComponentInfos)
		{
			size_t Bytes = 0;
			if (!Info.DiffableComponent.TryGetAllocatedSize(Bytes) || Bytes > Limit - Total) return Limit;
			Total += Bytes;
		}
		return Total;
	}

	auto FActorTransactionAnnotation::ComputeAdditionalObjectChanges(const ITransactionObjectAnnotation* Original) const
		-> std::vector<std::pair<DObject*, FTransactionObjectChange>>
	{
		const auto* Before = dynamic_cast<const FActorTransactionAnnotation*>(Original);
		if (Original && !Before) throw std::runtime_error("Incompatible Actor annotation diff baseline.");
		std::vector<std::pair<DObject*, FTransactionObjectChange>> Changes;
		auto* Actor = Cast<AActor>(Data.Target.Resolve());
		if (!Actor) return Changes;
		auto Find = [](const auto& Infos, const DActorComponent& Component) -> const FDiffableObject* {
			for (const auto& Info : Infos)
				if (Info.ComponentSourceInfo.MatchesComponent(Component)) return &Info.DiffableComponent;
			return nullptr;
		};
		for (const auto& Component : Actor->GetComponentsSnapshot())
		{
			if (!Component || Component->GetCreationMethod() != EComponentCreationMethod::Generated) continue;
			const auto* Old = Before ? Find(Before->DiffableComponentInfos, *Component) : nullptr;
			const auto* New = Find(DiffableComponentInfos, *Component);
			if (!Old && !New) continue;
			auto Skip = [&](FName Name) {
				if (Name == FName("CreationMethod")) return true;
				const auto* Scene = Cast<DSceneComponent>(Component.Get());
				return Scene && Actor->GetRootComponent() != Scene && Scene->IsTransformModifiedByConstructionScript()
					&& Name == FName("RelativeTransform");
			};
			FTransactionObjectChange Change;
			if (Old && New)
			{
				// Source matching establishes logical identity across generations;
				// observation paths do not carry an exact transfer target.
				auto Diff = DiffUtil::GenerateObjectDiff(*Old, *New, {.ShouldSkipProperty = Skip});
				if (!Diff) throw std::runtime_error(Diff.error());
				Change = std::move(*Diff);
			}
			else
			{
				const auto& State = Old ? *Old : *New;
				Change = FTransactionObjectChange(State.ObjectInfo.Identity, State.ObjectInfo.Flags);
				Change.bEditorPresenceChanged = true;
				for (const auto& Field : State.SerializedTaggedData)
					if (Field.bReflected && !Skip(Field.Name) && !std::ranges::contains(Change.ChangedProperties, Field.Name))
						Change.ChangedProperties.push_back(Field.Name);
			}
			if (Change.HasChanges()) Changes.emplace_back(Component.Get(), std::move(Change));
		}
		return Changes;
	}
}
