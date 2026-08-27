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
			std::unordered_map<const DClass*, const DFactory*> BySupportedClass;
			std::unordered_map<std::string, const DFactory*> ByExtension;
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
					{
						auto [It, bInserted] =
							Result.BySupportedClass.emplace(SupportedClass, Factory);
						if (!bInserted && It->second != Factory) It->second = nullptr;
					}
					for (const std::string& Format : Factory->GetFormats())
					{
						std::string Extension = NormalizeFactoryExtension(Format);
						if (Extension.empty()) continue;
						auto [It, bInserted] =
							Result.ByExtension.emplace(std::move(Extension), Factory);
						if (!bInserted && It->second != Factory) It->second = nullptr;
					}
				}
			}
			return *State.Cache;
		}
	}

	DFactory::DFactory(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DFactory::GetAvailableFactories()
		-> std::vector<const DFactory*>
	{
		CheckFactoryCacheThread();
		FFactoryCacheState& State = GetFactoryCacheState();
		return GetFactoryCache(State).Factories;
	}

	auto DFactory::FindFactory(const DClass* SupportedClass)
		-> const DFactory*
	{
		CheckFactoryCacheThread();
		if (!SupportedClass) return nullptr;
		FFactoryCacheState& State = GetFactoryCacheState();
		const auto& Factories = GetFactoryCache(State).BySupportedClass;
		const auto It = Factories.find(SupportedClass);
		return It == Factories.end() ? nullptr : It->second;
	}

	auto DFactory::FindFactoryByExtension(std::string_view Extension)
		-> const DFactory*
	{
		CheckFactoryCacheThread();
		const std::string Normalized = NormalizeFactoryExtension(Extension);
		if (Normalized.empty()) return nullptr;
		FFactoryCacheState& State = GetFactoryCacheState();
		const auto& Factories = GetFactoryCache(State).ByExtension;
		const auto It = Factories.find(Normalized);
		return It == Factories.end() ? nullptr : It->second;
	}

	auto DFactory::InvalidateFactoryCache() -> void
	{
		CheckFactoryCacheThread();
		FFactoryCacheState& State = GetFactoryCacheState();
		State.Cache.reset();
	}

	auto DFactory::FactoryCreateNew(
		DClass*,
		DObject*,
		FName,
		EObjectFlags,
		DObject*) -> DObject*
	{
		return nullptr;
	}

	auto DFactory::FactoryCreateFromFile(
		DClass*,
		DObject*,
		FName,
		EObjectFlags,
		std::string_view,
		DObject*) -> DObject*
	{
		return nullptr;
	}
}
