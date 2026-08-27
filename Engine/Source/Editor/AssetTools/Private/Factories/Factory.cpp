#include "Factories/Factory.h"

#include "DObject/Class.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		struct FFactoryCache
		{
			std::vector<const DFactory*> Factories;
			std::unordered_map<const DClass*, std::vector<const DFactory*>> BySupportedClass;
			std::unordered_map<std::string, std::vector<const DFactory*>> ByExtension;
		};

		struct FFactoryCacheState
		{
			std::optional<FFactoryCache> Cache;
		};

		auto CheckFactoryCacheThread() -> void
		{
			checkf(IsInGameThread(),
				"Factory discovery and cache access must run on the game thread.");
		}

		auto NormalizeFactoryExtension(std::string_view Extension) -> std::string
		{
			while (!Extension.empty() && Extension.front() == '.')
				Extension.remove_prefix(1);
			std::string Normalized(Extension);
			for (char& Character : Normalized)
			{
				if (Character >= 'A' && Character <= 'Z')
					Character = static_cast<char>(Character - 'A' + 'a');
			}
			return Normalized;
		}

		auto DiscoverFactories() -> std::vector<const DFactory*>
		{
			std::vector<const DFactory*> Factories;
			for (DClass* FactoryClass : GetDerivedClasses(DFactory::StaticClass()))
			{
				if (!FactoryClass
					|| FactoryClass->HasAnyClassFlags(EClassFlags::Abstract)) continue;
				if (const auto* Factory = Cast<DFactory>(FactoryClass->GetDefaultObject()))
					Factories.push_back(Factory);
			}
			return Factories;
		}

		auto GetFactoryCacheState() -> FFactoryCacheState&
		{
			static FFactoryCacheState State;
			return State;
		}

		auto GetFactoryCache(FFactoryCacheState& State) -> const FFactoryCache&
		{
			if (!State.Cache)
			{
				State.Cache.emplace();
				FFactoryCache& Result = *State.Cache;
				Result.Factories = DiscoverFactories();
				for (const DFactory* Factory : Result.Factories)
				{
					if (DClass* SupportedClass = Factory->GetSupportedClass())
						Result.BySupportedClass[SupportedClass].push_back(Factory);
					for (const std::string& Format : Factory->GetFormats())
					{
						std::string Extension = NormalizeFactoryExtension(Format);
						if (!Extension.empty())
							Result.ByExtension[std::move(Extension)].push_back(Factory);
					}
				}
			}
			return *State.Cache;
		}

		auto FindUnique(std::span<const DFactory* const> Factories) -> const DFactory*
		{
			return Factories.size() == 1 ? Factories.front() : nullptr;
		}
	}

	auto FFactoryDiagnostics::Report(std::string_view Message) -> void
	{
		if (Message.empty() || Messages.size() >= MaximumMessageCount) return;
		Messages.emplace_back(Message.substr(0, MaximumMessageLength));
	}

	auto FFactoryDiagnostics::ToString() const -> std::string
	{
		std::string Result;
		for (const std::string& Message : Messages)
		{
			if (!Result.empty()) Result += '\n';
			Result += Message;
		}
		return Result;
	}

	DFactory::DFactory(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DFactory::GetAvailableFactories() -> std::vector<const DFactory*>
	{
		CheckFactoryCacheThread();
		FFactoryCacheState& State = GetFactoryCacheState();
		return GetFactoryCache(State).Factories;
	}

	auto DFactory::FindFactory(const DClass* SupportedClass) -> const DFactory*
	{
		return FindUnique(FindFactories(SupportedClass));
	}

	auto DFactory::FindFactories(const DClass* SupportedClass)
		-> std::vector<const DFactory*>
	{
		CheckFactoryCacheThread();
		if (!SupportedClass) return {};
		const auto& ByClass = GetFactoryCache(GetFactoryCacheState()).BySupportedClass;
		const auto It = ByClass.find(SupportedClass);
		return It == ByClass.end() ? std::vector<const DFactory*>{} : It->second;
	}

	auto DFactory::FindFactoriesByExtension(std::string_view Extension)
		-> std::vector<const DFactory*>
	{
		CheckFactoryCacheThread();
		const std::string Normalized = NormalizeFactoryExtension(Extension);
		if (Normalized.empty()) return {};
		const auto& ByExtension = GetFactoryCache(GetFactoryCacheState()).ByExtension;
		const auto It = ByExtension.find(Normalized);
		return It == ByExtension.end() ? std::vector<const DFactory*>{} : It->second;
	}

	auto DFactory::FindFactoryByExtension(std::string_view Extension)
		-> const DFactory*
	{
		const std::vector<const DFactory*> Factories =
			FindFactoriesByExtension(Extension);
		return FindUnique(Factories);
	}

	auto DFactory::FindFactories(
		const DClass* SupportedClass,
		std::string_view Extension) -> std::vector<const DFactory*>
	{
		CheckFactoryCacheThread();
		if (!SupportedClass) return {};
		std::vector<const DFactory*> Result = FindFactoriesByExtension(Extension);
		std::erase_if(Result, [&](const DFactory* Factory) {
			const DClass* FactoryClass = Factory ? Factory->GetSupportedClass() : nullptr;
			return !FactoryClass || !SupportedClass->IsChildOf(FactoryClass);
		});
		return Result;
	}

	auto DFactory::InvalidateFactoryCache() -> void
	{
		CheckFactoryCacheThread();
		GetFactoryCacheState().Cache.reset();
	}

	auto DFactory::FactoryCreateNew(
		DClass*, DObject*, FName, EObjectFlags, DObject*, FFactoryDiagnostics*) const
		-> DObject*
	{
		return nullptr;
	}

	auto DFactory::FactoryCreateFromFile(
		DClass*, DObject*, FName, EObjectFlags, std::string_view, DObject*,
		FFactoryDiagnostics*) const -> DObject*
	{
		return nullptr;
	}

	auto DFactory::GetReimportCapabilities(const DObject&) const
		-> FReimportCapabilities
	{
		return {};
	}

	auto DFactory::FactoryReimport(
		DObject&, FReimportCompletion Completion) const -> void
	{
		if (Completion) Completion({EReimportStatus::Unsupported,
			"The selected factory does not support reimport."});
	}

	auto DFactory::FactoryReimportFromFiles(
		DObject&, std::span<const std::string>, FReimportCompletion Completion) const
		-> void
	{
		if (Completion) Completion({EReimportStatus::Unsupported,
			"The selected factory does not support reimport from file."});
	}
}
