#pragma once
#include "CoreMinimal.h"

namespace Durin
{
	class DObject;
	// Provider-owned execution consumed by AssetTools; task control and result publication stay private.
	class IAssetImportTaskOperation
	{
	public:
		virtual ~IAssetImportTaskOperation() = default;
		virtual auto Tick() -> void = 0;
		virtual auto Cancel() -> void = 0;
		virtual auto IsComplete() const -> bool = 0;
		virtual auto GetObjects() const -> std::span<DObject* const> = 0;
	};
}
