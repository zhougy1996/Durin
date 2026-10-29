#include "TextureDerivedDataBuild.h"
#if DURIN_WITH_EDITOR
	#include "BuildServiceBackendPrivate.h"
	#include "CacheDispatchPrivate.h"
	#include "Asset/ResourceBuildSessions.h"
	#include "Texture/Texture2DCompilationTestSupport.h"

namespace Durin::TexturePrivate
{
	namespace
	{
		AssetPrivate::FResourceBuildSessions Sessions;
	}

	auto SetAsyncCacheForTests(std::shared_ptr<DerivedData::ICacheBackend> Cache) -> void
	{
		std::shared_ptr<DerivedData::IBuild> Service;
		if (Cache)
		{
			Service = DerivedData::Private::CreateBuildForBackend({.Cache = std::move(Cache)});
			require(Service->Register(MakeTexture2DBuildFunction()).has_value());
			require(Service->Register(MakeTextureCubeBuildFunction()).has_value());
			require(Service->Register(MakeVolumeTextureBuildFunction()).has_value());
		}
		require(Sessions.SetTaskService(std::move(Service)) == DerivedData::EBuildDrainResult::Drained);
	}

	auto BuildAsync(DerivedData::FBuildDefinition Definition,
		std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
		DerivedData::IRequestOwner& Owner,
		DerivedData::FBuildCompletionCallback Completion,
		DerivedData::FBuildRequestOptions Options) -> std::expected<void, DerivedData::FBuildAdmissionError>
	{
		auto Persistent = Sessions.Get(DerivedData::EBuildCacheExecution::Task);
		if (!Persistent) return std::unexpected(std::move(Persistent.error()));
		Options.InputResolver = std::move(Resolver);
		(*Persistent)->Build(std::move(Definition), Owner, std::move(Completion), {}, std::move(Options));

		return {};
	}

	auto RegisterBuildFunctions() -> void
	{
		static std::once_flag Once;
		std::call_once(Once, [] {
			auto& Build = DerivedData::GetBuild();
			if (!Build.Register(MakeTexture2DBuildFunction())
				|| !Build.Register(MakeVolumeTextureBuildFunction())
				|| !Build.Register(MakeTextureCubeBuildFunction()))
				throw std::runtime_error("Failed to register texture derived-data build functions.");
		});
		Sessions.Open();
	}
	auto ShutdownBuildSessions() -> void
	{ require(Sessions.Close() == DerivedData::EBuildDrainResult::Drained); }

	auto Build(DerivedData::FBuildDefinition Definition,
		std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
		DerivedData::FBuildRequestOptions Options)
		-> DerivedData::FBuildCompleteParams
	{
		auto Result = Sessions.Build(std::move(Definition), std::move(Resolver), std::move(Options));
		if (Result) return std::move(*Result);
		return DerivedData::FBuildCompleteParams::Error(std::nullopt,
			DerivedData::EBuildExecutionFlags::None, std::move(Result.error().Description));
	}
}

namespace Durin::AssetPrivate
{
	auto SetTextureAsyncCacheForTests(std::shared_ptr<DerivedData::ICacheBackend> Cache) -> void
	{
		TexturePrivate::SetAsyncCacheForTests(std::move(Cache));
	}
}
#endif
