#pragma once

#include "CoreMinimal.h"
#include "RenderResource.h"

namespace Durin
{
	// An actual source-owned render resource, not a binding identity token.
	class FUniformBuffer final : public FRenderResource
	{
	public:
		RENDERCORE_API static auto Create() -> std::shared_ptr<FUniformBuffer>;
		RENDERCORE_API auto Update(FRHICommandListBase& Commands, FByteView Data,
			std::span<FRHIResource* const> Resources = {}, std::span<const FRHIUniformBufferResource> Members = {})
			-> bool;
		auto GetRange() const -> FRHIUniformBufferRange
		{ return {Buffer.GetReference(), 0, static_cast<uint32>(Data.size()), {}}; }
		auto GetData() const -> FByteView { return Data; }
		auto GetRHI() const -> FRHIUniformBuffer* { return Buffer.GetReference(); }
		auto ReleaseRHI() -> void override { Buffer = nullptr; Data.clear(); }
		auto GetFriendlyName() const -> std::string override { return "UniformBuffer"; }
		RENDERCORE_API static auto ReleaseRHIForAllResources() -> void;
	private:
		TRefCountPtr<FRHIUniformBuffer> Buffer;
		FByteBuffer Data;
	};
}
