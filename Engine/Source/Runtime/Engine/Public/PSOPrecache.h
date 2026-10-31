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
	// Descriptions retain their native inputs independently of collector reuse.
	struct FPSOPrecacheData
	{
		enum class EType : uint8 { Graphics, Compute };
		int32 PSOCollectorIndex = -1;
		const FVertexFactoryType* VertexFactoryType = nullptr;
		bool bRequired = true;
		EPSOPrecachePriority Priority = EPSOPrecachePriority::Medium;

		FPSOPrecacheData() = default;
		explicit FPSOPrecacheData(FGraphicsPipelineStateInitializer Initializer,
			bool bInRequired = true, EPSOPrecachePriority InPriority = EPSOPrecachePriority::Medium)
			: bRequired(bInRequired), Priority(InPriority) { SetGraphicsInitializer(std::move(Initializer)); }
		explicit FPSOPrecacheData(FComputePipelineStateInitializer Initializer,
			bool bInRequired = true, EPSOPrecachePriority InPriority = EPSOPrecachePriority::Medium)
			: bRequired(bInRequired), Priority(InPriority) { SetComputeInitializer(std::move(Initializer)); }
		auto GetType() const -> EType { return Type; }
		auto GetGraphicsInitializer() const -> const FGraphicsPipelineStateInitializer& { return GraphicsInitializer; }
		auto GetComputeInitializer() const -> const FComputePipelineStateInitializer& { return ComputeInitializer; }
		// Capture every new input before replacing any previous owner, including
		// replacement from an initializer borrowed from this description itself.
		auto SetGraphicsInitializer(FGraphicsPipelineStateInitializer Initializer) -> void
		{
			FShaderRHIRef Vertex = Initializer.BoundShaders.VertexShader, Fragment = Initializer.BoundShaders.FragmentShader;
			FVertexDeclarationRHIRef VertexDeclaration = Initializer.VertexDeclaration;
			GraphicsInitializer = std::move(Initializer); ComputeInitializer = {};
			VertexShader = std::move(Vertex); FragmentShader = std::move(Fragment); Declaration = std::move(VertexDeclaration);
			ComputeShader = nullptr; Type = EType::Graphics;
		}
		auto SetComputeInitializer(FComputePipelineStateInitializer Initializer) -> void
		{
			FShaderRHIRef Shader = Initializer.ComputeShader;
			ComputeInitializer = std::move(Initializer); GraphicsInitializer = {};
			ComputeShader = std::move(Shader);
			VertexShader = nullptr; FragmentShader = nullptr; Declaration = nullptr; Type = EType::Compute;
		}
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
	private:
		EType Type = EType::Graphics;
		FGraphicsPipelineStateInitializer GraphicsInitializer;
		FComputePipelineStateInitializer ComputeInitializer;
		FShaderRHIRef VertexShader, FragmentShader, ComputeShader;
		FVertexDeclarationRHIRef Declaration;
	};
	using FPSOPrecacheDataArray = std::vector<FPSOPrecacheData>;

	// Optional full-identity validation. Records own value keys, never native resources.
	namespace PSOPrecacheValidation
	{
		enum class EResult : uint8 { Untracked, Missed, TooLate, Hit, Failed };
		struct FStatistics
		{
			uint64 Untracked = 0, Missed = 0, TooLate = 0, Hit = 0, Failed = 0;
			uint64 Identities = 0, RetainedAttempts = 0;
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
		ENGINE_API auto Observe(const std::shared_ptr<FRecord>& Record,
			std::shared_ptr<const std::atomic<ERHIPipelineRequestState>> Outcome) -> void;
		ENGINE_API auto Withdraw(const std::shared_ptr<FRecord>& Record) -> void;
		ENGINE_API auto Check(const FGraphicsPipelineStateInitializer& Initializer, int32 CollectorIndex,
			const FVertexFactoryType* VertexFactoryType) -> FCheckResult;
		ENGINE_API auto Check(const FComputePipelineStateInitializer& Initializer, int32 CollectorIndex,
			const FVertexFactoryType* VertexFactoryType = nullptr) -> FCheckResult;
		// Record actual draw/dispatch use once per full identity in a capture.
		ENGINE_API auto RecordUse(const FGraphicsPipelineStateInitializer& Initializer, int32 CollectorIndex,
			const FVertexFactoryType* VertexFactoryType) -> FCheckResult;
		ENGINE_API auto RecordUse(const FComputePipelineStateInitializer& Initializer, int32 CollectorIndex,
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
