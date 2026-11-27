#include "DObject/TransactionSerialization.h"
#include "DObject/TransactionDiff.h"
#include "DObject/ObjectGraphReplacement.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Object.h"

namespace Durin
{
	auto FSerializedObjectCapture::HasReplacementReferences(const FObjectReplacementMap& Map) const -> bool
	{
		for (const auto* Reference : {&Info.Target, &Info.Class, &Info.Outer, &Info.Package})
			if (Map.Find(Reference->Resolve())) return true;
		if (Data)
		{
			for (const auto& Ref : Data->ReferencedObjects) if (Map.Find(Ref.Resolve())) return true;
			for (const auto& Key : Data->WeakReferencedObjects) if (Map.Find(ResolveObjectKey(Key))) return true;
		}
		return false;
	}

	auto PrepareSerializedObjectReplacement(const FSerializedObjectCapture& Source,
		const FObjectReplacementMap& Map) -> std::expected<FSerializedObjectCapture, std::string>
	{
		if (!Source.Data || !Source.Context) return std::unexpected("Replacement capture is incomplete.");
		FSerializedObjectCapture Result = Source;
		for (auto* Reference : {&Result.Info.Target, &Result.Info.Class, &Result.Info.Outer, &Result.Info.Package})
		{
			auto Replacement = Reference->CreateReplacement(Map);
			if (!Replacement) return std::unexpected(Replacement.error());
			*Reference = std::move(*Replacement);
		}
		std::shared_ptr<FSerializedObject> Data;
		for (size_t Index = 0; Index < Source.Data->ReferencedObjects.size(); ++Index)
		{
			const auto& Reference = Source.Data->ReferencedObjects[Index];
			auto Replacement = Reference.CreateReplacement(Map);
			if (!Replacement) return std::unexpected(Replacement.error());
			if (Reference.GetKey() == Replacement->GetKey() && Reference == *Replacement) continue;
			if (!Data) Data = std::make_shared<FSerializedObject>(*Source.Data);
			Data->ReferencedObjects[Index] = std::move(*Replacement);
		}
		for (size_t Index = 0; Index < Source.Data->WeakReferencedObjects.size(); ++Index)
		{
			const auto Key = Source.Data->WeakReferencedObjects[Index];
			auto Replacement = Map.RemapObjectKey(Key, true);
			if (!Replacement) return std::unexpected(ToString(Replacement.error()));
			if (Key == *Replacement) continue;
			if (!Data) Data = std::make_shared<FSerializedObject>(*Source.Data);
			Data->WeakReferencedObjects[Index] = *Replacement;
		}
		if (Data) Result.Data = std::move(Data);
		auto* Target = Result.Info.Target.Resolve();
		if (!Target) return std::unexpected("Replacement capture target is unavailable.");
		if (auto Valid = ValidateSerializedObject(*Target, Result); !Valid) return std::unexpected(Valid.error());
		return Result;
	}

	auto DiffUtil::PrepareObjectReplacement(const FDiffableObject& Source,
		const FObjectReplacementMap& Map) -> std::expected<FDiffableObject, std::string>
	{
		auto Capture = PrepareSerializedObjectReplacement({Source.SerializedObject, Source.ObjectInfo, Source.SerializationContext}, Map);
		if (!Capture) return std::unexpected(Capture.error());
		return FDiffableObject{std::move(Capture->Info), std::move(Capture->Data), std::move(Capture->Context), Source.SerializedTaggedData};
	}

	namespace
	{
		auto SameInfo(const FSerializedObjectInfo& A, const FSerializedObjectInfo& B) -> bool
		{ return A == B && A.Identity == B.Identity; }
		auto SameCapture(const FSerializedObjectCapture& A, const FSerializedObjectCapture& B) -> bool
		{ return SameInfo(A.Info, B.Info) && A.Data == B.Data && A.Context == B.Context; }
		auto SameObservation(const FDiffableObject& A, const FDiffableObject& B) -> bool
		{
			return SameInfo(A.ObjectInfo, B.ObjectInfo) && A.SerializedObject == B.SerializedObject
				&& A.SerializationContext == B.SerializationContext && A.SerializedTaggedData == B.SerializedTaggedData;
		}

		class FSerializedObjectReplacementParticipant final : public IObjectReplacementParticipant
		{
			struct FPreparedBinding
			{
				FSerializedObjectReplacementBinding Binding;
				FSerializedObjectCapture BeforeCapture, AfterCapture;
				FDiffableObject BeforeObservation, AfterObservation;
			};
		public:
			FSerializedObjectReplacementParticipant(DObject& InOwner, std::span<const FSerializedObjectReplacementBinding> InBindings)
				: Owner(&InOwner), Bindings(InBindings.begin(), InBindings.end()) {}
			auto Prepare(const FObjectReplacementMap& InMap) -> std::expected<void, FObjectReplacementError> override
			{
				Abort();
				Map = &InMap;
				auto Reject = [](std::string Message) -> std::expected<void, FObjectReplacementError> {
					return std::unexpected(FObjectReplacementError{.Code = EObjectReplacementError::ParticipantRejected,
						.Reason = EObjectReplacementReason::ParticipantRejected, .Message = std::move(Message)});
				};
				if (!Owner.Resolve() || InMap.Find(Owner.Resolve()) || Bindings.empty())
					return Reject("Serialized replacement requires a retained native storage owner and nonempty bindings.");
				std::unordered_set<const void*> Captures, Observations;
				Prepared.reserve(Bindings.size());
				for (auto Binding : Bindings)
				{
					if ((!Binding.Capture && !Binding.Observation)
						|| (Binding.Capture && !Captures.insert(Binding.Capture).second)
						|| (Binding.Observation && !Observations.insert(Binding.Observation).second))
						return Reject("Serialized replacement contains empty or duplicate bindings.");
					FPreparedBinding Item;
					Item.Binding = Binding;
					if (Binding.Capture)
					{
						Item.BeforeCapture = *Binding.Capture;
						auto After = PrepareSerializedObjectReplacement(Item.BeforeCapture, InMap);
						if (!After) return Reject(After.error());
						Item.AfterCapture = std::move(*After);
					}
					if (Binding.Observation)
					{
						Item.BeforeObservation = *Binding.Observation;
						if (Binding.Capture)
						{
							const auto& Before = Item.BeforeObservation;
							if (!SameCapture(Item.BeforeCapture, {Before.SerializedObject, Before.ObjectInfo, Before.SerializationContext}))
								return Reject("Paired capture and observation do not share the same transfer state.");
							const auto& After = Item.AfterCapture;
							Item.AfterObservation = {After.Info, After.Data, After.Context, Before.SerializedTaggedData};
						}
						else
						{
							auto After = DiffUtil::PrepareObjectReplacement(Item.BeforeObservation, InMap);
							if (!After) return Reject(After.error());
							Item.AfterObservation = std::move(*After);
						}
					}
					Prepared.push_back(std::move(Item));
				}
				if (!HasCompleteNativeCoverage()) return Reject("Serialized replacement does not cover all mapped native references of its owner.");
				bPrepared = true;
				return {};
			}
			auto Validate() const -> bool override
			{
				if (!bPrepared || !Owner.Resolve()) return false;
				for (const auto& Item : Prepared)
				{
					if (Item.Binding.Capture && !SameCapture(*Item.Binding.Capture, Item.BeforeCapture)) return false;
					if (Item.Binding.Observation && !SameObservation(*Item.Binding.Observation, Item.BeforeObservation)) return false;
					const auto& Info = Item.Binding.Capture ? Item.AfterCapture.Info : Item.AfterObservation.ObjectInfo;
					auto* Target = Info.Target.Resolve();
					if (!Target) return false;
					const FSerializedObjectCapture Capture = Item.Binding.Capture ? Item.AfterCapture
						: FSerializedObjectCapture{Item.AfterObservation.SerializedObject, Info, Item.AfterObservation.SerializationContext};
					if (!ValidateSerializedObject(*Target, Capture)) return false;
				}
				return HasCompleteNativeCoverage();
			}
			auto CoversNativeReferences(const DObject& Object) const -> bool override { return bPrepared && Owner.Resolve() == &Object; }
			auto GetStrongReferenceCount(const DObject&) const -> uint32 override { return 0; }
			auto Commit() noexcept -> void override
			{
				for (auto& Item : Prepared)
				{
					if (Item.Binding.Capture) std::swap(*Item.Binding.Capture, Item.AfterCapture);
					if (Item.Binding.Observation) std::swap(*Item.Binding.Observation, Item.AfterObservation);
				}
				// Drop old payload ownership before retirement; commit only moves/frees.
				Abort();
			}
			auto Abort() noexcept -> void override { bPrepared = false; Prepared.clear(); Map = nullptr; }
			auto CanRetire() const -> bool override { return true; }
		private:
			auto HasCompleteNativeCoverage() const -> bool
			{
				auto* Object = Owner.Resolve();
				if (!Object || !Map) return false;
				class FCounts final : public FReferenceCollector
				{
				public:
					explicit FCounts(const FObjectReplacementMap& InMap) : Map(InMap) {}
					auto AddReferencedObject(DObject*& Value) -> void override { if (Map.Find(Value)) ++Counts[Value]; }
					const FObjectReplacementMap& Map;
					std::unordered_map<DObject*, size_t> Counts;
				} Actual(*Map), Covered(*Map);
				Object->AddReferencedObjects(Actual);
				Object->DObject::AddReferencedObjects(Covered);
				for (const auto& Binding : Bindings)
				{
					if (Binding.Capture) Binding.Capture->AddReferencedObjects(Covered);
					if (Binding.Observation) Binding.Observation->AddReferencedObjects(Covered);
				}
				return Actual.Counts == Covered.Counts;
			}
			FPersistentObjectRef Owner;
			std::vector<FSerializedObjectReplacementBinding> Bindings;
			std::vector<FPreparedBinding> Prepared;
			const FObjectReplacementMap* Map = nullptr;
			bool bPrepared = false;
		};
	}

	auto CreateSerializedObjectReplacementParticipant(DObject& Owner,
		std::span<const FSerializedObjectReplacementBinding> Bindings) -> std::shared_ptr<IObjectReplacementParticipant>
	{ return std::make_shared<FSerializedObjectReplacementParticipant>(Owner, Bindings); }
}
