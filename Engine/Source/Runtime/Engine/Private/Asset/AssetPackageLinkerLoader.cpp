#include "AssetLoadAuditInternal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"
#include "AssetPackageLinker.h"
#include "AssetPackageLinkerSchema.h"
#include "DObject/OverridableManager.h"
#include "AssetLiveLoadGuard.h"
#include "AssetObjectLoadTiming.h"

#include "AssetPackageArchive.h"
#include "Asset/Load.h"
#include "Misc/MountPaths.h"
#include "AssetRuntimeStateInternal.h"
#include "AssetRegistry/Publication.h"

#include "DObject/Class.h"
#include "DObject/Archive.h"
#include "DObject/CanonicalMapKey.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Threading/RunnableThread.h"

namespace Durin::AssetPrivate
{
	using LinkerSchema::EPropertyLoadRoute;

	namespace
	{
		struct FLinkerApplyDiagnostic
		{
			std::optional<UnifiedError::FError> Cause;
			std::string LogicalPath;
			std::string Message;
		};

		auto LinkerApplyFail(FLinkerApplyDiagnostic& Diagnostic,
			UnifiedError::FError Error, uint64 Offset = 0, std::string Path = {}) -> bool
		{
			if (!Diagnostic.Cause)
			{
				if (!Path.empty()) Error.PushContext("object", Path);
				if (Offset) Error.PushContext("offset", std::to_string(Offset));
				Diagnostic.LogicalPath = std::move(Path);
				Diagnostic.Message = Error.ToString();
				Diagnostic.Cause = std::move(Error);
			}
			return false;
		}

		auto ShouldFail(const FLinkerLoadOptions& Options, ELinkerLoadPhase Phase, uint64 Index) -> bool
		{
			return Options.ShouldFail && Options.ShouldFail(Phase, Index);
		}

		auto FindExistingInner(DObject* Outer, std::string_view Name, DClass* Class,
			bool& bOutTypeMismatch) -> DObject*
		{
			bOutTypeMismatch = false;
			for (DObject* Object : GDObjectArray.GetObjectsWithOuter(Outer,
				Outer->GetPackage() && Outer->GetPackage()->IsGraphPrivate()
					? EObjectQueryScope::IncludeUnpublished : EObjectQueryScope::LiveOnly))
			{
				if (!Object || Object->GetName() != Name) continue;
				if (Object->GetClass() != Class) { bOutTypeMismatch = true; return nullptr; }
				return Object;
			}
			return nullptr;
		}


		// Scan validated tags without constructing paths or resolving names for ordinary values.
		auto HasNestedReplacement(const ObjectPackage::FSerializedValue& Value) -> bool
		{
			return std::ranges::contains(Value.Provenances, ObjectPackage::EPropertyProvenance::Forced)
				|| std::ranges::any_of(Value.Elements, HasNestedReplacement);
		}

		auto RestoreNestedReplacements(const ObjectPackage::FSerializedType& Type,
			const ObjectPackage::FSerializedValue& Value,
			const ObjectPackage::FLinkerTables& Linker, LinkerSchema::FLoadSchemaBindings& SchemaBindings, FAuthoredOverridePath& Path,
			std::vector<FAuthoredOverrideEntry>& Entries,
			FLinkerApplyDiagnostic& Diagnostic) -> bool
		{
			using K = ObjectPackage::EValueKind;
			if (!HasNestedReplacement(Value)) return true;
			if (Type.Kind == K::Struct)
			{
				const auto* Schema = LinkerSchema::FindSchema(Linker, Type.QualifiedName);
				if (!Schema || Value.FieldNames.size() != Value.Elements.size()
					|| Value.Provenances.size() != Value.Elements.size()
					|| ObjectPackage::StructFieldTypes(Type, Value).size() != Value.Elements.size())
					return LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Struct ledger projection is invalid."});
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto It = SchemaBindings.FindProperty(*Schema, Value.FieldNames[Index]);
					if (!It) return LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Struct ledger field is missing."});
					const auto& ChildType = ObjectPackage::StructFieldTypes(Type, Value)[Index];
					const auto Route = It->Match.Route;
					if (Route == EPropertyLoadRoute::Removed || Route == EPropertyLoadRoute::Deprecated) continue;
					Path.push_back(FAuthoredOverridePathToken::Field(FName(Schema->QualifiedName), FName(It->Descriptor->Name)));
					if (Value.Provenances[Index] == ObjectPackage::EPropertyProvenance::Forced)
						Entries.push_back({Path, EAuthoredOverrideProvenance::Forced});
					else if (!RestoreNestedReplacements(ChildType, Value.Elements[Index], Linker, SchemaBindings, Path,
						Entries, Diagnostic)) return false;
					Path.pop_back();
				}
			}
			else if ((Type.Kind == K::FixedArray || Type.Kind == K::Array)
				&& Type.Children.size() == 1)
			{
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					Path.push_back(Type.Kind == K::FixedArray
						? FAuthoredOverridePathToken::FixedArrayElement(Index)
						: FAuthoredOverridePathToken::ArrayElement(Index));
					if (!RestoreNestedReplacements(Type.Children[0], Value.Elements[Index], Linker, SchemaBindings, Path,
						Entries, Diagnostic)) return false;
					Path.pop_back();
				}
			}
			else if (Type.Kind == K::Map && Type.Children.size() == 2)
			{
				if (Value.Elements.size() % 2 != 0) return false;
				for (size_t Index = 0; Index < Value.Elements.size(); Index += 2)
				{
					FByteBuffer Token;
					if (const auto Result = ObjectPackage::BuildCanonicalMapKeyToken(
						Type.Children[0], Value.Elements[Index], Token); !Result)
						return LinkerApplyFail(Diagnostic, UnifiedError::FError{PackageReadErrors::CorruptFile, ObjectPackage::ToString(Result.error())});
					Path.push_back(FAuthoredOverridePathToken::MapValue(std::move(Token)));
					if (!RestoreNestedReplacements(Type.Children[1], Value.Elements[Index + 1], Linker, SchemaBindings, Path,
						Entries, Diagnostic)) return false;
					Path.pop_back();
				}
			}
			return true;
		}

		auto GatherLiveValueDependencies(const ObjectPackage::FSerializedType& Type,
			const ObjectPackage::FSerializedValue& Value, const ObjectPackage::FLinkerTables& Linker, LinkerSchema::FLoadSchemaBindings& SchemaBindings,
			std::vector<FPackagePath>& Dependencies, uint64& DiscardedFields,
			FLinkerApplyDiagnostic& Diagnostic) -> bool
		{
			using K = ObjectPackage::EValueKind;
			if (Type.Kind == K::Struct)
			{
				const auto* Schema = LinkerSchema::FindSchema(Linker, Type.QualifiedName);
				if (!Schema || !FindStructByQualifiedName(FName(Schema->QualifiedName)) || Value.FieldNames.size() != Value.Elements.size()
					|| ObjectPackage::StructFieldTypes(Type, Value).size() != Value.Elements.size())
					return LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::UnsupportedProperty, "Serialized struct is unavailable or has an invalid schema."});
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto Field = SchemaBindings.FindProperty(*Schema, Value.FieldNames[Index]);
					const auto& ChildType = ObjectPackage::StructFieldTypes(Type, Value)[Index];
					if (!Field || Field->Descriptor->Type != ChildType)
						return LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Struct field does not match its declared schema."});
					const auto Route = Field->Match.Route;
					if (Route == EPropertyLoadRoute::Removed) { ++DiscardedFields; continue; }
					if (Route == EPropertyLoadRoute::Incompatible)
						return LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::UnsupportedProperty, std::format("Serialized struct field {}::{} is incompatible with the live schema.",
								Schema->QualifiedName, Field->Descriptor->Name)});
					if (!GatherLiveValueDependencies(ChildType, Value.Elements[Index], Linker, SchemaBindings,
						Dependencies, DiscardedFields, Diagnostic)) return false;
				}
			}
			else if ((Type.Kind == K::Array || Type.Kind == K::FixedArray) && Type.Children.size() == 1)
			{
				for (const auto& Element : Value.Elements)
					if (!GatherLiveValueDependencies(Type.Children[0], Element, Linker, SchemaBindings,
						Dependencies, DiscardedFields, Diagnostic)) return false;
			}
			else if (Type.Kind == K::Map && Type.Children.size() == 2)
			{
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
					if (!GatherLiveValueDependencies(Type.Children[Index % 2], Value.Elements[Index], Linker, SchemaBindings,
						Dependencies, DiscardedFields, Diagnostic)) return false;
			}
			else if (Type.Kind == K::HardReference && Value.Reference.IsImport())
			{
				const ObjectPackage::FPackageImport* Import = nullptr;
				if (!Linker.TryGetImport(Value.Reference, Import) || !Import)
					return LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Hard-reference import is invalid."});
				Dependencies.push_back(Import->ObjectPath.GetPackagePath());
			}
			return true;
		}

		auto GatherNestedDeprecatedRouteEvidence(
			const ObjectPackage::FSerializedType& Type,
			const ObjectPackage::FSerializedValue& Value,
			const ObjectPackage::FLinkerTables& Linker, LinkerSchema::FLoadSchemaBindings& SchemaBindings,
			const FPackagePath& PackagePath,
			std::string_view ObjectPath, uint64& Count, std::vector<FAssetDeprecatedRouteEvidence>* Out) -> void
		{
			using K = ObjectPackage::EValueKind;
			if (Type.Kind == K::Struct)
			{
				const auto* Schema = LinkerSchema::FindSchema(Linker, Type.QualifiedName);
				if (!Schema || Value.FieldNames.size() != Value.Elements.size()
					|| ObjectPackage::StructFieldTypes(Type, Value).size() != Value.Elements.size()) return;
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto Field = SchemaBindings.FindProperty(*Schema, Value.FieldNames[Index]);
					if (!Field) continue;
					const auto& ChildType = ObjectPackage::StructFieldTypes(Type, Value)[Index];
					const auto& Binding = Field->Match;
					if (Binding.Route == EPropertyLoadRoute::Removed) continue;
					if (Binding.Route == EPropertyLoadRoute::Deprecated)
					{
						++Count;
						if (Out) Out->push_back({
							.PackagePath = PackagePath, .ObjectPath = std::string(ObjectPath),
							.DeclaringType = Schema->QualifiedName, .StoredFieldName = Field->Descriptor->Name,
							.DeprecatedPropertyName = Binding.Property->NamePrivate.ToString()});
					}
					else GatherNestedDeprecatedRouteEvidence(ChildType, Value.Elements[Index],
						Linker, SchemaBindings, PackagePath, ObjectPath, Count, Out);
				}
			}
			else if ((Type.Kind == K::FixedArray || Type.Kind == K::Array)
				&& Type.Children.size() == 1)
			{
				for (const auto& Element : Value.Elements)
					GatherNestedDeprecatedRouteEvidence(Type.Children[0], Element, Linker, SchemaBindings,
						PackagePath, ObjectPath, Count, Out);
			}
			else if (Type.Kind == K::Map && Type.Children.size() == 2)
			{
				for (size_t Index = 1; Index < Value.Elements.size(); Index += 2)
					GatherNestedDeprecatedRouteEvidence(Type.Children[1], Value.Elements[Index], Linker, SchemaBindings,
						PackagePath, ObjectPath, Count, Out);
			}
		}

		struct FExportView
		{
			const ObjectPackage::FPackageExport* Export = nullptr;
			DClass* Class = nullptr;
			std::string Path;
			uint64 OuterId = 0;
		};

		auto BuildExportViews(const ObjectPackage::FLinkerTables& Linker,
			std::vector<FExportView>& Out, FLinkerApplyDiagnostic& Diagnostic) -> bool
		{
			std::vector<FExportView> Views;
			Views.reserve(Linker.Exports.size());
			for (size_t Index = 0; Index < Linker.Exports.size(); ++Index)
			{
				ObjectPackage::FPackageIndex PackageIndex;
				std::string Path;
				if (!ObjectPackage::FPackageIndex::TryExport(Index, PackageIndex)
					|| !Linker.TryResolvePath(PackageIndex, Path))
					return LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "An export path cannot be resolved."});
				const auto& Export = Linker.Exports[Index];
				uint64 OuterId = 0;
				if (!Export.Outer.IsNull())
				{
					if (!Export.Outer.IsExport() || Export.Outer.GetTableIndex() >= Index)
						return LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Export Outer topology is not constructible in table order."}, 0, Path);
					OuterId = static_cast<uint64>(Export.Outer.GetTableIndex() + 1);
				}
				Views.push_back({&Export, nullptr, std::move(Path), OuterId});
			}
			Out = std::move(Views);
			return true;
		}
		// Runtime decisions use counts. Detailed records exist only under an audit scope.
		struct FLinkerCompatibilityEvidence
		{
			uint64 CanonicalizedIdentityCount = 0;
			uint64 DeprecatedFieldRouteCount = 0;
			uint64 DiscardedFieldCount = 0;
			std::optional<FAssetLoadAudit> Details;
			FLinkerCompatibilityEvidence()
			{
				if (GetActiveAssetLoadAudit()) Details.emplace();
			}
			auto TransferTo(FAssetLoadReport& Report) -> void
			{
				Report.CanonicalizedIdentityCount = CanonicalizedIdentityCount;
				Report.DeprecatedFieldRouteCount = DeprecatedFieldRouteCount;
				Report.DiscardedFieldCount = DiscardedFieldCount;
				if (auto* Audit = GetActiveAssetLoadAudit(); Audit && Details)
				{
					Audit->CanonicalizationEvidence.insert(Audit->CanonicalizationEvidence.end(),
						std::make_move_iterator(Details->CanonicalizationEvidence.begin()),
						std::make_move_iterator(Details->CanonicalizationEvidence.end()));
					Audit->DeprecatedRouteEvidence.insert(Audit->DeprecatedRouteEvidence.end(),
						std::make_move_iterator(Details->DeprecatedRouteEvidence.begin()),
						std::make_move_iterator(Details->DeprecatedRouteEvidence.end()));
				}
			}
		};

		// Shared decoded-package phases; publication and PostLoad belong to callers.
		struct FLinkerApplication
		{
			ObjectPackage::FLinkerTables Linker;
			std::optional<LinkerSchema::FLoadSchemaBindings> SchemaBindings;
			std::vector<FExportView> Exports;
			std::vector<FPackagePath> LiveDependencies;
			std::vector<DObject*> Objects;
			const std::unordered_set<DObject*>* ExistingObjects = nullptr;
			auto IsExisting(DObject* Object) const -> bool
			{
				return ExistingObjects && ExistingObjects->contains(Object);
			}
			DPackage* Package = nullptr;
			FLinkerCompatibilityEvidence Compatibility;
		};

		auto ValidateLinker(FLinkerApplication& Application, const FLinkerLoadOptions& Options,
			FLinkerApplyDiagnostic& Diagnostic) -> std::expected<void, Durin::UnifiedError::FError>
		{
			FScopedObjectLoadPhase Phase(Application.Linker.Summary.PackagePath, EObjectLoadPhase::ValidateSchema);
			auto& Linker = Application.Linker;
			std::string VersionError;
			if (!FCustomVersionRegistry::Validate(Linker.CustomVersions, VersionError))
			{
				LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::PackageReadErrors::UnsupportedVersion, VersionError});
				return std::unexpected(*Diagnostic.Cause);
			}
			if (auto Result = LinkerSchema::FixupSerializedNames(
				Linker, Application.Compatibility.CanonicalizedIdentityCount,
				Application.Compatibility.Details ? &Application.Compatibility.Details->CanonicalizationEvidence : nullptr); !Result)
			{
				LinkerApplyFail(Diagnostic, Result.error());
				return Result;
			}
            for (const auto* Dependencies : {&Linker.Summary.HardPackageDependencies,
                &Linker.Summary.SoftPackageDependencies, &Linker.Summary.BuildPackageDependencies})
                for (const auto& Dependency : *Dependencies)
                    if (Dependency.GetView().starts_with("/Plugins/")
                        && !FMountPaths::FindMountForVirtualPath(Dependency.GetView()))
                    {
                        LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::MissingDependency, "Serialized plugin reference '" + Dependency.ToString()
                            + "' has no active mount. Enable the required plugin and restart before editing or saving this package."});
                        return std::unexpected(*Diagnostic.Cause);
                    }
			Application.SchemaBindings.emplace(Linker);
			auto& SchemaBindings = *Application.SchemaBindings;
			auto& Exports = Application.Exports;
			if (!BuildExportViews(Linker, Exports, Diagnostic))
				return std::unexpected(*Diagnostic.Cause);
			if (FObjectLoadTimingSession::IsActive())
			{
				uint64 PropertyCount = 0;
				for (const auto& Export : Exports) PropertyCount += Export.Export->Properties.size();
				FObjectLoadTimingSession::RecordPackage(Linker.Summary.PackagePath, Exports.size(), PropertyCount);
			}
			uint64 DiscardedFields = 0;
			auto& LiveDependencies = Application.LiveDependencies;
			for (FExportView& Object : Exports)
			{
				DClass* Class = Object.Class = FindClassByQualifiedName(FName(Object.Export->ClassName));
				if (!Class)
				{
					LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::UnknownClass, std::format("Serialized class '{}' is unavailable. Enable its required plugin and restart, or restore its native provider, before editing or saving this package.", Object.Export->ClassName)}, 0, Object.Path);
					return std::unexpected(*Diagnostic.Cause);
				}
				for (const ObjectPackage::FPropertyTag& Property : Object.Export->Properties)
				{
					const auto* Schema = LinkerSchema::FindSchema(Linker, Property.DeclaringType);
					const auto Field = Schema ? SchemaBindings.FindProperty(*Schema, Property.FieldName) : nullptr;
					if (!Schema || !Field || Field->Descriptor->Type != Property.Type)
					{
						LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "A linker property is absent from its declared schema."}, 0, Object.Path);
						return std::unexpected(*Diagnostic.Cause);
					}
					DClass* DeclaringClass = FindClassByQualifiedName(FName(Schema->QualifiedName));
					bool bDeclaringClassMatches = false;
					for (DClass* Ancestor = Class; Ancestor; Ancestor = Ancestor->GetSuperClass())
						if (Ancestor == DeclaringClass)
						{
							bDeclaringClassMatches = true;
							break;
						}
					const auto& Binding = Field->Match;
					if (!Options.bCooked && bDeclaringClassMatches && Binding.Route == EPropertyLoadRoute::Removed)
					{
						++DiscardedFields;
						continue;
					}
					// Cooked native fields still require exact Archive manifest consumption.
					const bool bCookedNativeCandidate = Options.bCooked
						&& (!bDeclaringClassMatches || !Binding.bHasCurrentProperty);
					const bool bCompatible = (bDeclaringClassMatches && Binding.Route == EPropertyLoadRoute::Current)
						|| Binding.Route == EPropertyLoadRoute::Deprecated || bCookedNativeCandidate;
					if (!bCompatible)
					{
						LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::UnsupportedProperty, std::format("Serialized field {}::{} is incompatible with the live schema.",
								Schema->QualifiedName, Field->Descriptor->Name)}, 0, Object.Path);
						return std::unexpected(*Diagnostic.Cause);
					}
					if (!Options.bCooked && !GatherLiveValueDependencies(Property.Type, Property.Value,
						Linker, SchemaBindings, LiveDependencies, DiscardedFields, Diagnostic))
						return std::unexpected(*Diagnostic.Cause);
				}
			}
			std::ranges::sort(LiveDependencies);
			LiveDependencies.erase(std::ranges::unique(LiveDependencies).begin(), LiveDependencies.end());
			if (Exports.empty())
			{
				LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Package has no object exports."});
				return std::unexpected(*Diagnostic.Cause);
			}

			Application.Compatibility.DiscardedFieldCount = DiscardedFields;
			return {};
		}

		auto CreateLinkerSkeleton(FLinkerApplication& Application, const FLinkerLoadOptions& Options,
			FLinkerApplyDiagnostic& Diagnostic, std::vector<FStrongObjectPtr>* Pins = nullptr) -> std::expected<void, Durin::UnifiedError::FError>
		{
			FScopedObjectLoadPhase Phase(Application.Linker.Summary.PackagePath, EObjectLoadPhase::CreateExports, Application.Exports.size());
			auto& Exports = Application.Exports;
			auto& Objects = Application.Objects;
			auto* Package = Application.Package;
			std::unordered_set<DObject*> Pinned;
			for (size_t Index = 0; Index < Exports.size(); ++Index)
			{
				if (ShouldFail(Options, ELinkerLoadPhase::CreateSkeleton, Index))
				{
					LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Injected skeleton creation failure."});
					return std::unexpected(*Diagnostic.Cause);
				}
				const FExportView& Descriptor = Exports[Index];
				DClass* Class = Descriptor.Class;
				if (!Class || !Class->ClassConstructor)
				{
					LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::UnknownClass, std::format("Serialized class '{}' is unavailable. Enable its required plugin and restart, or restore its native provider, before editing or saving this package.", Descriptor.Export->ClassName)}, 0, Descriptor.Path);
					return std::unexpected(*Diagnostic.Cause);
				}
				DObject* Outer = Descriptor.OuterId == 0 ? static_cast<DObject*>(Package)
					: Objects[static_cast<size_t>(Descriptor.OuterId - 1)];
				bool bTypeMismatch = false;
				DObject* Object = FindExistingInner(Outer, Descriptor.Export->ObjectName, Class, bTypeMismatch);
				if (bTypeMismatch)
				{
					LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::TypeMismatch, "Existing default inner has a different class."}, 0, Descriptor.Path);
					return std::unexpected(*Diagnostic.Cause);
				}
				if (!Object)
				{
					FStaticConstructObjectParameters Parameters{
						Class, Outer, FName(Descriptor.Export->ObjectName), Class->GetStructureSize(),
						Descriptor.Export->ObjectFlags};
					Parameters.Purpose = EObjectConstructionPurpose::AssetLoad;
					Object = StaticConstructObject(Parameters);
					if (Object) DObjectForceRegistration(Object);
				}
				else if (!Application.IsExisting(Object))
				{
					// The outer constructor may have created this default subobject already.
					Object->ClearFlags(EObjectFlags::Standalone | EObjectFlags::Transactional | EObjectFlags::Public);
					Object->SetFlags(Descriptor.Export->ObjectFlags);
				}
				if (!Object)
				{
					LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Object construction failed."}, 0, Descriptor.Path);
					return std::unexpected(*Diagnostic.Cause);
				}
				Objects[Index] = Object;
				if (Pins && Pinned.insert(Object).second)
				{
					const size_t Begin = Pins->size();
					Pins->emplace_back(Object);
					for (size_t PinIndex = Begin; PinIndex < Pins->size(); ++PinIndex)
						for (DObject* Child : GDObjectArray.GetObjectsWithOuter((*Pins)[PinIndex].Get(), EObjectQueryScope::IncludeUnpublished))
							if (Pinned.insert(Child).second) Pins->emplace_back(Child);
				}
			}
			return {};
		}

		auto ApplyLinkerValues(FLinkerApplication& Application, const FLinkerLoadOptions& Options,
			FLinkerApplyDiagnostic& Diagnostic, const FPackageLoadBindings& Bindings) -> std::expected<void, Durin::UnifiedError::FError>
		{
			const auto& PackagePath = Application.Linker.Summary.PackagePath;
			uint64 PropertyCount = 0;
			for (const auto& Export : Application.Exports) PropertyCount += Export.Export->Properties.size();
			FScopedObjectLoadPhase Phase(PackagePath, EObjectLoadPhase::Preload, Application.Objects.size(), PropertyCount);
			const auto& Linker = Application.Linker;
			auto& SchemaBindings = *Application.SchemaBindings;
			auto& Exports = Application.Exports;
			auto& Objects = Application.Objects;
			auto& Report = Application.Compatibility;
			auto ObjectBindings = Bindings;
			ObjectBindings.SchemaBindings = &SchemaBindings;
			if (!Options.bCooked)
			{
				FScopedObjectLoadPhase DefaultsTiming(PackagePath, EObjectLoadPhase::InitDefaults, Objects.size());
				std::vector<DClass*> Classes;
				std::unordered_set<DObject*> NeedsDefaults;
				for (size_t Index = 0; Index < Objects.size(); ++Index)
					if (Exports[Index].Export->bUseClassDefaults && !Application.IsExisting(Objects[Index]))
					{
						NeedsDefaults.insert(Objects[Index]);
						Classes.push_back(Objects[Index]->GetClass());
					}
				if (![&] {
					FScopedObjectLoadPhase Timing(PackagePath, EObjectLoadPhase::CreateClassDefaults, Classes.size());
					return Private::CreateClassDefaultObjectsForBatch(Classes);
				}())
					return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Default object initialization failed."});
				std::unordered_set<DObject*> Initialized;
				for (DObject* Root : Objects)
				{
					if (!NeedsDefaults.contains(Root) || Initialized.contains(Root)) continue;
					const DObject* Default = Root->GetClass()->GetDefaultObject();
					std::optional<FScopedObjectLoadPhase> MatchTiming(std::in_place,
						PackagePath, EObjectLoadPhase::MatchDefaultSubobjects, 1);
					FDefaultObjectGraphMap Graph;
					FDefaultObjectGraphDiagnostic GraphDiagnostic;
					if (!Default || !Graph.Build(Default, Root, &GraphDiagnostic))
						return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Cannot pair loaded object defaults: " + GraphDiagnostic.LogicalPath});
					std::unordered_map<DObject*, DObject*> References;
					for (DObject* Object : Objects)
						if (const DObject* Template = Graph.FindTemplate(Object))
							References.emplace(const_cast<DObject*>(Template), Object);
					MatchTiming.reset();
					for (DObject* Object : Objects)
						if (const DObject* Template = Graph.FindTemplate(Object))
						{
							if (!NeedsDefaults.contains(Object)) continue;
							FScopedObjectLoadPhase CopyTiming(PackagePath, EObjectLoadPhase::CopyDefaultProperties, 1);
							const auto CopyResult = InitializeObjectFromDefaults(Template, Object, References);
							if (!CopyResult)
								return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Cannot initialize loaded defaults: " + ToString(CopyResult.error())});
							Initialized.insert(Object);
						}
				}
			}
			{
				FScopedObjectLoadPhase MetadataTiming(PackagePath, EObjectLoadPhase::RestoreEditorState);
				{
				FScopedObjectLoadPhase VersionsTiming(PackagePath, EObjectLoadPhase::RestoreCustomVersions, Objects.size());
				std::vector<std::pair<FGuid, int32>> LoadedCustomVersions;
				for (const ObjectPackage::FCustomVersion& Version : Linker.CustomVersions)
				{
					LoadedCustomVersions.emplace_back(Version.Guid, Version.Version);
				}
				for (DObject* Object : Objects)
					if (!Application.IsExisting(Object)) Object->SetLoadedCustomVersions(LoadedCustomVersions);
				}
				FScopedObjectLoadPhase CompatibilityTiming(PackagePath, EObjectLoadPhase::CollectCompatibilityEvidence, Objects.size(), PropertyCount);
				for (size_t ObjectIndex = 0; ObjectIndex < Exports.size(); ++ObjectIndex)
					for (const auto& Property : Exports[ObjectIndex].Export->Properties)
					{
						const auto Field = SchemaBindings.FindProperty(
							*LinkerSchema::FindSchema(Linker, Property.DeclaringType), Property.FieldName);
						if (!Field)
						{
							LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::UnsupportedProperty, "Reflection metadata changed during package loading."}, 0, Exports[ObjectIndex].Path);
							return std::unexpected(*Diagnostic.Cause);
						}
						if (!Options.bCooked && Field->Match.Route == EPropertyLoadRoute::Removed) continue;
						GatherNestedDeprecatedRouteEvidence(Property.Type, Property.Value, Linker, SchemaBindings,
							PackagePath, Exports[ObjectIndex].Path,
							Report.DeprecatedFieldRouteCount,
							Report.Details ? &Report.Details->DeprecatedRouteEvidence : nullptr);
					}
			}
			for (size_t ObjectIndex = 0; ObjectIndex < Objects.size(); ++ObjectIndex)
			{
				if (Application.IsExisting(Objects[ObjectIndex])) continue;
				if (ShouldFail(Options, ELinkerLoadPhase::ApplyValues, ObjectIndex))
				{
					LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Injected value application failure."});
					return std::unexpected(*Diagnostic.Cause);
				}
				FArchiveState LoadContext;
				LoadContext.bCooking = Options.bCooked;
				LoadContext.bFilterEditorOnly = Options.bCooked;
				LoadContext.Target = Options.Target;
				const FPackageOverrideRestore RestoreOverrides = [&](std::unique_ptr<FOverriddenPropertyRestoreContext> Restoration) -> std::expected<void, Durin::UnifiedError::FError> {
					const bool bFastRestoration = Restoration && Restoration->IsEligible();
					FScopedObjectLoadPhase EditorTiming(PackagePath, EObjectLoadPhase::RestoreEditorState, 1, Exports[ObjectIndex].Export->Properties.size());
					std::optional<FScopedObjectLoadPhase> LedgerTiming(std::in_place, PackagePath, EObjectLoadPhase::BuildOverrideLedger, 1, Exports[ObjectIndex].Export->Properties.size());
					std::vector<FAuthoredOverrideEntry> LedgerEntries;
					std::vector<FName> LoadedDeprecatedProperties;
					size_t PropertyIndex = 0;
					for (const auto& Property : Exports[ObjectIndex].Export->Properties)
					{
						const auto Field = SchemaBindings.FindProperty(
							*LinkerSchema::FindSchema(Linker, Property.DeclaringType), Property.FieldName);
						if (!Field) return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::UnsupportedProperty, "Reflection metadata changed during package loading."});
						const auto& Binding = Field->Match;
						if (Binding.Route == EPropertyLoadRoute::Removed) continue;
						if (ShouldFail(Options, ELinkerLoadPhase::RestoreLedger, ++PropertyIndex))
						{
							LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Injected ledger restoration failure."});
							return std::unexpected(*Diagnostic.Cause);
						}
						if (Binding.Route == EPropertyLoadRoute::Deprecated)
						{
							LoadedDeprecatedProperties.push_back(Binding.Property->NamePrivate);
							++Report.DeprecatedFieldRouteCount;
							if (Report.Details) Report.Details->DeprecatedRouteEvidence.push_back({
								.PackagePath = PackagePath,
								.ObjectPath = Exports[ObjectIndex].Path,
								.DeclaringType = Property.DeclaringType,
								.StoredFieldName = Property.FieldName,
								.DeprecatedPropertyName = Binding.Property->NamePrivate.ToString()});
							continue;
						}
						if (bFastRestoration) continue;
						if (Property.Provenance != ObjectPackage::EPropertyProvenance::Forced
							&& !HasNestedReplacement(Property.Value)) continue;
						FAuthoredOverridePath Path{FAuthoredOverridePathToken::Field(
							FName(Property.DeclaringType), FName(Property.FieldName))};
						if (Property.Provenance == ObjectPackage::EPropertyProvenance::Forced)
							LedgerEntries.push_back({Path, EAuthoredOverrideProvenance::Forced});
						else if (!RestoreNestedReplacements(Property.Type, Property.Value, Linker, SchemaBindings, Path,
							LedgerEntries, Diagnostic))
						{
							return std::unexpected(*Diagnostic.Cause);
						}
					}
					LedgerTiming.reset();
					FScopedObjectLoadPhase ApplyLedgerTiming(PackagePath, EObjectLoadPhase::ApplyOverrideLedger, 1, bFastRestoration ? Restoration->GetCandidateCount() : LedgerEntries.size());
					Objects[ObjectIndex]->SetLoadedDeprecatedProperties(LoadedDeprecatedProperties);
					FAuthoredOverrideDiagnostic LedgerDiagnostic;
					FAuthoredOverrideValidationWork Work;
					const auto Reason = Restoration ? Restoration->GetFallbackReason() : EOverrideRestoreFallbackReason::ArchiveFailure;
					bool bRestored = false;
					if (bFastRestoration)
					{
						if (FObjectLoadTimingSession::IsActive()) Work.PathVisits = Restoration->GetPathVisits();
						bRestored = FOverridableManager::RestoreOverriddenPropertyOperation(std::move(Restoration), &LedgerDiagnostic);
					}
					else bRestored = FOverridableManager::RestoreOverriddenPropertyOperation(*Objects[ObjectIndex], LedgerEntries, &LedgerDiagnostic,
						FObjectLoadTimingSession::IsActive() ? &Work : nullptr);
					FObjectLoadTimingSession::RecordOverrideRestoration(PackagePath, bFastRestoration, Reason, Work);
					if (!bRestored)
					{
						LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Could not restore authored intent."}, 0, LedgerDiagnostic.LogicalPath);
						return std::unexpected(*Diagnostic.Cause);
					}
					return {};
				};
				auto Loaded = LoadPackageObject(*Objects[ObjectIndex], Exports[ObjectIndex].Export->Properties, Linker, Objects,
					ObjectBindings, LoadContext, Options.bCooked ? FPackageOverrideRestore{} : RestoreOverrides);
				if (!Loaded)
				{
					LinkerApplyFail(Diagnostic, Loaded.error(), 0, Exports[ObjectIndex].Path);
					return Loaded;
				}
			}
			return {};
		}

	}

	struct FPreparedPackageGraph::FState
	{
		DPackage* Package = nullptr;
		std::vector<FStrongObjectPtr> Pins;
		FPreparedPackageResource Storage;
		FAssetLoadReport Report;

		~FState()
		{
			if (Package && Package->IsGraphPrivate()) MarkObjectHierarchyAsGarbage(Package);
		}
	};

	FPreparedPackageGraph::FPreparedPackageGraph() = default;
	FPreparedPackageGraph::~FPreparedPackageGraph() = default;
	FPreparedPackageGraph::FPreparedPackageGraph(FPreparedPackageGraph&&) noexcept = default;
	auto FPreparedPackageGraph::operator=(FPreparedPackageGraph&&) noexcept -> FPreparedPackageGraph& = default;
	auto FPreparedPackageGraph::GetPackage() const -> DPackage* { return State ? State->Package : nullptr; }
	auto FPreparedPackageGraph::GetStorage() const -> const FPreparedPackageResource& { require(State); return State->Storage; }
	auto FPreparedPackageGraph::GetReport() const -> const FAssetLoadReport& { require(State); return State->Report; }

	namespace
	{
		auto ValidateLoadedGraphs(std::span<DObject* const> Objects, const FObjectGraphLoadContext& Context)
			-> std::expected<void, Durin::UnifiedError::FError>
		{
			FAssetLiveLoadGuard Guard(true);
			for (const DObject* Object : Objects)
			{
				const auto Validation = Object->ValidateLoadedObjectGraph(Context);
				if (!Validation)
				{
					std::expected<void, Durin::UnifiedError::FError> Result{std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, std::format("Loaded graph '{}': {}",
						Object->GetObjectPath(), ToString(Validation.error()))})};
					return Result;
				}
			}
			return Guard.GetFailure();
		}
	}

	auto PreparePackageGraphs(std::span<const FPackageGraphSource> Sources,
		const FPackageGraphPrepareOptions& Options, std::vector<FPreparedPackageGraph>& Out)
		-> FPackageGraphPrepareResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		using S = EPackageGraphPrepareStatus;
		using R = EPackageGraphPrepareReason;
		static bool bPreparing = false;
		if (bPreparing) return {.Status = S::Busy, .Reason = R::Reentrant};
		if (!FAssetRuntimeState::Get().GetLoadService().IsIdle())
			return {.Status = S::Busy, .Reason = R::LoadServiceBusy};
		struct FExecutionScope
		{
			bool& Active;
			explicit FExecutionScope(bool& InActive) : Active(InActive) { Active = true; }
			~FExecutionScope() { Active = false; }
		} Execution(bPreparing);
		if (Sources.empty() || Sources.size() > Options.MaximumPackages)
			return {.Status = S::BudgetExceeded, .Reason = R::PackageBudget, .Actual = Sources.size(), .Maximum = Options.MaximumPackages};
		bool bCancelled = false;
		auto Cancelled = [&]() {
			bCancelled = bCancelled || (Options.IsCancelled && Options.IsCancelled());
			return bCancelled;
		};
		FPackagePath CurrentPath;
		try
		{
			uint64 RetainedBytes = 0;
			for (const auto& Source : Sources)
			{
				if (Source.Storage.GetRetainedBytes() > Options.MaximumRetainedBytes - RetainedBytes)
					return {.Status = S::BudgetExceeded, .PackagePath = Source.PackagePath, .Reason = R::RetainedByteBudget, .Actual = Source.Storage.GetRetainedBytes(), .Maximum = Options.MaximumRetainedBytes - RetainedBytes};
				RetainedBytes += Source.Storage.GetRetainedBytes();
			}
			std::vector<FLinkerApplication> Applications(Sources.size());
			std::vector<FPreparedPackageGraph> Candidates(Sources.size());
			uint64 ObjectCount = 0;
			// Validate the entire batch before invoking any asset constructor.
			for (size_t Index = 0; Index < Sources.size(); ++Index)
			{
				const auto& Source = Sources[Index];
				CurrentPath = Source.PackagePath;
				if (Cancelled()) return {.Status = S::Cancelled, .PackagePath = CurrentPath, .Reason = R::Cancelled};
				if (!CurrentPath.IsValid() || Source.Storage.GetMainBytes().IsEmpty())
					return {.Status = S::InvalidClosure, .PackagePath = CurrentPath, .Reason = R::InvalidSource};
				if (IsAssetRegistryProjectionFenced(CurrentPath))
					return {.Status = S::Busy, .PackagePath = CurrentPath, .Reason = R::ProjectionFenced};
				// An ordinary external load may recurse into the replacement set.
				// Require its live skeletons so that recursion cannot publish a target.
				if (Options.DependencyLoadScope && !FindResidentPackage(CurrentPath))
					return {.Status = S::Unsupported, .PackagePath = CurrentPath, .Reason = R::ResidentTargetRequired};
				for (size_t Previous = 0; Previous < Index; ++Previous)
					if (Sources[Previous].PackagePath == CurrentPath)
						return {.Status = S::InvalidClosure, .PackagePath = CurrentPath, .Reason = R::DuplicatePackage};
				auto& Application = Applications[Index];
				ObjectPackage::FPackageReaderResult ReaderDiagnostic;
				const auto& Bulk = Source.Storage.GetBulkResource();
				if (!(ReaderDiagnostic = ObjectPackage::ReadPackageMetadata(Source.Storage.GetMainBytes(),
					Bulk ? Bulk->GetSegmentExtent() : 0, CurrentPath, Application.Linker)))
					return {.Status = S::InvalidClosure, .PackagePath = CurrentPath, .Reason = R::Reader, .Message = ObjectPackage::FormatPackageError(ReaderDiagnostic)};
				FLinkerApplyDiagnostic Diagnostic;
				if (auto Result = ValidateLinker(Application, {}, Diagnostic); !Result)
					return {.Status = S::InvalidClosure, .PackagePath = CurrentPath, .Reason = R::LinkerValidation, .Message = Result.error().ToString(), .Cause = Result.error()};
				if (ObjectCount >= Options.MaximumObjects
					|| Application.Exports.size() > Options.MaximumObjects - ObjectCount - 1)
					return {.Status = S::BudgetExceeded, .PackagePath = CurrentPath, .Reason = R::ObjectBudget, .Actual = Application.Exports.size() + 1, .Maximum = Options.MaximumObjects - ObjectCount};
				ObjectCount += Application.Exports.size() + 1;
				for (const auto& Export : Application.Exports)
				{
					DClass* Class = Export.Class;
					if (!std::ranges::contains(Options.AdmittedClasses, Class))
						return {.Status = S::Unsupported, .PackagePath = CurrentPath, .Reason = R::ClassNotAdmitted, .Subject = Export.Export->ClassName};
				}
			}
			// Admit external residency before constructing any candidate. Only the
			// supplied scope may own new loads; callers retain it even on failure.
			std::vector<std::pair<FPackagePath, DPackage*>> ExternalPackages;
			std::vector<FStrongObjectPtr> ExternalPins;
			for (size_t PackageIndex = 0; PackageIndex < Applications.size(); ++PackageIndex)
			{
				const auto& Application = Applications[PackageIndex];
				for (size_t DependencyIndex = 0; DependencyIndex < Application.LiveDependencies.size(); ++DependencyIndex)
				{
					const auto& Path = Application.LiveDependencies[DependencyIndex];
					CurrentPath = Application.Linker.Summary.PackagePath;
					if (Cancelled()) return {.Status = S::Cancelled, .PackagePath = Application.Linker.Summary.PackagePath, .Reason = R::Cancelled};
					if (Options.ShouldFail && Options.ShouldFail(PackageIndex, ELinkerLoadPhase::ResolveDependency, DependencyIndex))
						return {.Status = S::MissingDependency, .PackagePath = Application.Linker.Summary.PackagePath, .Reason = R::InjectedDependencyFailure};
					if (IsAssetRegistryProjectionFenced(Path))
						return {.Status = S::Busy, .PackagePath = Path, .Reason = R::DependencyProjectionFenced};
					if (std::ranges::contains(Sources, Path, &FPackageGraphSource::PackagePath)) continue;
					if (std::ranges::contains(ExternalPackages, Path, &std::pair<FPackagePath, DPackage*>::first)) continue;
					DPackage* Package = FindPackage(Path.GetView());
					if (!Package && Options.DependencyLoadScope)
					{
						if (IsPackageLoading(Path)) return {.Status = S::Busy, .PackagePath = Path, .Reason = R::DependencyLoad};
						const auto Result = Options.DependencyLoadScope->LoadPackageDetailed(Path);
						Package = Result.value_or(nullptr);
						if (!Result) return {.Status = S::MissingDependency, .PackagePath = Path, .Reason = R::DependencyLoad, .Message = Result.error().ToString(), .Cause = Result.error()};
					}
					if (!Package) return {.Status = S::MissingDependency, .PackagePath = Application.Linker.Summary.PackagePath, .Reason = R::DependencyNotResident, .Subject = Path.ToString()};
					ExternalPackages.emplace_back(Path, Package);
					const size_t Begin = ExternalPins.size();
					ExternalPins.emplace_back(Package);
					for (size_t PinIndex = Begin; PinIndex < ExternalPins.size(); ++PinIndex)
						for (DObject* Child : GDObjectArray.GetObjectsWithOuter(ExternalPins[PinIndex].Get(), EObjectQueryScope::LiveOnly))
							ExternalPins.emplace_back(Child);
				}
			}
			// Explicit external loads have finished. Constructors, serializers and
			// migration callbacks must now use only the admitted bindings. A callback
			// that ignores a rejected live operation still invalidates the batch.
			FAssetLiveLoadGuard LiveLoadGuard(true);
			auto LoadOptions = [&](size_t PackageIndex) {
				FLinkerLoadOptions Result;
				Result.ShouldFail = [&, PackageIndex](ELinkerLoadPhase Phase, uint64 ObjectIndex) {
					return Cancelled() || (Options.ShouldFail && Options.ShouldFail(PackageIndex, Phase, ObjectIndex));
				};
				return Result;
			};
			ObjectCount = 0;
			for (size_t Index = 0; Index < Applications.size(); ++Index)
			{
				auto& Application = Applications[Index];
				CurrentPath = Application.Linker.Summary.PackagePath;
				if (Cancelled()) return {.Status = S::Cancelled, .PackagePath = CurrentPath, .Reason = R::Cancelled};
				auto& State = Candidates[Index].State;
				State = std::make_unique<FPreparedPackageGraph::FState>();
				State->Storage = Sources[Index].Storage;
				State->Package = NewObject<DPackage>(nullptr, FName(CurrentPath.GetPackageName()));
				if (!State->Package || !State->Package->InitializePreparedAssetPackage(CurrentPath))
					return {.Status = S::InvalidClosure, .PackagePath = CurrentPath, .Reason = R::PackageConstruction};
				State->Pins.emplace_back(State->Package);
				Application.Package = State->Package;
				Application.Objects.resize(Application.Exports.size());
				FLinkerApplyDiagnostic Diagnostic;
				if (auto Result = CreateLinkerSkeleton(Application, LoadOptions(Index), Diagnostic, &State->Pins); !Result)
					return {.Status = Cancelled() ? S::Cancelled : S::InvalidClosure, .PackagePath = CurrentPath, .Reason = R::Skeleton, .Message = Result.error().ToString(), .Cause = Result.error()};
				if (State->Pins.size() > Options.MaximumObjects - ObjectCount)
					return {.Status = S::BudgetExceeded, .PackagePath = CurrentPath, .Reason = R::DefaultInnerBudget, .Actual = State->Pins.size(), .Maximum = Options.MaximumObjects - ObjectCount};
				ObjectCount += State->Pins.size();
				State->Pins.insert(State->Pins.end(), ExternalPins.begin(), ExternalPins.end());
			}
			auto Resolve = [&](const FSoftObjectPath& Path, DObject*& Object) -> std::expected<void, Durin::UnifiedError::FError> {
				Object = nullptr;
				DPackage* Package = nullptr;
				for (const auto& Application : Applications)
					if (Application.Linker.Summary.PackagePath == Path.GetPackagePath()) Package = Application.Package;
				if (!Package)
					for (const auto& External : ExternalPackages)
						if (External.first == Path.GetPackagePath()) Package = External.second;
				if (!Package) return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::MissingDependency, "Object reference has no admitted package binding."});
				DObject* Current = Package;
				auto Descend = [&](std::string_view Name) {
					if (!Current) return;
					const auto Children = GDObjectArray.GetObjectsWithOuter(Current, EObjectQueryScope::IncludeUnpublished);
					const auto It = std::ranges::find(Children, FName(Name), &DObject::GetFName);
					Current = It == Children.end() ? nullptr : *It;
				};
				Descend(Path.GetAssetPath().GetAssetName());
				for (std::string_view Name : Path.GetSubobjectNames()) Descend(Name);
				Object = Current;
				return Object ? std::expected<void, Durin::UnifiedError::FError>{} : std::expected<void, Durin::UnifiedError::FError>{std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::MissingDependency, "Admitted package has no matching object."})};
			};
			for (size_t Index = 0; Index < Applications.size(); ++Index)
			{
				auto& Application = Applications[Index];
				CurrentPath = Application.Linker.Summary.PackagePath;
				const FPackageLoadBindings Bindings{Sources[Index].Storage.GetBulkResource(), Resolve};
				FLinkerApplyDiagnostic Diagnostic;
				if (auto Result = ApplyLinkerValues(Application, LoadOptions(Index), Diagnostic, Bindings); !Result)
					return {.Status = Cancelled() ? S::Cancelled : S::InvalidClosure, .PackagePath = CurrentPath, .Reason = R::ApplyValues, .Message = Result.error().ToString(), .Cause = Result.error()};
				Application.Package->RecordDiskExports({Application.Objects.begin(), Application.Objects.end()}, Sources[Index].Storage.GetMainDigest());
				Application.Package->ClearDirty();
				Application.Package->SetCanonicalResaveRecommended(Application.Package->IsCanonicalResaveRecommended()
					|| Application.Compatibility.CanonicalizedIdentityCount != 0
					|| Application.Compatibility.DeprecatedFieldRouteCount != 0 || Application.Compatibility.DiscardedFieldCount != 0);
				Candidates[Index].State->Report.PackagePath = CurrentPath;
				Application.Compatibility.TransferTo(Candidates[Index].State->Report);
			}
			// Cross-package references in this prepared batch now see restored values.
			for (const auto& Application : Applications)
			{
				CurrentPath = Application.Linker.Summary.PackagePath;
				if (auto Result = ValidateLoadedGraphs(Application.Objects, {.bPrivateGraph = true}); !Result)
					return {.Status = S::InvalidClosure, .PackagePath = CurrentPath, .Reason = R::GraphValidation, .Message = Result.error().ToString(), .Cause = Result.error()};
			}
			for (const auto& Source : Sources)
			{
				CurrentPath = Source.PackagePath;
				if (auto Result = Source.Storage.Revalidate(Cancelled); !Result)
					return {.Status = Result.error().Code == EPreparedPackageResourceError::Cancelled ? S::Cancelled : S::Stale,
						.PackagePath = CurrentPath, .Reason = R::ResourceRevalidation,
						.Message = FormatPreparedPackageResourceError(Result.error())};
			}
			for (const auto& Source : Sources)
				if (IsAssetRegistryProjectionFenced(Source.PackagePath))
					return {.Status = S::Busy, .PackagePath = Source.PackagePath, .Reason = R::ProjectionFenced};
			for (const auto& [Path, Package] : ExternalPackages)
			{
				if (IsAssetRegistryProjectionFenced(Path))
					return {.Status = S::Busy, .PackagePath = Path, .Reason = R::DependencyProjectionFenced};
				if (FindPackage(Path.GetView()) != Package)
					return {.Status = S::Stale, .PackagePath = Path, .Reason = R::DependencyIdentityChanged};
			}
			if (auto Result = LiveLoadGuard.GetFailure(); !Result)
				return {.Status = S::Unsupported, .PackagePath = CurrentPath, .Reason = R::LiveOperationRejected, .Message = Result.error().ToString(), .Cause = Result.error()};
			Out = std::move(Candidates);
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return {.Status = S::BudgetExceeded, .PackagePath = CurrentPath, .Reason = R::Allocation};
		}
		catch (...)
		{
			return {.Status = S::InvalidClosure, .PackagePath = CurrentPath, .Reason = R::CallbackException};
		}
	}

	auto ApplyLivePackageLinker(ObjectPackage::FLinkerTables Linker,
		const FPackagePath& PackagePath, DPackage*& OutPackage,
		FAssetLoadReport* OutReport, const FLinkerLoadOptions& Options,
		std::string* OutError) -> std::expected<void, Durin::UnifiedError::FError>
	{
		OutPackage = nullptr;
		FLinkerApplyDiagnostic Diagnostic;
		FAssetLiveLoadGuard LiveLoadGuard(Options.DependencyLoadPolicy
			&& Options.DependencyLoadPolicy->bRejectImplicitLiveLoads);
		auto Finish = [&](std::expected<void, Durin::UnifiedError::FError> Result) {
			if (OutError) *OutError = Result ? std::string{} : Result.error().ToString();
			return Result;
		};
		if (!PackagePath.IsValid())
		{
			LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::InvalidPath, "Live linker application requires a validated package path."});
			return Finish(std::unexpected(*Diagnostic.Cause));
		}
		if (Options.bPrivateGraph && (!Options.DependencyLoadPolicy
			|| !Options.DependencyLoadPolicy->bRejectImplicitLiveLoads))
		{
			LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Private package loading requires an explicit closed dependency policy."});
			return Finish(std::unexpected(*Diagnostic.Cause));
		}
		if (Options.DependencyLoadPolicy
			&& (!Options.DependencyLoadPolicy->ResolvePackage
				|| !Options.DependencyLoadPolicy->ResolveObject
				|| (!Options.DeferCompletion && !Options.DependencyLoadPolicy->Rollback)))
		{
			LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "An explicit dependency load policy requires package, object, and rollback callbacks."});
			return Finish(std::unexpected(*Diagnostic.Cause));
		}
		if (Linker.Summary.PackagePath != PackagePath)
		{
			LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::InvalidPath, "Linker package identity does not match the requested package path."});
			return Finish(std::unexpected(*Diagnostic.Cause));
		}
		if (FindPackage(PackagePath.GetView()) != Options.ExistingPackage)
		{
			LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::AlreadyExists, std::format("A package with the requested path '{}' is already live.", PackagePath.ToString())});
			return Finish(std::unexpected(*Diagnostic.Cause));
		}
		FLinkerApplication Application;
		Application.Linker = std::move(Linker);
		if (auto Result = ValidateLinker(Application, Options, Diagnostic); !Result)
			return Finish(Result);
		auto& Exports = Application.Exports;
		auto& Objects = Application.Objects;
		auto& Report = Application.Compatibility;

		DPackage* Package = Options.ExistingPackage ? Options.ExistingPackage : NewObject<DPackage>(
			nullptr,
			FName(PackagePath.GetPackageName()));
		if (!Package)
		{
			LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Could not allocate the package skeleton."});
			return Finish(std::unexpected(*Diagnostic.Cause));
		}
		bool bSkeletonPublished = false;
		bool bFinalized = false;
		auto Rollback = [&]() {
			if (std::exchange(bFinalized, true)) return;
			std::exception_ptr Failure;
			auto Cleanup = [&](auto&& Work) {
				try { Work(); }
				catch (...) { if (!Failure) Failure = std::current_exception(); }
			};
			Cleanup([&] {
				if (bSkeletonPublished && Options.OnSkeletonRollback)
					Options.OnSkeletonRollback(Package);
			});
			Cleanup([&] {
				if (!Options.ExistingPackage && (!bSkeletonPublished || !Options.DeferCompletion))
				{
					MarkObjectHierarchyAsGarbage(Package);
					if (Package->IsAssetPackage()) Package->RetireFailedLoadRegistration();
					RequestGarbageCollection();
				}
			});
			Cleanup([&] {
				if (Options.DependencyLoadPolicy && Options.DependencyLoadPolicy->Rollback) Options.DependencyLoadPolicy->Rollback();
			});
			if (Failure) std::rethrow_exception(Failure);
		};

		struct FScopedRollback
		{
			decltype(Rollback)& Execute;
			~FScopedRollback()
			{
				try { Execute(); }
				catch (...) { /* Preserve an in-flight callback exception during cleanup. */ }
			}
		} Scope{Rollback};

		if (Options.bPrivateGraph)
		{
			if (!Package->InitializePreparedAssetPackage(PackagePath))
			{
				LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Could not initialize a private capture package."});
				return Finish(std::unexpected(*Diagnostic.Cause));
			}
		}
		else if (Options.ExistingPackage) Application.ExistingObjects = Options.ExistingObjects;
		else Package->InitializeAssetPackage(PackagePath);
		Application.Package = Package;
		Objects.resize(Exports.size(), nullptr);

		std::vector<FStrongObjectPtr> Pins;
		Pins.reserve(Objects.size() + 1);
		Pins.emplace_back(Package);
		if (Application.ExistingObjects)
			for (DObject* Object : *Application.ExistingObjects) Pins.emplace_back(Object);

		auto Construct = [&]() -> std::expected<void, Durin::UnifiedError::FError> {
			FAssetLiveLoadGuard Guard(bool(Options.DeferCompletion));
			if (auto Result = CreateLinkerSkeleton(Application, Options, Diagnostic, &Pins); !Result) return Result;
			return Guard.GetFailure();
		};
		if (auto Result = Construct(); !Result)
		{
			return Finish(Result);
		}

		if (!LiveLoadGuard.GetFailure())
		{
			const auto Result = LiveLoadGuard.GetFailure();
			LinkerApplyFail(Diagnostic, Result.error());
			return Finish(Result);
		}
		if (Options.OnSkeletonReady)
		{
			auto PublishResult = Options.OnSkeletonReady(Package);
			if (!PublishResult)
			{
				LinkerApplyFail(Diagnostic, PublishResult.error());
				return Finish(PublishResult);
			}
			bSkeletonPublished = true;
		}

		{
			FScopedObjectLoadPhase DependencyTiming(PackagePath, EObjectLoadPhase::ResolveImports);
			const auto& Dependencies = Options.bCooked ? Application.Linker.Summary.HardPackageDependencies : Application.LiveDependencies;
			for (size_t Index = 0; Index < Dependencies.size(); ++Index)
			{
				if (ShouldFail(Options, ELinkerLoadPhase::ResolveDependency, Index))
				{
					LinkerApplyFail(Diagnostic, UnifiedError::FError{Durin::AssetLoadErrors::MissingDependency, "Injected dependency failure."});
					return Finish(std::unexpected(*Diagnostic.Cause));
				}
				const FPackagePath& Path = Dependencies[Index];
				DPackage* Dependency = nullptr;
				std::expected<void, Durin::UnifiedError::FError> Result;
				if (Options.DependencyLoadPolicy)
					Result = Options.DependencyLoadPolicy->ResolvePackage(Path, Dependency);
				else
				{
					std::optional<Durin::UnifiedError::FError> LoadedError;
					auto Loaded = LoadPackage(Path, nullptr, &LoadedError);
					if (!Loaded) Result = std::unexpected((*LoadedError));
					else Dependency = Loaded;
				}
				if (Result && !Dependency)
					Result = std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::MissingDependency, "Dependency resolver returned no package."});
				if (!Result)
				{
					LinkerApplyFail(Diagnostic, Result.error());
					return Finish(std::unexpected(*Diagnostic.Cause));
				}
			}

		}

		// Choose sources once for this package; object archives only consume them.
		const FPackageLoadBindings Bindings{
			.BulkResource = Options.BulkResource,
			.ResolveExternalObject = [Policy = Options.DependencyLoadPolicy](const FSoftObjectPath& Path, DObject*& Object) {
				std::optional<Durin::UnifiedError::FError> LoadedError;
				if (Policy) return Policy->ResolveObject(Path, Object);
				auto Loaded = LoadObject(Path, nullptr, nullptr, &LoadedError);
				Object = Loaded;
				return Loaded ? std::expected<void, Durin::UnifiedError::FError>{} : std::unexpected((*LoadedError));
			}};
		auto RestoreValues = [&]() -> std::expected<void, Durin::UnifiedError::FError> {
			FAssetLiveLoadGuard Guard(bool(Options.DeferCompletion));
			if (auto Result = ApplyLinkerValues(Application, Options, Diagnostic, Bindings); !Result) return Result;
			return Guard.GetFailure();
		};
		if (auto Result = RestoreValues(); !Result)
		{
			return Finish(Result);
		}

		// Recoverable gates precede every initialization notification.
		for (size_t Index = 0; Index < Objects.size(); ++Index)
			if (ShouldFail(Options, ELinkerLoadPhase::PostLoad, Index))
				return Finish(std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Injected pre-PostLoad failure."}));
		if (ShouldFail(Options, ELinkerLoadPhase::Publish, 0))
			return Finish(std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Injected graph publication preparation failure."}));
		if (auto Result = LiveLoadGuard.GetFailure(); !Result) return Finish(Result);
		auto Validate = [Objects, Pins = std::move(Pins), PackagePath, bCooked = Options.bCooked, bPrivate = Options.bPrivateGraph]() {
			FScopedObjectLoadPhase Timing(PackagePath, EObjectLoadPhase::ValidateLoadedGraph, Objects.size());
			return ValidateLoadedGraphs(Objects, {.bCooked = bCooked, .bPrivateGraph = bPrivate});
		};
		auto DiskExports = std::vector<FObjectKey>(Objects.begin(), Objects.end());
		auto RestoredObjects = Objects;
		std::erase_if(RestoredObjects, [&](DObject* Object) { return Application.IsExisting(Object); });
		auto Notify = [Objects = std::move(RestoredObjects), Package, PackagePath, DiskExports = std::move(DiskExports), MainDigest = Options.MainDigest, bRestoring = bool(Options.ExistingPackage)]() mutable {
			Package->RecordDiskExports(std::move(DiskExports), MainDigest);
			FScopedObjectLoadPhase Timing(PackagePath, EObjectLoadPhase::PostLoad, Objects.size());
			if (!bRestoring) Package->ClearDirty();
			for (size_t Reverse = Objects.size(); Reverse > 0; --Reverse)
			{
				DObject* Object = Objects[Reverse - 1];
				Object->PostLoad();
				Object->ClearLoadedCustomVersions();
				Object->ClearLoadedDeprecatedProperties();
			}
		};
		if (Options.DeferCompletion) Options.DeferCompletion(std::move(Validate), std::move(Notify));
		else
		{
			if (auto Result = Validate(); !Result) return Finish(Result);
			Notify();
			// Closed private policies still reject forbidden live side effects.
			if (auto Result = LiveLoadGuard.GetFailure(); !Result) return Finish(Result);
		}

		OutPackage = Package;
		Package->SetCanonicalResaveRecommended(Package->IsCanonicalResaveRecommended()
			|| Report.CanonicalizedIdentityCount != 0
			|| Report.DeprecatedFieldRouteCount != 0 || Report.DiscardedFieldCount != 0);
		if (OutReport)
		{
			OutReport->PackagePath = PackagePath;
			Report.TransferTo(*OutReport);
		}
		bFinalized = true;
		return Finish({});
	}

}

namespace Durin
{
	auto FormatPackageGraphPrepareError(const FPackageGraphPrepareResult& Result) -> std::string
	{
		if (Result) return {};
		if (!Result.Message.empty()) return Result.Message;
		using R = EPackageGraphPrepareReason;
		switch (Result.Reason)
		{
		case R::Reentrant: return "Package graph preparation cannot be reentered.";
		case R::LoadServiceBusy: return "Package graph preparation requires an idle load service.";
		case R::PackageBudget: return "Package count is outside the preparation budget.";
		case R::RetainedByteBudget: return "Saved batch closures exceed the retained byte budget.";
		case R::Cancelled: return "Package graph preparation cancelled.";
		case R::InvalidSource: return "A validated saved package closure is required.";
		case R::ProjectionFenced: return "Package projection became fenced during preparation.";
		case R::ResidentTargetRequired: return "Scoped dependency loading requires resident replacement targets.";
		case R::DuplicatePackage: return "Duplicate package identity in preparation batch.";
		case R::ObjectBudget: return "Package skeletons exceed the object budget.";
		case R::InjectedDependencyFailure: return "Injected dependency binding failure.";
		case R::DependencyProjectionFenced: return "Dependency projection became fenced during preparation.";
		case R::PackageConstruction: return "Could not create private package skeleton.";
		case R::DefaultInnerBudget: return "Default inners exceed the object budget.";
		case R::DependencyIdentityChanged: return "Admitted dependency identity changed during preparation.";
		case R::Allocation: return "Allocation failed during package graph preparation.";
		case R::CallbackException: return "A callback threw during package graph preparation.";
		case R::ClassNotAdmitted: return std::format("Class {} has no isolated deserialization admission.", Result.Subject);
		case R::DependencyNotResident: return std::format("External dependency {} must be admitted and resident before graph preparation.", Result.Subject);
		default: return "Package graph preparation failed.";
		}
	}
}
