#pragma once

#include "RenderCoreAPI.h"

#include "Hash/XxHash.h"
#include "RHI.h"
#include "RenderResource.h"

namespace Durin
{
	class FRHICommandListImmediate;

	class FVertexFactoryType
	{
	public:
		RENDERCORE_API explicit FVertexFactoryType(std::string_view InName);
		auto GetName() const -> std::string_view { return Name; }
		auto GetStableKey() const -> FXxHash64 { return StableKey; }
		RENDERCORE_API static auto GetTypeList()
			-> const std::vector<const FVertexFactoryType*>&;

	private:
		std::string Name;
		FXxHash64 StableKey;
	};

	struct FVertexStreamComponent
	{
		const FVertexBuffer* VertexBuffer = nullptr;
		uint32 Offset = 0;
		uint16 Stride = 0;
		EVertexElementType Type = EVertexElementType::None;
		uint8 AttributeIndex = 0;
		uint8 StreamIndex = 0;

		auto IsValid() const -> bool
		{
			return VertexBuffer != nullptr && Stride > 0
				&& Offset <= std::numeric_limits<uint8>::max()
				&& Type != EVertexElementType::None;
		}
		auto ToVertexElement() const -> FVertexElement
		{
			check(IsValid());
			return FVertexElement(
				StreamIndex, static_cast<uint8>(Offset), Type,
				AttributeIndex, Stride);
		}
	};

	struct FVertexInputStream
	{
		uint8 StreamIndex = 0;
		FBufferRHIRef VertexBuffer;
		uint32 Offset = 0;
		uint16 Stride = 0;
	};

	// Owns vertex declaration lifetime and draw-facing stream bindings.
	class FVertexFactory : public FRenderResource
	{
	public:
		RENDERCORE_API FVertexFactory();
		RENDERCORE_API ~FVertexFactory() override;

		RENDERCORE_API auto InitRHI(
			FRHICommandListBase& RHICmdList) -> void override;
		RENDERCORE_API auto ReleaseRHI() -> void override;
		auto GetFriendlyName() const -> std::string override
		{
			return "FVertexFactory";
		}
		virtual auto GetTypeName() const -> std::string_view
		{
			return "FVertexFactory";
		}
		auto GetDeclaration() const -> const FVertexDeclarationRHIRef&
		{
			return VertexDeclarationRHI;
		}
		auto GetStreams() const -> const std::vector<FVertexInputStream>&
		{
			return Streams;
		}
		auto IsReady() const -> bool
		{
			return VertexDeclarationRHI != nullptr && !Streams.empty()
				&& std::ranges::all_of(
					Streams,
					[](const FVertexInputStream& Stream) {
						return Stream.VertexBuffer != nullptr
							&& Stream.Stride > 0;
					});
		}
		RENDERCORE_API auto BindStreams(
			FRHICommandListImmediate& CommandList) const -> void;

	protected:
		auto SetDeclarationElements(
			FVertexDeclarationElementList InElements) -> void
		{
			DeclarationElements = std::move(InElements);
		}
		auto SetStreams(std::vector<FVertexInputStream> InStreams) -> void
		{
			Streams = std::move(InStreams);
		}

	private:
		FVertexDeclarationElementList DeclarationElements{};
		FVertexDeclarationRHIRef VertexDeclarationRHI;
		std::vector<FVertexInputStream> Streams;
	};
}
