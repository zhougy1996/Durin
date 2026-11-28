#include "PropertyEditor/PropertyControl.h"

#include "Editor/EditorEngine.h"
#include "Misc/AssertionMacros.h"
#include "DObject/Property.h"
#include "PropertyEditor/PropertyValueDraft.h"

namespace Durin::PropertyEditor
{
	namespace
	{
		auto Report(std::string* OutError, std::string Message) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto CapturePickerValue(const FPropertyEditTarget& Target, FPropertyValueSnapshot& Snapshot) -> std::expected<void, std::string>
		{
			FResolvedPropertyValue Leaf;
			if (auto Path = ResolveReflectedPropertyValue(Target, Leaf); !Path)
				return std::unexpected(FormatPropertyEditPathError(Path.error()));
			if (auto Captured = CapturePropertyValue(Leaf.Property, Leaf.Container, Leaf.ArrayIndex, Snapshot); !Captured)
				return std::unexpected(ToString(Captured.error()));
			return {};
		}

	}

	auto FPropertyControl::FColorPicker::Capture(const FPropertyEditTarget& Target) -> std::expected<void, std::string>
	{
		return CapturePickerValue(Target, Original);
	}

	// Rebuild a nested proposal from the current root so cancelling a picker preserves sibling edits.
	auto FPropertyControl::FColorPicker::Restore(IPropertyHandle& Handle, const FPropertyEditTarget& Target) const
		-> std::expected<bool, std::string>
	{
		FPropertyValueSnapshot Root;
		const FPropertyValueSnapshot* Proposed = &Original;
		if (Target.SnapshotProperty != Target.LeafProperty)
		{
			FPropertyValueDraft Draft(Target);
			FResolvedPropertyValue Leaf;
			if (auto Path = Draft.Resolve(Target, Leaf.Property, Leaf.Container, Leaf.ArrayIndex); !Path)
				return std::unexpected(FormatPropertyValueDraftError(Path.error()));
			if (auto Restored = RestorePropertyValue(Leaf.Property, Leaf.Container, Leaf.ArrayIndex, Original); !Restored)
				return std::unexpected(ToString(Restored.error()));
			if (auto Captured = Draft.Capture(Root); !Captured)
				return std::unexpected(FormatPropertyValueDraftError(Captured.error()));
			Proposed = &Root;
		}
		const TWeakObjectPtr<DObject> Owner(Target.Object);
		auto Result = Handle.SetValueFromSnapshot(*Proposed, EPropertyValueSetFlags::NotTransactable);
		if (!Result) return std::unexpected(Handle.GetLastError());
		if (!Owner.IsValid()) return std::unexpected("The property edit target is unavailable.");
		FPropertyValueSnapshot Restored;
		if (auto Capture = CapturePickerValue(Target, Restored); !Capture) return std::unexpected(Capture.error());
		if (Restored != Original) return std::unexpected("The color picker could not restore the original color.");
		return *Result;
	}

	FPropertyControl::~FPropertyControl() { Finish(false); }

	auto FPropertyControl::HasRecording() const -> bool
	{
		return !Recording || (GEditor && History.Get() == GEditor->GetTransactor() && GUndo && GUndo->GetContext().TransactionId == Recording);
	}

	auto FPropertyControl::BeginGesture(std::shared_ptr<IPropertyHandle> Handle, bool bShouldTransact,
		bool bColorPicker, std::string* OutError) -> bool
	{
		if (IsActive() || !Handle) return Report(OutError, "The property control is unavailable or already active.");
		const auto Target = Handle->ResolveTarget();
		if (!Target.Object && Target.MemberProperty) return Report(OutError, "The reflected-property edit target is no longer live.");
		if (const auto Valid = Target.Validate(); !Valid) return Report(OutError, FormatPropertyEditPathError(Valid.error()));
		TStrongObjectPtr<DObject> TargetOwner(FObjectKey(Target.Object));
		if (!TargetOwner) return Report(OutError, "The reflected-property edit target is no longer live.");
		if (GEditor && GEditor->IsShutdownRequested()) return Report(OutError, "The editor is shutting down.");
		std::optional<FColorPicker> ColorPicker;
		if (bColorPicker)
		{
			auto Capture = ColorPicker.emplace().Capture(Target);
			if (!Capture) return Report(OutError, Capture.error());
		}
		if (!Target.SnapshotProperty->HasAnyPropertyFlags(EPropertyFlags::NonTransactional))
		{
			const auto Transactor = ResolveEditorTransactor(bShouldTransact);
			if (!Transactor) return Report(OutError, Transactor.error());
			if (*Transactor)
			{
				const bool bBegin = !GUndo;
				if (bBegin && GEditor->BeginTransaction("PropertyEditor", std::format("Edit {}", Target.MemberProperty->NamePrivate), Target.Object) < 0)
					return Report(OutError, "The reflected-property transactor rejected the edit scope.");
				if (!GUndo) return Report(OutError, "The property transaction is no longer active.");
				History = *Transactor;
				Recording = GUndo->GetContext().TransactionId;
				bOwnsRecording = bBegin;
			}
		}
		Owner = std::move(TargetOwner);
		ActiveHandle = std::move(Handle);
		Picker = std::move(ColorPicker);
		return true;
	}

	FPropertyControl::FInput::FInput(FPropertyControl& InControl, std::string* OutError)
		: Control(InControl), Error(OutError)
	{
		if (!Control.HasRecording())
		{
			Report(Error, "The property transaction is no longer active.");
			Control.Finish(false);
		}
		else if (!Control.IsActive()) Report(Error, "No property gesture is active.");
		else
		{
			Handle = Control.ActiveHandle.get();
			++Control.InputDepth;
		}
	}

	FPropertyControl::FInput::~FInput()
	{
		if (Handle && --Control.InputDepth == 0 && Control.PendingFinish)
			Control.Finish(*std::exchange(Control.PendingFinish, {}), Error);
	}

	auto FPropertyControl::Finish(bool bCancelPicker, std::string* OutError, bool* OutChanged) -> bool
	{
		if (InputDepth)
		{
			PendingFinish = bCancelPicker;
			return true;
		}
		if (Recording && HasRecording())
			requiref(History.Get()->CanTransact(), "cannot finish a property gesture during a protected transaction operation");
		auto Handle = std::move(ActiveHandle);
		if (!Handle) return true;
		auto TargetOwner = std::move(Owner);
		auto Color = std::exchange(Picker, {});
		const bool bLive = HasRecording();
		const auto Id = std::exchange(Recording, 0);
		const auto Buffer = std::exchange(History, {});
		const bool bEnd = std::exchange(bOwnsRecording, false);
		if (!bLive) return Report(OutError, "The property transaction is no longer active.");
		bool bSuccess = true;
		if (bCancelPicker && Color)
		{
			const auto Result = TargetOwner.IsValid() ? Color->Restore(*Handle, Handle->ResolveTarget())
				: std::expected<bool, std::string>(std::unexpected("The property edit target is unavailable."));
			if (OutChanged) *OutChanged |= Result.value_or(false);
			if (!Result) bSuccess = Report(OutError, Result.error());
		}
		else if (!TargetOwner.IsValid() || !TargetOwner->CanEditChange(Handle->ResolveTarget().MemberProperty))
			bSuccess = Report(OutError, "The property is no longer editable.");
		else Handle->FinishInteractiveChange();
		if (bEnd && GEditor && Buffer.IsValid() && Buffer.Get() == GEditor->GetTransactor() && GUndo && GUndo->GetContext().TransactionId == Id)
		{
			if (bCancelPicker && Color && bSuccess) Buffer.Get()->Cancel();
			else Buffer.Get()->End();
		}
		return bSuccess;
	}

}
