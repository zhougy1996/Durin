#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "EngineAPI.h"

#include "Asset/Load.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	// Private loader/tool transaction. Captures exact newly admitted generations for rollback.
	// Records only packages admitted by explicit top-level synchronous load calls, including
	// their completed dependencies, even when the requested root fails.
	// Checked unloading is explicit and retryable. This ledger is not a GC owner;
	// native/reflected callers retain graphs they need across collection. Weak
	// identities never claim a replacement loaded at the same path.
	class FPackageLoadTransaction
	{
	public:
		FPackageLoadTransaction() = default;
		FPackageLoadTransaction(FPackageLoadTransaction&&) = default;
		auto operator=(FPackageLoadTransaction&&) -> FPackageLoadTransaction& = default;
		FPackageLoadTransaction(const FPackageLoadTransaction&) = delete;
		auto operator=(const FPackageLoadTransaction&) -> FPackageLoadTransaction& = delete;
		[[nodiscard]] ENGINE_API auto LoadPackageDetailed(const FPackagePath& Path,
			FAssetLoadReport* OutReport = nullptr) -> std::expected<DPackage*, Durin::UnifiedError::FError>;
		[[nodiscard]] ENGINE_API auto LoadObjectDetailed(const FSoftObjectPath& Path, const DClass* ExpectedClass,
			FAssetLoadReport* OutReport = nullptr) -> std::expected<DObject*, Durin::UnifiedError::FError>;


		template<typename T>
		[[nodiscard]] auto LoadObjectDetailed(const FSoftObjectPath& Path,
			FAssetLoadReport* OutReport = nullptr) -> std::expected<T*, Durin::UnifiedError::FError>
		{
			static_assert(std::is_base_of_v<DObject, T>);
			auto Result = LoadObjectDetailed(Path, T::StaticClass(), OutReport);
			if (!Result) return std::unexpected(std::move(Result.error()));
			return static_cast<T*>(*Result);
		}


		// Rejects unsaved state and restores residency for live references, returning InUse.
		// Reload abort may ignore saved dependency edges of the specified exact live
		// packages after dropping candidate graphs. Actual references and unsaved state
		// still protect owned packages; a later package at the same path is not exempt.
		ENGINE_API auto Unload(
			std::span<const TWeakObjectPtr<DPackage>> IgnoreSavedDependencies = {}) -> std::expected<void, Durin::UnifiedError::FError>;

	private:
		std::vector<TWeakObjectPtr<DPackage>> Packages;
	};

}
