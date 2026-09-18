#include "Factories/Factory.h"

#include "DObject/Class.h"
#include "Asset/SourceHint.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2DCompilationTypes.h"
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

	auto FormatFactoryError(const FFactoryError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EFactoryError::ExactClass:
			return Error.ExpectedClass + " factory requires the exact supported class.";
		case EFactoryError::AssetPackageParent:
			return Error.ExpectedClass + " factory requires an asset package parent.";
		case EFactoryError::StaticMeshSettings:
			return Error.StaticMeshSettingsCause ? FormatStaticMeshImportSettingsError(*Error.StaticMeshSettingsCause)
				: "Static mesh import settings are invalid.";
		case EFactoryError::SourceHint:
			return Error.SourceHintCause ? FormatSourceHintError(*Error.SourceHintCause)
				: "Factory source hint could not be created.";
		case EFactoryError::TextureCompilation:
			return Error.TextureCompilationCause ? FormatTexture2DCompilationError(*Error.TextureCompilationCause)
				: "Texture compilation failed.";
		case EFactoryError::SourceMissing:
			return Error.ExpectedClass + " source file does not exist: " + Error.Filename;
		case EFactoryError::SourceFormat:
			return Error.ExpectedClass + " source format is unsupported: " + Error.Filename;
		case EFactoryError::PreparedSourceMismatch:
			return "Prepared texture source does not match the requested file: " + Error.Filename;
		case EFactoryError::SourceLayout:
			return Error.ExpectedClass + " factory source layout is unsupported.";
		case EFactoryError::SourceRoleMissing:
			return Error.SourceRole + " " + Error.ExpectedClass + " source is required.";
		case EFactoryError::ObjectCreation:
			return Error.ExpectedClass + " object could not be created.";
		}
		return {};
	}

	auto FFactoryDiagnostics::ReportFailure(FFactoryError Error) -> void
	{
		if (Entries.size() >= MaximumMessageCount) return;
		Entries.push_back({.Failure = std::move(Error)});
	}

	auto FFactoryDiagnostics::ReportDomainFailure(std::shared_ptr<const IFactoryErrorDetail> Error) -> void
	{
		if (!Error || Entries.size() >= MaximumMessageCount) return;
		Entries.push_back({.DomainFailure = std::move(Error)});
	}

	auto FFactoryDiagnostics::Report(std::string_view Message) -> void
	{
		if (Message.empty() || Entries.size() >= MaximumMessageCount) return;
		Entries.push_back({.Message = std::string(Message.substr(0, MaximumMessageLength))});
	}

	auto FFactoryDiagnostics::ToString() const -> std::string
	{
		std::string Result;
		for (const auto& Entry : Entries)
		{
			if (!Result.empty()) Result += '\n';
			if (Entry.DomainFailure) Result += Entry.DomainFailure->Format();
			else Result += Entry.Failure ? FormatFactoryError(*Entry.Failure) : Entry.Message;
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

}
