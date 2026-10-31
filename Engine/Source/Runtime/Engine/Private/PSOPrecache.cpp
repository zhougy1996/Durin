#include "PSOPrecache.h"

namespace Durin
{
	namespace
	{
		auto CanonicalizePrecacheElements(const FVertexDeclarationElementList& Elements)
			-> std::expected<FVertexDeclarationElementList, EPSOPrecacheVertexFactoryError>
		{
			if (!ValidateVertexDeclarationElements(Elements))
				return std::unexpected(EPSOPrecacheVertexFactoryError::InvalidVertexElements);
			FVertexDeclarationElementList Result{};
			bool End = false;
			for (size_t Index = 0; Index < Elements.size(); ++Index)
			{
				if (Elements[Index].Type == EVertexElementType::None) { End = true; continue; }
				// Reject holes rather than silently dropping live attributes after None.
				if (End) return std::unexpected(EPSOPrecacheVertexFactoryError::InvalidVertexElements);
				Result[Index] = Elements[Index];
			}
			return Result;
		}
	}

	auto FPSOPrecacheVertexFactoryData::GetVertexElements(EVertexInputStreamType StreamType) const
		-> std::expected<FVertexDeclarationElementList, EPSOPrecacheVertexFactoryError>
	{
		if (!VertexFactoryType || !VertexFactoryType->SupportsPSOPrecaching())
			return std::unexpected(EPSOPrecacheVertexFactoryError::UnsupportedVertexFactory);
		if (StreamType >= EVertexInputStreamType::Count
			|| (StreamType == EVertexInputStreamType::PositionOnly
				&& !EnumHasAnyFlags(VertexFactoryType->GetFlags(), EVertexFactoryFlags::SupportsPositionOnly)))
			return std::unexpected(EPSOPrecacheVertexFactoryError::UnsupportedStream);
		FVertexDeclarationElementList Elements{};
		const bool Custom = StreamType == EVertexInputStreamType::Default && CustomDefaultVertexElements.has_value();
		if (Custom) Elements = *CustomDefaultVertexElements;
		else
		{
			if (!VertexFactoryType->HasPSOPrecacheVertexFetchElements())
				return std::unexpected(EPSOPrecacheVertexFactoryError::MissingVertexElements);
			VertexFactoryType->GetPSOPrecacheVertexFetchElements(StreamType, Elements);
		}
		if (Elements[0].Type == EVertexElementType::None
			&& !(StreamType == EVertexInputStreamType::Default
				&& EnumHasAnyFlags(VertexFactoryType->GetFlags(), EVertexFactoryFlags::SupportsManualVertexFetch)))
			return std::unexpected(Custom ? EPSOPrecacheVertexFactoryError::InvalidVertexElements
				: EPSOPrecacheVertexFactoryError::UnsupportedStream);
		return CanonicalizePrecacheElements(Elements);
	}

	auto FPSOPrecacheVertexFactoryData::MatchesVertexElements(const FVertexDeclarationElementList& Elements,
		EVertexInputStreamType StreamType) const -> bool
	{
		const auto Expected = GetVertexElements(StreamType);
		const auto Actual = CanonicalizePrecacheElements(Elements);
		return Expected && Actual && *Expected == *Actual;
	}

}
