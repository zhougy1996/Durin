#pragma once

#include "CoreMinimal.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildSession.h"

namespace Durin::AssetPrivate
{
	// Convert the terminal build diagnostic once at the asset-family boundary.
	inline auto GetBuildFailureDiagnostic(const DerivedData::FBuildCompleteParams& Completion, std::string_view Fallback) -> std::string
	{
		for (const auto& Message : Completion.Output.GetMessages())
			if (Message.Severity == DerivedData::EBuildMessageSeverity::Error) return Message.Text;
		for (const auto& Log : Completion.Output.GetLogs())
			if (Log.Severity == DerivedData::EBuildLogSeverity::Error) return Log.Text;
		return std::string(Fallback);
	}
	// Asset managers retire their request owners before releasing session resources.
	class FResourceBuildSessions
	{
	public:
		explicit FResourceBuildSessions(std::function<std::shared_ptr<DerivedData::IBuild>()> CreateService) : CreateService(std::move(CreateService)) {}
		auto Get(std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver = {}) -> DerivedData::FBuildSession
		{
			std::lock_guard Lock(Mutex);
			if (!Service) Service = CreateService();
			return Service->CreateSession("AssetBuild", std::move(Resolver));
		}
		auto SetService(std::shared_ptr<DerivedData::IBuild> Value) -> void
		{
			std::shared_ptr<DerivedData::IBuild> Retired;
			{ std::lock_guard Lock(Mutex); Retired = std::exchange(Service, std::move(Value)); }
		}
		auto Reset() -> void { SetService({}); }
		auto Build(DerivedData::FBuildDefinition Definition,
			std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
			DerivedData::FBuildPolicy Policy = {}) -> DerivedData::FBuildCompleteParams
		{
			auto Session = Get(std::move(Resolver));
			DerivedData::FRequestOwner Owner;
			std::optional<DerivedData::FBuildCompleteParams> Completion;
			Session.Build(std::move(Definition), {}, Policy, Owner, [&](auto Value) { Completion = std::move(Value); });
			Owner.Wait();
			require(Completion.has_value());
			return std::move(*Completion);
		}
	private:
		std::mutex Mutex;
		std::shared_ptr<DerivedData::IBuild> Service;
		std::function<std::shared_ptr<DerivedData::IBuild>()> CreateService;
	};
}
#endif
