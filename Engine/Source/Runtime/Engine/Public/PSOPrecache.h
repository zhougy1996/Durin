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
		int32 PSOCollectorIndex = -1;
		const FVertexFactoryType* VertexFactoryType = nullptr;
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

	// Optional full-identity validation. Records own value keys, never native resources.
	namespace PSOPrecacheValidation
	{
		enum class EResult : uint8 { Untracked, Missed, TooLate, Hit, Failed };
		struct FStatistics
		{
			uint64 Untracked = 0, Missed = 0, TooLate = 0, Hit = 0, Failed = 0;
		};
		struct FRecord;
		struct FCheckResult
		{
			EResult Result = EResult::Untracked;
			int32 PSOCollectorIndex = -1;
			const FVertexFactoryType* VertexFactoryType = nullptr;
		};
		// Enabling starts a new capture. Clear at shader/device invalidation and shutdown.
		ENGINE_API auto SetEnabled(bool bEnabled) -> void;
		ENGINE_API auto IsEnabled() -> bool;
		ENGINE_API auto Clear() -> void;
		ENGINE_API auto Track(const FPSOPrecacheData& Data) -> std::shared_ptr<FRecord>;
		ENGINE_API auto Update(const std::shared_ptr<FRecord>& Record, ERHIPipelineRequestState State) -> void;
		ENGINE_API auto Check(const FGraphicsPipelineStateInitializer& Initializer, int32 CollectorIndex,
			const FVertexFactoryType* VertexFactoryType) -> FCheckResult;
		ENGINE_API auto Check(const FComputePipelineStateInitializer& Initializer, int32 CollectorIndex,
			const FVertexFactoryType* VertexFactoryType = nullptr) -> FCheckResult;
		ENGINE_API auto GetStatistics() -> FStatistics;
	}

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
