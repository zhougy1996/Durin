#pragma once

#include "AssetBuildCoreAPI.h"
#include "Hash/XxHash.h"

namespace Durin::Asset::Build
{
	// Immutable named bytes exchanged with the derived-data cache.
	class FBuildValue
	{
	public:
		ASSETBUILDCORE_API static auto FromOwned(
			std::string Name, std::vector<uint8> Bytes) -> FBuildValue;

		auto GetName() const -> std::string_view { return Name; }
		auto GetContentIdentity() const -> const FXxHash128& { return ContentIdentity; }
		auto GetBytes() const -> std::span<const uint8>
		{
			return Bytes ? std::span<const uint8>(*Bytes) : std::span<const uint8>();
		}
		auto GetSize() const -> uint64 { return Bytes ? Bytes->size() : 0; }
		auto IsValid() const -> bool { return !Name.empty() && Bytes != nullptr; }

	private:
		std::string Name;
		FXxHash128 ContentIdentity;
		std::shared_ptr<const std::vector<uint8>> Bytes;
	};

	// Explicit cache query/store policy for one operation.
	struct FBuildCachePolicy
	{
		bool bQueryCache = true;
		bool bStoreBuildResult = true;
		bool bRequireStoreSuccess = false;
	};
}
