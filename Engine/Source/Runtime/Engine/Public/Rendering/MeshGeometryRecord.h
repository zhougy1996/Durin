#pragma once

#include "EngineAPI.h"
#include "GeometrySubmission.h"
#include "VertexFactory.h"
#include <expected>

namespace Durin
{
	struct FMeshGeometryElement
	{
		uint64 ElementId = 0;
		FGeometryDrawRange Draw;
		FGeometryBufferView Vertices;
		FGeometryBufferView Indices;
		std::vector<FGeometryBufferView> InstanceStreams;
		FBox LocalBounds;
		uint32 MaterialSlotDiagnostic = 0;
	};

	// Shared by dynamic admission and immutable publication; neither path trusts
	// a producer-supplied validation flag.
	ENGINE_API auto ValidateMeshGeometryElement(const FGeometryDrawRange& Draw,
		const FGeometryBufferView& Vertices, const FGeometryBufferView& Indices,
		std::span<const FGeometryBufferView> InstanceStreams) -> EGeometrySubmissionOutcome;
	ENGINE_API auto ValidateMeshGeometryInputs(const FVertexFactoryInputBinding& Binding,
		const FGeometryDrawRange& Draw, const FGeometryBufferView& Vertices,
		std::span<const FGeometryBufferView> InstanceStreams) -> EGeometrySubmissionOutcome;

	// Explicit persistent-provider capability. Publication takes exclusive input
	// ownership; producers must not retain mutable aliases to the consumed data.
	class FMeshGeometryRecord final
	{
	public:
		using FRef = std::shared_ptr<const FMeshGeometryRecord>;
		using FResult = std::expected<FRef, EGeometrySubmissionOutcome>;
		ENGINE_API static auto Publish(std::unique_ptr<FVertexFactoryInputBinding> Binding,
			std::vector<FMeshGeometryElement> Elements) -> FResult;
		// Shares checked draw data, validating only the replacement factory inputs.
		ENGINE_API auto WithBinding(std::unique_ptr<FVertexFactoryInputBinding> Binding) const -> FResult;
		auto GetRecordId() const -> uint64 { return RecordId; }
		auto GetGeometryId() const -> uint64 { return GeometryId; }
		auto GetBinding() const -> const std::shared_ptr<const FVertexFactoryInputBinding>& { return Binding; }
		auto GetElements() const -> std::span<const FMeshGeometryElement> { return *Elements; }

	private:
		FMeshGeometryRecord() = default;
		uint64 RecordId = 0;
		uint64 GeometryId = 0;
		std::shared_ptr<const FVertexFactoryInputBinding> Binding;
		std::shared_ptr<const std::vector<FMeshGeometryElement>> Elements;
	};
}
