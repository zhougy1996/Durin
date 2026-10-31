#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "RHIPipelineCreation.h"
#include "RHIResources.h"
#include "VertexFactory.h"
#include "Rendering/PrimitiveRenderParticipation.h"

namespace Durin
{
	struct FPSOPrecacheParams
	{
		FPrimitiveRenderParticipation Participation;
		FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology = FGraphicsPipelineStateInitializer::EPrimitiveTopology::TriangleList;
		bool bWireframe = false, bOrientationChanging = false, bReversedZ = false;
		auto operator==(const FPSOPrecacheParams&) const -> bool = default;
	};


	// Submission description, separate from the asynchronous pipeline observation.
	// The collector keeps referenced shaders/declarations alive until admission.
	struct FPSOPrecacheData
	{
		enum class EType : uint8 { Graphics, Compute };
		EType Type = EType::Graphics;
		FGraphicsPipelineStateInitializer GraphicsInitializer;
		FComputePipelineStateInitializer ComputeInitializer;
		bool bRequired = true;
		EPSOPrecachePriority Priority = EPSOPrecachePriority::Medium;

		FPSOPrecacheData() = default;
		explicit FPSOPrecacheData(FGraphicsPipelineStateInitializer Initializer,
			bool bInRequired = true, EPSOPrecachePriority InPriority = EPSOPrecachePriority::Medium)
			: Type(EType::Graphics), GraphicsInitializer(std::move(Initializer)), bRequired(bInRequired), Priority(InPriority) {}
		explicit FPSOPrecacheData(FComputePipelineStateInitializer Initializer,
			bool bInRequired = true, EPSOPrecachePriority InPriority = EPSOPrecachePriority::Medium)
			: Type(EType::Compute), ComputeInitializer(std::move(Initializer)), bRequired(bInRequired), Priority(InPriority) {}
		[[nodiscard]] auto IsValid() const -> bool
		{
			if (Priority >= EPSOPrecachePriority::Num) return false;
			switch (Type)
			{
			case EType::Graphics: return GraphicsInitializer.IsValid();
			case EType::Compute: return ComputeInitializer.IsValid();
			}
			return false;
		}
	};
	using FPSOPrecacheDataArray = std::vector<FPSOPrecacheData>;

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

		ENGINE_API auto GetVertexElements(EVertexInputStreamType StreamType = EVertexInputStreamType::Default) const
			-> std::expected<FVertexDeclarationElementList, EPSOPrecacheVertexFactoryError>;
		ENGINE_API auto MatchesVertexElements(const FVertexDeclarationElementList& Elements,
			EVertexInputStreamType StreamType = EVertexInputStreamType::Default) const -> bool;
		auto operator==(const FPSOPrecacheVertexFactoryData&) const -> bool = default;
	};
}
