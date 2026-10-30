#include "Texture/Texture2DCache.h"

#if DURIN_WITH_EDITOR
	#include "BuildCacheRecordPrivate.h"
	#include "CacheDispatchPrivate.h"
	#include "DerivedDataCache/DerivedDataCache.h"
	#include "Texture2DSharedOutput.h"

namespace Durin
{
	auto ReadTexture2DCachedMips(const FTexture2DCacheRequest& Request)
		-> std::expected<std::optional<FTexture2DCachedMips>, FTexture2DCacheError>
	{
		if (!Request.Key.IsValid() || Request.MipIndices.empty() || Request.MipIndices.size() > MaximumTextureMipCount)
			return std::unexpected(FTexture2DCacheError{ETexture2DCacheError::InvalidRequest, "Texture query requires a key and selected mip indices."});
		std::vector<uint32> Indices(Request.MipIndices.begin(), Request.MipIndices.end());
		std::ranges::sort(Indices);
		if (Indices.back() >= MaximumTextureMipCount || std::adjacent_find(Indices.begin(), Indices.end()) != Indices.end())
			return std::unexpected(FTexture2DCacheError{ETexture2DCacheError::InvalidRequest, "Texture mip indices must be unique and bounded."});
		DerivedData::FCacheRecordPolicyBuilder Policy(DerivedData::ECachePolicy::None);
		for (uint32 Index : Indices)
			Policy.AddValuePolicy(DerivedData::FValueId::FromName("Durin.Texture2D.Mip").MakeIndexed(Index), DerivedData::ECachePolicy::Default);
		auto Cached = DerivedData::GetCacheBackend().Get({.Key = *Request.Key.AsCacheKey(), .MaximumEncodedBytes = Request.MaximumEncodedBytes, .MaximumWorkingSetBytes = Request.MaximumWorkingSetBytes, .Policy = Policy.Build(), .ShouldCancel = Request.ShouldCancel});
		if (!Cached)
		{
			auto Code = ETexture2DCacheError::CacheFailure;
			if (Cached.error().Code == DerivedData::ECacheError::MissingValue) Code = ETexture2DCacheError::MissingValue;
			else if (Cached.error().Code == DerivedData::ECacheError::Canceled) Code = ETexture2DCacheError::Canceled;
			return std::unexpected(FTexture2DCacheError{Code, std::move(Cached.error().Diagnostic)});
		}
		if (!*Cached) return std::optional<FTexture2DCachedMips>{};
		// A policy controls existing values; the texture contract requires each requested mip.
		for (uint32 Index : Indices)
		{
			const auto* Value = (**Cached).FindValue(DerivedData::FValueId::FromName("Durin.Texture2D.Mip").MakeIndexed(Index));
			if (!Value || !Value->HasData())
				return std::unexpected(FTexture2DCacheError{ETexture2DCacheError::MissingValue, "Selected texture mip is unavailable."});
		}
		auto BuildRecord = DerivedData::Private::ReadBuildCacheRecord(**Cached);
		if (!BuildRecord) return std::unexpected(FTexture2DCacheError{ETexture2DCacheError::InvalidOutput, BuildRecord.error().Diagnostic});
		const auto& Record = *BuildRecord;
		if (std::ranges::any_of(Record.GetMessages(), [](const auto& Message) { return Message.Severity == DerivedData::EBuildMessageSeverity::Error; }))
			return std::unexpected(FTexture2DCacheError{ETexture2DCacheError::InvalidOutput, "Texture cache contains a failed build output."});
		auto Layout = TexturePrivate::ReadTexture2DOutputLayout(Record, Request.TargetPlatform, Request.TargetProfile);
		if (!Layout) return std::unexpected(FTexture2DCacheError{ETexture2DCacheError::InvalidOutput, std::move(Layout.error())});
		FTexture2DCachedMips Result;
		Result.PixelFormat = Layout->Format; Result.OriginalMipCount = Layout->Count;
		for (uint32 Index : Indices)
		{
			if (Index >= Layout->Count || !Layout->Mips[Index].Value->HasData())
				return std::unexpected(FTexture2DCacheError{ETexture2DCacheError::MissingValue, "Selected texture mip is unavailable."});
			const auto& Mip = Layout->Mips[Index];
			Result.Mips.push_back({Index, {.Pixels = Mip.Value->GetData(), .Width = Mip.Width, .Height = Mip.Height, .RowPitch = Mip.RowPitch}});
		}
		return std::optional<FTexture2DCachedMips>{std::move(Result)};
	}
}
#endif
