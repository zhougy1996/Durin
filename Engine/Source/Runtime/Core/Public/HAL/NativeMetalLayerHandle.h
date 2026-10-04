#pragma once

namespace CA
{
	class MetalLayer;
}

namespace Durin
{
	// Borrows a platform-owned Metal layer without exposing SDK headers to consumers.
	// Native producers must supply a live CA::MetalLayer; consumers retain it as needed.
	class FNativeMetalLayerHandle
	{
	public:
		constexpr FNativeMetalLayerHandle() = default;
		explicit constexpr FNativeMetalLayerHandle(CA::MetalLayer* InLayer)
			: Layer(InLayer) {}

		auto Get() const -> CA::MetalLayer* { return Layer; }
		explicit operator bool() const { return Layer != nullptr; }
		auto operator==(const FNativeMetalLayerHandle&) const -> bool = default;

	private:
		CA::MetalLayer* Layer = nullptr;
	};
}
