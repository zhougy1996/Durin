#include "Transactions/Transactor.h"

namespace Durin
{
	auto ApplyEditorChange(DTransactor& Transactor, std::unique_ptr<FCommandChange> Change) -> FTransactorResult
	{
		return Transactor.ApplyCommand(std::move(Change));
	}
}
