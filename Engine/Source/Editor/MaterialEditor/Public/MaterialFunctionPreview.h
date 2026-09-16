#pragma once

#include "MaterialGraphDocument.h"

namespace Durin::Editor::Material
{
	// Coalesces explicit requests and semantic edits across the dependency closure.
	// Layout-only edits do not invalidate the preview. Does not retain functions.
	class FMaterialFunctionPreviewInvalidation
	{
	public:
		MATERIALEDITOR_API FMaterialFunctionPreviewInvalidation();
		MATERIALEDITOR_API ~FMaterialFunctionPreviewInvalidation();
		FMaterialFunctionPreviewInvalidation(const FMaterialFunctionPreviewInvalidation&) = delete;
		auto operator=(const FMaterialFunctionPreviewInvalidation&) -> FMaterialFunctionPreviewInvalidation& = delete;
		MATERIALEDITOR_API auto SetFunction(DMaterialFunctionInterface* Function) -> void;
		MATERIALEDITOR_API auto RequestRefresh() -> void;
		MATERIALEDITOR_API auto ConsumeRefreshRequest() -> bool;
	private:
		struct FState;
		std::shared_ptr<FState> State;
		FDelegateHandle Handle;
	};

	// Builds an explicit transient root graph. Required inputs receive neutral
	// preview values; optional inputs retain the function's declared defaults.
	MATERIALEDITOR_API auto BuildMaterialFunctionPreview(DMaterialFunctionInterface& Function,
		const FGuid& OutputId, DMaterial& Preview) -> FMaterialGraphCommandResult;
}
