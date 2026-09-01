#include "Asset/PackageSchema.h"
#include "AssetPackageCodec.h"
#include "AssetPackageByteSource.h"

#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "Misc/FileHelper.h"

namespace Durin
{
	namespace
	{
		auto SerializedTypeSignature(FProperty* Property) -> std::string
		{
			if (!Property) return "Invalid";
			const auto Kind = Property->GetKind();
			if (Kind == DurinCodeGen::EPropertyGenFlags::Array)
				return std::format("Array<{}>", SerializedTypeSignature(static_cast<FArrayProperty*>(Property)->GetInner()));
			if (Kind == DurinCodeGen::EPropertyGenFlags::Map)
			{
				auto* Map = static_cast<FMapProperty*>(Property);
				return std::format("Map<{},{}>", SerializedTypeSignature(Map->GetKeyProp()), SerializedTypeSignature(Map->GetValueProp()));
			}
			if (Kind == DurinCodeGen::EPropertyGenFlags::Object)
				return std::format("Object:{}:{}", Property->GetReferencedClass()
					? Property->GetReferencedClass()->GetQualifiedName().ToString() : "DObject", Property->IsObjectPtrWrapper());
			if (Kind == DurinCodeGen::EPropertyGenFlags::SoftObject)
			{
				auto* SoftObject = static_cast<FSoftObjectProperty*>(Property);
				return std::format("SoftObject:{}:v1", SoftObject->GetExpectedClass()
					? SoftObject->GetExpectedClass()->GetQualifiedName().ToString() : "DObject");
			}
			if (Kind == DurinCodeGen::EPropertyGenFlags::Enum)
			{
				auto* Enum = static_cast<FEnumProperty*>(Property);
				return std::format("Enum:{}:{}", Enum->GetEnum() ? Enum->GetEnum()->GetQualifiedName().ToString() : "", Property->GetElementSize());
			}
			if (Kind == DurinCodeGen::EPropertyGenFlags::Struct)
			{
				auto* Struct = static_cast<FStructProperty*>(Property);
				return std::format("Struct<{}>", Struct->GetStruct() ? Struct->GetStruct()->GetQualifiedName().ToString() : "");
			}
			if (Kind == DurinCodeGen::EPropertyGenFlags::String
				|| Kind == DurinCodeGen::EPropertyGenFlags::Name
				|| Kind == DurinCodeGen::EPropertyGenFlags::Guid
				|| Kind == DurinCodeGen::EPropertyGenFlags::Byte
				|| Kind == DurinCodeGen::EPropertyGenFlags::Blob
				|| Kind == DurinCodeGen::EPropertyGenFlags::BulkData)
				return std::format("{}:v1", static_cast<uint32>(Kind));
			return std::format("{}:{}", static_cast<uint32>(Kind), Property->GetElementSize());
		}

	}

	auto FReflectionSchemaCatalog::Capture() -> FReflectionSchemaCatalog
	{
		FReflectionSchemaCatalog Result;
		std::unordered_set<const FProperty*> CapturedRouteProperties;
		std::function<void(FProperty*, std::string)> CaptureRoutes;
		CaptureRoutes = [&](FProperty* Property, std::string DeclaringType) {
			if (!Property || DeclaringType.empty() || !CapturedRouteProperties.insert(Property).second) return;
			if (const FPropertyDeprecation* Deprecation = Property->GetDeprecation())
			{
				FReflectionDeprecatedPropertyRoute Route{
					.DeclaringType = DeclaringType,
					.DeprecatedPropertyName = Property->NamePrivate.ToString(),
					.StoredName = Deprecation->HistoricalName.ToString(),
					.Kind = Property->GetKind(),
					.TypeSignature = SerializedTypeSignature(Property),
					.CustomVersionGuid = Deprecation->CustomVersionGuid,
					.DeprecatedBefore = Deprecation->DeprecatedBefore,
					.LatestVersion = Deprecation->LatestVersion};
				for (FName Target : Deprecation->MigrationTargets)
					Route.MigrationTargets.push_back(Target.ToString());
				Result.DeprecatedPropertyRoutes.push_back(std::move(Route));
			}
			if (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Struct)
			{
				auto* StructProperty = static_cast<FStructProperty*>(Property);
				if (DStruct* Struct = StructProperty->GetStruct())
					Struct->ForEachProperty([&](FProperty* Nested) {
						CaptureRoutes(Nested, Struct->GetQualifiedName().ToString());
					}, false);
			}
			else if (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Array)
				CaptureRoutes(static_cast<FArrayProperty*>(Property)->GetInner(), DeclaringType);
			else if (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Map)
			{
				auto* MapProperty = static_cast<FMapProperty*>(Property);
				CaptureRoutes(MapProperty->GetKeyProp(), DeclaringType);
				CaptureRoutes(MapProperty->GetValueProp(), DeclaringType);
			}
		};
		for (const FSerializedReflectionAlias& Alias : CaptureSerializedReflectionAliases())
		{
			EAssetReflectedIdentityKind Kind = EAssetReflectedIdentityKind::Class;
			if (Alias.Kind == ESerializedReflectedKind::Struct) Kind = EAssetReflectedIdentityKind::Struct;
			else if (Alias.Kind == ESerializedReflectedKind::Enum) Kind = EAssetReflectedIdentityKind::Enum;
			Result.SerializedAliases.push_back({Alias.StoredName, Alias.CurrentName, Kind});
		}
		std::ranges::sort(Result.SerializedAliases, {}, &FReflectionSerializedAlias::StoredIdentity);
		for (const FSerializedPropertyAlias& Alias : CaptureSerializedPropertyAliases())
			Result.SerializedPropertyAliases.push_back({
				Alias.DeclaringType, Alias.StoredName, Alias.CurrentName});
		std::ranges::sort(Result.SerializedPropertyAliases, [](const auto& Left, const auto& Right) {
			return std::tie(Left.DeclaringType, Left.StoredName)
				< std::tie(Right.DeclaringType, Right.StoredName);
		});
		for (DClass* Class : GetDerivedClasses(DObject::StaticClass(), true))
		{
			if (!Class || Class->GetQualifiedName().IsNone()) continue;
			FReflectionSchemaClass Entry;
			Entry.QualifiedName = Class->GetQualifiedName().ToString();
			Entry.bConstructible = Class->ClassConstructor != nullptr;
			for (DClass* Ancestor = Class; Ancestor; Ancestor = Ancestor->GetSuperClass())
				Entry.Ancestry.push_back(Ancestor->GetQualifiedName().ToString());
			for (DClass* Declaring = Class; Declaring; Declaring = Declaring->GetSuperClass())
			{
				Declaring->ForEachProperty([&](FProperty* Property) {
					CaptureRoutes(Property, Declaring->GetQualifiedName().ToString());
					if (Property->GetDeprecation()) return;
					Entry.Fields.push_back({
						.DeclaringType = Declaring->GetQualifiedName().ToString(),
						.Name = Property->NamePrivate.ToString(),
						.Kind = Property->GetKind(),
						.TypeSignature = SerializedTypeSignature(Property)});
				}, false);
			}
			std::ranges::sort(Entry.Ancestry);
			std::ranges::sort(Entry.Fields, [](const auto& Left, const auto& Right) {
				return std::tie(Left.DeclaringType, Left.Name) < std::tie(Right.DeclaringType, Right.Name);
			});
			Result.Classes.push_back(std::move(Entry));
		}
		std::ranges::sort(Result.Classes, {}, &FReflectionSchemaClass::QualifiedName);
		std::ranges::sort(Result.DeprecatedPropertyRoutes, [](const auto& Left, const auto& Right) {
			return std::tie(Left.DeclaringType, Left.StoredName, Left.TypeSignature)
				< std::tie(Right.DeclaringType, Right.StoredName, Right.TypeSignature);
		});
		return Result;
	}

	auto FReflectionSchemaCatalog::FindDeprecatedPropertyRoute(
		std::string_view DeclaringType, std::string_view StoredName,
		DurinCodeGen::EPropertyGenFlags Kind, std::string_view TypeSignature,
		std::span<const std::pair<FGuid, int32>> CustomVersions) const
		-> const FReflectionDeprecatedPropertyRoute*
	{
		const FReflectionDeprecatedPropertyRoute* Match = nullptr;
		for (const FReflectionDeprecatedPropertyRoute& Route : DeprecatedPropertyRoutes)
		{
			if (Route.DeclaringType != DeclaringType || Route.StoredName != StoredName
				|| Route.Kind != Kind || Route.TypeSignature != TypeSignature) continue;
			const auto Version = std::ranges::find_if(CustomVersions,
				[&](const auto& Pair) { return Pair.first == Route.CustomVersionGuid; });
			const int32 SourceVersion = Version == CustomVersions.end() ? -1 : Version->second;
			if (SourceVersion >= Route.DeprecatedBefore) continue;
			if (Match) return nullptr;
			Match = &Route;
		}
		return Match;
	}

	auto FReflectionSchemaCatalog::FindSerializedAlias(std::string_view StoredIdentity) const
		-> const FReflectionSerializedAlias*
	{
		const auto It = std::ranges::lower_bound(
			SerializedAliases, StoredIdentity, {}, &FReflectionSerializedAlias::StoredIdentity);
		return It != SerializedAliases.end() && It->StoredIdentity == StoredIdentity ? &*It : nullptr;
	}

	auto FReflectionSchemaCatalog::FindSerializedPropertyAlias(
		std::string_view DeclaringType,
		std::string_view StoredName) const -> const FReflectionSerializedPropertyAlias*
	{
		const auto Key = std::pair(DeclaringType, StoredName);
		const auto It = std::ranges::lower_bound(
			SerializedPropertyAliases, Key, {}, [](const auto& Alias) {
				return std::pair(std::string_view(Alias.DeclaringType), std::string_view(Alias.StoredName));
			});
		return It != SerializedPropertyAliases.end()
			&& It->DeclaringType == DeclaringType && It->StoredName == StoredName ? &*It : nullptr;
	}

	auto FReflectionSchemaCatalog::FindClass(std::string_view QualifiedName) const -> const FReflectionSchemaClass*
	{
		const auto It = std::ranges::lower_bound(Classes, QualifiedName, {}, &FReflectionSchemaClass::QualifiedName);
		return It != Classes.end() && It->QualifiedName == QualifiedName ? &*It : nullptr;
	}

	auto FReflectionSchemaCatalog::FindField(
		const FReflectionSchemaClass& ObjectClass,
		std::string_view DeclaringType,
		std::string_view Name) const -> const FReflectionSchemaField*
	{
		if (!std::ranges::binary_search(ObjectClass.Ancestry, DeclaringType)) return nullptr;
		const auto It = std::ranges::lower_bound(ObjectClass.Fields, std::pair(DeclaringType, Name), {}, [](const auto& Field) {
			return std::pair(std::string_view(Field.DeclaringType), std::string_view(Field.Name));
		});
		return It != ObjectClass.Fields.end() && It->DeclaringType == DeclaringType && It->Name == Name ? &*It : nullptr;
	}

	auto InspectAssetPackageSchema(FFileHelper::IFileHandle& Handle,
		const FPackagePath& PackagePath, const FReflectionSchemaCatalog& Catalog,
		FPackageSchemaInspection& OutInspection, FPackageSchemaReadStats* OutStats,
		bool bIncludeNestedMigrationEvidence,
		const FPackageReadCancellationCheck& IsCancellationRequested) -> FAssetResult
	{
		FPackageSchemaReadStats LocalStats;
		FPackageSchemaReadStats& Stats = OutStats ? *OutStats : LocalStats;
		AssetPrivate::FBorrowedFileAssetPackageByteSource FileSource(Handle);
		AssetPrivate::FCountingAssetPackageByteSource Source(FileSource, Stats);
		const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
		uint32 FormatVersion = 0;
		FAssetResult Result = AssetPrivate::ResolveAssetPackageReader(Source, Codec,
			&FormatVersion, IsCancellationRequested);
		if (!Result) return Result;
		Result = Codec->InspectSchema(Source, PackagePath, Catalog, OutInspection,
			&Stats, bIncludeNestedMigrationEvidence, IsCancellationRequested);
		if (Result) OutInspection.FormatVersion = FormatVersion;
		return Result;
	}
}
