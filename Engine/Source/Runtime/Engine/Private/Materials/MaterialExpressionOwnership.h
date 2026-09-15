#pragma once

#include "Materials/MaterialExpressions.h"

namespace Durin::Private
{
	// Checks that the collection accounts for every expression child of this owner.
	auto ValidateExpressionOwnership(const DObject& Owner,
		const FMaterialExpressionCollection& Collection, std::string& OutError) -> bool;

	// GameThread only. The caller validates graph semantics before applying.
	// Duplicates every candidate before changing ownership; failure leaves the
	// current collection intact. Revision and notification policy stays with the caller.
	auto ReplaceOwnedExpressions(DObject& Owner, FMaterialExpressionCollection& Collection,
		std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult;
}
