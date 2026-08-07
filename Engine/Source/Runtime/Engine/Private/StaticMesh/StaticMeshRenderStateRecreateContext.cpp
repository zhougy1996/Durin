#include "StaticMesh/StaticMeshRenderStateRecreateContext.h"

#include "Components/StaticMeshComponent.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "StaticMesh/StaticMesh.h"

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

	FStaticMeshRenderStateRecreateContext::FStaticMeshRenderStateRecreateContext(
		DStaticMesh* StaticMesh)
		: StaticMeshHandle(MakeObjectHandle(StaticMesh))
	{
		if (!IsValid(StaticMesh) || IsObjectHandleNull(StaticMeshHandle)) return;

		for (DObject* Object : GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly))
		{
			auto* Component = Cast<DStaticMeshComponent>(Object);
			if (!IsValid(Component) || !Component->IsRegistered()
				|| Component->GetStaticMesh() != StaticMesh)
				continue;
			const FObjectHandle Handle = MakeObjectHandle(Component);
			if (IsObjectHandleNull(Handle)) continue;
			ComponentHandles.push_back(Handle);
		}
		std::ranges::sort(ComponentHandles, HandleLess);

		for (FObjectHandle Handle : ComponentHandles)
		{
			auto* Component = Cast<DStaticMeshComponent>(ResolveObjectHandle(Handle));
			if (IsValid(Component) && Component->IsRegistered()
				&& Component->GetStaticMesh() == StaticMesh)
				Component->DestroyRenderState();
		}
	}

	FStaticMeshRenderStateRecreateContext::~FStaticMeshRenderStateRecreateContext()
	{
		auto* StaticMesh = Cast<DStaticMesh>(ResolveObjectHandle(StaticMeshHandle));
		if (!IsValid(StaticMesh)) return;

		for (FObjectHandle Handle : ComponentHandles)
		{
			auto* Component = Cast<DStaticMeshComponent>(ResolveObjectHandle(Handle));
			if (IsValid(Component) && Component->IsRegistered()
				&& Component->GetStaticMesh() == StaticMesh)
				Component->HandleStaticMeshRenderDataChanged(StaticMesh);
		}
	}
} // namespace Durin
