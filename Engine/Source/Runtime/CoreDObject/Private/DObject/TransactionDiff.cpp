#include "TransactionDiffPrivate.h"

namespace Durin
{
	auto FDiffableObject::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		ObjectInfo.AddReferencedObjects(Collector);
		if (SerializedObject) SerializedObject->AddReferencedObjects(Collector);
	}

	auto DiffUtil::GetDiffableObject(DObject& Object, const FSerializedObjectCapture* CapturePolicy)
		-> std::expected<FDiffableObject, std::string>
	{
		FDiffableObject Observation;
		auto Captured = CaptureSerializedObject(Object, CapturePolicy, &Observation);
		if (!Captured) return std::unexpected(Captured.error());
		return Observation;
	}

	auto DiffUtil::GenerateObjectDiff(const FDiffableObject& OldObject, const FDiffableObject& NewObject,
		const FGenerateObjectDiffOptions& Options) -> std::expected<FTransactionObjectChange, std::string>
	{
		const auto& Before = OldObject.ObjectInfo; const auto& After = NewObject.ObjectInfo;
		const bool bResurrection = !OldObject.SerializedObject && Before.bIsPendingKill && !After.bIsPendingKill;
		if (!NewObject.SerializedObject || !NewObject.SerializationContext || Before.Target != After.Target || Before.Class != After.Class
			|| (!bResurrection && (!OldObject.SerializedObject || !OldObject.SerializationContext
				|| !OldObject.SerializationContext->IsCompatibleWith(*NewObject.SerializationContext))))
			return std::unexpected("Transaction object diff requires the same compatible target and schema.");
		FTransactionObjectChange Change(Before.Identity, Before.Flags);
		if (Options.bFullDiff)
		{
			Change.bHasPendingKillChange = Before.bIsPendingKill != After.bIsPendingKill;
			Change.bNameChanged = Before.Identity.OriginalName != After.Identity.OriginalName;
			Change.bOuterChanged = Before.Outer != After.Outer;
			Change.bNonPropertyDataChanged = Before.Flags != After.Flags || Before.Package != After.Package
				|| Before.Identity.OriginalPackagePath != After.Identity.OriginalPackagePath;
		}
		FDiffableObject Archetype;
		if (bResurrection)
		{
			auto Baseline = GetArchetypeDiffableObject(NewObject);
			if (!Baseline) return std::unexpected(Baseline.error());
			Archetype = std::move(*Baseline);
		}
		const auto& OldData = bResurrection ? Archetype : OldObject;
		// Slot numbers are local to each capture. Compare in one index space
		// while preserving field offsets, including untagged native references.
		std::unordered_map<FObjectKey, int32> Indices;
		auto ComparisonBytes = [&](const FDiffableObject& State, FByteBuffer& Scratch) -> std::expected<FByteView, std::string> {
			const auto& Data = *State.SerializedObject;
			if (!State.SerializationContext || State.SerializationContext->bUsesReferencePaths) return FByteView(*Data.SerializedData);
			auto Layout = VisitTransactionReferenceIndices(Data, *State.SerializationContext, [&](const auto& Slot, int32 Index) {
				if (Index < 0) return;
				const auto Key = Slot.bWeak ? Data.WeakReferencedObjects[Index] : Data.ReferencedObjects[Index].GetKey();
				int32 Common = Key.IsNull() ? -1 : Indices.try_emplace(Key, static_cast<int32>(Indices.size())).first->second;
				if (Common == Index) return;
				if (Scratch.empty()) Scratch = *Data.SerializedData;
				if constexpr (std::endian::native != std::endian::little) Common = std::byteswap(Common);
				std::memcpy(Scratch.data() + Slot.Offset, &Common, sizeof(Common));
			});
			if (!Layout) return std::unexpected(Layout.error());
			return FByteView(Scratch.empty() ? *Data.SerializedData : Scratch);
		};
		FByteBuffer OldScratch, NewScratch;
		auto OldBytes = ComparisonBytes(OldData, OldScratch);
		if (!OldBytes) return std::unexpected(OldBytes.error());
		auto NewBytes = ComparisonBytes(NewObject, NewScratch);
		if (!NewBytes) return std::unexpected(NewBytes.error());
		for (const auto& Field : OldData.SerializedTaggedData)
		{
			const auto Other = std::ranges::find_if(NewObject.SerializedTaggedData, [&](const auto& Value) {
				return Value.DeclaringType == Field.DeclaringType && Value.Name == Field.Name && Value.bReflected == Field.bReflected;
			});
			if (Other == NewObject.SerializedTaggedData.end())
			{
				if (Options.bFullDiff && Field.bReflected && (!Options.ShouldSkipProperty || !Options.ShouldSkipProperty(Field.Name)))
					Change.ChangedProperties.push_back(Field.Name);
				continue;
			}
			if (Field.Begin > Field.End || Field.End > OldBytes->size() || Other->Begin > Other->End || Other->End > NewBytes->size())
				return std::unexpected("Transaction diff field bounds are invalid.");
			const bool Changed = !std::ranges::equal(OldBytes->subspan(Field.Begin, Field.End - Field.Begin),
				NewBytes->subspan(Other->Begin, Other->End - Other->Begin));
			if (Changed && Field.bReflected && (!Options.ShouldSkipProperty || !Options.ShouldSkipProperty(Field.Name)) && !std::ranges::contains(Change.ChangedProperties, Field.Name)) Change.ChangedProperties.push_back(Field.Name);
		}
		// Project out reflected ranges. This includes named native fields and raw
		// native bytes without mistaking shifted offsets for non-property changes.
		auto NativeBytes = [](const FDiffableObject& State, FByteView Bytes) -> std::expected<FByteBuffer, std::string> {
			FByteBuffer Result; size_t Offset = 0;
			for (const auto& Field : State.SerializedTaggedData)
			{
				if (Field.Begin < Offset || Field.Begin > Field.End || Field.End > Bytes.size())
					return std::unexpected("Transaction diff ranges overlap or exceed serialized bytes.");
				Result.insert(Result.end(), Bytes.begin() + Offset, Bytes.begin() + Field.Begin);
				if (!Field.bReflected) Result.insert(Result.end(), Bytes.begin() + Field.Begin, Bytes.begin() + Field.End);
				Offset = Field.End;
			}
			Result.insert(Result.end(), Bytes.begin() + Offset, Bytes.end()); return Result;
		};
		auto BeforeNative = NativeBytes(OldData, *OldBytes); if (!BeforeNative) return std::unexpected(BeforeNative.error());
		auto AfterNative = NativeBytes(NewObject, *NewBytes); if (!AfterNative) return std::unexpected(AfterNative.error());
		Change.bNonPropertyDataChanged = Change.bNonPropertyDataChanged || *BeforeNative != *AfterNative;
		if (Options.bFullDiff)
			for (const auto& Field : NewObject.SerializedTaggedData)
				if (Field.bReflected && (!Options.ShouldSkipProperty || !Options.ShouldSkipProperty(Field.Name))
					&& !std::ranges::any_of(OldData.SerializedTaggedData, [&](const auto& Old) {
						return Old.DeclaringType == Field.DeclaringType && Old.Name == Field.Name && Old.bReflected == Field.bReflected;
					}) && !std::ranges::contains(Change.ChangedProperties, Field.Name)) Change.ChangedProperties.push_back(Field.Name);
		return Change;
	}
}
