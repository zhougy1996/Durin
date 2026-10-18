#pragma once

#include "CoreMinimal.h"
#include "Transactions/Transactor.h"

namespace Durin::Editor
{
	// Executes one domain change and records it in the current editor history.
	DURINED_API auto ApplyEditorChange(DTransactor& Transactor, std::unique_ptr<FCommandChange> Change) -> FTransactorResult;
}
