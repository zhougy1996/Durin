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
		auto LinkerApplyError(UnifiedError::FError Error, std::string_view Path = {})
			-> std::unexpected<UnifiedError::FError>
		{
			if (!Path.empty()) Error.PushContext("object", std::string(Path));
			return std::unexpected(std::move(Error));
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

		auto RestoreNestedReplacements(FPackageLoadContext& Load, const ObjectPackage::FSerializedType& Type,
			const ObjectPackage::FSerializedValue& Value,
			const ObjectPackage::FLinkerTables& Linker, LinkerSchema::FLoadSchemaBindings& SchemaBindings, FAuthoredOverridePath& Path,
			std::vector<FAuthoredOverrideEntry>& Entries) -> void
		{
			if (Load.IsError()) return;
			using K = ObjectPackage::EValueKind;
			if (!HasNestedReplacement(Value)) return;
			if (Type.Kind == K::Struct)
			{
				const auto* Schema = LinkerSchema::FindSchema(Linker, Type.QualifiedName);
				if (!Schema || Value.FieldNames.size() != Value.Elements.size()
					|| Value.Provenances.size() != Value.Elements.size()
					|| ObjectPackage::StructFieldTypes(Type, Value).size() != Value.Elements.size())
					return Load.Fail(UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Struct ledger projection is invalid."});
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto It = SchemaBindings.FindProperty(*Schema, Value.FieldNames[Index]);
					if (!It) return Load.Fail(UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Struct ledger field is missing."});
					const auto& ChildType = ObjectPackage::StructFieldTypes(Type, Value)[Index];
					const auto Route = It->Match.Route;
					if (Route == EPropertyLoadRoute::Removed || Route == EPropertyLoadRoute::Deprecated) continue;
					Path.push_back(FAuthoredOverridePathToken::Field(FName(Schema->QualifiedName), FName(It->Descriptor->Name)));
					if (Value.Provenances[Index] == ObjectPackage::EPropertyProvenance::Forced)
						Entries.push_back({Path, EAuthoredOverrideProvenance::Forced});
					else RestoreNestedReplacements(Load, ChildType, Value.Elements[Index], Linker, SchemaBindings, Path,
						Entries);
					Path.pop_back();
					if (Load.IsError()) return;
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
					RestoreNestedReplacements(Load, Type.Children[0], Value.Elements[Index], Linker, SchemaBindings, Path,
						Entries);
					Path.pop_back();
					if (Load.IsError()) return;
				}
			}
			else if (Type.Kind == K::Map && Type.Children.size() == 2)
			{
				if (Value.Elements.size() % 2 != 0)
					return Load.Fail(UnifiedError::FError{PackageReadErrors::CorruptFile, "Map ledger has an incomplete entry."});
				for (size_t Index = 0; Index < Value.Elements.size(); Index += 2)
				{
					FByteBuffer Token;
					if (const auto Result = ObjectPackage::BuildCanonicalMapKeyToken(
						Type.Children[0], Value.Elements[Index], Token); !Result)
						return Load.Fail(UnifiedError::FError{PackageReadErrors::CorruptFile, ObjectPackage::ToString(Result.error())});
					Path.push_back(FAuthoredOverridePathToken::MapValue(std::move(Token)));
					RestoreNestedReplacements(Load, Type.Children[1], Value.Elements[Index + 1], Linker, SchemaBindings, Path,
						Entries);
					Path.pop_back();
					if (Load.IsError()) return;
				}
			}
			return;
		}

		auto GatherLiveValueDependencies(FPackageLoadContext& Load, const ObjectPackage::FSerializedType& Type,
			const ObjectPackage::FSerializedValue& Value, const ObjectPackage::FLinkerTables& Linker, LinkerSchema::FLoadSchemaBindings& SchemaBindings,
			std::vector<FPackagePath>& Dependencies, uint64& DiscardedFields) -> void
		{
			if (Load.IsError()) return;
			using K = ObjectPackage::EValueKind;
			if (Type.Kind == K::Struct)
			{
				const auto* Schema = LinkerSchema::FindSchema(Linker, Type.QualifiedName);
				if (!Schema || !FindStructByQualifiedName(FName(Schema->QualifiedName)) || Value.FieldNames.size() != Value.Elements.size()
					|| ObjectPackage::StructFieldTypes(Type, Value).size() != Value.Elements.size())
					return Load.Fail(UnifiedError::FError{Durin::AssetLoadErrors::UnsupportedProperty, "Serialized struct is unavailable or has an invalid schema."});
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto Field = SchemaBindings.FindProperty(*Schema, Value.FieldNames[Index]);
					const auto& ChildType = ObjectPackage::StructFieldTypes(Type, Value)[Index];
					if (!Field || Field->Descriptor->Type != ChildType)
						return Load.Fail(UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Struct field does not match its declared schema."});
					const auto Route = Field->Match.Route;
					if (Route == EPropertyLoadRoute::Removed) { ++DiscardedFields; continue; }
					if (Route == EPropertyLoadRoute::Incompatible)
						return Load.Fail(UnifiedError::FError{Durin::AssetLoadErrors::UnsupportedProperty, std::format("Serialized struct field {}::{} is incompatible with the live schema.",
								Schema->QualifiedName, Field->Descriptor->Name)});
					GatherLiveValueDependencies(Load, ChildType, Value.Elements[Index], Linker, SchemaBindings,
						Dependencies, DiscardedFields);
					if (Load.IsError()) return;
				}
			}
			else if ((Type.Kind == K::Array || Type.Kind == K::FixedArray) && Type.Children.size() == 1)
			{
				for (const auto& Element : Value.Elements)
				{
					GatherLiveValueDependencies(Load, Type.Children[0], Element, Linker, SchemaBindings,
						Dependencies, DiscardedFields);
					if (Load.IsError()) return;
				}
			}
			else if (Type.Kind == K::Map && Type.Children.size() == 2)
			{
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					GatherLiveValueDependencies(Load, Type.Children[Index % 2], Value.Elements[Index], Linker, SchemaBindings,
						Dependencies, DiscardedFields);
					if (Load.IsError()) return;
				}
			}
			else if (Type.Kind == K::HardReference && Value.Reference.IsImport())
			{
				const ObjectPackage::FPackageImport* Import = nullptr;
				if (!Linker.TryGetImport(Value.Reference, Import) || !Import)
					return Load.Fail(UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Hard-reference import is invalid."});
				Dependencies.push_back(Import->ObjectPath.GetPackagePath());
			}
			return;
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
			bool bNeedsLoad = true;
		};

		auto BuildExportViews(FPackageLoadContext& Load, const ObjectPackage::FLinkerTables& Linker,
			std::vector<FExportView>& Out) -> void
		{
			std::vector<FExportView> Views;
			Views.reserve(Linker.Exports.size());
			for (size_t Index = 0; Index < Linker.Exports.size(); ++Index)
			{
				ObjectPackage::FPackageIndex PackageIndex;
				std::string Path;
				if (!ObjectPackage::FPackageIndex::TryExport(Index, PackageIndex)
					|| !Linker.TryResolvePath(PackageIndex, Path))
					return Load.Fail(UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "An export path cannot be resolved."});
				const auto& Export = Linker.Exports[Index];
				uint64 OuterId = 0;
				if (!Export.Outer.IsNull())
				{
					if (!Export.Outer.IsExport() || Export.Outer.GetTableIndex() >= Index)
						return Load.Fail(UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Export Outer topology is not constructible in table order."}, Path);
					OuterId = static_cast<uint64>(Export.Outer.GetTableIndex() + 1);
				}
				Views.push_back({&Export, nullptr, std::move(Path), OuterId});
			}
			Out = std::move(Views);
			return;
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

		class FPackageLinkerLoader;
		struct FObjectLoadBinding
		{
			FPackageLinkerLoader* Loader;
			size_t ExportIndex;
		};
		// Like UE's linker annotation: one temporary object -> loader/index relation.
		// Owner-thread only; loaders detach before rollback or candidate destruction.
		std::unordered_map<DObject*, FObjectLoadBinding> ObjectLoadBindings;

		// Owns decoded-package data and loading behavior. Publication and PostLoad
		// remain with the enclosing ordinary operation or private batch.
		class FPackageLinkerLoader
		{
		public:
			FPackageLinkerLoader() = default;
			FPackageLinkerLoader(const FPackageLinkerLoader&) = delete;
			auto operator=(const FPackageLinkerLoader&) -> FPackageLinkerLoader& = delete;
			~FPackageLinkerLoader() { DetachExports(); }

			auto InitializePreload(const FLinkerLoadOptions& Options, FPackageLoadBindings InBindings) -> void;
			auto DetachExports() -> void;
			auto Preload(DObject* Object) -> void { PreloadPackageObject(Load, Object); }
			auto PreloadExport(size_t Index) -> void;
			auto RestoreOverrides(size_t ObjectIndex, std::unique_ptr<FOverriddenPropertyRestoreContext> Restoration) -> void;

			ObjectPackage::FLinkerTables Linker;
			FPackageLoadContext Load;
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
			FPackageLoadBindings Bindings;
			FArchiveState Context;
			std::function<bool(ELinkerLoadPhase, uint64)> ShouldFail;
		};

		auto ValidateLinker(FPackageLinkerLoader& Application, const FLinkerLoadOptions& Options) -> void
		{
			auto& Load = Application.Load;
			if (Load.IsError()) return;
			FScopedObjectLoadPhase Phase(Application.Linker.Summary.PackagePath, EObjectLoadPhase::ValidateSchema);
			auto& Linker = Application.Linker;
			std::string VersionError;
			if (!FCustomVersionRegistry::Validate(Linker.CustomVersions, VersionError))
			{
				return Load.Fail(UnifiedError::FError{Durin::PackageReadErrors::UnsupportedVersion, VersionError});
			}
			if (auto Result = LinkerSchema::FixupSerializedNames(
				Linker, Application.Compatibility.CanonicalizedIdentityCount,
				Application.Compatibility.Details ? &Application.Compatibility.Details->CanonicalizationEvidence : nullptr); !Result)
			{
				return Load.Fail(std::move(Result.error()));
			}
            for (const auto* Dependencies : {&Linker.Summary.HardPackageDependencies,
                &Linker.Summary.SoftPackageDependencies, &Linker.Summary.BuildPackageDependencies})
                for (const auto& Dependency : *Dependencies)
                    if (Dependency.GetView().starts_with("/Plugins/")
                        && !FMountPaths::FindMountForVirtualPath(Dependency.GetView()))
                    {
                        return Load.Fail(UnifiedError::FError{Durin::AssetLoadErrors::MissingDependency, "Serialized plugin reference '" + Dependency.ToString()
                            + "' has no active mount. Enable the required plugin and restart before editing or saving this package."});
                    }
			Application.SchemaBindings.emplace(Linker);
			auto& SchemaBindings = *Application.SchemaBindings;
			auto& Exports = Application.Exports;
			BuildExportViews(Load, Linker, Exports);
			if (Load.IsError()) return;
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
					return Load.Fail(UnifiedError::FError{Durin::AssetLoadErrors::UnknownClass, std::format("Serialized class '{}' is unavailable. Enable its required plugin and restart, or restore its native provider, before editing or saving this package.", Object.Export->ClassName)}, Object.Path);
				}
				for (const ObjectPackage::FPropertyTag& Property : Object.Export->Properties)
				{
					const auto* Schema = LinkerSchema::FindSchema(Linker, Property.DeclaringType);
					const auto Field = Schema ? SchemaBindings.FindProperty(*Schema, Property.FieldName) : nullptr;
					if (!Schema || !Field || Field->Descriptor->Type != Property.Type)
					{
						return Load.Fail(UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "A linker property is absent from its declared schema."}, Object.Path);
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
						return Load.Fail(UnifiedError::FError{Durin::AssetLoadErrors::UnsupportedProperty, std::format("Serialized field {}::{} is incompatible with the live schema.",
								Schema->QualifiedName, Field->Descriptor->Name)}, Object.Path);
					}
					if (!Options.bCooked)
					{
						GatherLiveValueDependencies(Load, Property.Type, Property.Value, Linker, SchemaBindings, LiveDependencies, DiscardedFields);
						if (Load.IsError()) return;
					}
				}
			}
			std::ranges::sort(LiveDependencies);
			LiveDependencies.erase(std::ranges::unique(LiveDependencies).begin(), LiveDependencies.end());
			if (Exports.empty())
			{
				return Load.Fail(UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Package has no object exports."});
			}

			Application.Compatibility.DiscardedFieldCount = DiscardedFields;
			return;
		}

		auto CreateLinkerSkeleton(FPackageLinkerLoader& Application, const FLinkerLoadOptions& Options,
			std::vector<FStrongObjectPtr>* Pins = nullptr) -> void
		{
			auto& Load = Application.Load;
			if (Load.IsError()) return;
			FScopedObjectLoadPhase Phase(Application.Linker.Summary.PackagePath, EObjectLoadPhase::CreateExports, Application.Exports.size());
			auto& Exports = Application.Exports;
			auto& Objects = Application.Objects;
			auto* Package = Application.Package;
			std::unordered_set<DObject*> Pinned;
			for (size_t Index = 0; Index < Exports.size(); ++Index)
			{
				if (ShouldFail(Options, ELinkerLoadPhase::CreateSkeleton, Index))
				{
					return Load.Fail(UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Injected skeleton creation failure."});
				}
				const FExportView& Descriptor = Exports[Index];
				DClass* Class = Descriptor.Class;
				if (!Class || !Class->ClassConstructor)
				{
					return Load.Fail(UnifiedError::FError{Durin::AssetLoadErrors::UnknownClass, std::format("Serialized class '{}' is unavailable. Enable its required plugin and restart, or restore its native provider, before editing or saving this package.", Descriptor.Export->ClassName)}, Descriptor.Path);
				}
				DObject* Outer = Descriptor.OuterId == 0 ? static_cast<DObject*>(Package)
					: Objects[static_cast<size_t>(Descriptor.OuterId - 1)];
				bool bTypeMismatch = false;
				DObject* Object = FindExistingInner(Outer, Descriptor.Export->ObjectName, Class, bTypeMismatch);
				if (bTypeMismatch)
				{
					return Load.Fail(UnifiedError::FError{Durin::AssetLoadErrors::TypeMismatch, "Existing default inner has a different class."}, Descriptor.Path);
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
					return Load.Fail(UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Object construction failed."}, Descriptor.Path);
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
			return;
		}

		auto FPackageLinkerLoader::InitializePreload(const FLinkerLoadOptions& Options, FPackageLoadBindings InBindings) -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			if (Load.IsError()) return;
			const auto& PackagePath = Linker.Summary.PackagePath;
			uint64 PropertyCount = 0;
			for (const auto& Export : Exports) PropertyCount += Export.Export->Properties.size();
			if (!Options.bCooked)
			{
				FScopedObjectLoadPhase DefaultsTiming(PackagePath, EObjectLoadPhase::InitDefaults, Objects.size());
				std::vector<DClass*> Classes;
				std::unordered_set<DObject*> NeedsDefaults;
				for (size_t Index = 0; Index < Objects.size(); ++Index)
					if (Exports[Index].Export->bUseClassDefaults && !IsExisting(Objects[Index]))
					{
						NeedsDefaults.insert(Objects[Index]);
						Classes.push_back(Objects[Index]->GetClass());
					}
				if (![&] {
					FScopedObjectLoadPhase Timing(PackagePath, EObjectLoadPhase::CreateClassDefaults, Classes.size());
					return Private::CreateClassDefaultObjectsForBatch(Classes);
				}())
					return Load.Fail(Durin::UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Default object initialization failed."});
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
						return Load.Fail(Durin::UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Cannot pair loaded object defaults: " + GraphDiagnostic.LogicalPath});
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
								return Load.Fail(Durin::UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Cannot initialize loaded defaults: " + ToString(CopyResult.error())});
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
					if (!IsExisting(Object)) Object->SetLoadedCustomVersions(LoadedCustomVersions);
				}
				FScopedObjectLoadPhase CompatibilityTiming(PackagePath, EObjectLoadPhase::CollectCompatibilityEvidence, Objects.size(), PropertyCount);
				for (size_t ObjectIndex = 0; ObjectIndex < Exports.size(); ++ObjectIndex)
					for (const auto& Property : Exports[ObjectIndex].Export->Properties)
					{
						const auto Field = SchemaBindings->FindProperty(
							*LinkerSchema::FindSchema(Linker, Property.DeclaringType), Property.FieldName);
						if (!Field)
						{
							return Load.Fail(UnifiedError::FError{Durin::AssetLoadErrors::UnsupportedProperty, "Reflection metadata changed during package loading."}, Exports[ObjectIndex].Path);
						}
						if (!Options.bCooked && Field->Match.Route == EPropertyLoadRoute::Removed) continue;
						GatherNestedDeprecatedRouteEvidence(Property.Type, Property.Value, Linker, *SchemaBindings,
							PackagePath, Exports[ObjectIndex].Path,
							Compatibility.DeprecatedFieldRouteCount,
							Compatibility.Details ? &Compatibility.Details->DeprecatedRouteEvidence : nullptr);
					}
			}
			Bindings = std::move(InBindings);
			Bindings.SchemaBindings = &*SchemaBindings;
			Context.bCooking = Options.bCooked;
			Context.bFilterEditorOnly = Options.bCooked;
			Context.Target = Options.Target;
			ShouldFail = Options.ShouldFail;
			for (size_t Index = 0; Index < Objects.size(); ++Index)
			{
				Exports[Index].bNeedsLoad = !IsExisting(Objects[Index]);
				if (!ObjectLoadBindings.emplace(Objects[Index], FObjectLoadBinding{this, Index}).second)
					return Load.Fail(UnifiedError::FError{AssetLoadErrors::InvalidObjectGraph, "Object already has a package loader binding."});
			}
		}

		auto FPackageLinkerLoader::DetachExports() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
			for (DObject* Object : Objects)
			{
				const auto It = ObjectLoadBindings.find(Object);
				if (It != ObjectLoadBindings.end() && It->second.Loader == this) ObjectLoadBindings.erase(It);
			}
		}

		auto FPackageLinkerLoader::PreloadExport(size_t Index) -> void
		{
			if (Load.IsError() || !Exports[Index].bNeedsLoad) return;
			// Clearing before recursion is UE's NeedLoad cycle break. Completion is
			// established by returning from the package's synchronous export traversal.
			Exports[Index].bNeedsLoad = false;
			DObject* Object = Objects[Index];
			Preload(Object->GetOuter());
			if (Load.IsError()) return;
			if (ShouldFail && ShouldFail(ELinkerLoadPhase::ApplyValues, Index))
				return Load.Fail(UnifiedError::FError{PackageReadErrors::CorruptFile, "Injected value application failure."}, Exports[Index].Path);
			const FPackageOverrideRestore Restore = [this, Index](FPackageLoadContext&, std::unique_ptr<FOverriddenPropertyRestoreContext> Evidence) {
				RestoreOverrides(Index, std::move(Evidence));
			};
			Load.SerializeExport(*Object, Exports[Index].Export->Properties, Linker, Objects, Bindings, Context,
				Context.bCooking ? FPackageOverrideRestore{} : Restore);
		}

		auto FPackageLinkerLoader::RestoreOverrides(size_t ObjectIndex, std::unique_ptr<FOverriddenPropertyRestoreContext> Restoration) -> void
		{
			const auto& PackagePath = Linker.Summary.PackagePath;
			const bool bFastRestoration = Restoration && Restoration->IsEligible();
			FScopedObjectLoadPhase EditorTiming(PackagePath, EObjectLoadPhase::RestoreEditorState, 1, Exports[ObjectIndex].Export->Properties.size());
			std::optional<FScopedObjectLoadPhase> LedgerTiming(std::in_place, PackagePath, EObjectLoadPhase::BuildOverrideLedger, 1, Exports[ObjectIndex].Export->Properties.size());
			std::vector<FAuthoredOverrideEntry> LedgerEntries;
			std::vector<FName> LoadedDeprecatedProperties;
			size_t PropertyIndex = 0;
			for (const auto& Property : Exports[ObjectIndex].Export->Properties)
			{
				const auto Field = SchemaBindings->FindProperty(
					*LinkerSchema::FindSchema(Linker, Property.DeclaringType), Property.FieldName);
				if (!Field) return Load.Fail(Durin::UnifiedError::FError{Durin::AssetLoadErrors::UnsupportedProperty, "Reflection metadata changed during package loading."});
				const auto& Binding = Field->Match;
				if (Binding.Route == EPropertyLoadRoute::Removed) continue;
				if (ShouldFail && ShouldFail(ELinkerLoadPhase::RestoreLedger, ++PropertyIndex))
				{
					return Load.Fail(UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Injected ledger restoration failure."});
				}
				if (Binding.Route == EPropertyLoadRoute::Deprecated)
				{
					LoadedDeprecatedProperties.push_back(Binding.Property->NamePrivate);
					++Compatibility.DeprecatedFieldRouteCount;
					if (Compatibility.Details) Compatibility.Details->DeprecatedRouteEvidence.push_back({
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
				else RestoreNestedReplacements(Load, Property.Type, Property.Value, Linker, *SchemaBindings, Path, LedgerEntries);
				if (Load.IsError()) return;
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
				return Load.Fail(UnifiedError::FError{Durin::PackageReadErrors::CorruptFile, "Could not restore authored intent."}, LedgerDiagnostic.LogicalPath);
			}
			return;
		}

		auto PreloadLinkerExports(FPackageLinkerLoader& Loader) -> void
		{
			uint64 PropertyCount = 0;
			for (const auto& Export : Loader.Exports) PropertyCount += Export.Export->Properties.size();
			FScopedObjectLoadPhase Phase(Loader.Linker.Summary.PackagePath, EObjectLoadPhase::Preload, Loader.Objects.size(), PropertyCount);
			for (DObject* Object : Loader.Objects)
			{
				Loader.Preload(Object);
				if (Loader.Load.IsError()) return;
			}
		}

	}

	auto PreloadPackageObject(FPackageLoadContext& Load, DObject* Object) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (Load.IsError() || !Object) return;
		const auto It = ObjectLoadBindings.find(Object);
		if (It == ObjectLoadBindings.end()) return;
		const auto Binding = It->second;
		Binding.Loader->PreloadExport(Binding.ExportIndex);
		if (Binding.Loader->Load.IsError()) Load.Fail(Binding.Loader->Load.GetError());
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
			std::vector<FPreparedPackageGraph> Candidates(Sources.size());
			// Reverse destruction detaches loaders before private graphs are retired.
			std::vector<FPackageLinkerLoader> Applications(Sources.size());
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
					return {.Status = S::InvalidClosure, .PackagePath = CurrentPath, .Reason = R::Reader, .Cause = UnifiedError::FError{ReaderDiagnostic.Reason == ObjectPackage::EPackageReaderReason::UnsupportedVersion
						? PackageReadErrors::UnsupportedVersion : PackageReadErrors::CorruptFile, ObjectPackage::FormatPackageError(ReaderDiagnostic)}};
				ValidateLinker(Application, {});
				if (Application.Load.IsError())
					return {.Status = S::InvalidClosure, .PackagePath = CurrentPath, .Reason = R::LinkerValidation, .Cause = Application.Load.GetError()};
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
						if (!Result) return {.Status = S::MissingDependency, .PackagePath = Path, .Reason = R::DependencyLoad, .Cause = Result.error()};
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
				CreateLinkerSkeleton(Application, LoadOptions(Index), &State->Pins);
				if (Application.Load.IsError())
					return {.Status = Cancelled() ? S::Cancelled : S::InvalidClosure, .PackagePath = CurrentPath, .Reason = R::Skeleton, .Cause = Application.Load.GetError()};
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
				Application.InitializePreload(LoadOptions(Index), Bindings);
				if (Application.Load.IsError())
					return {.Status = Cancelled() ? S::Cancelled : S::InvalidClosure, .PackagePath = CurrentPath, .Reason = R::ApplyValues, .Cause = Application.Load.GetError()};
			}
			for (size_t Index = 0; Index < Applications.size(); ++Index)
			{
				auto& Application = Applications[Index];
				CurrentPath = Application.Linker.Summary.PackagePath;
				PreloadLinkerExports(Application);
				if (Application.Load.IsError())
					return {.Status = Cancelled() ? S::Cancelled : S::InvalidClosure, .PackagePath = CurrentPath, .Reason = R::ApplyValues, .Cause = Application.Load.GetError()};
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
					return {.Status = S::InvalidClosure, .PackagePath = CurrentPath, .Reason = R::GraphValidation, .Cause = Result.error()};
			}
			for (const auto& Source : Sources)
			{
				CurrentPath = Source.PackagePath;
				if (auto Result = Source.Storage.Revalidate(Cancelled); !Result)
					return {.Status = Result.error().Code == EPreparedPackageResourceError::Cancelled ? S::Cancelled : S::Stale,
						.PackagePath = CurrentPath, .Reason = R::ResourceRevalidation,
						.Cause = UnifiedError::FError{Result.error().Code == EPreparedPackageResourceError::Cancelled
							? AssetLoadErrors::Cancelled : AssetLoadErrors::StaleData, FormatPreparedPackageResourceError(Result.error())}};
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
				return {.Status = S::Unsupported, .PackagePath = CurrentPath, .Reason = R::LiveOperationRejected, .Cause = Result.error()};
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
		FAssetLoadReport* OutReport, const FLinkerLoadOptions& Options) -> std::expected<void, Durin::UnifiedError::FError>
	{
		OutPackage = nullptr;
		FAssetLiveLoadGuard LiveLoadGuard(Options.DependencyLoadPolicy
			&& Options.DependencyLoadPolicy->bRejectImplicitLiveLoads);
		if (!PackagePath.IsValid())
		{
			return LinkerApplyError(UnifiedError::FError{Durin::AssetLoadErrors::InvalidPath, "Live linker application requires a validated package path."});
		}
		if (Options.bPrivateGraph && (!Options.DependencyLoadPolicy
			|| !Options.DependencyLoadPolicy->bRejectImplicitLiveLoads))
		{
			return LinkerApplyError(UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Private package loading requires an explicit closed dependency policy."});
		}
		if (Options.DependencyLoadPolicy
			&& (!Options.DependencyLoadPolicy->ResolvePackage
				|| !Options.DependencyLoadPolicy->ResolveObject
				|| (!Options.DeferCompletion && !Options.DependencyLoadPolicy->Rollback)))
		{
			return LinkerApplyError(UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "An explicit dependency load policy requires package, object, and rollback callbacks."});
		}
		if (Linker.Summary.PackagePath != PackagePath)
		{
			return LinkerApplyError(UnifiedError::FError{Durin::AssetLoadErrors::InvalidPath, "Linker package identity does not match the requested package path."});
		}
		if (FindPackage(PackagePath.GetView()) != Options.ExistingPackage)
		{
			return LinkerApplyError(UnifiedError::FError{Durin::AssetLoadErrors::AlreadyExists, std::format("A package with the requested path '{}' is already live.", PackagePath.ToString())});
		}
		FPackageLinkerLoader Application;
		Application.Linker = std::move(Linker);
		ValidateLinker(Application, Options);
		if (Application.Load.IsError()) return std::unexpected(Application.Load.GetError());
		auto& Exports = Application.Exports;
		auto& Objects = Application.Objects;
		auto& Report = Application.Compatibility;

		DPackage* Package = Options.ExistingPackage ? Options.ExistingPackage : NewObject<DPackage>(
			nullptr,
			FName(PackagePath.GetPackageName()));
		if (!Package)
		{
			return LinkerApplyError(UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Could not allocate the package skeleton."});
		}
		bool bSkeletonPublished = false;
		bool bFinalized = false;
		auto Rollback = [&]() {
			if (std::exchange(bFinalized, true)) return;
			// Detach routes before rollback callbacks or graph retirement can run.
			Application.DetachExports();
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
					if (Package->IsAssetPackage()) Package->RetireGarbageRegistration();
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
				return LinkerApplyError(UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Could not initialize a private capture package."});
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

		{
			FAssetLiveLoadGuard Guard(bool(Options.DeferCompletion));
			CreateLinkerSkeleton(Application, Options, &Pins);
			if (auto Result = Guard.GetFailure(); !Result) Application.Load.Fail(std::move(Result.error()));
		}
		if (Application.Load.IsError()) return std::unexpected(Application.Load.GetError());

		if (!LiveLoadGuard.GetFailure())
		{
			const auto Result = LiveLoadGuard.GetFailure();
			return Result;
		}
		// Choose sources once for this package; object archives only consume them.
		const FPackageLoadBindings Bindings{
			.BulkResource = Options.BulkResource,
			.ResolveExternalObject = [Policy = Options.DependencyLoadPolicy](const FSoftObjectPath& Path, DObject*& Object) {
				if (Policy) return Policy->ResolveObject(Path, Object);
				auto Loaded = FAssetRuntimeState::Get().GetLoadService().LoadObjectDetailed(Path, nullptr);
				Object = Loaded.value_or(nullptr);
				if (!Loaded) return std::expected<void, UnifiedError::FError>{std::unexpected(std::move(Loaded.error()))};
				return std::expected<void, UnifiedError::FError>{};
			}};
		{
			FAssetLiveLoadGuard Guard(bool(Options.DeferCompletion));
			Application.InitializePreload(Options, Bindings);
			if (auto Result = Guard.GetFailure(); !Result) Application.Load.Fail(std::move(Result.error()));
		}
		if (Application.Load.IsError()) return std::unexpected(Application.Load.GetError());

		if (Options.OnSkeletonReady)
		{
			auto PublishResult = Options.OnSkeletonReady(Package);
			if (!PublishResult)
			{
				return PublishResult;
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
					return LinkerApplyError(UnifiedError::FError{Durin::AssetLoadErrors::MissingDependency, "Injected dependency failure."});
				}
				const FPackagePath& Path = Dependencies[Index];
				DPackage* Dependency = nullptr;
				std::expected<void, Durin::UnifiedError::FError> Result;
				if (Options.DependencyLoadPolicy)
					Result = Options.DependencyLoadPolicy->ResolvePackage(Path, Dependency);
				else
				{
					auto Loaded = FAssetRuntimeState::Get().GetLoadService().LoadPackageDetailed(Path);
					if (!Loaded) Result = std::unexpected(std::move(Loaded.error()));
					else Dependency = *Loaded;
				}
				if (Result && !Dependency)
					Result = std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::MissingDependency, "Dependency resolver returned no package."});
				if (!Result)
				{
					return LinkerApplyError(Result.error());
				}
			}

		}

		{
			FAssetLiveLoadGuard Guard(bool(Options.DeferCompletion));
			PreloadLinkerExports(Application);
			if (auto Result = Guard.GetFailure(); !Result) Application.Load.Fail(std::move(Result.error()));
		}
		if (Application.Load.IsError()) return std::unexpected(Application.Load.GetError());

		// Recoverable gates precede every initialization notification.
		for (size_t Index = 0; Index < Objects.size(); ++Index)
			if (ShouldFail(Options, ELinkerLoadPhase::PostLoad, Index))
				return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Injected pre-PostLoad failure."});
		if (ShouldFail(Options, ELinkerLoadPhase::Publish, 0))
			return std::unexpected(Durin::UnifiedError::FError{Durin::AssetLoadErrors::InvalidObjectGraph, "Injected graph publication preparation failure."});
		if (auto Result = LiveLoadGuard.GetFailure(); !Result) return Result;
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
			if (auto Result = Validate(); !Result) return Result;
			Notify();
			// Closed private policies still reject forbidden live side effects.
			if (auto Result = LiveLoadGuard.GetFailure(); !Result) return Result;
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
		return {};
	}

}

namespace Durin
{
	auto FormatPackageGraphPrepareError(const FPackageGraphPrepareResult& Result) -> std::string
	{
		if (Result) return {};
		if (Result.Cause) return Result.Cause->ToString();
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
