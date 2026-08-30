#pragma once

#include "DObject/ObjectHandle.h"

namespace Durin
{
	class DSkeletalMesh;

	class FSkeletalMeshRenderStateRecreateContext final
	{
	public:
		explicit FSkeletalMeshRenderStateRecreateContext(DSkeletalMesh* SkeletalMesh);
		~FSkeletalMeshRenderStateRecreateContext();
		FSkeletalMeshRenderStateRecreateContext(
			const FSkeletalMeshRenderStateRecreateContext&) = delete;
		auto operator=(const FSkeletalMeshRenderStateRecreateContext&)
			-> FSkeletalMeshRenderStateRecreateContext& = delete;

	private:
		FObjectHandle SkeletalMeshHandle;
		std::vector<FObjectHandle> ComponentHandles;
	};
}
