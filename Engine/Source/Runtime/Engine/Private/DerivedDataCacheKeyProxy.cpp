#include "DerivedDataCacheKeyProxy.h"

#if DURIN_WITH_EDITOR
#include "DerivedDataCache/DerivedDataCache.h"
#endif

namespace Durin
{
#if DURIN_WITH_EDITOR
	static_assert(sizeof(DerivedData::FCacheKey) == sizeof(FCacheKeyProxy));
	static_assert(alignof(DerivedData::FCacheKey) == alignof(FCacheKeyProxy));

	auto FCacheKeyProxy::AsCacheKey() -> DerivedData::FCacheKey*
	{
		return std::launder(
			reinterpret_cast<DerivedData::FCacheKey*>(Storage.data()));
	}

	auto FCacheKeyProxy::AsCacheKey() const -> const DerivedData::FCacheKey*
	{
		return std::launder(
			reinterpret_cast<const DerivedData::FCacheKey*>(Storage.data()));
	}
#endif

	FCacheKeyProxy::FCacheKeyProxy()
	{
#if DURIN_WITH_EDITOR
		std::construct_at(reinterpret_cast<DerivedData::FCacheKey*>(Storage.data()));
#endif
	}

	FCacheKeyProxy::FCacheKeyProxy(const FCacheKeyProxy& Other)
	{
#if DURIN_WITH_EDITOR
		std::construct_at(reinterpret_cast<DerivedData::FCacheKey*>(Storage.data()),
			*Other.AsCacheKey());
#else
		Storage = Other.Storage;
#endif
	}

	FCacheKeyProxy::FCacheKeyProxy(FCacheKeyProxy&& Other) noexcept
	{
#if DURIN_WITH_EDITOR
		std::construct_at(reinterpret_cast<DerivedData::FCacheKey*>(Storage.data()),
			std::move(*Other.AsCacheKey()));
#else
		Storage = Other.Storage;
#endif
	}

	auto FCacheKeyProxy::operator=(const FCacheKeyProxy& Other)
		-> FCacheKeyProxy&
	{
		if (this == &Other) return *this;
#if DURIN_WITH_EDITOR
		*AsCacheKey() = *Other.AsCacheKey();
#else
		Storage = Other.Storage;
#endif
		return *this;
	}

	auto FCacheKeyProxy::operator=(FCacheKeyProxy&& Other) noexcept
		-> FCacheKeyProxy&
	{
		if (this == &Other) return *this;
#if DURIN_WITH_EDITOR
		*AsCacheKey() = std::move(*Other.AsCacheKey());
#else
		Storage = Other.Storage;
#endif
		return *this;
	}

	FCacheKeyProxy::~FCacheKeyProxy()
	{
#if DURIN_WITH_EDITOR
		std::destroy_at(AsCacheKey());
#endif
	}

#if DURIN_WITH_EDITOR
	FCacheKeyProxy::FCacheKeyProxy(const DerivedData::FCacheKey& InKey)
	{
		std::construct_at(
			reinterpret_cast<DerivedData::FCacheKey*>(Storage.data()), InKey);
	}
#endif

	auto FCacheKeyProxy::IsValid() const -> bool
	{
#if DURIN_WITH_EDITOR
		return AsCacheKey()->IsValid();
#else
		return false;
#endif
	}

	auto FCacheKeyProxy::ToString() const -> std::string
	{
#if DURIN_WITH_EDITOR
		return AsCacheKey()->ToString();
#else
		return {};
#endif
	}

	auto FCacheKeyProxy::operator==(const FCacheKeyProxy& Other) const -> bool
	{
#if DURIN_WITH_EDITOR
		return *AsCacheKey() == *Other.AsCacheKey();
#else
		return Storage == Other.Storage;
#endif
	}
}
