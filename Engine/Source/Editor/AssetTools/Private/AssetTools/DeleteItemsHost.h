#pragma once
#include "CoreMinimal.h"
#include "ObjectTools.h"
namespace Durin::ObjectTools::Private
{
	using FFolderDeleteHandler = std::function<FDeleteItemsResult(const FDeleteItemsParameters&)>;
	ASSETTOOLS_API auto SetFolderDeleteHandler(FFolderDeleteHandler Handler) -> void;
}
