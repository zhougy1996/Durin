#pragma once

#include "CoreMinimal.h"
#include "MaterialGraphDocument.h"

namespace Durin::MaterialEditor
{
	auto DrawMaterialFunctionCallInputs(DObject& Owner, const FGuid& CallId,
		DTransactor& Transactions, std::string& Error) -> void;
}
