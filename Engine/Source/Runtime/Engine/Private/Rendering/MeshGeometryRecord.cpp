#include "Rendering/MeshGeometryRecord.h"
#include <atomic>
#include <unordered_set>

namespace Durin
{
	namespace
	{
		auto NextGeometryRecordId() -> uint64
		{
			static std::atomic<uint64> Next{1};
			const uint64 Id = Next.fetch_add(1, std::memory_order_relaxed);
			requiref(Id != 0, "Mesh geometry record identity exhausted.");
			return Id;
		}
	}

	auto ValidateMeshGeometryElement(const FGeometryDrawRange& Draw,
		const FGeometryBufferView& Vertices, const FGeometryBufferView& Indices,
		std::span<const FGeometryBufferView> InstanceStreams) -> EGeometrySubmissionOutcome
	{
		const auto Outcome = Draw.Validate(Vertices.Range, Indices.Range);
		if (Outcome != EGeometrySubmissionOutcome::Submitted) return Outcome;
		if (Draw.bIndexed && Indices.Range.ByteOffset > std::numeric_limits<uint32>::max())
			return EGeometrySubmissionOutcome::Unsupported;
		if (!Vertices.IsValid() || (Draw.bIndexed && !Indices.IsValid()))
			return EGeometrySubmissionOutcome::ResourceFailure;
		if (Draw.bIndexed && Indices.Buffer->GetStride() != Indices.Range.ElementBytes)
			return EGeometrySubmissionOutcome::InvalidSubmission;
		if (!EnumHasAnyFlags(Vertices.Buffer->GetUsage(), EBufferUsageFlags::VertexBuffer)
			|| (Draw.bIndexed && !EnumHasAnyFlags(Indices.Buffer->GetUsage(), EBufferUsageFlags::IndexBuffer)))
			return EGeometrySubmissionOutcome::InvalidSubmission;
		for (const auto& Stream : InstanceStreams)
		{
			if (!Stream.Range.Contains(Draw.FirstInstance, Draw.InstanceCount))
				return EGeometrySubmissionOutcome::InvalidSubmission;
			if (!Stream.IsValid()) return EGeometrySubmissionOutcome::ResourceFailure;
			if (!EnumHasAnyFlags(Stream.Buffer->GetUsage(), EBufferUsageFlags::VertexBuffer))
				return EGeometrySubmissionOutcome::InvalidSubmission;
		}
		return EGeometrySubmissionOutcome::Submitted;
	}

	auto ValidateMeshGeometryInputs(const FVertexFactoryInputBinding& Binding,
		const FGeometryDrawRange& Draw, const FGeometryBufferView& Vertices,
		std::span<const FGeometryBufferView> InstanceStreams) -> EGeometrySubmissionOutcome
	{
		const bool bResourceViewsMatch = std::ranges::any_of(Binding.Streams, [&](const auto& Stream) {
			return Stream.VertexBuffer == Vertices.Buffer && Stream.Offset == Vertices.Range.ByteOffset
				&& Stream.Stride == Vertices.Range.Stride;
		}) && std::ranges::all_of(InstanceStreams, [&](const auto& Instance) {
			return std::ranges::any_of(Binding.Streams, [&](const auto& Stream) {
				return Stream.VertexBuffer == Instance.Buffer && Stream.Offset == Instance.Range.ByteOffset
					&& Stream.Stride == Instance.Range.Stride
					&& std::ranges::any_of(Binding.DeclarationElements, [&](const auto& Attribute) {
						return Attribute.Type != EVertexElementType::None && Attribute.StreamIndex == Stream.StreamIndex
							&& Attribute.InputRate == FRHIVertexElementIdentity::EInputRate::Instance;
					});
			});
		});
		return bResourceViewsMatch ? Binding.ValidateInputs(Draw) : EGeometrySubmissionOutcome::InvalidSubmission;
	}

	auto FMeshGeometryRecord::Publish(std::unique_ptr<FVertexFactoryInputBinding> InBinding,
		std::vector<FMeshGeometryElement> InElements) -> FResult
	{
		if (InElements.empty()) return std::unexpected(EGeometrySubmissionOutcome::Empty);
		if (!InBinding || !InBinding->Declaration || InBinding->Streams.empty())
			return std::unexpected(EGeometrySubmissionOutcome::ResourceFailure);
		if (InBinding->GetFactoryKey().IsZero() || InBinding->GetLayoutKey().IsZero())
			return std::unexpected(EGeometrySubmissionOutcome::InvalidSubmission);
		std::unordered_set<uint64> Ids;
		bool bHasDraws = false;
		for (const auto& Element : InElements)
		{
			if (!Ids.insert(Element.ElementId).second)
				return std::unexpected(EGeometrySubmissionOutcome::InvalidSubmission);
			const auto Outcome = ValidateMeshGeometryElement(Element.Draw, Element.Vertices,
				Element.Indices, Element.InstanceStreams);
			if (Outcome == EGeometrySubmissionOutcome::Empty) continue;
			if (Outcome != EGeometrySubmissionOutcome::Submitted) return std::unexpected(Outcome);
			const auto InputOutcome = ValidateMeshGeometryInputs(*InBinding, Element.Draw,
				Element.Vertices, Element.InstanceStreams);
			if (InputOutcome != EGeometrySubmissionOutcome::Submitted) return std::unexpected(InputOutcome);
			bHasDraws = true;
		}
		if (!bHasDraws) return std::unexpected(EGeometrySubmissionOutcome::Empty);
		// Keep the published span draw-only, including for mixed empty/nonempty inputs.
		std::erase_if(InElements, [](const auto& Element) {
			return Element.Draw.ElementCount == 0 || Element.Draw.InstanceCount == 0;
		});
		auto Result = std::shared_ptr<FMeshGeometryRecord>(new FMeshGeometryRecord);
		Result->RecordId = Result->GeometryId = NextGeometryRecordId();
		for (auto& Element : InElements)
		{
			if (!Element.Vertices.ResourceGeneration) Element.Vertices.ResourceGeneration = Result->GeometryId;
			if (!Element.Indices.ResourceGeneration) Element.Indices.ResourceGeneration = Result->GeometryId;
			for (auto& Stream : Element.InstanceStreams)
				if (!Stream.ResourceGeneration) Stream.ResourceGeneration = Result->GeometryId;
		}
		Result->Binding = std::move(InBinding);
		Result->Elements = std::make_shared<const std::vector<FMeshGeometryElement>>(std::move(InElements));
		return Result;
	}

	auto FMeshGeometryRecord::WithBinding(std::unique_ptr<FVertexFactoryInputBinding> InBinding) const -> FResult
	{
		if (!InBinding || !InBinding->Declaration || InBinding->Streams.empty())
			return std::unexpected(EGeometrySubmissionOutcome::ResourceFailure);
		if (InBinding->GetFactoryKey().IsZero() || InBinding->GetLayoutKey().IsZero())
			return std::unexpected(EGeometrySubmissionOutcome::InvalidSubmission);
		for (const auto& Element : *Elements)
		{
			const auto Outcome = ValidateMeshGeometryInputs(*InBinding, Element.Draw, Element.Vertices, Element.InstanceStreams);
			if (Outcome != EGeometrySubmissionOutcome::Submitted) return std::unexpected(Outcome);
		}
		auto Result = std::shared_ptr<FMeshGeometryRecord>(new FMeshGeometryRecord);
		Result->RecordId = NextGeometryRecordId();
		Result->GeometryId = GeometryId;
		Result->Binding = std::move(InBinding);
		Result->Elements = Elements;
		return Result;
	}
}
