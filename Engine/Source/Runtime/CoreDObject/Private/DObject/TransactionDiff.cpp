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
			if (Field.Begin > Field.End || Field.End > OldData.SerializedObject->Bytes.size() || Other->Begin > Other->End || Other->End > NewObject.SerializedObject->Bytes.size())
				return std::unexpected("Transaction diff field bounds are invalid.");
			const bool Changed = !std::ranges::equal(FByteView(OldData.SerializedObject->Bytes).subspan(Field.Begin, Field.End - Field.Begin),
				FByteView(NewObject.SerializedObject->Bytes).subspan(Other->Begin, Other->End - Other->Begin));
			if (Changed && Field.bReflected && (!Options.ShouldSkipProperty || !Options.ShouldSkipProperty(Field.Name)) && !std::ranges::contains(Change.ChangedProperties, Field.Name)) Change.ChangedProperties.push_back(Field.Name);
		}
		// Project out reflected ranges. This includes named native fields and raw
		// native bytes without mistaking shifted offsets for non-property changes.
		auto NativeBytes = [](const FDiffableObject& State) -> std::expected<FByteBuffer, std::string> {
			FByteBuffer Result; size_t Offset = 0;
			for (const auto& Field : State.SerializedTaggedData)
			{
				if (Field.Begin < Offset || Field.Begin > Field.End || Field.End > State.SerializedObject->Bytes.size())
					return std::unexpected("Transaction diff ranges overlap or exceed serialized bytes.");
				Result.insert(Result.end(), State.SerializedObject->Bytes.begin() + Offset, State.SerializedObject->Bytes.begin() + Field.Begin);
				if (!Field.bReflected) Result.insert(Result.end(), State.SerializedObject->Bytes.begin() + Field.Begin, State.SerializedObject->Bytes.begin() + Field.End);
				Offset = Field.End;
			}
			Result.insert(Result.end(), State.SerializedObject->Bytes.begin() + Offset, State.SerializedObject->Bytes.end()); return Result;
		};
		auto BeforeNative = NativeBytes(OldData); if (!BeforeNative) return std::unexpected(BeforeNative.error());
		auto AfterNative = NativeBytes(NewObject); if (!AfterNative) return std::unexpected(AfterNative.error());
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
