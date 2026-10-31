#include "DObject/OverridableManager.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"

#include "DObject/DefaultDeltaPlan.h"
#include "DObject/Object.h"

namespace Durin
{
	FOverriddenPropertyRestoreContext::FOverriddenPropertyRestoreContext(DObject& Object, const FObjectArchive& Archive)
		: Target(&Object)
	{
		using R = EOverrideRestoreFallbackReason;
		if (!Archive.IsLoading() || Archive.GetPurpose() != EArchivePurpose::AuthoredPackage || Archive.IsFilterEditorOnly())
		{ Reason = R::FilteredContext; return; }
		if (!Object.GetClass() || !Object.GetClass()->UsesDefaultObjectSerializer(Object))
		{ Reason = R::CustomObjectSerializer; return; }
		if (Target.IsNull() || Object.IsTemplateObject()) { Reason = R::UnprovenMetadata; return; }
		std::unordered_set<DStructBase*> Visited;
		std::function<void(FProperty*, uint32)> VisitProperty;
		std::function<void(DStructBase*, uint32)> VisitType = [&](DStructBase* Type, uint32 Depth) {
			if (Depth > DefaultDeltaMaxDepth || Visited.size() >= DefaultDeltaMaxFields)
			{ Reason = R::UnprovenMetadata; return; }
			if (Reason != R::None || !Type || !Visited.insert(Type).second) return;
			const FObjectKey Key(Type);
			if (Key.IsNull()) { Reason = R::UnprovenMetadata; return; }
			const auto* Class = Cast<DClass>(Type);
			const auto* StructType = Cast<DStruct>(Type);
			if (!Class && !StructType) { Reason = R::UnprovenMetadata; return; }
			Types.emplace_back(Class ? Class->GetQualifiedName() : StructType->GetQualifiedName(), Key);
			if (auto* Struct = Cast<DStruct>(Type))
			{
				if (Struct->HasSerializer()) { Reason = R::CustomStructSerializer; return; }
				if (Struct->HasPostDeserialize()) { Reason = R::PostDeserialize; return; }
				if (!Struct->HasCompleteAuthoredFields()) { Reason = R::IncompleteAuthoredFields; return; }
			}
			Type->ForEachProperty([&](FProperty* Field) {
				if (Field && !Field->HasAnyPropertyFlags(EPropertyFlags::Transient | EPropertyFlags::Deprecated)) VisitProperty(Field, Depth + 1);
			}, false);
		};
		VisitProperty = [&](FProperty* Property, uint32 Depth) {
			if (Reason != R::None) return;
			if (Depth > DefaultDeltaMaxDepth) { Reason = R::UnprovenMetadata; return; }
			if (!Property) { Reason = R::UnsupportedPath; return; }
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::Struct:
				VisitType(static_cast<FStructProperty*>(Property)->GetStruct(), Depth + 1); break;
			case DurinCodeGen::EPropertyGenFlags::Array:
				VisitProperty(static_cast<FArrayProperty*>(Property)->GetInner(), Depth + 1); break;
			case DurinCodeGen::EPropertyGenFlags::Map:
				VisitProperty(static_cast<FMapProperty*>(Property)->GetKeyProp(), Depth + 1);
				VisitProperty(static_cast<FMapProperty*>(Property)->GetValueProp(), Depth + 1); break;
			default: break;
			}
		};
		for (auto* Class = Object.GetClass(); Class && Reason == R::None; Class = Class->GetSuperClass()) VisitType(Class, 0);
	}

	namespace
	{
		auto ReplacementPath(const FAuthoredOverridePath& Path) -> FAuthoredOverridePath
		{
			const auto End = std::ranges::find_if(Path, [](const auto& Token) {
				return Token.Kind != EAuthoredOverridePathTokenKind::Field;
			});
			return {Path.begin(), End};
		}

		auto CoalesceReplacements(std::vector<FAuthoredOverrideEntry>& Entries) -> void
		{
			for (auto& Entry : Entries) Entry.Path = ReplacementPath(Entry.Path);
			std::ranges::sort(Entries, [](const auto& Left, const auto& Right) {
				return CompareAuthoredOverridePaths(Left.Path, Right.Path) < 0;
			});
			size_t Count = 0;
			for (size_t Index = 0; Index < Entries.size(); ++Index)
			{
				if (Count && IsAuthoredOverridePathPrefix(Entries[Count - 1].Path, Entries[Index].Path)) continue;
				if (Count != Index) Entries[Count] = std::move(Entries[Index]);
				++Count;
			}
			Entries.resize(Count);
		}

		auto CompareToken(const FAuthoredOverridePathToken& Left,
			const FAuthoredOverridePathToken& Right) -> std::strong_ordering
		{
			if (Left.Kind != Right.Kind) return Left.Kind < Right.Kind
				? std::strong_ordering::less : std::strong_ordering::greater;
			if (const auto Order = Left.DeclaringType.ToString() <=> Right.DeclaringType.ToString(); Order != 0)
				return Order;
			if (const auto Order = Left.FieldName.ToString() <=> Right.FieldName.ToString(); Order != 0)
				return Order;
			if (Left.Index != Right.Index) return Left.Index < Right.Index
				? std::strong_ordering::less : std::strong_ordering::greater;
			return Left.MapKeyToken <=> Right.MapKeyToken;
		}

		auto TokenEqual(const FAuthoredOverridePathToken& Left,
			const FAuthoredOverridePathToken& Right) -> bool
		{
			return CompareToken(Left, Right) == 0;
		}

		auto FindEntry(std::vector<FAuthoredOverrideEntry>& Entries,
			const FAuthoredOverridePath& Path)
		{
			return std::lower_bound(Entries.begin(), Entries.end(), Path,
				[](const FAuthoredOverrideEntry& Entry, const FAuthoredOverridePath& Candidate) {
					return CompareAuthoredOverridePaths(Entry.Path, Candidate) < 0;
				});
		}
	}

	auto FAuthoredOverridePathToken::Field(FName DeclaringType, FName FieldName)
		-> FAuthoredOverridePathToken
	{
		return {.Kind = EAuthoredOverridePathTokenKind::Field,
			.DeclaringType = DeclaringType, .FieldName = FieldName};
	}

	auto FAuthoredOverridePathToken::FixedArrayElement(uint64 Index) -> FAuthoredOverridePathToken
	{
		return {.Kind = EAuthoredOverridePathTokenKind::FixedArrayElement, .Index = Index};
	}

	auto FAuthoredOverridePathToken::ArrayElement(uint64 Index) -> FAuthoredOverridePathToken
	{
		return {.Kind = EAuthoredOverridePathTokenKind::ArrayElement, .Index = Index};
	}

	auto FAuthoredOverridePathToken::MapValue(FByteBuffer CanonicalKeyToken)
		-> FAuthoredOverridePathToken
	{
		return {.Kind = EAuthoredOverridePathTokenKind::MapValue,
			.MapKeyToken = std::move(CanonicalKeyToken)};
	}

	auto CompareAuthoredOverridePaths(const FAuthoredOverridePath& Left,
		const FAuthoredOverridePath& Right) -> std::strong_ordering
	{
		const size_t Shared = std::min(Left.size(), Right.size());
		for (size_t Index = 0; Index < Shared; ++Index)
			if (const auto Order = CompareToken(Left[Index], Right[Index]); Order != 0) return Order;
		return Left.size() <=> Right.size();
	}

	auto IsAuthoredOverridePathPrefix(const FAuthoredOverridePath& Prefix,
		const FAuthoredOverridePath& Path) -> bool
	{
		if (Prefix.size() > Path.size()) return false;
		for (size_t Index = 0; Index < Prefix.size(); ++Index)
			if (!TokenEqual(Prefix[Index], Path[Index])) return false;
		return true;
	}

	auto FOverridableManager::OverrideProperty(DObject& Object, const FAuthoredOverridePath& Path,
		EAuthoredOverrideProvenance Provenance, FAuthoredOverrideDiagnostic* OutDiagnostic) -> bool
	{
		if (Provenance != EAuthoredOverrideProvenance::Forced)
		{
			if (OutDiagnostic) *OutDiagnostic = {
				.Reason = EAuthoredOverrideFailureReason::InvalidProvenance};
			return false;
		}
		if (!ValidateAuthoredOverridePath(&Object, Path, OutDiagnostic)) return false;
		for (;;)
		{
			auto Current = std::atomic_load_explicit(&Object.OverriddenProperties, std::memory_order_acquire);
			auto Next = Current ? std::make_shared<FOverriddenPropertySet>(*Current)
				: std::make_shared<FOverriddenPropertySet>();
			auto It = FindEntry(Next->Entries, Path);
			if (It != Next->Entries.end() && CompareAuthoredOverridePaths(It->Path, Path) == 0)
			{
				if (Provenance == EAuthoredOverrideProvenance::Forced) It->Provenance = Provenance;
			}
			else Next->Entries.insert(It, {Path, Provenance});
			CoalesceReplacements(Next->Entries);
			std::shared_ptr<const FOverriddenPropertySet> Published = std::move(Next);
			if (std::atomic_compare_exchange_weak_explicit(&Object.OverriddenProperties, &Current, Published,
				std::memory_order_release, std::memory_order_acquire))
			{
				if (OutDiagnostic) OutDiagnostic->Reset();
				return true;
			}
		}
	}

	auto FOverridableManager::RestoreOverriddenPropertyOperation(DObject& Object, std::span<const FAuthoredOverrideEntry> Entries,
		FAuthoredOverrideDiagnostic* OutDiagnostic, FAuthoredOverrideValidationWork* Work) -> bool
	{
		std::vector<FAuthoredOverrideEntry> Sorted(Entries.begin(), Entries.end());
		for (const FAuthoredOverrideEntry& Entry : Sorted)
		{
			if (Entry.Provenance != EAuthoredOverrideProvenance::Forced)
			{
				if (OutDiagnostic) *OutDiagnostic = {
					.Reason = EAuthoredOverrideFailureReason::InvalidProvenance};
				return false;
			}
		}
		if (Sorted.empty())
		{
			std::atomic_store_explicit(&Object.OverriddenProperties,
				std::shared_ptr<const FOverriddenPropertySet>{}, std::memory_order_release);
			if (OutDiagnostic) OutDiagnostic->Reset();
			return true;
		}
		if (!ValidateAuthoredOverrideEntries(&Object, Sorted, OutDiagnostic, Work)) return false;
		std::ranges::sort(Sorted, [](const FAuthoredOverrideEntry& Left, const FAuthoredOverrideEntry& Right) {
			return CompareAuthoredOverridePaths(Left.Path, Right.Path) < 0;
		});
		for (size_t Index = 1; Index < Sorted.size(); ++Index)
		{
			if (CompareAuthoredOverridePaths(Sorted[Index - 1].Path, Sorted[Index].Path) == 0)
			{
				if (OutDiagnostic)
				{
					OutDiagnostic->Reason = EAuthoredOverrideFailureReason::DuplicatePath;
					OutDiagnostic->LogicalPath.clear();
				}
				return false;
			}
		}
		Publish(Object, std::move(Sorted));
		if (OutDiagnostic) OutDiagnostic->Reset();
		return true;
	}

	auto FOverridableManager::Publish(DObject& Object, std::vector<FAuthoredOverrideEntry> Entries) -> void
	{
		CoalesceReplacements(Entries);
		std::shared_ptr<const FOverriddenPropertySet> Published;
		if (!Entries.empty())
		{
			auto Set = std::make_shared<FOverriddenPropertySet>();
			Set->Entries = std::move(Entries);
			Published = std::move(Set);
		}
		std::atomic_store_explicit(&Object.OverriddenProperties, std::move(Published), std::memory_order_release);
	}

	auto FOverridableManager::RestoreOverriddenPropertyOperation(
		std::unique_ptr<FOverriddenPropertyRestoreContext> Context, FAuthoredOverrideDiagnostic* OutDiagnostic) -> bool
	{
		auto Fail = [&](EAuthoredOverrideFailureReason Reason) {
			if (OutDiagnostic) *OutDiagnostic = {.Reason = Reason};
			return false;
		};
		if (!Context || !Context->IsEligible()) return Fail(EAuthoredOverrideFailureReason::ArchiveFailure);
		auto* Object = ResolveObjectKey(Context->Target);
		if (!Object || Object->IsTemplateObject()) return Fail(EAuthoredOverrideFailureReason::InvalidObject);
		if (!Object->GetClass() || !Object->GetClass()->UsesDefaultObjectSerializer(*Object))
			return Fail(EAuthoredOverrideFailureReason::SchemaMismatch);
		for (const auto& [Name, Key] : Context->Types)
		{
			DStructBase* Current = FindClassByQualifiedName(Name);
			if (!Current) Current = FindStructByQualifiedName(Name);
			if (!Current || ResolveObjectKey(Key) != Current) return Fail(EAuthoredOverrideFailureReason::SchemaMismatch);
		}
		Publish(*Object, std::move(Context->Candidates));
		if (OutDiagnostic) OutDiagnostic->Reset();
		return true;
	}

	auto FOverridableManager::ClearOverriddenProperty(DObject& Object, const FAuthoredOverridePath& Path) -> bool
	{
		if (Path.empty()) return false;
		const auto Boundary = ReplacementPath(Path);
		for (;;)
		{
			auto Current = std::atomic_load_explicit(&Object.OverriddenProperties, std::memory_order_acquire);
			if (!Current) return false;
			auto Next = std::make_shared<FOverriddenPropertySet>(*Current);
			auto It = FindEntry(Next->Entries, Boundary);
			if (It == Next->Entries.end() || CompareAuthoredOverridePaths(It->Path, Boundary) != 0) return false;
			Next->Entries.erase(It);
			std::shared_ptr<const FOverriddenPropertySet> Published = Next->Entries.empty()
				? std::shared_ptr<const FOverriddenPropertySet>{} : std::move(Next);
			if (std::atomic_compare_exchange_weak_explicit(&Object.OverriddenProperties, &Current, Published,
				std::memory_order_release, std::memory_order_acquire)) return true;
		}
	}

	auto FOverridableManager::ClearOverriddenPropertySubtree(DObject& Object, const FAuthoredOverridePath& Path) -> uint64
	{
		if (Path.empty()) return 0;
		const auto Boundary = ReplacementPath(Path);
		if (Boundary.empty()) return 0;
		for (;;)
		{
			auto Current = std::atomic_load_explicit(&Object.OverriddenProperties, std::memory_order_acquire);
			if (!Current) return 0;
			auto Next = std::make_shared<FOverriddenPropertySet>(*Current);
			const size_t Before = Next->Entries.size();
			std::erase_if(Next->Entries, [&](const FAuthoredOverrideEntry& Entry) {
				return IsAuthoredOverridePathPrefix(Boundary, Entry.Path);
			});
			const uint64 Removed = static_cast<uint64>(Before - Next->Entries.size());
			if (Removed == 0) return 0;
			std::shared_ptr<const FOverriddenPropertySet> Published = Next->Entries.empty()
				? std::shared_ptr<const FOverriddenPropertySet>{} : std::move(Next);
			if (std::atomic_compare_exchange_weak_explicit(&Object.OverriddenProperties, &Current, Published,
				std::memory_order_release, std::memory_order_acquire)) return Removed;
		}
	}

	auto FOverridableManager::ResetOverriddenProperties(DObject& Object) -> void
	{
		std::atomic_store_explicit(&Object.OverriddenProperties,
			std::shared_ptr<const FOverriddenPropertySet>{}, std::memory_order_release);
	}

	auto FOverridableManager::GetOverriddenProperties(const DObject& Object) -> std::shared_ptr<const FOverriddenPropertySet>
	{
		return std::atomic_load_explicit(&Object.OverriddenProperties, std::memory_order_acquire);
	}

	auto FOverridableManager::CopyOverriddenProperties(DObject& Object, const DObject& Source,
		FAuthoredOverrideDiagnostic* OutDiagnostic) -> bool
	{
		const auto State = GetOverriddenProperties(Source);
		return RestoreOverriddenPropertyOperation(Object, State ? std::span(State->GetEntries()) : std::span<const FAuthoredOverrideEntry>{}, OutDiagnostic);
	}

	auto FOverriddenPropertySet::GetOverriddenPropertyOperation(const FAuthoredOverridePath& Path) const -> FOverriddenPropertyQuery
	{
		FOverriddenPropertyQuery Result;
		for (const auto& Entry : Entries)
		{
			if (IsAuthoredOverridePathPrefix(Entry.Path, Path))
				return {EOverriddenPropertyOperation::Replace, Entry.Path.size() < Path.size(), false,
					static_cast<uint32>(Entry.Path.size()), Entry.Provenance};
			Result.bHasOverriddenDescendants |= IsAuthoredOverridePathPrefix(Path, Entry.Path);
		}
		return Result;
	}

	auto FOverridableManager::GetOverriddenPropertyOperation(DObject& Object, const FAuthoredOverridePath& Path)
		-> std::expected<FOverriddenPropertyQuery, FAuthoredOverrideDiagnostic>
	{
		FAuthoredOverrideDiagnostic Diagnostic;
		if (!Path.empty() && !ValidateAuthoredOverridePath(&Object, Path, &Diagnostic)) return std::unexpected(Diagnostic);
		const auto State = GetOverriddenProperties(Object);
		return State ? State->GetOverriddenPropertyOperation(Path) : FOverriddenPropertyQuery{};
	}

	auto FOverridableManager::GetOverriddenPropertyOperation(DObject& Object, const FPropertyVisitorPath& Path)
		-> std::expected<FOverriddenPropertyQuery, FAuthoredOverrideDiagnostic>
	{
		auto Durable = Path.ToDurablePath();
		if (!Durable) return std::unexpected(FAuthoredOverrideDiagnostic{.Reason = Durable.error()});
		return GetOverriddenPropertyOperation(Object, *Durable);
	}

	auto FOverridableManager::GetOverriddenPropertyEntries(const DObject& Object) -> std::vector<FAuthoredOverrideEntry>
	{
		const auto State = GetOverriddenProperties(Object);
		return State ? State->GetEntries() : std::vector<FAuthoredOverrideEntry>{};
	}
}
