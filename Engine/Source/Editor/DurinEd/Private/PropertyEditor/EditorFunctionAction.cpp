#include "PropertyEditor/EditorFunctionAction.h"

#include "DObject/Class.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/Package.h"
#include "Threading/RunnableThread.h"

namespace Durin::Editor
{
	namespace { bool bExecutingFunction = false; }

	auto DescribeEditorFunctionError(EEditorFunctionError Error) -> std::string_view
	{
		switch (Error)
		{
		case EEditorFunctionError::None: return {};
		case EEditorFunctionError::WrongThread: return "Editor actions require the game thread.";
		case EEditorFunctionError::Reentrant: return "An editor action is already running.";
		case EEditorFunctionError::ReadOnly: return "This view is read-only.";
		case EEditorFunctionError::InvalidTarget: return "The target is no longer available.";
		case EEditorFunctionError::TemplateTarget: return "Actions cannot edit class defaults or templates.";
		case EEditorFunctionError::FunctionUnavailable: return "This editor action is no longer available.";
		case EEditorFunctionError::MissingTransactor: return "This host has no transaction service.";
		case EEditorFunctionError::BusyTransactor: return "Finish the current editor operation first.";
		case EEditorFunctionError::ActiveEdit: return "The active property edit could not be completed.";
		case EEditorFunctionError::History: return "The action's transaction could not be recorded.";
		case EEditorFunctionError::Invocation: return "Native function admission failed.";
		}
		return "Editor action failed.";
	}

	auto CanExecuteEditorFunction(const FPropertyViewContext& Context,
		const FEditorFunctionRequest& Request) -> EEditorFunctionError
	{
		if (!IsInGameThread()) return EEditorFunctionError::WrongThread;
		if (bExecutingFunction) return EEditorFunctionError::Reentrant;
		if (Context.bReadOnly) return EEditorFunctionError::ReadOnly;
		auto* Object = Request.Target.ResolveObjectPtr();
		if (!IsValid(Object)) return EEditorFunctionError::InvalidTarget;
		if (Object->IsTemplateObject()) return EEditorFunctionError::TemplateTarget;
		const auto* Function = Object->GetClass()->FindFunctionByName(Request.FunctionName);
		if (!Function || !Function->HasAnyFunctionFlags(EFunctionFlags::CallInEditor) || Function->GetNumParms() != 0
			|| Function->GetRootFunction().GetOuterDClass()->GetQualifiedName() != Request.RootOwner)
			return EEditorFunctionError::FunctionUnavailable;
		if (!Context.Transactor) return EEditorFunctionError::MissingTransactor;
		if (!Context.Transactor->CanBeginStandaloneTransaction()) return EEditorFunctionError::BusyTransactor;
		return EEditorFunctionError::None;
	}

	auto ExecuteEditorFunction(FPropertyView& View, const FPropertyViewContext& Context,
		const FEditorFunctionRequest& Request) -> FEditorFunctionResult
	{
		FEditorFunctionResult Result;
		auto Report = [&] {
			if (Context.ReportError && Result.Error != EEditorFunctionError::None)
			{
				std::string Message(DescribeEditorFunctionError(Result.Error));
				if (Result.bInvoked) Message += " Native code already ran; the mutation was not rolled back.";
				if (Result.Error == EEditorFunctionError::History) Message += " " + FormatTransactorResult(Result.History);
				Context.ReportError(std::move(Message));
			}
			return Result;
		};
		// Do not touch an edit session from another thread or a recursive invocation.
		if (!IsInGameThread()) { Result.Error = EEditorFunctionError::WrongThread; return Report(); }
		if (bExecutingFunction) { Result.Error = EEditorFunctionError::Reentrant; return Report(); }
		if (!View.FinishActiveEdit(&Context, false))
		{ Result.Error = EEditorFunctionError::ActiveEdit; return Report(); }
		Result.Error = CanExecuteEditorFunction(Context, Request);
		if (Result.Error != EEditorFunctionError::None) return Report();
		TStrongObjectPtr<DObject> Target(Request.Target);
		TStrongObjectPtr<DTransactor> Transactor(Context.Transactor);
		const auto* Function = Target->GetClass()->FindFunctionByName(Request.FunctionName);
		struct FExecutionGuard
		{
			FExecutionGuard() { bExecutingFunction = true; }
			~FExecutionGuard() { bExecutingFunction = false; }
		} Guard;
		FScopedTransaction Transaction;
		Result.History = Transaction.BeginChecked(Transactor.Get(), {
			.Name = Request.RootOwner.ToString() + "::" + Request.FunctionName.ToString(),
			.Description = Function->GetDisplayName(), .PrimaryObject = FPersistentObjectRef(Target.Get())});
		if (!Result.History) { Result.Error = EEditorFunctionError::History; return Report(); }
		Result.History = Transaction.TryModify(Target.Get());
		if (!Result.History && Result.History.Code != ETransactorResultCode::NoOp)
		{ Result.Error = EEditorFunctionError::History; return Report(); }
		const auto Invoked = Target->ProcessEvent(Function, {});
		if (Invoked != EFunctionInvokeError::None)
		{
			Result.History = Transaction.Cancel();
			Result.Error = Result.History.Code != ETransactorResultCode::Discarded
				? EEditorFunctionError::History : EEditorFunctionError::Invocation;
			return Report();
		}
		Result.bInvoked = true;
		Target->PostEditChange();
		Result.History = Transaction.End();
		Result.bChanged = Result.History.Code == ETransactorResultCode::Succeeded;
		if (Result.History.Code != ETransactorResultCode::Succeeded && Result.History.Code != ETransactorResultCode::NoOp)
		{
			Result.Error = EEditorFunctionError::History;
			Target->MarkPackageDirty();
			if (auto* Package = Target->GetPackage()) Transactor->InvalidateSavedState(*Package);
		}
		return Report();
	}
}
