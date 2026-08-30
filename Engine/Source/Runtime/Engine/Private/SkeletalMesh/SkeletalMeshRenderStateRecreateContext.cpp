#include "SkeletalMesh/SkeletalMeshRenderStateRecreateContext.h"

#include "Components/SkeletalMeshComponent.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "SkeletalMesh/SkeletalMesh.h"

namespace Durin
{
	namespace
	{
		auto HandleLess(FObjectHandle Left, FObjectHandle Right) -> bool
		{
			return Left.Index < Right.Index
				|| (Left.Index == Right.Index && Left.Generation < Right.Generation);
		}
	}

	FSkeletalMeshRenderStateRecreateContext::FSkeletalMeshRenderStateRecreateContext(
		DSkeletalMesh* SkeletalMesh)
		: SkeletalMeshHandle(MakeObjectHandle(SkeletalMesh))
	{
		if (!IsValid(SkeletalMesh) || IsObjectHandleNull(SkeletalMeshHandle)) return;
		for (DObject* Object : GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly))
		{
			auto* Component = Cast<DSkeletalMeshComponent>(Object);
			if (!IsValid(Component) || !Component->IsRegistered()
				|| Component->GetSkeletalMesh() != SkeletalMesh) continue;
			const FObjectHandle Handle = MakeObjectHandle(Component);
			if (!IsObjectHandleNull(Handle)) ComponentHandles.push_back(Handle);
		}
		std::ranges::sort(ComponentHandles, HandleLess);
		for (FObjectHandle Handle : ComponentHandles)
		{
			auto* Component = Cast<DSkeletalMeshComponent>(ResolveObjectHandle(Handle));
			if (IsValid(Component) && Component->IsRegistered()
				&& Component->GetSkeletalMesh() == SkeletalMesh)
				Component->DestroyRenderState();
		}
	}

	FSkeletalMeshRenderStateRecreateContext::~FSkeletalMeshRenderStateRecreateContext()
	{
		auto* SkeletalMesh = Cast<DSkeletalMesh>(ResolveObjectHandle(SkeletalMeshHandle));
		if (!IsValid(SkeletalMesh)) return;
		for (FObjectHandle Handle : ComponentHandles)
		{
			auto* Component = Cast<DSkeletalMeshComponent>(ResolveObjectHandle(Handle));
			if (IsValid(Component) && Component->IsRegistered()
				&& Component->GetSkeletalMesh() == SkeletalMesh)
				Component->HandleSkeletalMeshRenderDataChanged(SkeletalMesh);
		}
	}
}
