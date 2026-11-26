#include "PropertyEditor/PropertyControlFrame.h"

namespace Durin::PropertyEditor
{
	auto FPropertyControlFrame::Observe(ImGuiID Id, const FReflectedPropertyHandle& Handle,
		EEnd End, std::optional<FValue> Value) -> void
	{
		Frame.Observe(Id, Handle, End, std::move(Value));
	}

	auto FPropertyControlFrame::Finish(bool bCancel, std::string* OutError) -> bool
	{
		return Frame.Finish(bCancel, [&](bool bRestore) { return FinishEdit(bRestore, OutError); });
	}

	auto FPropertyControlFrame::EndFrame(bool bCancel, bool bShouldTransact,
		const std::function<bool()>& PrepareNewEdit, std::string* OutError, bool* OutChanged) -> bool
	{
		bool bChanged = false;
		const bool bSucceeded = Frame.EndFrame(bCancel,
			[&](ImGuiID, const FReflectedPropertyHandle& Handle) {
				return (!PrepareNewEdit || PrepareNewEdit()) && Handle.IsEditable() && Edit.Begin(Handle.ResolveTarget(), {}, bShouldTransact, OutError);
			},
			[&](const FValue& Value) {
				bool bAppliedChange = false;
				if (!Edit.WriteControlValue(Value, OutError, &bAppliedChange))
				{
					Finish(true, OutError);
					return false;
				}
				bChanged |= bAppliedChange;
				return true;
			},
			[&](bool bRestore) { return FinishEdit(bRestore, OutError, &bChanged); });
		if (bSucceeded && OutChanged) *OutChanged |= bChanged;
		return bSucceeded;
	}

	auto FPropertyControlFrame::FinishEdit(bool bCancel, std::string* OutError, bool* OutChanged) -> bool
	{
		if (!Edit.IsActive()) return true;
		if (bCancel && Edit.HasChanges())
		{
			const bool bRestored = Edit.Cancel(OutError);
			if (OutChanged) *OutChanged |= bRestored;
		}
		else if (bCancel) Edit.CancelUnchanged(OutError);
		else Edit.Commit(OutError);
		return !Edit.IsActive();
	}
}
