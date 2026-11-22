#pragma once

#include "CoreMinimal.h"

namespace Durin::RendererPrivate
{
	// Shared by mesh-family shader-map slot keys. Hash a
	// discriminating semantic subset; the index still compares the complete key.
	// Never hash struct bytes: padding and float signed zero are not identities.
	struct FMaterialResourceCacheKeyHash
	{
		template <typename KeyType>
		auto operator()(const KeyType& Key) const -> size_t
		{
			const auto& Material = Key.Material;
			size_t Hash = std::hash<FXxHash128>{}(Material.ProgramIdentity.Digest);
			auto Mix = [&Hash](size_t Value) { Hash ^= Value + size_t{0x9e3779b9} + (Hash << 6) + (Hash >> 2); };
			Mix(Key.FactoryKey.HashValue);
			Mix(Key.LayoutKey.HashValue);
			Mix(Material.RenderLayout.Version);
			Mix(static_cast<uint8>(Material.BlendMode));
			Mix(static_cast<uint8>(Material.ShadingModel));
			Mix(std::hash<float>{}(Material.OpacityMaskThreshold));
			if constexpr (requires { Key.VertexDomain; }) Mix(static_cast<size_t>(Key.VertexDomain));
			if constexpr (requires { Key.FragmentPass; }) Mix(static_cast<size_t>(Key.FragmentPass));
			return Hash;
		}
	};
}
