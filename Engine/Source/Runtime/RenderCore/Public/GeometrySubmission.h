#pragma once

#include "RHIResources.h"

namespace Durin
{
	// Collection and resolution preserve the distinction between policy and failure.
	enum class EGeometrySubmissionOutcome : uint8
	{
		Submitted,
		Empty,
		Excluded,
		Unsupported,
		InvalidSubmission,
		ResourceFailure,
	};

	// Describes an element stream within a buffer; offsets and sizes are bytes.
	// Validation uses division/subtraction so adversarial counts cannot wrap.
	struct FGeometryStreamRange
	{
		uint64 BufferBytes = 0;
		uint64 ByteOffset = 0;
		uint32 Stride = 0;
		uint32 ElementBytes = 0;

		auto Contains(uint64 First, uint64 Count) const -> bool
		{
			if (Stride == 0 || ElementBytes == 0 || ElementBytes > Stride
				|| ByteOffset > BufferBytes) return false;
			const uint64 Available = BufferBytes - ByteOffset;
			const uint64 Capacity = Available < ElementBytes
				? 0 : 1 + (Available - ElementBytes) / Stride;
			return First <= Capacity && Count <= Capacity - First;
		}
	};

	// Retains the resource through execution; generation identifies replacement,
	// while content revision identifies updates that need not create a new PSO.
	struct FGeometryBufferView
	{
		FBufferRHIRef Buffer;
		FGeometryStreamRange Range;
		uint64 ResourceGeneration = 0;
		uint64 ContentRevision = 0;

		auto IsValid() const -> bool
		{
			return Buffer && Range.BufferBytes <= Buffer->GetSize()
				&& Range.Contains(0, 0);
		}
	};

	using EGeometryTopology = FGraphicsPipelineStateInitializer::EPrimitiveTopology;

	// Direct draw validation is independent of assets, materials and backend state.
	// Indexed min/max values must describe the selected index range, before adding
	// the signed base vertex. Providers compute them when publishing geometry.
	struct FGeometryDrawRange
	{
		EGeometryTopology Topology = EGeometryTopology::TriangleList;
		bool bIndexed = true;
		uint32 ElementCount = 0;
		uint32 FirstElement = 0;
		int32 VertexOffset = 0;
		uint32 InstanceCount = 1;
		uint32 FirstInstance = 0;
		uint32 MinVertexIndex = 0;
		uint32 MaxVertexIndex = 0;

		auto Validate(
			const FGeometryStreamRange& Vertices,
			const FGeometryStreamRange& Indices,
			std::span<const FGeometryStreamRange> InstanceStreams = {}) const
			-> EGeometrySubmissionOutcome
		{
			if (Topology != EGeometryTopology::TriangleList
				&& Topology != EGeometryTopology::LineList)
				return EGeometrySubmissionOutcome::Unsupported;
			if (ElementCount == 0 || InstanceCount == 0)
				return EGeometrySubmissionOutcome::Empty;
			const uint32 PrimitiveElements = Topology == EGeometryTopology::TriangleList ? 3 : 2;
			if (ElementCount % PrimitiveElements != 0)
				return EGeometrySubmissionOutcome::InvalidSubmission;
			if (bIndexed)
			{
				if ((Indices.ElementBytes != 2 && Indices.ElementBytes != 4)
					|| Indices.Stride != Indices.ElementBytes
					|| Indices.ByteOffset % Indices.ElementBytes != 0
					|| !Indices.Contains(FirstElement, ElementCount)
					|| MinVertexIndex > MaxVertexIndex
					|| (Indices.ElementBytes == 2 && MaxVertexIndex > 65535))
					return EGeometrySubmissionOutcome::InvalidSubmission;
				const int64 FirstVertex = static_cast<int64>(MinVertexIndex) + VertexOffset;
				const int64 LastVertex = static_cast<int64>(MaxVertexIndex) + VertexOffset;
				if (FirstVertex < 0 || !Vertices.Contains(
					static_cast<uint64>(FirstVertex),
					static_cast<uint64>(LastVertex - FirstVertex) + 1))
					return EGeometrySubmissionOutcome::InvalidSubmission;
			}
			else if (VertexOffset != 0 || !Vertices.Contains(FirstElement, ElementCount))
				return EGeometrySubmissionOutcome::InvalidSubmission;
			for (const auto& Instances : InstanceStreams)
			{
				if (!Instances.Contains(FirstInstance, InstanceCount))
					return EGeometrySubmissionOutcome::InvalidSubmission;
			}
			return EGeometrySubmissionOutcome::Submitted;
		}

		auto GetDrawArguments() const -> FRHIDrawArguments
		{
			return {ElementCount, InstanceCount, FirstElement, FirstInstance};
		}
		auto GetIndexedDrawArguments() const -> FRHIDrawIndexedArguments
		{
			return {ElementCount, InstanceCount, FirstElement, VertexOffset, FirstInstance};
		}
	};
}
