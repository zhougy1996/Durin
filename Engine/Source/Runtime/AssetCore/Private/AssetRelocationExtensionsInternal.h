#pragma once

#include "AssetTestSupport.h"

namespace Durin::Asset::Private
{
	// Holds the provider gate for the complete load-and-contribute call so a
	// module-owned relocator cannot disappear while relocation is being prepared.
	struct FAssetOwnedPayloadRelocatorInvocation
	{
		FAssetOwnedPayloadRelocator Relocator;
		FModuleOwnedCallbackInvocation OwnerCall;
	};

	// Acquires the first extension registered for the concrete asset class or
	// one of its superclasses. An empty relocator means no extension owns payload.
	auto AcquireAssetOwnedPayloadRelocator(
		DClass* AssetClass,
		FAssetOwnedPayloadRelocatorInvocation& OutInvocation) -> FAssetResult;

	auto NotifyAssetMoveObservers(
		std::span<const FAssetRelocationMapping> Mappings) -> void;

	auto ConsumeAssetRelocationFailure(
		EAssetRelocationFailurePoint Point) -> bool;
}
