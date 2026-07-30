#include "VertexFactory.h"

#include "DynamicRHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
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
