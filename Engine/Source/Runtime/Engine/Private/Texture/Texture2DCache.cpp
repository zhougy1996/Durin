#include "Texture/Texture2DCache.h"

#if DURIN_WITH_EDITOR
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
		std::vector<DerivedData::FValueId> Values;
		for (uint32 Index : Indices) Values.push_back(DerivedData::FValueId::FromName("Durin.Texture2D.Mip").MakeIndexed(Index));
		auto Cached = DerivedData::GetCache().Get({.Key = *Request.Key.AsCacheKey(),
			.MaximumEncodedBytes = Request.MaximumEncodedBytes, .MaximumWorkingSetBytes = Request.MaximumWorkingSetBytes,
			.Mode = DerivedData::ECacheGetMode::SelectedValues, .RequiredValues = Values, .ShouldCancel = Request.ShouldCancel});
		if (!Cached)
		{
			auto Code = ETexture2DCacheError::CacheFailure;
			if (Cached.error().Code == DerivedData::ECacheError::MissingValue) Code = ETexture2DCacheError::MissingValue;
			else if (Cached.error().Code == DerivedData::ECacheError::Canceled) Code = ETexture2DCacheError::Canceled;
			return std::unexpected(FTexture2DCacheError{Code, std::move(Cached.error().Diagnostic)});
		}
		if (!*Cached) return std::optional<FTexture2DCachedMips>{};
		const auto& Record = **Cached;
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
