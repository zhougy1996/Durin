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

	auto FPropertyControlFrame::EndFrame(bool bCancel, bool bShouldTransact,
		const std::function<bool()>& PrepareNewEdit, std::string* OutError, bool* OutChanged) -> bool
	{
		bool bChanged = bCancel && Edit.HasChanges();
		const std::string PreviousError = FinishError;
		const bool bSucceeded = Frame.EndFrame(bCancel,
			[&](ImGuiID, const FReflectedPropertyHandle& Handle) {
				return (!PrepareNewEdit || PrepareNewEdit()) && Handle.IsEditable() && Edit.Begin(Handle.ResolveTarget(), {}, bShouldTransact, OutError);
			},
			[&](const FValue& Value) {
				bool bAppliedChange = false;
				if (!Edit.WriteControlValue(Value, OutError, &bAppliedChange))
				{
					Finish(true);
					return false;
				}
				bChanged |= bAppliedChange;
				return true;
			},
			[this](bool bRestore) { return FinishEdit(bRestore); });
		if (!bSucceeded && OutError && OutError->empty() && FinishError != PreviousError) *OutError = FinishError;
		if (bSucceeded && OutChanged) *OutChanged |= bChanged;
		return bSucceeded;
	}

	auto FPropertyControlFrame::FinishEdit(bool bCancel) -> bool
	{
		if (!Edit.IsActive()) return true;
		const int FrameNumber = ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : -1;
		if (!bCancel && FrameNumber >= 0 && FrameNumber == FailedFinishFrame) return false;
		const bool bFinished = bCancel
			? (Edit.HasChanges() ? Edit.Cancel(&FinishError) : Edit.CancelUnchanged(&FinishError))
			: Edit.Commit(&FinishError);
		// A target that became read-only is closed at commit.
		if (!bFinished && !Edit.IsActive()) Frame.RetireActiveControl();
		FailedFinishFrame = Edit.IsActive() && !bFinished ? FrameNumber : -1;
		if (bFinished) FinishError.clear();
		return bFinished;
	}
}
