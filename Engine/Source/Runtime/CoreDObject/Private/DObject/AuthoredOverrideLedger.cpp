#include "DObject/AuthoredOverrideLedger.h"

#include "DObject/DefaultDeltaPlan.h"
#include "DObject/Object.h"

namespace Durin
{
	namespace
	{
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

	auto FAuthoredOverridePathToken::MapValue(FByteArray CanonicalKeyToken)
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

	auto DObject::SetAuthoredOverride(const FAuthoredOverridePath& Path,
		EAuthoredOverrideProvenance Provenance, FAuthoredOverrideDiagnostic* OutDiagnostic) -> bool
	{
		if (Provenance != EAuthoredOverrideProvenance::LoadedExplicit
			&& Provenance != EAuthoredOverrideProvenance::Forced)
		{
			if (OutDiagnostic) *OutDiagnostic = {
				.Reason = EAuthoredOverrideFailureReason::InvalidProvenance};
			return false;
		}
		if (!ValidateAuthoredOverridePath(this, Path, OutDiagnostic)) return false;
		for (;;)
		{
			auto Current = std::atomic_load_explicit(&AuthoredOverrideLedger, std::memory_order_acquire);
			auto Next = Current ? std::make_shared<FAuthoredOverrideLedger>(*Current)
				: std::make_shared<FAuthoredOverrideLedger>();
			auto It = FindEntry(Next->Entries, Path);
			if (It != Next->Entries.end() && CompareAuthoredOverridePaths(It->Path, Path) == 0)
			{
				if (Provenance == EAuthoredOverrideProvenance::Forced) It->Provenance = Provenance;
			}
			else Next->Entries.insert(It, {Path, Provenance});
			std::shared_ptr<const FAuthoredOverrideLedger> Published = std::move(Next);
			if (std::atomic_compare_exchange_weak_explicit(&AuthoredOverrideLedger, &Current, Published,
				std::memory_order_release, std::memory_order_acquire))
			{
				if (OutDiagnostic) OutDiagnostic->Reset();
				return true;
			}
		}
	}

	auto DObject::ReplaceAuthoredOverrides(std::span<const FAuthoredOverrideEntry> Entries,
		FAuthoredOverrideDiagnostic* OutDiagnostic) -> bool
	{
		std::vector<FAuthoredOverrideEntry> Sorted(Entries.begin(), Entries.end());
		for (const FAuthoredOverrideEntry& Entry : Sorted)
		{
			if (Entry.Provenance != EAuthoredOverrideProvenance::LoadedExplicit
				&& Entry.Provenance != EAuthoredOverrideProvenance::Forced)
			{
				if (OutDiagnostic) *OutDiagnostic = {
					.Reason = EAuthoredOverrideFailureReason::InvalidProvenance};
				return false;
			}
		}
		if (Sorted.empty())
		{
			std::atomic_store_explicit(&AuthoredOverrideLedger,
				std::shared_ptr<const FAuthoredOverrideLedger>{}, std::memory_order_release);
			if (OutDiagnostic) OutDiagnostic->Reset();
			return true;
		}
		if (!ValidateAuthoredOverrideEntries(this, Sorted, OutDiagnostic)) return false;
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
		auto Ledger = std::make_shared<FAuthoredOverrideLedger>();
		Ledger->Entries = std::move(Sorted);
		std::atomic_store_explicit(&AuthoredOverrideLedger,
			std::shared_ptr<const FAuthoredOverrideLedger>(std::move(Ledger)), std::memory_order_release);
		if (OutDiagnostic) OutDiagnostic->Reset();
		return true;
	}

	auto DObject::ClearAuthoredOverride(const FAuthoredOverridePath& Path) -> bool
	{
		if (Path.empty()) return false;
		for (;;)
		{
			auto Current = std::atomic_load_explicit(&AuthoredOverrideLedger, std::memory_order_acquire);
			if (!Current) return false;
			auto Next = std::make_shared<FAuthoredOverrideLedger>(*Current);
			auto It = FindEntry(Next->Entries, Path);
			if (It == Next->Entries.end() || CompareAuthoredOverridePaths(It->Path, Path) != 0) return false;
			Next->Entries.erase(It);
			std::shared_ptr<const FAuthoredOverrideLedger> Published = Next->Entries.empty()
				? std::shared_ptr<const FAuthoredOverrideLedger>{} : std::move(Next);
			if (std::atomic_compare_exchange_weak_explicit(&AuthoredOverrideLedger, &Current, Published,
				std::memory_order_release, std::memory_order_acquire)) return true;
		}
	}

	auto DObject::ClearAuthoredOverrideSubtree(const FAuthoredOverridePath& Path) -> uint64
	{
		if (Path.empty()) return 0;
		for (;;)
		{
			auto Current = std::atomic_load_explicit(&AuthoredOverrideLedger, std::memory_order_acquire);
			if (!Current) return 0;
			auto Next = std::make_shared<FAuthoredOverrideLedger>(*Current);
			const size_t Before = Next->Entries.size();
			std::erase_if(Next->Entries, [&](const FAuthoredOverrideEntry& Entry) {
				return IsAuthoredOverridePathPrefix(Path, Entry.Path);
			});
			const uint64 Removed = static_cast<uint64>(Before - Next->Entries.size());
			if (Removed == 0) return 0;
			std::shared_ptr<const FAuthoredOverrideLedger> Published = Next->Entries.empty()
				? std::shared_ptr<const FAuthoredOverrideLedger>{} : std::move(Next);
			if (std::atomic_compare_exchange_weak_explicit(&AuthoredOverrideLedger, &Current, Published,
				std::memory_order_release, std::memory_order_acquire)) return Removed;
		}
	}

	auto DObject::ResetAuthoredOverrides() -> void
	{
		std::atomic_store_explicit(&AuthoredOverrideLedger,
			std::shared_ptr<const FAuthoredOverrideLedger>{}, std::memory_order_release);
	}

	auto DObject::GetAuthoredOverrideEntries() const -> std::vector<FAuthoredOverrideEntry>
	{
		const auto Ledger = std::atomic_load_explicit(&AuthoredOverrideLedger, std::memory_order_acquire);
		return Ledger ? Ledger->GetEntries() : std::vector<FAuthoredOverrideEntry>{};
	}

	auto DObject::HasAllocatedAuthoredOverrideLedger() const -> bool
	{
		return std::atomic_load_explicit(&AuthoredOverrideLedger, std::memory_order_acquire) != nullptr;
	}

	auto DObject::CopyAuthoredOverridesFrom(const DObject& Source,
		FAuthoredOverrideDiagnostic* OutDiagnostic) -> bool
	{
		const std::vector<FAuthoredOverrideEntry> Entries = Source.GetAuthoredOverrideEntries();
		return ReplaceAuthoredOverrides(Entries, OutDiagnostic);
	}
}
