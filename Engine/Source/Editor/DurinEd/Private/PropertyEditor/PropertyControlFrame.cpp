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
		std::string* OutError) -> bool
	{
		return Frame.EndFrame(bCancel,
			[&](ImGuiID, const FReflectedPropertyHandle& Handle) {
				return Handle.IsEditable() && Edit.Begin(Handle.ResolveTarget(), {}, bShouldTransact, OutError);
			},
			[&](const FValue& Value) {
				if (!Edit.SetValue(Value, OutError))
				{
					Finish(true, OutError);
					return false;
				}
				return true;
			},
			[&](bool bRestore) { return FinishEdit(bRestore, OutError); });
	}

	auto FPropertyControlFrame::FinishEdit(bool bCancel, std::string* OutError) -> bool
	{
		if (!Edit.IsActive()) return true;
		if (bCancel && Edit.HasChanges()) Edit.Cancel(OutError);
		else if (bCancel) Edit.CancelUnchanged(OutError);
		else Edit.Commit(OutError);
		return !Edit.IsActive();
	}
}
