#include "VertexFactory.h"
#include "PSOPrecache.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		auto GetVertexFactoryTypes()
			-> std::vector<const FVertexFactoryType*>&
		{
			static std::vector<const FVertexFactoryType*> Types;
			return Types;
		}
	}

	FVertexFactoryType::FVertexFactoryType(std::string_view InName)
		: FVertexFactoryType(InName, EVertexFactoryFlags::None) {}

	FVertexFactoryType::FVertexFactoryType(std::string_view InName, EVertexFactoryFlags InFlags,
		FGetPSOPrecacheVertexFetchElements InGetPSOPrecacheVertexFetchElements)
		: Name(InName), StableKey(FXxHash64::HashBuffer(InName)), Flags(InFlags),
		GetPSOPrecacheVertexFetchElementsFunction(InGetPSOPrecacheVertexFetchElements)
	{
		checkf(!Name.empty(), "Vertex Factory type name must not be empty");
		auto& Types = GetVertexFactoryTypes();
		checkf(std::ranges::none_of(Types, [this](const FVertexFactoryType* Type) {
			return Type->GetName() == Name;
		}), "Duplicate Vertex Factory type: {}", Name);
		Types.push_back(this);
	}

	auto FVertexFactoryType::GetPSOPrecacheVertexFetchElements(EVertexInputStreamType StreamType,
		FVertexDeclarationElementList& Elements) const -> void
	{
		Elements = {};
		if (SupportsPSOPrecaching() && StreamType < EVertexInputStreamType::Count && GetPSOPrecacheVertexFetchElementsFunction)
			GetPSOPrecacheVertexFetchElementsFunction(StreamType, Elements);
	}

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

	auto FVertexFactoryType::GetTypeList()
		-> const std::vector<const FVertexFactoryType*>&
	{
		return GetVertexFactoryTypes();
	}

	FVertexFactory::FVertexFactory() = default;
	FVertexFactory::~FVertexFactory() = default;

	auto FVertexFactory::InitRHI(FRHICommandListBase&) -> void
	{
		check(IsInRenderingThread());
		if (VertexDeclarationRHI != nullptr || GDynamicRHI == nullptr) return;
		const bool bHasElements = std::ranges::any_of(
			DeclarationElements,
			[](const FVertexElement& Element) {
				return Element.Type != EVertexElementType::None;
			});
		if (!bHasElements || Streams.empty()) return;
		VertexDeclarationRHI =
			GDynamicRHI->RHICreateVertexDeclaration(DeclarationElements);
	}

	auto FVertexFactory::ReleaseRHI() -> void
	{
		check(IsInRenderingThread());
		VertexDeclarationRHI = nullptr;
		Streams.clear();
	}

	auto FVertexFactory::BindStreams(
		FRHICommandListImmediate& CommandList) const -> void
	{
		check(IsInRenderingThread());
		check(IsReady());
		for (const FVertexInputStream& Stream : Streams)
		{
			CommandList.BindVertexBuffer(
				Stream.StreamIndex, Stream.VertexBuffer, Stream.Offset);
		}
	}
}
