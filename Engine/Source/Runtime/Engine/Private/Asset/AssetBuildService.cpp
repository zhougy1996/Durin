#include "Asset/AssetBuildService.h"
#if DURIN_WITH_EDITOR
#include "AssetBuildServicePrivate.h"
#include "Texture/Texture2DBuildFunction.h"
#include "Texture/VolumeTextureBuildFunction.h"
#include "Texture/TextureCubeBuildFunction.h"
#include "Texture/ITextureBuildModule.h"
#include "StaticMesh/IMeshBuilderModule.h"
#include "StaticMesh/StaticMeshBuildFunction.h"
#include "Physics/PhysicsBuildFunction.h"
#include <mutex>

namespace Durin
{
	namespace
	{
		std::mutex ServiceMutex;
		std::mutex ShutdownMutex;
		bool Stopping = false;
		std::optional<DerivedData::FBuildRegistrySnapshot> Registry;
		std::vector<std::shared_ptr<DerivedData::FBuildSession>> Sessions;
	}
	auto InitializeAssetBuildService() -> bool
	{
		std::lock_guard Lock(ServiceMutex);
		if (Stopping) return false;
		if (Registry) return true;
		auto* Textures = ITextureBuildModule::Get();
		auto* Meshes = IMeshBuilderModule::Get();
		if (!Textures || !Meshes) return false;
		DerivedData::FBuildRegistry Mutable;
		if (!Mutable.Register(TexturePrivate::MakeTexture2DBuildFunction(*Textures))
			|| !Mutable.Register(TexturePrivate::MakeVolumeTextureBuildFunction(*Textures))
			|| !Mutable.Register(TexturePrivate::MakeTextureCubeBuildFunction(*Textures))
			|| !Mutable.Register(StaticMeshPrivate::MakeRenderBuildFunction(*Meshes))
			|| !Mutable.Register(PhysicsPrivate::MakeCollisionBuildFunction())) return false;
		auto Frozen = Mutable.Freeze();
		if (!Frozen) return false;
		Registry = std::move(*Frozen);
		return true;
	}
	auto AssetBuildPrivate::CreateSession(std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver)
		-> std::expected<std::shared_ptr<DerivedData::FBuildSession>, DerivedData::FBuildError>
	{
		std::lock_guard Lock(ServiceMutex);
		if (!Registry || Stopping || !Resolver)
			return std::unexpected(DerivedData::FBuildError{.Category = DerivedData::EBuildErrorCategory::Unavailable,
				.Description = "Asset build service is not accepting requests."});
		auto Session = std::make_shared<DerivedData::FBuildSession>(*Registry, std::move(Resolver));
		Sessions.push_back(Session);
		return Session;
	}
	auto AssetBuildPrivate::ReleaseSession(const std::shared_ptr<DerivedData::FBuildSession>& Session) -> void
	{
		if (Session->Drain() != DerivedData::EBuildDrainResult::Drained) std::terminate();
		std::lock_guard Lock(ServiceMutex);
		std::erase(Sessions, Session);
	}
	auto ShutdownAssetBuildService() -> void
	{
		std::lock_guard ShutdownLock(ShutdownMutex);
		std::vector<std::shared_ptr<DerivedData::FBuildSession>> Pending;
		{
			std::lock_guard Lock(ServiceMutex);
			if (!Registry) return;
			Stopping = true;
			Pending = Sessions;
		}
		for (const auto& Session : Pending) Session->Close();
		for (const auto& Session : Pending)
			if (Session->Drain() != DerivedData::EBuildDrainResult::Drained) std::terminate();
		std::optional<DerivedData::FBuildRegistrySnapshot> Retired;
		{
			std::lock_guard Lock(ServiceMutex);
			Retired = std::move(Registry); Registry.reset(); Sessions.clear();
		}
		Retired.reset();
		{ std::lock_guard Lock(ServiceMutex); Stopping = false; }
	}
}
#else
namespace Durin
{
	auto InitializeAssetBuildService() -> bool { return false; }
	auto ShutdownAssetBuildService() -> void {}
}
#endif
