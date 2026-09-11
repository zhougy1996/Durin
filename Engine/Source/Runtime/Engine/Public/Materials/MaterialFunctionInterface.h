#pragma once

#include "DObject/Object.h"
#include "Materials/MaterialFunctionTypes.h"

#include "MaterialFunctionInterface.gen.h"

namespace Durin
{
	// Calls consume this contract without requiring an editable graph owner.
	DCLASS(Abstract, NoClassDefaultObject)
	class DMaterialFunctionInterface : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DMaterialFunctionInterface(const FObjectInitializer& Initializer);
		virtual auto GetFunctionSignature() const -> const FMaterialFunctionSignature& = 0;
		virtual auto GetFunctionDependencies() const
			-> std::vector<TObjectPtr<DMaterialFunctionInterface>> = 0;
		virtual auto GetFunctionRevision() const -> uint64 = 0;
		// Owning-thread only. Implementations must return detached values.
		virtual auto BuildFunctionSnapshot(FMaterialFunctionSnapshot& OutSnapshot) const
			-> FMaterialProgramValidationResult = 0;
	};
}
