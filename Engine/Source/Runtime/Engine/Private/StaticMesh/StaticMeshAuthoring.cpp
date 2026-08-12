#include "StaticMesh/StaticMeshAuthoring.h"

namespace Durin
{
	namespace
	{
		std::mutex GStaticMeshAuthoringMutex;
		FStaticMeshAuthoringHandlers GStaticMeshAuthoringHandlers;
	}

	auto RegisterStaticMeshAuthoringHandlers(
		FStaticMeshAuthoringHandlers Handlers) -> bool
	{
		if (!Handlers.BuildFileProduct || !Handlers.PostLoadUncooked
			|| !Handlers.ChangeSourceReference) return false;
		std::lock_guard Lock(GStaticMeshAuthoringMutex);
		if (GStaticMeshAuthoringHandlers.BuildFileProduct) return false;
		GStaticMeshAuthoringHandlers.BuildFileProduct = std::move(Handlers.BuildFileProduct);
		GStaticMeshAuthoringHandlers.PostLoadUncooked = std::move(Handlers.PostLoadUncooked);
		GStaticMeshAuthoringHandlers.ChangeSourceReference =
			std::move(Handlers.ChangeSourceReference);
		return true;
	}

	auto UnregisterStaticMeshAuthoringHandlers() -> void
	{
		std::lock_guard Lock(GStaticMeshAuthoringMutex);
		GStaticMeshAuthoringHandlers.BuildFileProduct = {};
		GStaticMeshAuthoringHandlers.PostLoadUncooked = {};
		GStaticMeshAuthoringHandlers.ChangeSourceReference = {};
	}

	auto RegisterStaticMeshCollisionBuildHandler(
		FStaticMeshCollisionBuildHandler Handler) -> bool
	{
		if (!Handler) return false;
		std::lock_guard Lock(GStaticMeshAuthoringMutex);
		if (GStaticMeshAuthoringHandlers.BuildCollisionProduct) return false;
		GStaticMeshAuthoringHandlers.BuildCollisionProduct = std::move(Handler);
		return true;
	}

	auto UnregisterStaticMeshCollisionBuildHandler() -> void
	{
		std::lock_guard Lock(GStaticMeshAuthoringMutex);
		GStaticMeshAuthoringHandlers.BuildCollisionProduct = {};
	}

	auto InvokeStaticMeshSourceChangeHandler(
		DStaticMesh& Mesh,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		FStaticMeshSourceChangeHandler Handler;
		{
			std::lock_guard Lock(GStaticMeshAuthoringMutex);
			Handler = GStaticMeshAuthoringHandlers.ChangeSourceReference;
		}
		if (!Handler)
		{
			OutError = "StaticMesh source translation is unavailable.";
			return false;
		}
		return Handler(Mesh, SourceVirtualPath, OutError);
	}

	auto GetStaticMeshAuthoringHandlers() -> FStaticMeshAuthoringHandlers
	{
		std::lock_guard Lock(GStaticMeshAuthoringMutex);
		return GStaticMeshAuthoringHandlers;
	}
}
