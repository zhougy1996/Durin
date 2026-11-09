#pragma once

#include "CoreMinimal.h"
#include "Asset/LoadAudit.h"

namespace Durin::AssetPrivate
{
	auto GetActiveAssetLoadAudit() -> FAssetLoadAudit*;
}
