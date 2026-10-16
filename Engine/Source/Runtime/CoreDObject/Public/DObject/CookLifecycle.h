#pragma once

#include "CoreMinimal.h"
#include "Serialization/Archive.h"
#include "Misc/Guid.h"
#include "CoreDObjectAPI.h"

namespace Durin
{
	class DObject;
	COREDOBJECT_API auto IsObjectIncludedForCook(const DObject* Object, const FArchiveTarget& Target) -> bool;
	// Shared by type schemas and object events; acquisition paths are not identities.
	enum class ECookBuildDependencyKind : uint8
	{
		SourcePackage = 1, OwnedBulk, DirectPackage, TransitivePackage,
		ExternalFile, ConfigurationValue, SchemaProducerVersion
	};

	struct FCookDependencyDeclaration
	{
		ECookBuildDependencyKind Kind = ECookBuildDependencyKind::ConfigurationValue;
		std::string LogicalName;
		std::filesystem::path FilePath;
		FByteBuffer Value;
	};

	enum class EObjectCookError : uint8 { None, Cancelled, InvalidInput, Preparation, Limit, Timeout };
	struct FObjectCookResult
	{
		EObjectCookError Error = EObjectCookError::None;
		std::string Diagnostic;
		explicit operator bool() const { return Error == EObjectCookError::None; }
	};

	enum class ECookEvent : uint8 { PlatformCookDependencies };

	// Invocation-owned generic inputs. Engine-specific settings use canonical named
	// values; object hooks never cast this context to an Engine implementation.
	struct FObjectCookContext
	{
		FArchiveTarget Target;
		// Empty during class-schema collection: those values are shared by all packages.
		std::string Package;
		bool bCooking = false;
		bool bRetainEditorOnlyData = false;
		// Set false when native inputs cannot be fully declared; independent of loading.
		bool bDependenciesComplete = true;
		std::function<bool()> IsCancelled;
		// Owner-thread blocking completion adapter. It must join owned work and
		// report a bounded result, including cancellation; never throw on teardown.
		std::function<void()> WaitForCompletion;
		std::map<std::string, FByteBuffer> RequestValues;
		std::set<std::string> RequestedValues;
		std::function<std::expected<FByteBuffer, FObjectCookResult>(std::string_view)> RequestReader;
		// Class hooks qualify exact class names after auditing inherited/nested native
		// behavior. Inherited hooks do not automatically qualify new subclasses.
		std::set<std::string> VersionCoveredClasses;
		std::set<FGuid> UsedCustomVersions;
		std::set<std::string> ConstructedClasses;
		std::vector<FCookDependencyDeclaration> Dependencies;
		FObjectCookResult Result;

		auto CheckCancellation() -> bool
		{
			if (!Result) return Result.Error == EObjectCookError::Cancelled;
			if (IsCancelled && IsCancelled()) Result = {EObjectCookError::Cancelled, {}};
			return Result.Error == EObjectCookError::Cancelled;
		}
		auto FindRequestValue(std::string_view Name) const -> FByteView
		{
			const auto Found = RequestValues.find(std::string(Name));
			return Found == RequestValues.end() ? FByteView{} : FByteView(Found->second);
		}
		auto RequireRequestValue(std::string_view Name) -> FByteView
		{
			if (!Result) return {};
			if (!RequestValues.contains(std::string(Name)))
			{
				if (!RequestReader) { Result = {EObjectCookError::InvalidInput, "Missing Cook request input: " + std::string(Name)}; return {}; }
				auto Read = RequestReader(Name);
				if (!Read) { Result = std::move(Read.error()); return {}; }
				RequestValues.emplace(std::string(Name), std::move(*Read));
			}
			const auto Value = FindRequestValue(Name);
			if (Value.size() > 1024 * 1024) { Result = {EObjectCookError::Limit, "Cook request input exceeds its value bound."}; return {}; }
			if (RequestedValues.insert(std::string(Name)).second)
				Dependencies.push_back({ECookBuildDependencyKind::ConfigurationValue, "request/" + std::string(Name), {}, FByteBuffer(Value.begin(), Value.end())});
			return Value;
		}
	};

	struct FObjectPreSaveContext
	{
		bool bCooking = false;
		FArchiveTarget Target;
		FObjectCookResult Result;
	};
}
