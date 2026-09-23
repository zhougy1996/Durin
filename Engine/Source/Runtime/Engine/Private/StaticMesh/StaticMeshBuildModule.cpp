#include "StaticMesh/IMeshBuilderModule.h"

namespace Durin
{
	auto FStaticMeshBuildSession::Acquire() -> FStaticMeshBuildSession
	{
		FStaticMeshBuildSession Session;
#if DURIN_WITH_EDITOR
		auto& Manager = FModuleManager::Get();
		Session.CodeLease = Manager.AcquireCodeLease("MeshBuilder");
		if (Session.CodeLease)
		{
			const auto Info = Manager.FindModule("MeshBuilder");
			Session.Module = static_cast<IMeshBuilderModule*>(Info->Module.get());
			Session.Generation = Info->OwnerGeneration;
		}
#endif
		return Session;
	}
}
