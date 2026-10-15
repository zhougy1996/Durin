#pragma once

#include "CoreMinimal.h"
#include "Asset/Cook.h"

namespace Durin::AssetPrivate
{
	inline constexpr std::string_view CookPackageSerializer = "engine-package";
	inline constexpr uint32 CookPackageSerializerVersion = 2;
	// Legacy wire field: fixed compatibility value, never a family recipe version.
	inline constexpr uint32 CookLegacyProducerVersion = 1;
	struct FCookRegistrySnapshot
	{
		// Pin immutable registrations without running provider copy constructors under the registry lock.
		std::unordered_map<DClass*, std::shared_ptr<const FCookContributorRegistration>> Entries;
		auto Resolve(DClass* Class, bool AllowGeneric = true) const -> const FCookContributorRegistration*;
		auto Includes(const DObject* Object) const -> bool;
	};
	auto CaptureCookRegistry() -> std::shared_ptr<const FCookRegistrySnapshot>;
	// Engine module owns a persistent scope. Standalone saves use the same registry
	// through a temporary scope; nested saves never retire another owner's handles.
	class FEngineCookRegistrationScope
	{
	public:
		FEngineCookRegistrationScope();
		~FEngineCookRegistrationScope();
		FEngineCookRegistrationScope(const FEngineCookRegistrationScope&) = delete;
		auto operator=(const FEngineCookRegistrationScope&) -> FEngineCookRegistrationScope& = delete;
		FCookContributorRegistrationResult Result;
	private:
		std::vector<FCookContributorHandle> Handles;
	};
}
