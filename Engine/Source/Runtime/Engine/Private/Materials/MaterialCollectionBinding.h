#pragma once
#include "CoreMinimal.h"
#include "Materials/MaterialProgramCompiler.h"
namespace Durin::MaterialBuildPrivate
{
	enum class ECollectionPublicationError { MissingObject, IncompatibleSchema };
	// Game-thread publication resolves current defaults without changing schema.
	ENGINE_API auto ResolvePublicationCollections(std::span<const FMaterialParameterCollectionLayout> Expected)
		-> std::expected<std::vector<FMaterialParameterCollectionLayout>, ECollectionPublicationError>;
}
