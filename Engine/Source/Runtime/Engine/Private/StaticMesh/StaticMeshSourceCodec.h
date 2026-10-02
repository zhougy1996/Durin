#pragma once
#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "StaticMesh/StaticMeshSource.h"

namespace Durin::StaticMeshPrivate
{
	// Decodes the existing authored bulk representation, independently of source residency.
	ENGINE_API auto DecodeSourceGeometry(FByteView Bytes, uint32 MaterialSlotCount, uint32 MeshCount,
		const std::function<bool()>& ShouldCancel = {})
		-> std::expected<FMeshDescriptionReadHandle, FStaticMeshSourceError>;
}

#endif
