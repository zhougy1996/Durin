#include "Viewport/TransformGizmoStateTarget.h"

#include "Transactions/Transactor.h"

namespace Durin
{
	void DTransformGizmoStateTarget::Initialize(DTransformProxy* InProxy, DTransactor* Transactions, std::string_view InAction)
	{
		Proxy = InProxy;
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
		const auto Description = Proxy->DescribeChange(Action);
		if (GUndo || Transactions->Begin("TransformGizmo", Description) < 0)
			throw std::runtime_error("Unable to begin the transform transaction.");
		RecordingOperationId = GUndo->GetContext().OperationId;
		ChangeSource.emplace(*Proxy.Get(), Transactions->GetHistoryId(), Description);
	}

	auto DTransformGizmoStateTarget::EndUpdate() -> void
	{
		auto* Transactions = TransactionManager.Get();
		if (Transactions && !OwnsRecording()) throw std::runtime_error("Transform transaction is no longer active.");
		if (Transactions)
		{
			if (bCancelPending || !Transactions->StoreUndo(Proxy.Get(), ChangeSource->EndChange()))
				throw std::runtime_error("Unable to record the transform change.");
			if (bCancelPending || !OwnsRecording()) throw std::runtime_error("Transform update interrupted.");
			Transactions->End();
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
		ChangeSource.reset();
		if (Proxy) Proxy->EndTransformEditSequence();
		Proxy.Reset();
		TransactionManager.Reset();
		RecordingOperationId = 0;
		bCancelPending = false;
	}
}
