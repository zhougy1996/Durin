#include "PropertyEditor/EditorFunctionAction.h"
#include "Editor/EditorEngine.h"

#include "DObject/Class.h"
#include "DObject/Function.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/Package.h"
#include "Threading/RunnableThread.h"

namespace Durin
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
		if (!Context.bShouldTransact || !GetTransactionEditor() || !GetTransactionEditor()->GetTransactor())
			return EEditorFunctionError::MissingTransactor;
		if (!GetTransactionEditor()->CanTransact()) return EEditorFunctionError::BusyTransactor;
		return EEditorFunctionError::None;
	}

	auto ExecuteEditorFunction(FPropertyView& View, const FPropertyViewContext& Context,
		const FEditorFunctionRequest& Request) -> FEditorFunctionResult
	{
		FEditorFunctionResult Result;
		auto Report = [&](std::string_view Detail = {}) {
			if (Context.ReportError && Result.Error != EEditorFunctionError::None)
			{
				std::string Message(DescribeEditorFunctionError(Result.Error));
				if (!Detail.empty()) { Message += " "; Message += Detail; }
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
		const auto* Function = Target->GetClass()->FindFunctionByName(Request.FunctionName);
		if (ValidateFunctionInvocation(Target.Get(), Function, {}) != EFunctionInvokeError::None)
		{
			Result.Error = EEditorFunctionError::Invocation;
			return Report();
		}
		struct FExecutionGuard
		{
			FExecutionGuard() { bExecutingFunction = true; }
			~FExecutionGuard() { bExecutingFunction = false; }
		} Guard;
		{
			FScopedTransaction Transaction(Request.RootOwner.ToString() + "::" + Request.FunctionName.ToString(),
				Function->GetDisplayName(), Target.Get());
			if (!Transaction.IsOutstanding())
			{
				Result.Error = EEditorFunctionError::History;
				return Report();
			}
			Target->Modify();
			if (ValidateFunctionInvocation(Target.Get(), Function, {}) != EFunctionInvokeError::None)
			{
				Result.Error = EEditorFunctionError::Invocation;
				return Report();
			}
			Target->ProcessEvent(Function, nullptr);
			Result.bInvoked = true;
			Target->PostEditChange();
		}
		return Report();
	}
}
