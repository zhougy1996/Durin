#include "TextureBuildSession.h"
#if DURIN_WITH_EDITOR
#include "Texture2DBuildFunction.h"
#include "TextureCubeBuildFunction.h"
#include "VolumeTextureBuildFunction.h"
#include "Logging/LogMacros.h"
#include "Texture/Texture2DCompilationTestSupport.h"

namespace Durin::TexturePrivate
{
	namespace
	{
		std::mutex SessionMutex;
		std::shared_ptr<DerivedData::FBuildSession> Session;
		std::shared_ptr<DerivedData::FBuildSession> AsyncSession;
		std::shared_ptr<DerivedData::IBuild> TestBuild;

		auto GetSession() -> std::shared_ptr<DerivedData::FBuildSession>
		{
			std::lock_guard Lock(SessionMutex);
			if (!Session)
			{
				auto Created = DerivedData::GetBuild().CreateSession({}, DerivedData::CreateInlineBuildScheduler());
				if (!Created) return {};
				Session = std::move(*Created);
			}
			return Session;
		}
	}

	auto SetAsyncCacheForTests(std::shared_ptr<DerivedData::ICache> Cache) -> void
	{
		std::lock_guard Lock(SessionMutex);
		if (AsyncSession) require(AsyncSession->Drain() == DerivedData::EBuildDrainResult::Drained);
		AsyncSession.reset();
		TestBuild.reset();
		if (!Cache) return;
		TestBuild = DerivedData::CreateBuild({.Cache = std::move(Cache)});
		require(TestBuild->Register(MakeTexture2DBuildFunction()).has_value());
		require(TestBuild->Register(MakeTextureCubeBuildFunction()).has_value());
		auto Created = TestBuild->CreateSession({}, DerivedData::CreateTaskBuildScheduler());
		require(Created.has_value());
		AsyncSession = std::move(*Created);
	}

	auto BuildAsync(DerivedData::FBuildDefinition Definition,
		std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
		DerivedData::FBuildRequestOwner& Owner,
		DerivedData::FBuildCompletionCallback Completion,
		DerivedData::FBuildRequestOptions Options) -> bool
	{
		std::shared_ptr<DerivedData::FBuildSession> Persistent;
		{
			std::lock_guard Lock(SessionMutex);
			if (!AsyncSession)
			{
				auto Created = DerivedData::GetBuild().CreateSession({}, DerivedData::CreateTaskBuildScheduler());
				if (!Created) return false;
				AsyncSession = std::move(*Created);
			}
			Persistent = AsyncSession;
		}
		Options.InputResolver = std::move(Resolver);
		return Persistent->Build(std::move(Definition), Owner, std::move(Completion), {}, std::move(Options)).has_value();
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
	}

	auto Build(DerivedData::FBuildDefinition Definition,
		std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
		DerivedData::FBuildRequestOptions Options)
		-> std::optional<DerivedData::FBuildCompleteParams>
	{
		const std::string FunctionName(Definition.GetFunctionName());
		auto Reject = [&](std::string_view Description) -> std::optional<DerivedData::FBuildCompleteParams>
		{
			DURIN_ERROR_CATEGORY("TextureCompressor", "{} request failed: {}", FunctionName, Description);
			return std::nullopt;
		};
		if (Options.Cancellation.IsCancelled()) return DerivedData::FBuildCompleteParams::Canceled(
			std::nullopt, DerivedData::EBuildStatus::None);
		auto Persistent = GetSession();
		if (!Persistent) return Reject("Texture build session is unavailable.");
		Options.InputResolver = std::move(Resolver);
		DerivedData::FBuildRequestOwner Owner;
		std::optional<DerivedData::FBuildCompleteParams> Completion;
		auto Admitted = Persistent->Build(std::move(Definition), Owner, [&](auto Value) {
			Completion = std::move(Value);
		}, {}, std::move(Options));
		if (!Admitted) return Reject(Admitted.error().Description);
		require(Owner.Wait() == DerivedData::EBuildWaitResult::Completed);
		if (!Completion) return Reject("Texture build completion is unavailable.");
		return std::move(*Completion);
	}
}

namespace Durin::AssetPrivate
{
	auto SetTextureAsyncCacheForTests(std::shared_ptr<DerivedData::ICache> Cache) -> void
	{
		TexturePrivate::SetAsyncCacheForTests(std::move(Cache));
	}
}
#endif
