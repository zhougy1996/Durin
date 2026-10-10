#pragma once

#include "CoreMinimal.h"
#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "Hash/XxHash.h"
#include "StaticMesh/StaticMeshSource.h"

namespace Durin::StaticMeshPrivate
{
	inline auto BuildSourceIdentity(uint32 MaterialSlotCount, uint32 MeshCount, FXxHash128 PayloadId) -> FXxHash128
	{
		FXxHash128Builder Builder;
		Builder.UpdateValue(StaticMeshSourceGeometryIdentityVersion);
		Builder.UpdateValue(MaterialSlotCount);
		Builder.UpdateValue(MeshCount);
		Builder.UpdateValue(PayloadId);
		return Builder.Finalize();
	}

	// Decodes the existing authored bulk representation, independently of source residency.
	ENGINE_API auto DecodeSourceGeometry(FByteView Bytes, uint32 MaterialSlotCount, uint32 MeshCount,
		const std::function<bool()>& ShouldCancel = {})
		-> std::expected<FMeshDescriptionReadHandle, FStaticMeshSourceError>;
}

#endif
