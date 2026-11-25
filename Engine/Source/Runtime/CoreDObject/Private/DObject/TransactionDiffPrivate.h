#pragma once

#include "DObject/TransactionDiff.h"

namespace Durin::DiffUtil
{
	auto GetArchetypeDiffableObject(const FDiffableObject& Restored) -> std::expected<FDiffableObject, std::string>;
}
