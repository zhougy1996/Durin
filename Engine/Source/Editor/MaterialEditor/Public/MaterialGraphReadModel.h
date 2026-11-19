#pragma once

#include "CoreMinimal.h"

#include "MaterialGraph.h"

namespace Durin::MaterialEditor
{
	// One document's event-driven inspection cache, shared by canvas and Details.
	// The graph publishes synchronized changes; each reader coalesces its own invalidations.
	class FMaterialGraphReadModel
	{
	public:
		MATERIALEDITOR_API ~FMaterialGraphReadModel();
		explicit FMaterialGraphReadModel(DMaterialGraph* Graph = nullptr) : BoundGraph(Graph) {}
		FMaterialGraphReadModel(const FMaterialGraphReadModel&) = delete;
		auto operator=(const FMaterialGraphReadModel&) -> FMaterialGraphReadModel& = delete;
		MATERIALEDITOR_API auto Refresh(DObject& Owner)
			-> FMaterialGraphChangeSet;
		auto GetView() const -> const FMaterialGraphView& { return View; }
	private:
		TStrongObjectPtr<DMaterialGraph> BoundGraph;
		FDelegateHandle Handle;
		FMaterialGraphChangeSet Pending;
		FMaterialGraphView View;
	};
}
