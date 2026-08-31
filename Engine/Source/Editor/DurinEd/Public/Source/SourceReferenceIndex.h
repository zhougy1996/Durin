#pragma once

#include "DObject/AssetPath.h"
#include "DurinEdAPI.h"

namespace Durin::Editor
{
	namespace Private { struct FSourceReferenceSnapshot; }

	struct FSourceReference
	{
		FPackagePath AssetPath;
		std::string AssetClassName;
	};

	// Reads a process-shared immutable reverse index of serialized source provenance.
	// UI callers request asynchronous revision-driven rebuilds; explicit asset mutation
	// workflows may use Refresh when they must synchronously obtain a current snapshot.
	class FSourceReferenceIndex
	{
	public:
		DURINED_API auto RequestRefresh(size_t MaximumPackageInspections = 4096) -> void;
		DURINED_API auto Refresh(size_t MaximumPackageInspections = 4096) -> void;
		DURINED_API auto Invalidate() -> void;
		DURINED_API auto FindReferences(std::string_view SourceVirtualPath) const
			-> std::span<const FSourceReference>;

		DURINED_API auto IsBuilding() const -> bool;
		DURINED_API auto IsCurrent() const -> bool;
		DURINED_API auto GetRegistryRevision() const -> uint64;
		DURINED_API auto GetInspectedPackageCount() const -> size_t;
		DURINED_API auto GetWarning() const -> const std::string&;

	private:
		std::shared_ptr<const Private::FSourceReferenceSnapshot> Snapshot;
		uint64 RequestedGeneration = 0;
		size_t RequestedMaximumPackageInspections = 4096;
	};
} // namespace Durin::Editor
