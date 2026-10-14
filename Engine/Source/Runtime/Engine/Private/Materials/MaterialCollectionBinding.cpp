#include "MaterialCollectionBinding.h"
#include "Materials/MaterialParameterCollection.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin::MaterialBuildPrivate
{
	auto ResolvePublicationCollections(std::span<const FMaterialParameterCollectionLayout> Expected)
		-> std::expected<std::vector<FMaterialParameterCollectionLayout>, ECollectionPublicationError>
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		std::vector<FMaterialParameterCollectionLayout> Current;
		if (Expected.empty()) return Current;
		const auto Objects = GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly);
		for (const auto& Layout : Expected)
		{
			DMaterialParameterCollection* Found = nullptr;
			for (auto* Object : Objects)
				if (auto* Collection = Cast<DMaterialParameterCollection>(Object); IsValid(Collection)
					&& Collection->GetCollectionId() == Layout.CollectionId && (Layout.AssetPath.empty() || Collection->GetObjectPath() == Layout.AssetPath))
				{ Found = Collection; break; }
			if (!Found) return std::unexpected(ECollectionPublicationError::MissingObject);
			auto Updated = Found->BuildLayout();
			if (!Updated || !Layout.HasCompatibleSchema(*Updated)) return std::unexpected(ECollectionPublicationError::IncompatibleSchema);
			Current.push_back(std::move(*Updated));
		}
		return Current;
	}
}
