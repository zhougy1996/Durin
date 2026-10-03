#pragma once

#include "CoreMinimal.h"
#include "MaterialGraphDocument.h"

namespace Durin::Editor::Material
{
	auto DrawMaterialFunctionCallInputs(DObject& Owner, const FGuid& CallId,
		DTransactor& Transactions, std::string& Error) -> void;
}
