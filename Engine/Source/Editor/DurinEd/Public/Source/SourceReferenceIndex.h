#pragma once

#include "DObject/AssetPath.h"
#include "DurinEdAPI.h"

namespace Durin::Editor
{
	struct FSourceReference
	{
		FAssetPath AssetPath;
		std::string AssetClassName;
	};

	// Builds a bounded reverse index from serialized source provenance without
	// loading packages or invoking PostLoad. The snapshot is rebuilt only when
	// the asset registry revision changes or an explicit invalidation is requested.
	class FSourceReferenceIndex
	{
	public:
		DURINED_API auto Refresh(size_t MaximumPackageInspections = 4096) -> void;
		DURINED_API auto Invalidate() -> void;
		DURINED_API auto FindReferences(std::string_view SourceVirtualPath) const
			-> std::span<const FSourceReference>;

		auto GetRegistryRevision() const -> uint64 { return RegistryRevision; }
		auto GetInspectedPackageCount() const -> size_t { return InspectedPackageCount; }
		auto GetWarning() const -> const std::string& { return Warning; }

	private:
		std::unordered_map<std::string, std::vector<FSourceReference>> References;
		uint64 RegistryRevision = 0;
		size_t InspectedPackageCount = 0;
		std::string Warning;
	};
} // namespace Durin::Editor
