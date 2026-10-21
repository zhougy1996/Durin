#pragma once

#include "CoreMinimal.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Class.h"
#include "Misc/ITransaction.h"

namespace Durin
{
	// Resolve a checked reflected path without retaining its current allocation.
	inline auto ResolveTransactionArray(const FTransactionArrayCapture& Request) -> void*
	{
		if (!Request.Object || !Request.Member || !Request.Object->GetClass()
			|| Request.Member->GetKind() != DurinCodeGen::EPropertyGenFlags::Array
			|| Request.MemberIndex >= Request.Member->GetArrayDim()) return nullptr;
		DStructBase* Type = Request.Object->GetClass();
		void* Container = Request.Object;
		for (const auto* Property : Request.StructPath)
		{
			if (!Property || Type->FindPropertyByName(Property->NamePrivate) != Property || Property->GetArrayDim() != 1
				|| Property->HasAnyPropertyFlags(EPropertyFlags::NonTransactional | EPropertyFlags::Deprecated)
				|| Property->GetKind() != DurinCodeGen::EPropertyGenFlags::Struct) return nullptr;
			Container = Property->GetValuePtr(Container);
			Type = static_cast<const FStructProperty*>(Property)->GetStruct();
			if (!Type) return nullptr;
		}
		if (Type->FindPropertyByName(Request.Member->NamePrivate) != Request.Member
			|| Request.Member->HasAnyPropertyFlags(EPropertyFlags::NonTransactional | EPropertyFlags::Deprecated)) return nullptr;
		return Request.Member->GetValuePtr(Container, Request.MemberIndex);
	}

	// Explicit std::vector mutation adapter. Prepare allocations before recording;
	// successful capture is followed only by nonthrowing relocation. Other shapes
	// and callback-driven edits must select their whole-object fallback up front.
	template<typename T>
		requires std::is_copy_constructible_v<T> && std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>
	auto EditTransactionArray(DObject& Owner, const FProperty* Member, std::vector<T>& Values,
		size_t Index, size_t RemoveCount, std::span<const T> Replacement,
		std::span<const FProperty* const> StructPath = {}, uint32 MemberIndex = 0) -> FTransactionCaptureResult
	{
		if (ResolveTransactionArray({.Object = &Owner, .Member = Member, .MemberIndex = MemberIndex, .StructPath = StructPath}) != &Values)
			return std::unexpected("The array mutation does not match its reflected owner/member path.");
		if (Index > Values.size() || RemoveCount > Values.size() - Index
			|| Replacement.size() > Values.max_size() - (Values.size() - RemoveCount))
			return std::unexpected("Array mutation range is out of bounds.");
		if (RemoveCount != 0 && !Replacement.empty() && RemoveCount != Replacement.size())
			return std::unexpected("A range replacement must preserve its element count.");
		if (RemoveCount == 0 && Replacement.empty()) return {};
		if constexpr (requires(const T& A, const T& B) { { A == B } -> std::convertible_to<bool>; })
			if (RemoveCount == Replacement.size() && std::equal(Replacement.begin(), Replacement.end(), Values.begin() + Index)) return {};
		try
		{
			std::vector<T> Prepared(Replacement.begin(), Replacement.end());
			Values.reserve(Values.size() - RemoveCount + Prepared.size());
			if (GUndo && Owner.HasAnyObjectFlags(EObjectFlags::Transactional))
			{
				const auto Operation = RemoveCount == 0 ? ETransactionArrayOperation::Insert
					: Prepared.empty() ? ETransactionArrayOperation::Remove : ETransactionArrayOperation::Replace;
				if (auto Saved = GUndo->SaveArray({.Object = &Owner, .Member = Member, .MemberIndex = MemberIndex,
					.Index = Index, .Count = RemoveCount == 0 ? Prepared.size() : RemoveCount,
					.Operation = Operation, .StructPath = StructPath}); !Saved) return Saved;
			}
			Owner.MarkPackageDirty();
			const auto& Ops = *ResolveArrayOps<std::vector<T>>();
			if (Ops.Splice(&Values, Index, RemoveCount, &Prepared) != EContainerOpResult::Success)
				return std::unexpected("Prepared array mutation was rejected.");
			return {};
		}
		catch (const std::exception& Error) { return std::unexpected(std::format("Array mutation preparation failed: {}", Error.what())); }
		catch (...) { return std::unexpected("Array mutation preparation failed."); }
	}
}
