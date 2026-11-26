#pragma once

#include "CoreMinimal.h"

#include "Math/Color.h"
#include "PropertyEditor/PropertyHandle.h"
#include "Widgets/MonaImGuiPropertyTable.h"

namespace Durin::PropertyEditor
{
	// Collects control observations and settles the old edit before starting the next.
	// The caller owns the edit session and supplies its domain operations.
	template<typename TId, typename TTarget, typename TValue>
	class TPropertyControlFrame
	{
	public:
		using EEnd = MonaImGui::PropertyEdit::EPropertyInteractionEnd;

		auto Observe(const TId& Id, const TTarget& Target, EEnd End, std::optional<TValue> Value) -> void
		{
			if (ActiveId == Id) ActiveObservation = {End, std::move(Value)};
			else if (Value && End != EEnd::Cancel) NewProposal = {Id, Target, std::move(*Value), End};
		}

		auto Finish(bool bCancel, auto&& FinishEdit) -> bool
		{
			ActiveObservation.reset();
			NewProposal.reset();
			if (!ActiveId) return true;
			const bool bFinished = FinishEdit(bCancel);
			if (bFinished) ActiveId.reset();
			return bFinished;
		}

		auto EndFrame(bool bCancel, auto&& BeginEdit, auto&& ApplyValue, auto&& FinishEdit) -> bool
		{
			const auto Previous = std::exchange(ActiveObservation, {});
			const auto Next = std::exchange(NewProposal, {});
			if (bCancel) return Finish(true, FinishEdit);

			if (ActiveId)
			{
				if (!Previous || Previous->End == EEnd::Cancel)
				{
					if (!Finish(true, FinishEdit)) return false;
				}
				else
				{
					if (Previous->Value && !ApplyValue(*Previous->Value)) return false;
					if (Previous->End == EEnd::Commit || Next)
						if (!Finish(false, FinishEdit)) return false;
				}
			}
			if (!Next) return true;
			if (!BeginEdit(Next->Id, Next->Target)) return false;
			ActiveId = Next->Id;
			if (!ApplyValue(Next->Value)) return false;
			return Next->End != EEnd::Commit || Finish(false, FinishEdit);
		}

	private:
		struct FObservation
		{
			EEnd End;
			std::optional<TValue> Value;
		};

		struct FProposal
		{
			TId Id;
			TTarget Target;
			TValue Value;
			EEnd End;
		};

		std::optional<TId> ActiveId;
		std::optional<FObservation> ActiveObservation;
		std::optional<FProposal> NewProposal;
	};

	// ImGui adapter for handle-backed controls. Observations are consumed after drawing.
	class FPropertyControlFrame
	{
	public:
		using EEnd = MonaImGui::PropertyEdit::EPropertyInteractionEnd;
		using FValue = FPropertyControlValue;

		DURINED_API auto Observe(ImGuiID Id, const FReflectedPropertyHandle& Handle, EEnd End,
			std::optional<FValue> Value) -> void;
		DURINED_API auto EndFrame(bool bCancel, bool bShouldTransact,
			const std::function<bool()>& PrepareNewEdit = {}, std::string* OutError = nullptr, bool* OutChanged = nullptr) -> bool;
		DURINED_API auto Finish(bool bCancel, std::string* OutError = nullptr) -> bool;
		auto IsActive() const -> bool { return Edit.IsActive(); }
		auto IsEditingObject(const DObject* Object) const -> bool { return Edit.IsEditingObject(Object); }
		auto MatchesTarget(const FPropertyEditTarget& Target) const -> bool { return Edit.MatchesTarget(Target); }

	private:
		auto FinishEdit(bool bCancel, std::string* OutError, bool* OutChanged = nullptr) -> bool;

		FPropertyEdit Edit;
		TPropertyControlFrame<ImGuiID, FReflectedPropertyHandle, FValue> Frame;
	};
}
