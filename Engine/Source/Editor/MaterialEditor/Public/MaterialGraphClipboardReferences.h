#pragma once

#include "MaterialEditorAPI.h"
#include "DObject/Object.h"
#include "DObject/ObjectPtr.h"
#include "MaterialGraphClipboardReferences.gen.h"

namespace Durin
{
	// A transient GC root whose reflected slots participate in package replacement.
	DCLASS()
	class DMaterialGraphClipboardReferences : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DMaterialGraphClipboardReferences(const FObjectInitializer& Initializer) : Super(Initializer) {}
		DPROPERTY()
		std::vector<TObjectPtr<DObject>> Functions;
		DPROPERTY()
		std::vector<TObjectPtr<DObject>> Textures;
	};
}
