#pragma once

#include "DObject/Object.h"
#include "Delegates/Delegate.h"
#include "Materials/MaterialFunctionTypes.h"

#include "MaterialFunctionInterface.gen.h"

namespace Durin
{
	class DMaterialFunctionInterface;
	DECLARE_MULTICAST_DELEGATE_OneParam(FMaterialFunctionChangedEvent, const DMaterialFunctionInterface&)
	ENGINE_API auto GetMaterialFunctionChangedEvent() -> FMaterialFunctionChangedEvent&;
	// Owning-thread semantic notification shared by all function implementations.
	ENGINE_API auto NotifyMaterialFunctionChanged(const DMaterialFunctionInterface& Function) -> void;
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
	};
}
