#pragma once

#include "EngineAPI.h"

namespace Durin
{
	namespace DerivedData
	{
		class FCacheKey;
	}

	// Owns an opaque FCacheKey without exposing the Developer/DerivedDataCache
	// type through Engine public headers.
	class FCacheKeyProxy
	{
	public:
		ENGINE_API FCacheKeyProxy();
		ENGINE_API FCacheKeyProxy(const FCacheKeyProxy& Other);
		ENGINE_API FCacheKeyProxy(FCacheKeyProxy&& Other) noexcept;
		ENGINE_API auto operator=(const FCacheKeyProxy& Other)
			-> FCacheKeyProxy&;
		ENGINE_API auto operator=(FCacheKeyProxy&& Other) noexcept
			-> FCacheKeyProxy&;
		ENGINE_API ~FCacheKeyProxy();
#if DURIN_WITH_EDITOR
		ENGINE_API explicit FCacheKeyProxy(const DerivedData::FCacheKey& InKey);
		[[nodiscard]] ENGINE_API auto AsCacheKey() -> DerivedData::FCacheKey*;
		[[nodiscard]] ENGINE_API auto AsCacheKey() const
			-> const DerivedData::FCacheKey*;
#endif

		[[nodiscard]] ENGINE_API auto IsValid() const -> bool;
		[[nodiscard]] ENGINE_API auto ToString() const -> std::string;
		explicit operator bool() const { return IsValid(); }
		ENGINE_API auto operator==(const FCacheKeyProxy& Other) const -> bool;

	private:
		static constexpr size_t StorageSize = 24;
		alignas(uint64) std::array<std::byte, StorageSize> Storage{};
	};

	static_assert(sizeof(FCacheKeyProxy) == 24);
}
