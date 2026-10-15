#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "DObject/ObjectKey.h"
#include "PropertyEditor/PropertyView.h"
#include "Transactions/Transactor.h"

namespace Durin::Editor
{
	// Requests own identities only; they are re-resolved after finishing a property edit.
	struct FEditorFunctionRequest
	{
		FObjectKey Target;
		FName FunctionName;
		FName RootOwner;
	};

	enum class EEditorFunctionError : uint8
	{
		None, WrongThread, Reentrant, ReadOnly, InvalidTarget, TemplateTarget,
		FunctionUnavailable, MissingTransactor, BusyTransactor, ActiveEdit, History, Invocation
	};
	struct FEditorFunctionResult
	{
		EEditorFunctionError Error = EEditorFunctionError::None;
		bool bInvoked = false;
		bool bChanged = false;
		FTransactorResult History;
	};

	DURINED_API auto CanExecuteEditorFunction(const FPropertyViewContext& Context,
		const FEditorFunctionRequest& Request) -> EEditorFunctionError;
	DURINED_API auto ExecuteEditorFunction(FPropertyView& View, const FPropertyViewContext& Context,
		const FEditorFunctionRequest& Request) -> FEditorFunctionResult;
	DURINED_API auto DescribeEditorFunctionError(EEditorFunctionError Error) -> std::string_view;
}
