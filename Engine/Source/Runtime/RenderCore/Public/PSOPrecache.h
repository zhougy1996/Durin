#pragma once

#include "CoreMinimal.h"
#include "VertexFactory.h"

namespace Durin
{
	enum class EPSOPrecacheVertexFactoryError : uint8
	{
		UnsupportedVertexFactory,
		UnsupportedStream,
		MissingVertexElements,
		InvalidVertexElements,
	};

	// CPU-only snapshot. Custom elements override Default; reduced streams always
	// come from the type, matching the UE precache contract. Type registrations
	// must outlive these values. No native declaration or vertex buffer is retained.
	struct FPSOPrecacheVertexFactoryData
	{
		const FVertexFactoryType* VertexFactoryType = nullptr;
		std::optional<FVertexDeclarationElementList> CustomDefaultVertexElements;

		FPSOPrecacheVertexFactoryData() = default;
		explicit FPSOPrecacheVertexFactoryData(const FVertexFactoryType* Type) : VertexFactoryType(Type) {}
		FPSOPrecacheVertexFactoryData(const FVertexFactoryType* Type, const FVertexDeclarationElementList& Elements)
			: VertexFactoryType(Type), CustomDefaultVertexElements(Elements) {}

		RENDERCORE_API auto GetVertexElements(EVertexInputStreamType StreamType = EVertexInputStreamType::Default) const
			-> std::expected<FVertexDeclarationElementList, EPSOPrecacheVertexFactoryError>;
		RENDERCORE_API auto MatchesVertexElements(const FVertexDeclarationElementList& Elements,
			EVertexInputStreamType StreamType = EVertexInputStreamType::Default) const -> bool;
		auto operator==(const FPSOPrecacheVertexFactoryData&) const -> bool = default;
	};
}
