#pragma once

#include "CoreMinimal.h"

#include "MaterialGraphDocument.h"
#include "MaterialGraph.h"

namespace Durin::MaterialEditor
{
	// One document's event-driven inspection cache, shared by canvas and Details.
	// Subscriptions hold weak owners; callbacks only accumulate invalidations.
	class FMaterialGraphReadModel
	{
	public:
		MATERIALEDITOR_API ~FMaterialGraphReadModel();
		explicit FMaterialGraphReadModel(DMaterialGraph* Graph = nullptr) : BoundGraph(Graph) {}
		FMaterialGraphReadModel(const FMaterialGraphReadModel&) = delete;
		auto operator=(const FMaterialGraphReadModel&) -> FMaterialGraphReadModel& = delete;
		MATERIALEDITOR_API auto Refresh(DObject& Owner, std::span<const FMaterialGraphCatalogEntry> Catalog)
			-> FMaterialGraphChangeSet;
		auto GetView() const -> const FMaterialGraphView& { return View; }
	private:
		struct FSubscription
		{
			TWeakObjectPtr<DObject> Owner;
			FDelegateHandle Handle;
		};
		auto UnsubscribeDependencies() -> void;
		auto ObserveDependencies(DObject& Owner) -> void;
		TStrongObjectPtr<DMaterialGraph> BoundGraph;
		FSubscription Document;
		std::vector<FSubscription> Dependencies;
		FMaterialGraphChangeSet Pending;
		FMaterialGraphView View;
	};
}
