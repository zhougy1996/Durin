#include "PropertyEditor/PropertyControlFrame.h"

namespace Durin::PropertyEditor
{
	auto FPropertyControlFrame::Observe(ImGuiID Id, const FReflectedPropertyHandle& Handle,
		EEnd End, std::optional<FValue> Value) -> void
	{
		Frame.Observe(Id, Handle, End, std::move(Value));
	}

	auto FPropertyControlFrame::Finish(bool bCancel) -> bool
	{
		return Frame.Finish(bCancel, [this](bool bRestore) { return FinishEdit(bRestore); });
	}

	auto FPropertyControlFrame::EndFrame(bool bCancel, ETransactionMode Mode,
		const std::function<bool()>& PrepareNewEdit, std::string* OutError, bool* OutChanged) -> bool
	{
		bool bChanged = bCancel && Edit.HasChanges();
		const bool bSucceeded = Frame.EndFrame(bCancel,
			[&](ImGuiID, const FReflectedPropertyHandle& Handle) {
				return PrepareNewEdit() && Handle.BeginEdit(Edit, Mode, OutError);
			},
			[&](const FValue& Value) {
				bool bAppliedChange = false;
				if (!Edit.WriteControlValue(Value, OutError, bAppliedChange))
				{
					Finish(true);
					return false;
				}
				bChanged |= bAppliedChange;
				return true;
			},
			[this](bool bRestore) { return FinishEdit(bRestore); });
		if (bSucceeded && OutChanged) *OutChanged |= bChanged;
		return bSucceeded;
	}

	auto FPropertyControlFrame::FinishEdit(bool bCancel) -> bool
	{
		const bool bFinished = bCancel
			? (Edit.HasChanges() ? Edit.Cancel(&FinishError) : Edit.CancelUnchanged(&FinishError))
			: Edit.Commit(&FinishError);
		if (bFinished) FinishError.clear();
		return bFinished;
	}
}
