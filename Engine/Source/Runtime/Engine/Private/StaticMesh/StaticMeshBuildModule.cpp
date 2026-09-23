#include "StaticMesh/IStaticMeshBuildModule.h"

namespace Durin
{
	auto FStaticMeshBuildSession::Acquire() -> FStaticMeshBuildSession
	{
		FStaticMeshBuildSession Session;
#if DURIN_WITH_EDITOR
		auto& Manager = FModuleManager::Get();
		Session.CodeLease = Manager.AcquireCodeLease("StaticMeshBuild");
		if (Session.CodeLease)
		{
			const auto Info = Manager.FindModule("StaticMeshBuild");
			Session.Module = static_cast<IStaticMeshBuildModule*>(Info->Module.get());
			Session.Generation = Info->OwnerGeneration;
		}
#endif
		return Session;
	}
}
