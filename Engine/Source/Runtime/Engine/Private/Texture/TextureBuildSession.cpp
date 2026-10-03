#include "TextureBuildSession.h"
#if DURIN_WITH_EDITOR
#include "Texture2DBuildFunction.h"
#include "TextureCubeBuildFunction.h"
#include "VolumeTextureBuildFunction.h"
#include "Logging/LogMacros.h"
#include <mutex>

namespace Durin::TexturePrivate
{
	namespace
	{
		std::mutex SessionMutex;
		std::shared_ptr<DerivedData::FBuildSession> Session;

		auto GetSession() -> std::shared_ptr<DerivedData::FBuildSession>
		{
			std::lock_guard Lock(SessionMutex);
			if (!Session)
			{
				auto Created = DerivedData::GetBuild().CreateSession();
				if (!Created) return {};
				Session = std::move(*Created);
			}
			return Session;
		}
	}

	auto RegisterBuildFunctions(ITextureBuildModule& Module) -> void
	{
		static std::once_flag Once;
		std::call_once(Once, [&] {
			auto& Build = DerivedData::GetBuild();
			if (!Build.Register(MakeTexture2DBuildFunction(Module))
				|| !Build.Register(MakeVolumeTextureBuildFunction(Module))
				|| !Build.Register(MakeTextureCubeBuildFunction(Module)))
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
			DURIN_ERROR_CATEGORY("TextureBuild", "{} request failed: {}", FunctionName, Description);
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
#endif
