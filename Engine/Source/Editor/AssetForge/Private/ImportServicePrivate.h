#pragma once

#include "AssetForge/ImportService.h"
#include "ComponentRegistryInternal.h"

namespace Durin::AssetForge
{
	class FImportJobStore;

	struct FImportService::FImpl
	{
		FImpl();
		~FImpl();

		Private::FComponentRegistryStore Import;
		std::shared_ptr<FImportJobStore> AsyncJobs;
	};
}
