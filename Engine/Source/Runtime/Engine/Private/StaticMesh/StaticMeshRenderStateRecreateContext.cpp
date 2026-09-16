#include "StaticMesh/StaticMeshRenderStateRecreateContext.h"

#include "Components/StaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	namespace
	{
		auto HandleLess(FObjectKey Left, FObjectKey Right) -> bool
		{
			return Left < Right;
		}
	}

	FStaticMeshRenderStateRecreateContext::FStaticMeshRenderStateRecreateContext(
		DStaticMesh* StaticMesh)
		: StaticMeshHandle(FObjectKey(StaticMesh))
	{
		if (!IsValid(StaticMesh) || IsObjectKeyNull(StaticMeshHandle)) return;

		for (DObject* Object : GDObjectArray.Snapshot(EObjectQueryScope::LiveOnly))
		{
			auto* StaticComponent = Cast<DStaticMeshComponent>(Object);
			auto* SplineComponent = Cast<DSplineMeshComponent>(Object);
			DPrimitiveComponent* Component = StaticComponent
				? static_cast<DPrimitiveComponent*>(StaticComponent)
				: static_cast<DPrimitiveComponent*>(SplineComponent);
			if (!IsValid(Component) || !Component->IsRegistered()
				|| (StaticComponent && StaticComponent->GetStaticMesh() != StaticMesh)
				|| (SplineComponent && SplineComponent->GetStaticMesh() != StaticMesh))
				continue;
			const FObjectKey Handle = FObjectKey(Component);
			if (IsObjectKeyNull(Handle)) continue;
			ComponentHandles.push_back(Handle);
		}
		std::ranges::sort(ComponentHandles, HandleLess);

		for (FObjectKey Handle : ComponentHandles)
		{
			auto* Object = ResolveObjectKey(Handle);
			auto* StaticComponent = Cast<DStaticMeshComponent>(Object);
			auto* SplineComponent = Cast<DSplineMeshComponent>(Object);
			DPrimitiveComponent* Component = StaticComponent
				? static_cast<DPrimitiveComponent*>(StaticComponent)
				: static_cast<DPrimitiveComponent*>(SplineComponent);
			if (IsValid(Component) && Component->IsRegistered()
				&& ((!StaticComponent || StaticComponent->GetStaticMesh() == StaticMesh)
					&& (!SplineComponent || SplineComponent->GetStaticMesh() == StaticMesh)))
				Component->DestroyRenderState();
		}
	}

	FStaticMeshRenderStateRecreateContext::~FStaticMeshRenderStateRecreateContext()
	{
		auto* StaticMesh = Cast<DStaticMesh>(ResolveObjectKey(StaticMeshHandle));
		if (!IsValid(StaticMesh)) return;

		for (FObjectKey Handle : ComponentHandles)
		{
			auto* Object = ResolveObjectKey(Handle);
			if (auto* Component = Cast<DStaticMeshComponent>(Object);
				IsValid(Component) && Component->IsRegistered()
				&& Component->GetStaticMesh() == StaticMesh)
				Component->HandleStaticMeshRenderDataChanged(StaticMesh);
			else if (auto* SplineComponent = Cast<DSplineMeshComponent>(Object);
				IsValid(SplineComponent) && SplineComponent->IsRegistered()
				&& SplineComponent->GetStaticMesh() == StaticMesh)
				SplineComponent->HandleStaticMeshRenderDataChanged(StaticMesh);
		}
	}
} // namespace Durin
