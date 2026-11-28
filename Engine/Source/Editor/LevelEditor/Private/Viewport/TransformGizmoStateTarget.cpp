#include "Viewport/TransformGizmoStateTarget.h"

#include "DObject/Package.h"
#include "Transactions/Transactor.h"

namespace Durin
{
	void DTransformGizmoStateTarget::Initialize(std::shared_ptr<LevelEditor::FTransformProxy> InProxy, DTransactor* Transactions, std::string_view InAction)
	{
		Proxy = std::move(InProxy);
		TransactionManager = Transactions;
		Action = InAction;
	}

	bool DTransformGizmoStateTarget::CanContinueWith(const DTransactor* Transactions) const
	{
		return !bCancelPending && Transactions == TransactionManager.Get()
			&& (!(RecordingOperationId || GUndo) || OwnsRecording());
	}

	auto DTransformGizmoStateTarget::OwnsRecording() const -> bool
	{
		return RecordingOperationId && GUndo && GUndo->GetContext().OperationId == RecordingOperationId;
	}

	auto DTransformGizmoStateTarget::BeginUpdate() -> void
	{
		auto* Transactions = TransactionManager.Get();
		if (!Transactions) return;
		ChangeSources = Proxy->CreateChangeSources(Transactions->GetHistoryId(), Action);
		const auto Description = Proxy->DescribeChange(Action);
		if (GUndo || Transactions->Begin("TransformGizmo", Description) < 0)
			throw std::runtime_error("Unable to begin the transform transaction.");
		RecordingOperationId = GUndo->GetContext().OperationId;
	}

	auto DTransformGizmoStateTarget::EndUpdate() -> void
	{
		const auto Changed = Proxy->CaptureFinalValues();
		if (!Changed) throw std::runtime_error("Transform target is unavailable.");
		const auto PackageRevisions = Proxy->GetPackageEditRevisions();
		if (auto* Transactions = TransactionManager.Get())
		{
			if (!OwnsRecording()) throw std::runtime_error("Transform transaction is no longer active.");
			for (const auto& Source : ChangeSources)
			{
				auto Change = Source.EndChange();
				if (bCancelPending || !OwnsRecording()) throw std::runtime_error("Transform update interrupted.");
				if (Change && !Transactions->StoreUndo(Source.GetChangeTarget(), std::move(Change)))
					throw std::runtime_error("Unable to record the transform change.");
			}
			if (bCancelPending || !OwnsRecording()) throw std::runtime_error("Transform update interrupted.");
			Transactions->End();
		}
		// A committed transaction marks its packages. A release without history still needs a dirty mark.
		if (*Changed)
		{
			for (const auto& [Package, Revision] : PackageRevisions)
				if (Package && Package->GetEditRevision() == Revision) Package->MarkDirty();
		}
		ReleaseGesture();
	}

	void DTransformGizmoStateTarget::CancelUpdate()
	{
		bCancelPending = true;
		if (auto* Transactions = TransactionManager.Get(); Transactions && OwnsRecording()) Transactions->Cancel();
		if (GUndo) return;
		if (Proxy && !Proxy->Restore()) return;
		ReleaseGesture();
	}

	void DTransformGizmoStateTarget::ReleaseGesture()
	{
		ChangeSources.clear();
		Proxy.reset();
		TransactionManager.Reset();
		RecordingOperationId = 0;
		bCancelPending = false;
	}
}
