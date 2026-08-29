#include "VertexFactory.h"

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
		: Name(InName), StableKey(FXxHash64::HashBuffer(InName))
	{
		checkf(!Name.empty(), "Vertex Factory type name must not be empty");
		auto& Types = GetVertexFactoryTypes();
		checkf(std::ranges::none_of(Types, [this](const FVertexFactoryType* Type) {
			return Type->GetName() == Name;
		}), "Duplicate Vertex Factory type: {}", Name);
		Types.push_back(this);
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
