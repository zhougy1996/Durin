#pragma once

#if DURIN_WITH_EDITOR
#include "Serialization/Archive.h"
#include "Asset/CookedAsset.h"

namespace Durin::TexturePrivate
{
	// Family-neutral archive mechanics; adapters retain product validation and typed errors.
	template<typename TPlatform>
	auto DecodePlatformData(FByteView Bytes, ECookTargetProfile Profile, TPlatform& Product)
		-> std::expected<void, FArchiveFailure>
	{
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::DerivedDataPayload,
			{.Target = {"Win64", Profile == ECookTargetProfile::Game ? "Game" : "EditorValidation"}});
		Product.Serialize(Ar);
		if (Ar.IsError() || !RequireArchiveEnd(Ar))
			return std::unexpected(Ar.GetFailure() ? *Ar.GetFailure() : FArchiveFailure{});
		return {};
	}

	template<typename TPlatform>
	auto EncodePlatformData(TPlatform& Product, ECookTargetProfile Profile)
		-> std::expected<FByteBuffer, FArchiveFailure>
	{
		FByteBuffer Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::DerivedDataPayload,
			{.Target = {"Win64", Profile == ECookTargetProfile::Game ? "Game" : "EditorValidation"}});
		Product.Serialize(Ar);
		if (Ar.IsError()) return std::unexpected(Ar.GetFailure() ? *Ar.GetFailure() : FArchiveFailure{});
		return Bytes;
	}
}
#endif
