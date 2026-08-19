#include "Resources/FullscreenGeometryResources.h"

#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	auto FFullscreenGeometryResources::EnsureResources_RenderThread(
		FRHICommandListImmediate& CommandList) -> bool
	{
		check(IsInRenderingThread());
		using FResult = TRenderResourceCreateResult<FPayload>;
		return Slot.Resolve(
			Generation,
			[&CommandList]() -> FResult {
				FVertexDeclarationElementList Elements;
				constexpr uint32 Stride = sizeof(FVertex);
				Elements[0] = FVertexElement(
					0, offsetof(FVertex, Position),
					EVertexElementType::Float2, 0, Stride);
				Elements[1] = FVertexElement(
					0, offsetof(FVertex, UV),
					EVertexElementType::Float2, 1, Stride);
				const std::array<FVertex, 3> Vertices = {
					FVertex{FVector2f{-1.0f, -1.0f}, FVector2f{0.0f, 0.0f}},
					FVertex{FVector2f{3.0f, -1.0f}, FVector2f{2.0f, 0.0f}},
					FVertex{FVector2f{-1.0f, 3.0f}, FVector2f{0.0f, 2.0f}},
				};
				const std::array<uint32, 3> Indices = {0, 1, 2};
				FPayload Candidate;
				Candidate.VertexDeclaration =
					GDynamicRHI->RHICreateVertexDeclaration(Elements);
				FRHIBufferCreateDesc VertexDesc =
					FRHIBufferCreateDesc::CreateVertex(
						"RendererFullscreenVertexBuffer",
						sizeof(FVertex)
							* static_cast<uint32>(Vertices.size()));
				VertexDesc.Usage |= EBufferUsageFlags::Static;
				VertexDesc.InitialData = {
					Vertices.data(),
					static_cast<uint32>(
						sizeof(FVertex) * Vertices.size())};
				Candidate.VertexBuffer =
					GDynamicRHI->RHICreateBuffer(CommandList, VertexDesc);
				FRHIBufferCreateDesc IndexDesc =
					FRHIBufferCreateDesc::CreateIndex(
						"RendererFullscreenIndexBuffer",
						sizeof(uint32)
							* static_cast<uint32>(Indices.size()),
						sizeof(uint32));
				IndexDesc.Usage |= EBufferUsageFlags::Static;
				IndexDesc.InitialData = {
					Indices.data(),
					static_cast<uint32>(
						sizeof(uint32) * Indices.size())};
				Candidate.IndexBuffer =
					GDynamicRHI->RHICreateBuffer(CommandList, IndexDesc);
				if (Candidate.VertexDeclaration == nullptr
					|| Candidate.VertexBuffer == nullptr
					|| Candidate.IndexBuffer == nullptr)
				{
					return FResult::Failure({
						.Category =
							ERenderResourceCreateErrorCategory::RHIResource,
						.Context = "FullscreenGeometryResources",
						.Identity = "shared-triangle",
						.Message =
							"RHI declaration or buffer creation returned null.",
						.RetryDependencies =
							ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual,
					});
				}
				return FResult::Success(std::move(Candidate));
			},
			[](const FRenderResourceCreateDiagnostic& Diagnostic) {
				if (!Diagnostic.Error)
				{
					return;
				}
				if (Diagnostic.Kind
					== ERenderResourceCreateDiagnosticKind::Recovery)
				{
					DURIN_INFO("Recovered shared fullscreen geometry.");
					return;
				}
				DURIN_ERROR(
					"Shared fullscreen geometry creation failed: {}",
					Diagnostic.Error->Message);
			}) != nullptr;
	}

	auto FFullscreenGeometryResources::GetVertexDeclaration_RenderThread() const
		-> const FVertexDeclarationRHIRef&
	{
		check(IsInRenderingThread());
		static const FVertexDeclarationRHIRef NullDeclaration;
		const FPayload* Payload = Slot.GetPayload();
		return Payload != nullptr
			? Payload->VertexDeclaration
			: NullDeclaration;
	}

	auto FFullscreenGeometryResources::GetVertexBuffer_RenderThread() const
		-> const FBufferRHIRef&
	{
		check(IsInRenderingThread());
		static const FBufferRHIRef NullBuffer;
		const FPayload* Payload = Slot.GetPayload();
		return Payload != nullptr ? Payload->VertexBuffer : NullBuffer;
	}

	auto FFullscreenGeometryResources::GetIndexBuffer_RenderThread() const
		-> const FBufferRHIRef&
	{
		check(IsInRenderingThread());
		static const FBufferRHIRef NullBuffer;
		const FPayload* Payload = Slot.GetPayload();
		return Payload != nullptr ? Payload->IndexBuffer : NullBuffer;
	}

	auto FFullscreenGeometryResources::RetryFailedResources_RenderThread()
		-> void
	{
		check(IsInRenderingThread());
		Generation.Advance(
			ERenderResourceGenerationDependency::Manual);
	}

	auto FFullscreenGeometryResources::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		Slot.Reset();
		Generation = {};
	}

} // namespace Durin
