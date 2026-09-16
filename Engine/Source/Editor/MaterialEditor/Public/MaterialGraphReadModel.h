#pragma once

#include "MaterialGraphDocument.h"

namespace Durin::Editor::Material
{
	// One document's event-driven inspection cache, shared by canvas and Details.
	// Subscriptions hold weak owners; callbacks only accumulate invalidations.
	class FMaterialGraphReadModel
	{
	public:
		MATERIALEDITOR_API ~FMaterialGraphReadModel();
		FMaterialGraphReadModel() = default;
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
		FSubscription Document;
		std::vector<FSubscription> Dependencies;
		FMaterialGraphChangeSet Pending;
		FMaterialGraphView View;
	};
}
