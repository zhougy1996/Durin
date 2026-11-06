#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"
#include "DObject/ObjectKey.h"

namespace Durin
{
	class DAssetDefinition;
	namespace AssetDefinitionPrivate { struct FRegistryState; }

	// Retains a dynamic definition until removal; may safely outlive its registry.
	class FAssetDefinitionRegistration
	{
	public:
		FAssetDefinitionRegistration() = default;
		DURINED_API ~FAssetDefinitionRegistration();
		FAssetDefinitionRegistration(const FAssetDefinitionRegistration&) = delete;
		auto operator=(const FAssetDefinitionRegistration&) -> FAssetDefinitionRegistration& = delete;
		DURINED_API FAssetDefinitionRegistration(FAssetDefinitionRegistration&& Other) noexcept;
		DURINED_API auto operator=(FAssetDefinitionRegistration&& Other) noexcept -> FAssetDefinitionRegistration&;
		DURINED_API auto Reset() -> void;
		auto IsValid() const -> bool { return Id != 0 && !State.expired(); }
		explicit operator bool() const { return IsValid(); }
	private:
		friend class FAssetDefinitionRegistry;
		std::weak_ptr<AssetDefinitionPrivate::FRegistryState> State;
		uint64 Id = 0;
	};

	// Game-thread registry of non-owning CDO identities and scoped dynamic definitions.
	class FAssetDefinitionRegistry
	{
	public:
		DURINED_API FAssetDefinitionRegistry();
		DURINED_API ~FAssetDefinitionRegistry();
		FAssetDefinitionRegistry(const FAssetDefinitionRegistry&) = delete;
		auto operator=(const FAssetDefinitionRegistry&) -> FAssetDefinitionRegistry& = delete;
		DURINED_API static auto Get() -> FAssetDefinitionRegistry&;
		// Returned definitions are borrowed until the next lifecycle mutation.
		// Exact identity wins; missing reflected classes still support exact lookup.
		DURINED_API auto FindDefinition(std::string_view AssetClassPath) -> const DAssetDefinition*;
		DURINED_API auto RegisterDefinition(DAssetDefinition& Definition,
			std::string* OutError = nullptr) -> FAssetDefinitionRegistration;
		DURINED_API auto RegisterClassDefaultObject(DAssetDefinition& Definition,
			std::string* OutError = nullptr) -> bool;
		DURINED_API auto UnregisterDefinition(FObjectKey Definition) -> void;
		DURINED_API auto GetRevision() const -> uint64;
		DURINED_API auto GetRegisteredClassPaths() const -> std::vector<std::string>;
	private:
		std::shared_ptr<AssetDefinitionPrivate::FRegistryState> State;
	};
}
