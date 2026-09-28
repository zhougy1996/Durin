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
		std::shared_ptr<DerivedData::IBuild> Service;
		std::shared_ptr<DerivedData::FBuildSession> Session;
		auto Failure(std::string Description, DerivedData::EBuildOperation Operation = DerivedData::EBuildOperation::Admission)
			-> DerivedData::FBuildCompleteParams
		{
			return DerivedData::FBuildCompleteParams::Error({
				.Reason = DerivedData::EBuildFailureReason::InputUnavailable,
				.Operation = Operation, .Description = std::move(Description)},
				std::nullopt, DerivedData::EBuildStatus::None, {});
		}
	}
	auto InitializeAssetBuildService() -> bool
	{
		std::lock_guard Lock(ServiceMutex);
		if (Stopping) return false;
		if (Service) return true;
		auto* Textures = ITextureBuildModule::Get();
		auto* Meshes = IMeshBuilderModule::Get();
		if (!Textures || !Meshes) return false;
		auto Created = DerivedData::CreateBuild({.CompressRecords = true});
		if (!Created->Register(TexturePrivate::MakeTexture2DBuildFunction(*Textures))
			|| !Created->Register(TexturePrivate::MakeVolumeTextureBuildFunction(*Textures))
			|| !Created->Register(TexturePrivate::MakeTextureCubeBuildFunction(*Textures))
			|| !Created->Register(StaticMeshPrivate::MakeRenderBuildFunction(*Meshes))
			|| !Created->Register(PhysicsPrivate::MakeCollisionBuildFunction())) return false;
		auto Persistent = Created->CreateSession();
		if (!Persistent) return false;
		Service = std::move(Created);
		Session = std::move(*Persistent);
		return true;
	}
	auto AssetBuildPrivate::Build(DerivedData::FBuildDefinition Definition,
		std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
		DerivedData::FBuildRequestOptions Options) -> DerivedData::FBuildCompleteParams
	{
		if (Options.Cancellation.IsCancelled()) return DerivedData::FBuildCompleteParams::Canceled(
			std::nullopt, DerivedData::EBuildStatus::None, {});
		std::shared_ptr<DerivedData::FBuildSession> Persistent;
		{
			std::lock_guard Lock(ServiceMutex);
			if (!Service || !Session || Stopping) return Failure("Asset build service is not accepting requests.");
			Persistent = Session;
		}
		auto Inputs = DerivedData::FBuildInputs::TryCreate(Definition.GetSources(), std::move(Resolver), Options.Cancellation);
		if (!Inputs) return DerivedData::FBuildCompleteParams::Error(std::move(Inputs.error()),
			std::nullopt, DerivedData::EBuildStatus::None, {});
		std::optional<DerivedData::FBuildCompleteParams> Completion;
		auto Admitted = Persistent->Build(std::move(Definition), [&](auto Value) {
			Completion = std::move(Value);
		}, std::move(*Inputs), std::move(Options));
		if (!Admitted) return Failure(std::move(Admitted.error().Description));
		if (!Completion) return Failure("Asset build session did not complete inline.", DerivedData::EBuildOperation::Dispatch);
		return std::move(*Completion);
	}
	auto ShutdownAssetBuildService() -> void
	{
		std::lock_guard ShutdownLock(ShutdownMutex);
		std::shared_ptr<DerivedData::IBuild> Retired;
		{
			std::lock_guard Lock(ServiceMutex);
			if (!Service) return;
			Stopping = true;
			Retired = Service;
		}
		Retired->Close();
		if (Retired->Drain() != DerivedData::EBuildDrainResult::Drained) std::terminate();
		{
			std::lock_guard Lock(ServiceMutex);
			Session.reset(); Service.reset(); Stopping = false;
		}
	}
}
#else
namespace Durin
{
	auto InitializeAssetBuildService() -> bool { return false; }
	auto ShutdownAssetBuildService() -> void {}
}
#endif
