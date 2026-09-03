#pragma once

#include "Asset/Testing.h"

namespace Durin::AssetPrivate
{
	// Copies the first extension registered for the class or one of its superclasses.
	// An empty callback means no extension owns payload. The caller must release
	// the copy before unloading provider code.
	auto FindAssetOwnedPayloadRelocator(DClass* AssetClass)
		-> FAssetOwnedPayloadRelocator;

	auto NotifyAssetMoveObservers(
		std::span<const FAssetRelocationMapping> Mappings) -> void;

	auto ConsumeAssetRelocationFailure(
		EAssetRelocationFailurePoint Point) -> bool;
}
