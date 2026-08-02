#pragma once

#include "AssetImportCore.h"

namespace Durin
{
	auto CreateSceneImportProvider()
		-> std::shared_ptr<AssetImport::IImportProvider>;
}
