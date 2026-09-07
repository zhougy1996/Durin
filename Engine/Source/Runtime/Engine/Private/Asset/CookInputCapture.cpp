#include "CookInputCapture.h"
#include "Asset/RegistryOperations.h"
#include "Asset/References.h"
#include "Asset/EditorBulkDataStorage.h"
#include "AssetRegistry/Publication.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "Misc/FileHelper.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::AssetPrivate
{
	thread_local FCookInputCapture* FCookInputCapture::Active = nullptr;

	namespace
	{
		constexpr uint64 MaximumCaptureBytes = 1024ull * 1024 * 1024;
		constexpr uint64 MaximumCaptureFileBytes = 256ull * 1024 * 1024;
		auto DigestValue(FByteView Bytes) -> FByteBuffer
		{
			FBinaryWriter Writer;
			Writer.WriteHash128(FXxHash128::HashBuffer(Bytes));
			return Writer.TakeBytes();
		}
	}

	FCookInputCapture::FCookInputCapture(const FCookRequest& InRequest,
		FAssetRegistrySnapshot InRegistry, FResolveContributor InResolve)
		: Request(InRequest), Registry(std::move(InRegistry)), ResolveContributor(std::move(InResolve)),
		Schema(FReflectionSchemaCatalog::Capture())
	{
		const auto CurrentMounts = FMountPaths::GetRegisteredMountPoints();
		Mounts.assign(CurrentMounts.begin(), CurrentMounts.end());
		check(!Active);
		Active = this;
	}

	FCookInputCapture::~FCookInputCapture()
	{
		ReleaseObjects();
		Active = nullptr;
	}

	auto FCookInputCapture::Fail(EAssetError Error, std::string Message, ECookInputStatus InputStatus) -> FAssetResult
	{
		if (Failure)
		{
			Failure = {Error, std::move(Message)};
			Status = Phase == ECookCapturePhase::Cancelled ? ECookInputStatus::Cancelled
				: Error == EAssetError::IoError ? ECookInputStatus::IoError
				: Error == EAssetError::StaleData ? ECookInputStatus::InputChanged : InputStatus;
		}
		if (Phase != ECookCapturePhase::Cancelled) Phase = ECookCapturePhase::Failed;
		return Failure;
	}

	auto FCookInputCapture::Fail(const FAssetResult& Result) -> FAssetResult
	{
		if (!Failure) return Failure;
		(void)Fail(Result.Error, Result.Message);
		Failure = Result;
		if (Result.Disposition == EAssetResultDisposition::ContentCommittedProjectionPending)
			Status = ECookInputStatus::ProjectionPending;
		return Failure;
	}

	auto FCookInputCapture::Verify() -> FAssetResult
	{
		if (!Failure) return Failure;
		if (Request.IsCancelled && Request.IsCancelled())
		{
			Phase = ECookCapturePhase::Cancelled;
			return Fail(EAssetError::InUse, "Cook input capture cancelled.");
		}
		const auto CurrentMounts = FMountPaths::GetRegisteredMountPoints();
		if (!std::ranges::equal(Mounts, CurrentMounts, [](const auto& A, const auto& B) {
			return A.VirtualRoot == B.VirtualRoot && A.Owner == B.Owner && A.Root == B.Root
				&& A.ContentPath == B.ContentPath && A.bAutoScan == B.bAutoScan
				&& A.bContentWritable == B.bContentWritable && A.Dependencies == B.Dependencies;
		})) return Fail(EAssetError::StaleData, "Cook mount definitions changed during capture.");
		const std::vector<FPackagePath> Paths(Participants.begin(), Participants.end());
		const auto Admission = ValidateAssetRegistryParticipants(Registry.Catalog, Paths);
		if (!Admission)
		{
			(void)Fail(EAssetError::StaleData, std::format("Cook input participant changed: {}", Admission.FailedParticipant.GetView()));
			Failure.FailedParticipant = Admission.FailedParticipant.ToString();
			if (Admission.State == EAssetRegistryAdmissionState::ProjectionPending)
			{
				Status = ECookInputStatus::ProjectionPending;
				Failure.Disposition = EAssetResultDisposition::ContentCommittedProjectionPending;
			}
			return Failure;
		}
		for (const auto& Path : Participants)
			if (FindPackage(Path.GetView())) return Fail(EAssetError::InUse,
				std::format("ResidentInputConflict: {}", Path.GetView()), ECookInputStatus::ResidentInputConflict);
		return {};
	}

	auto FCookInputCapture::ReadFile(const std::filesystem::path& Path, FByteBuffer& Out) -> FAssetResult
	{
		FFileHelper::FFileIoError Error;
		auto File = FFileHelper::OpenRead(Path, &Error);
		if (!File) return Fail(EAssetError::IoError, Error.ToString());
		const uint64 Size = File->GetSize();
		if (Size > MaximumCaptureFileBytes || Size > MaximumCaptureBytes - RetainedBytes)
			return Fail(EAssetError::CorruptFile, "Cook input byte limit exceeded.", ECookInputStatus::LimitExceeded);
		Out.resize(static_cast<size_t>(Size));
		RetainedBytes += Size;
		constexpr size_t Chunk = 4 * 1024 * 1024;
		for (size_t Offset = 0; Offset < Out.size(); Offset += Chunk)
		{
			if (auto Result = Verify(); !Result) return Result;
			if (!File->ReadAt(Offset, std::span(Out).subspan(Offset, std::min(Chunk, Out.size() - Offset)), &Error))
				return Fail(EAssetError::IoError, Error.ToString());
		}
		return {};
	}

	auto FCookInputCapture::AcquirePackage(const FPackagePath& Path) -> FAssetResult
	{
		if (Inputs.contains(Path)) return {};
		if (Inputs.size() >= MaximumCookDependencyRecords)
			return Fail(EAssetError::CorruptFile, "Cook input package limit exceeded.", ECookInputStatus::LimitExceeded);
		Participants.insert(Path);
		if (Request.ReportProgress) Request.ReportProgress({ECookOperationStage::Discovery, Path, Inputs.size(), 0});
		if (auto Result = Verify(); !Result) return Result;
		const auto* Data = Registry.Catalog.FindExact(Path);
		if (!Data) return Fail(EAssetError::MissingDependency, std::format("Cook input not found: {}", Path.GetView()));
		FInput Input;
		if (auto Result = ReadFile(Data->PhysicalPath, Input.PackageBytes); !Result) return Result;
		if (Data->BulkSegmentExtent)
		{
			auto BulkPath = std::filesystem::path(Data->PhysicalPath); BulkPath.replace_extension(".dbulk");
			if (auto Result = ReadFile(BulkPath, Input.BulkBytes); !Result) return Result;
		}
		if (auto Result = ResolveAssetPackageReader(Input.PackageBytes, Input.Codec); !Result)
			return Fail(Result);
		Input.BulkIdentity = DigestValue(Input.BulkBytes);
		Input.BulkSize = Input.BulkBytes.size();
		FAssetPackageReadContext Context{.PackageBytes = Input.PackageBytes, .PackagePath = Path,
			.PhysicalBulkBytes = Input.BulkSize, .bResourceBackedBulk = true};
		if (auto Result = Input.Codec->Inspect(Context, Input.Inspection); !Result) return Fail(Result);
		const auto& Header = Input.Inspection.Header;
		if (Header.TopLevelAssets != Data->TopLevelAssets || Header.Dependencies != Data->Dependencies
			|| Header.SoftDependencies != Data->SoftDependencies || Header.FormatVersion != Data->FormatVersion
			|| Header.ObjectCount != Data->ObjectCount || Header.BulkSegmentExtent != Data->BulkSegmentExtent
			|| Header.BulkSegmentDigest != Data->BulkSegmentDigest)
			return Fail(EAssetError::StaleData, std::format("Captured package metadata differs from Registry: {}", Path.GetView()));
		if (auto Result = Input.Codec->ExtractReferences(Context, Input.References); !Result) return Fail(Result);
		if (!Input.BulkBytes.empty())
		{
			std::vector<FEditorBulkDataStorageDescriptor> Descriptors;
			std::string Error;
			if (!InspectEditorBulkDataStorageDescriptors(Input.Inspection, Descriptors, &Error))
				return Fail(EAssetError::CorruptFile, std::move(Error));
			std::vector<FPackageBulkDataEntry> Entries;
			for (size_t Index = 0; Index < Descriptors.size(); ++Index)
			{
				const auto& D = Descriptors[Index];
				Entries.push_back({.FieldIndex = Index + 1, .Placement = D.StorageKind == EEditorBulkDataStorageKind::External
					? EPackageBulkDataPlacement::External : EPackageBulkDataPlacement::Inline,
					.LogicalSize = D.LogicalByteCount, .StoredSize = D.StoredByteCount, .SegmentOffset = D.SegmentOffset,
					.Alignment = D.Alignment, .ContentId = D.ContentHash});
			}
			if (!CreateOwnedPackageResource({Header.BulkSegmentExtent, Header.BulkSegmentDigest},
				Entries, Input.BulkBytes, Input.Resource, &Error)) return Fail(EAssetError::CorruptFile, std::move(Error));
		}
		Input.BulkBytes.clear(); Input.BulkBytes.shrink_to_fit();
		Inputs.emplace(Path, std::move(Input));
		return {};
	}

	auto FCookInputCapture::Resolve(const FPackagePath& Requested, FPackagePath& Final) -> FAssetResult
	{
		const auto Resolution = Registry.ResolveAssetPath(Requested);
		if (auto Result = ValidateResolvedAssetForOperation(Registry, Resolution); !Result) return Fail(Result);
		for (const auto& Alias : Resolution.RedirectChain)
			if (auto Result = AcquirePackage(Alias); !Result) return Result;
		Final = Resolution.FinalPath;
		return AcquirePackage(Final);
	}

	auto FCookInputCapture::CaptureSchema(FInput& Input, FCookPackageBuildInputs& Node) -> FAssetResult
	{
		std::unordered_set<std::string> Names;
		for (const auto& Object : Input.Inspection.Objects) Names.insert(Object.ClassName);
		for (const auto& Name : Names)
		{
			if (const auto Existing = SchemaValues.find(Name); Existing != SchemaValues.end())
			{
				Node.Inputs.push_back({ECookBuildDependencyKind::SchemaProducerVersion, "schema/" + Name, Existing->second});
				continue;
			}
			const auto* Class = Schema.FindClass(Name);
			DClass* LiveClass = FindClassByQualifiedName(FName(Name));
			if (!Class || !LiveClass) return Fail(EAssetError::UnknownClass, std::format("Cook schema unavailable: {}", Name));
			FBinaryWriter Writer({MaximumCookDependencyValueBytes, 4096});
			Writer.WriteString(Class->QualifiedName);
			Writer.WriteU64(static_cast<uint64>(LiveClass->GetClassFlags()));
			Writer.WriteU8(Class->bConstructible);
			Writer.WriteU32(static_cast<uint32>(Class->Ancestry.size()));
			std::unordered_set<std::string> SeenTypes{Name};
			for (const auto& Parent : Class->Ancestry) { Writer.WriteString(Parent); SeenTypes.insert(Parent); }
			bool Valid = true;
			uint64 Fields = 0;
			auto DeclaringType = [](const FProperty* Field) -> std::string {
				auto Owner = Field->Owner;
				while (auto* Parent = Owner.ToField()) Owner = Parent->Owner;
				const auto* Object = Owner.ToDObject();
				return Object ? Object->GetObjectPath() : std::string{};
			};
			std::function<void(FProperty*, uint32)> Property;
			std::function<void(DStructBase*, uint32)> Struct;
			Struct = [&](DStructBase* Type, uint32 Depth) {
				if (Depth > 64) { Valid = false; return; }
				std::vector<FProperty*> Properties;
				Type->ForEachProperty([&](FProperty* Field) { Properties.push_back(Field); });
				std::ranges::sort(Properties, [&](const auto* A, const auto* B) {
					return std::pair(DeclaringType(A), A->NamePrivate.ToString()) < std::pair(DeclaringType(B), B->NamePrivate.ToString());
				});
				Writer.WriteU32(static_cast<uint32>(Properties.size()));
				for (auto* Field : Properties) Property(Field, Depth + 1);
			};
			Property = [&](FProperty* Field, uint32 Depth) {
				if (!Field || Depth > 64 || ++Fields > MaximumCookDependencyRecords) { Valid = false; return; }
				using K = DurinCodeGen::EPropertyGenFlags;
				Writer.WriteString(DeclaringType(Field));
				Writer.WriteString(Field->NamePrivate.ToString());
				Writer.WriteU32(static_cast<uint32>(Field->GetKind()));
				Writer.WriteU64(static_cast<uint64>(Field->GetPropertyFlags()));
				Writer.WriteU16(Field->GetArrayDim());
				if (Field->GetKind() == K::Struct)
				{
					auto* Type = static_cast<FStructProperty*>(Field)->GetStruct();
					if (!Type) { Valid = false; return; }
					const auto TypeName = Type->GetQualifiedName().ToString();
					Writer.WriteString(TypeName);
					Writer.WriteU32(Type->GetOps().Version);
					Writer.WriteU64(static_cast<uint64>(Type->GetOps().Flags));
					const bool Fresh = SeenTypes.insert(TypeName).second;
					Writer.WriteU8(Fresh);
					if (Fresh) Struct(Type, Depth + 1);
				}
				else if (Field->GetKind() == K::Array) Property(static_cast<FArrayProperty*>(Field)->GetInner(), Depth + 1);
				else if (Field->GetKind() == K::Map)
				{
					auto* Map = static_cast<FMapProperty*>(Field);
					Property(Map->GetKeyProp(), Depth + 1); Property(Map->GetValueProp(), Depth + 1);
				}
				else if (Field->GetKind() == K::Enum)
				{
					const auto* Enum = static_cast<FEnumProperty*>(Field)->GetEnum();
					if (!Enum) { Valid = false; return; }
					Writer.WriteString(Enum->GetQualifiedName().ToString());
					Writer.WriteU32(static_cast<uint32>(Enum->GetUnderlyingType()));
					Writer.WriteU32(static_cast<uint32>(Enum->GetValues().size()));
					for (const auto& Value : Enum->GetValues()) { Writer.WriteString(Value.Name.ToString()); Writer.WriteU64(Value.Value); }
					SeenTypes.insert(Enum->GetQualifiedName().ToString());
				}
				else if (Field->GetKind() == K::Object || Field->GetKind() == K::SoftObject)
				{
					const DClass* Expected = Field->GetKind() == K::Object ? Field->GetReferencedClass()
						: static_cast<FSoftObjectProperty*>(Field)->GetExpectedClass();
					Writer.WriteString(Expected ? Expected->GetQualifiedName().ToString() : "");
				}
			};
			Struct(LiveClass, 0);
			std::vector<const FReflectionSerializedAlias*> Aliases;
			for (const auto& Alias : Schema.GetSerializedAliases()) if (SeenTypes.contains(Alias.CurrentIdentity)) Aliases.push_back(&Alias);
			Writer.WriteU32(static_cast<uint32>(Aliases.size()));
			for (const auto* Alias : Aliases) { Writer.WriteString(Alias->StoredIdentity); Writer.WriteString(Alias->CurrentIdentity); Writer.WriteU8(static_cast<uint8>(Alias->Kind)); }
			std::vector<const FReflectionSerializedPropertyAlias*> PropertyAliases;
			for (const auto& Alias : Schema.GetSerializedPropertyAliases()) if (SeenTypes.contains(Alias.DeclaringType)) PropertyAliases.push_back(&Alias);
			Writer.WriteU32(static_cast<uint32>(PropertyAliases.size()));
			for (const auto* Alias : PropertyAliases) { Writer.WriteString(Alias->DeclaringType); Writer.WriteString(Alias->StoredName); Writer.WriteString(Alias->CurrentName); }
			std::vector<const FReflectionDeprecatedPropertyRoute*> Routes;
			for (const auto& Route : Schema.GetDeprecatedPropertyRoutes()) if (SeenTypes.contains(Route.DeclaringType)) Routes.push_back(&Route);
			Writer.WriteU32(static_cast<uint32>(Routes.size()));
			for (const auto* Route : Routes) { Writer.WriteString(Route->DeclaringType); Writer.WriteString(Route->StoredName); Writer.WriteString(Route->DeprecatedPropertyName); Writer.WriteU32(static_cast<uint32>(Route->Kind)); Writer.WriteString(Route->TypeSignature); }
			if (!Valid || Writer.HasError()) return Fail(EAssetError::CorruptFile, "Cook schema limit exceeded.", ECookInputStatus::LimitExceeded);
			auto Bytes = Writer.TakeBytes();
			if (Bytes.size() > MaximumCaptureBytes - RetainedBytes)
				return Fail(EAssetError::CorruptFile, "Cook schema storage limit exceeded.", ECookInputStatus::LimitExceeded);
			RetainedBytes += Bytes.size();
			SchemaValues.emplace(Name, Bytes);
			Node.Inputs.push_back({ECookBuildDependencyKind::SchemaProducerVersion, "schema/" + Name, std::move(Bytes)});
		}
		return {};
	}

	auto FCookInputCapture::Acquire(std::span<const FPackagePath> Roots,
		const FAssetReferenceStoreCapture& ExternalRoots) -> FAssetResult
	{
		if (Phase != ECookCapturePhase::Acquiring) return Fail(EAssetError::InUse, "Cook input capture is not acquiring.");
		if (Roots.size() > MaximumCookDependencyRecords)
			return Fail(EAssetError::CorruptFile, "Cook root limit exceeded.", ECookInputStatus::LimitExceeded);
		std::vector<FPackagePath> Pending(Roots.begin(), Roots.end());
		for (const auto& Store : ExternalRoots.Stores)
			for (const auto& Root : Store.Occurrences)
				if (Root.bCookRoot)
				{
					const DClass* Expected = Root.ExpectedClass.empty() ? nullptr : FindClassByQualifiedName(FName(Root.ExpectedClass));
					if (!Root.ExpectedClass.empty() && !Expected) return Fail(EAssetError::UnknownClass, "Cook root class unavailable.");
					if (auto Result = ValidateResolvedAssetForOperation(Registry, Registry.ResolveAssetPath(Root.TargetPath), Expected); !Result)
						return Fail(Result);
					if (Pending.size() >= MaximumCookDependencyRecords)
						return Fail(EAssetError::CorruptFile, "Cook root limit exceeded.", ECookInputStatus::LimitExceeded);
					Pending.push_back(Root.TargetPath);
				}
		std::unordered_set<FPackagePath> Runtime;
		uint64 RuntimeEdges = 0;
		while (!Pending.empty())
		{
			const auto Requested = Pending.back(); Pending.pop_back();
			FPackagePath Path;
			if (auto Result = Resolve(Requested, Path); !Result) return Result;
			if (!Runtime.insert(Path).second) continue;
			const auto& Input = Inputs.at(Path);
			RuntimeEdges += Input.Inspection.Header.Dependencies.size() + Input.References.size();
			if (RuntimeEdges > MaximumCookDependencyRecords)
				return Fail(EAssetError::CorruptFile, "Cook runtime edge limit exceeded.", ECookInputStatus::LimitExceeded);
			Pending.insert(Pending.end(), Input.Inspection.Header.Dependencies.begin(), Input.Inspection.Header.Dependencies.end());
			for (const auto& Reference : Input.References)
			{
				if (Reference.Kind == EAssetReferenceKind::Redirect) continue;
				const auto Resolution = Registry.ResolveAssetObjectPath(Reference.TargetPath);
				const DClass* Expected = Reference.ExpectedClass.empty() ? nullptr : FindClassByQualifiedName(FName(Reference.ExpectedClass));
				if (!Reference.ExpectedClass.empty() && !Expected) return Fail(EAssetError::UnknownClass, "Cook reference class unavailable.");
				if (auto Result = ValidateResolvedAssetForOperation(Registry, Resolution, Expected); !Result)
					return Fail(Result.Error == EAssetError::NotFound ? EAssetError::MissingDependency : Result.Error, Result.Message);
				for (const auto& Alias : Resolution.RedirectChain)
					if (auto Result = AcquirePackage(Alias.GetPackagePath()); !Result) return Result;
				Pending.push_back(Resolution.FinalPath.GetPackagePath());
			}
		}
		RuntimePackages.assign(Runtime.begin(), Runtime.end());
		std::ranges::sort(RuntimePackages, [](const auto& A, const auto& B) { return A.GetView() < B.GetView(); });
		if (RuntimePackages.empty()) return Fail(EAssetError::NotFound, "Cook selected no runtime packages.");

		std::vector<FCookPackageBuildInputs> Graph;
		std::unordered_set<FPackagePath> Declared;
		while (Declared.size() < Inputs.size())
		{
			std::vector<FPackagePath> Batch;
			for (const auto& [Path, Input] : Inputs) if (!Declared.contains(Path)) Batch.push_back(Path);
			std::ranges::sort(Batch, [](const auto& A, const auto& B) { return A.GetView() < B.GetView(); });
			for (const auto& Path : Batch)
			{
				if (auto Result = Verify(); !Result) return Result;
				Declared.insert(Path);
				auto& Input = Inputs.at(Path);
				FCookPackageBuildInputs Node{.Package = Path};
				Node.Inputs.push_back({ECookBuildDependencyKind::SourcePackage, Path.ToString(), DigestValue(Input.PackageBytes)});
				Node.Inputs.push_back({ECookBuildDependencyKind::OwnedBulk, Path.ToString(), Input.BulkIdentity});
				std::unordered_set<FPackagePath> Automatic;
				for (const auto& Dependency : Input.Inspection.Header.Dependencies)
				{
					FPackagePath Final;
					if (auto Result = Resolve(Dependency, Final); !Result) return Result;
					// Keep aliases in the build graph so their actual bytes invalidate dependents.
					if (Automatic.insert(Dependency).second) Node.Packages.push_back({Dependency, true});
				}
				if (auto Result = CaptureSchema(Input, Node); !Result) return Result;
				const auto* Data = Registry.Catalog.FindExact(Path);
				const auto ContributorResult = ResolveContributor(*Data, Input.Contributor);
				if (!ContributorResult && Runtime.contains(Path)) return Fail(ContributorResult.Error, ContributorResult.Message);
				if (ContributorResult)
				{
					FBinaryWriter Versions;
					Versions.WriteU32(Input.Contributor.ContributorVersion);
					Versions.WriteU32(Input.Contributor.FamilyProducerVersion);
					Node.Inputs.push_back({ECookBuildDependencyKind::SchemaProducerVersion,
						"contributor/" + Input.Contributor.Name, Versions.TakeBytes()});
					if (Input.Contributor.DeclareDependencies)
					{
						std::vector<FCookDependencyDeclaration> Declarations;
						const auto Result = Input.Contributor.DeclareDependencies(
							{Path, Request.TargetPlatform, Request.TargetProfile, Request.bRetainEditorOnlyData}, Declarations);
						if (auto Admission = Verify(); !Admission) return Admission;
						if (!Result) return Fail(Result);
						if (Declarations.size() > MaximumCookDependencyRecords) return Fail(EAssetError::CorruptFile, "Cook declaration limit exceeded.");
						std::set<std::pair<ECookBuildDependencyKind, std::string>> Unique;
						for (const auto& Declaration : Declarations)
						{
							if (Declaration.LogicalName.empty() || Declaration.LogicalName.size() > 4096
								|| Declaration.LogicalName.find('\0') != std::string::npos
								|| !Unique.emplace(Declaration.Kind, Declaration.LogicalName).second)
								return Fail(EAssetError::CorruptFile, "Invalid or duplicate Cook declaration.");
							if (Declaration.Kind == ECookBuildDependencyKind::DirectPackage
								|| Declaration.Kind == ECookBuildDependencyKind::TransitivePackage)
							{
								FPackagePath Dependency, Final;
								if (!Declaration.FilePath.empty() || !Declaration.Value.empty()
									|| !FPackagePath::TryCreate(Declaration.LogicalName, Dependency))
									return Fail(EAssetError::InvalidPath, "Invalid declared build package.");
								if (auto Result = Resolve(Dependency, Final); !Result) return Result;
								if (Declaration.Kind == ECookBuildDependencyKind::TransitivePackage)
									Node.Packages.push_back({Dependency, true});
								else
								{
									const auto Resolution = Registry.ResolveAssetPath(Dependency);
									for (const auto& Alias : Resolution.RedirectChain) Node.Packages.push_back({Alias, false});
									Node.Packages.push_back({Final, false});
								}
								continue;
							}
							FByteBuffer Value;
							if (Declaration.Kind == ECookBuildDependencyKind::ExternalFile)
							{
								if (!Declaration.Value.empty() || Declaration.FilePath.empty()) return Fail(EAssetError::InvalidPath, "Invalid external file declaration.");
								if (auto Result = ReadFile(Declaration.FilePath, Value); !Result) return Result;
								Node.Inputs.push_back({Declaration.Kind, Declaration.LogicalName, DigestValue(Value)});
							}
							else if (Declaration.Kind == ECookBuildDependencyKind::ConfigurationValue
								|| Declaration.Kind == ECookBuildDependencyKind::SchemaProducerVersion)
							{
								if (!Declaration.FilePath.empty() || Declaration.Value.size() > MaximumCookDependencyValueBytes)
									return Fail(EAssetError::CorruptFile, "Invalid Cook value declaration.");
								const uint64 RetainedValueBytes = Declaration.Value.size() * 2;
								if (RetainedValueBytes > MaximumCaptureBytes - RetainedBytes)
									return Fail(EAssetError::CorruptFile, "Cook declared value limit exceeded.", ECookInputStatus::LimitExceeded);
								RetainedBytes += RetainedValueBytes;
								Value = Declaration.Value;
								Node.Inputs.push_back({Declaration.Kind, Declaration.LogicalName, Value});
							}
							else return Fail(EAssetError::CorruptFile, "Contributor declared a reserved Cook dependency kind.");
							Input.DeclaredValues.emplace(std::pair(Declaration.Kind, Declaration.LogicalName), std::move(Value));
						}
						Input.bDeclared = true;
					}
				}
				FBinaryWriter Settings;
				Settings.WriteU32(static_cast<uint32>(Request.TargetPlatform));
				Settings.WriteU32(static_cast<uint32>(Request.TargetProfile));
				Settings.WriteU8(Request.bRetainEditorOnlyData);
				Node.Inputs.push_back({ECookBuildDependencyKind::ConfigurationValue, "cook/target", Settings.TakeBytes()});
				// Resolution may make distinct declarations share an observed participant.
				std::ranges::sort(Node.Packages, [](const auto& A, const auto& B) {
					return std::pair(A.Package.GetView(), !A.bTransitive) < std::pair(B.Package.GetView(), !B.bTransitive);
				});
				Node.Packages.erase(std::unique(Node.Packages.begin(), Node.Packages.end(), [](const auto& A, const auto& B) {
					return A.Package == B.Package;
				}), Node.Packages.end());
				Graph.push_back(std::move(Node));
			}
		}
		for (const auto& [Path, Input] : Inputs)
			if (!Input.Contributor.Name.empty() && !Input.bDeclared) UnversionedPackages.insert(Path.ToString());
		FCookBuildDependencyGraph Dependencies;
		std::string Error;
		if (!Dependencies.Initialize(Graph, &Error)) return Fail(EAssetError::CorruptFile, std::move(Error));
		for (const auto& Path : RuntimePackages)
		{
			if (auto Result = Verify(); !Result) return Result;
			if (!Dependencies.Expand(Path, Inputs.at(Path).Dependencies, &Error)) return Fail(EAssetError::CorruptFile, std::move(Error));
			for (const auto& Record : Inputs.at(Path).Dependencies)
			{
				const uint64 Size = Record.LogicalName.size() + Record.Value.size() + 9;
				if (Size > MaximumCaptureBytes - RetainedBytes) return Fail(EAssetError::CorruptFile,
					"Cook retained dependency limit exceeded.", ECookInputStatus::LimitExceeded);
				RetainedBytes += Size;
			}
		}
		if (auto Result = Verify(); !Result) return Result;
		Phase = ECookCapturePhase::Sealed;
		return {};
	}

	auto FCookInputCapture::IsReusable(const FPackagePath& Path) const -> bool
	{
		if (!Inputs.at(Path).bDeclared) return false;
		for (const auto& Dependency : Inputs.at(Path).Dependencies)
			if (Dependency.Kind == ECookBuildDependencyKind::SourcePackage)
				if (UnversionedPackages.contains(Dependency.LogicalName)) return false;
		return true;
	}

	auto FCookInputCapture::ReadInput(ECookBuildDependencyKind Kind, std::string_view Name, FByteBuffer& Out) -> FAssetResult
	{
		Out.clear();
		const auto Found = Inputs.find(CurrentPackage);
		if ((Phase != ECookCapturePhase::Sealed && Phase != ECookCapturePhase::Capturing) || Found == Inputs.end())
			return Fail(EAssetError::InUse, "Cook input read outside sealed capture.");
		const auto Value = Found->second.DeclaredValues.find({Kind, std::string(Name)});
		if (Value == Found->second.DeclaredValues.end()) return Fail(EAssetError::MissingDependency, std::format("UndeclaredInput: {}", Name), ECookInputStatus::UndeclaredInput);
		Out = Value->second;
		return {};
	}

	auto FCookInputCapture::LoadPackage(const FPackagePath& Path, DPackage*& Out) -> FAssetResult
	{
		Out = nullptr;
		if (auto Result = Verify(); !Result) return Result;
		if (Phase != ECookCapturePhase::Sealed && Phase != ECookCapturePhase::Capturing)
			return Fail(EAssetError::InUse, "Cook load outside sealed capture.");
		const auto Resolution = Registry.ResolveAssetPath(Path);
		if (!Resolution) return Fail(EAssetError::MissingDependency, "Unresolved captured package.");
		const auto Final = Resolution.FinalPath;
		if (const auto Found = Loaded.find(Final); Found != Loaded.end())
		{
			Out = static_cast<DPackage*>(Found->second.Get());
			return Out ? FAssetResult{} : Fail(EAssetError::InvalidObjectGraph, "Captured skeleton was destroyed.");
		}
		const auto Found = Inputs.find(Final);
		if (Found == Inputs.end()) return Fail(EAssetError::MissingDependency, "UndeclaredInput: package is outside sealed capture.", ECookInputStatus::UndeclaredInput);
		Phase = ECookCapturePhase::Capturing;
		auto& Input = Found->second;
		const auto Previous = CurrentPackage; CurrentPackage = Final;
		struct FRestore { FPackagePath& Current; FPackagePath Previous; ~FRestore() { Current = Previous; } } Restore{CurrentPackage, Previous};
		FAssetPackageReadContext Context{.PackageBytes = Input.PackageBytes, .BulkBytes = Input.BulkBytes,
			.PackagePath = Final, .PhysicalPackageBytes = Input.PackageBytes.size(), .PhysicalBulkBytes = Input.BulkSize,
			.bResourceBackedBulk = true, .BulkResource = Input.Resource,
			.DependencyLoadPolicy = FAssetPackageDependencyLoadPolicy{
				.ResolvePackage = [this](const auto& Dependency, DPackage*& Package) { return LoadPackage(Dependency, Package); },
				.ResolveObject = [this](const auto& Object, DObject*& Result) { return LoadObject(Object, Result); },
				.Rollback = [] {}, .bRejectImplicitLiveLoads = true}, .bPrivateGraph = true};
		const auto Result = Input.Codec->Load(Context, Out, nullptr,
			[&](DPackage* Package) -> FAssetResult { Loaded.emplace(Final, FStrongObjectPtr(Package)); return Verify(); },
			[&](DPackage*) { Loaded.erase(Final); });
		if (!Result) return Fail(Result);
		std::vector<DObject*> Pending{Out};
		while (!Pending.empty())
		{
			auto* Object = Pending.back(); Pending.pop_back();
			if (ObjectPins.size() >= MaximumCookDependencyRecords)
				return Fail(EAssetError::InvalidObjectGraph, "Cook object limit exceeded.", ECookInputStatus::LimitExceeded);
			ObjectPins.emplace_back(Object);
			const auto Children = GDObjectArray.GetObjectsWithOuter(Object, EObjectQueryScope::IncludeUnpublished);
			for (auto* Child : Children) if (!Child->IsTemplateObject()) Pending.push_back(Child);
		}
		return Verify();
	}

	auto FCookInputCapture::LoadObject(const FObjectPath& Path, DObject*& Out) -> FAssetResult
	{
		Out = nullptr;
		const auto Resolution = Registry.ResolveAssetObjectPath(Path);
		if (!Resolution) return Fail(EAssetError::MissingDependency, "Unresolved captured object.");
		DPackage* Package = nullptr;
		if (auto Result = LoadPackage(Resolution.FinalPath.GetPackagePath(), Package); !Result) return Result;
		DObject* Object = Package->FindTopLevelAsset(Resolution.FinalPath.GetAssetPath().GetAssetName());
		for (const auto Name : Resolution.FinalPath.GetSubobjectNames())
		{
			if (!Object) break;
			DObject* Child = nullptr;
			for (DObject* Candidate : GDObjectArray.GetObjectsWithOuter(Object, EObjectQueryScope::IncludeUnpublished))
				if (Candidate->GetFName() == FName(Name)) { Child = Candidate; break; }
			Object = Child;
		}
		if (!Object) return Fail(EAssetError::MissingDependency, "Captured exact object was not found.");
		Out = Object;
		return {};
	}

	auto FCookInputCapture::ReleaseObjects() -> void
	{
		for (auto& [Path, Object] : Loaded) if (Object.Get()) MarkObjectHierarchyAsGarbage(Object.Get());
		ObjectPins.clear();
		Loaded.clear();
		CollectGarbage();
	}

	auto FCookInputCapture::Detach() -> void
	{
		ReleaseObjects();
		Inputs.clear();
		ResolveContributor = {};
		Phase = ECookCapturePhase::Detached;
	}
}

namespace Durin
{
	auto IsCookInputCaptureActive() -> bool { return AssetPrivate::FCookInputCapture::GetActive() != nullptr; }
	auto ReadCapturedCookInput(ECookBuildDependencyKind Kind, std::string_view Name,
		FByteBuffer& Out) -> FAssetResult
	{
		if (auto* Capture = AssetPrivate::FCookInputCapture::GetActive()) return Capture->ReadInput(Kind, Name, Out);
		Out.clear();
		return {EAssetError::InUse, "No active Cook input capture."};
	}
}
