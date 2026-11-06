#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "Toolkits/AssetEditor.h"
#include "GenericAssetEditor.gen.h"

namespace Durin
{
	DCLASS(NoClassDefaultObject)
	class DGenericAssetEditor : public DAssetEditor
	{
		GENERATED_BODY()
	public:
		explicit DGenericAssetEditor(const FObjectInitializer& Initializer);
	protected:
		auto CreateToolkit() -> std::shared_ptr<FAssetEditorToolkit> override;
	};
}
