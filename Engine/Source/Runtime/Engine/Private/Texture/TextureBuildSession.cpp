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
		AssetPrivate::FResourceBuildSessions Sessions([] {
			DerivedData::FBuildServiceOptions Options;
			Options.BuildLimits.Input.MaximumTotalBytes = MaximumTextureSourceBytes + 16;
			Options.BuildLimits.Output.MaximumTotalBytes = MaximumTexturePayloadBytes;
			return DerivedData::CreateBuildWithRegisteredFunctions(std::move(Options));
		});
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
		Sessions.SetService(std::move(Service));
	}

	auto BuildAsync(DerivedData::FBuildDefinition Definition,
		std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
		DerivedData::IRequestOwner& Owner,
		DerivedData::FBuildCompletionCallback Completion,
		DerivedData::FBuildPolicy Policy) -> void
	{
		auto Persistent = Sessions.Get(std::move(Resolver));
		Persistent.Build(std::move(Definition), {}, Policy, Owner, std::move(Completion));

	}

	auto ShutdownBuildSessions() -> void
	{ Sessions.Reset(); }

	auto Build(DerivedData::FBuildDefinition Definition,
		std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
		DerivedData::FBuildPolicy Policy, DerivedData::FRequestOwner* RequestOwner)
		-> DerivedData::FBuildCompleteParams
	{
		return Sessions.Build(std::move(Definition), std::move(Resolver), Policy, RequestOwner);
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
