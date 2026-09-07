#include "CookDependencyDiscovery.h"
#include "Shader/ShaderBuildProvider.h"
#include "Asset/RegistryOperations.h"
#include "Asset/References.h"
#include "Asset/EditorBulkDataStorage.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectGlobals.h"
#include "Misc/FileHelper.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::AssetPrivate
{

	namespace
	{
		constexpr uint64 MaximumDiscoveryBytes = 1024ull * 1024 * 1024;
		constexpr uint64 MaximumDiscoveryFileBytes = 256ull * 1024 * 1024;
		auto DigestValue(FByteView Bytes) -> FByteBuffer
		{
			FBinaryWriter Writer;
			Writer.WriteHash128(FXxHash128::HashBuffer(Bytes));
			return Writer.TakeBytes();
		}
	}

	FCookDependencyDiscovery::FCookDependencyDiscovery(const FCookRequest& InRequest,
		FAssetRegistrySnapshot InRegistry, FResolveContributor InResolve)
		: Request(InRequest), Registry(std::move(InRegistry)), ResolveContributor(std::move(InResolve)),
		Schema(FReflectionSchemaCatalog::Capture())
	{
	}

	auto FCookDependencyDiscovery::Fail(EAssetError Error, std::string Message, ECookInputStatus InputStatus) -> FAssetResult
	{
		if (Failure)
		{
			Failure = {Error, std::move(Message)};
			Status = Error == EAssetError::IoError ? ECookInputStatus::IoError
				: InputStatus;
		}
		return Failure;
	}

	auto FCookDependencyDiscovery::Fail(const FAssetResult& Result) -> FAssetResult
	{
		if (!Failure) return Failure;
		(void)Fail(Result.Error, Result.Message);
		Failure = Result;
		if (Result.Disposition == EAssetResultDisposition::ContentCommittedProjectionPending)
			Status = ECookInputStatus::ProjectionPending;
		return Failure;
	}

	auto FCookDependencyDiscovery::CheckCancellation() -> FAssetResult
	{
		if (!Failure) return Failure;
		if (Request.IsCancelled && Request.IsCancelled())
			return Fail(EAssetError::InUse, "Cook cancelled.", ECookInputStatus::Cancelled);
		return {};
	}

	auto FCookDependencyDiscovery::ReadFile(const std::filesystem::path& Path, FByteBuffer& Out) -> FAssetResult
	{
		FFileHelper::FFileIoError Error;
		auto File = FFileHelper::OpenRead(Path, &Error);
		if (!File) return Fail(EAssetError::IoError, Error.ToString());
		const uint64 Size = File->GetSize();
		if (Size > MaximumDiscoveryFileBytes || Size > MaximumDiscoveryBytes - RetainedBytes)
			return Fail(EAssetError::CorruptFile, "Cook input byte limit exceeded.", ECookInputStatus::LimitExceeded);
		Out.resize(static_cast<size_t>(Size));
		constexpr size_t Chunk = 4 * 1024 * 1024;
		for (size_t Offset = 0; Offset < Out.size(); Offset += Chunk)
		{
			if (auto Result = CheckCancellation(); !Result) return Result;
			if (!File->ReadAt(Offset, std::span(Out).subspan(Offset, std::min(Chunk, Out.size() - Offset)), &Error))
				return Fail(EAssetError::IoError, Error.ToString());
		}
		return {};
	}

	auto FCookDependencyDiscovery::AcquirePackage(const FPackagePath& Path) -> FAssetResult
	{
		if (Inputs.contains(Path)) return {};
		if (Inputs.size() >= MaximumCookDependencyRecords)
			return Fail(EAssetError::CorruptFile, "Cook input package limit exceeded.", ECookInputStatus::LimitExceeded);
		if (Request.ReportProgress) Request.ReportProgress({ECookOperationStage::Discovery, Path, Inputs.size(), 0});
		if (auto Result = CheckCancellation(); !Result) return Result;
		const auto* Data = Registry.Catalog.FindExact(Path);
		if (!Data) return Fail(EAssetError::MissingDependency, std::format("Cook input not found: {}", Path.GetView()));
		FInput Input;
		FByteBuffer PackageBytes, BulkBytes;
		if (auto Result = ReadFile(Data->PhysicalPath, PackageBytes); !Result) return Result;
		if (Data->BulkSegmentExtent)
		{
			auto BulkPath = std::filesystem::path(Data->PhysicalPath); BulkPath.replace_extension(".dbulk");
			if (auto Result = ReadFile(BulkPath, BulkBytes); !Result) return Result;
		}
		Input.PackageIdentity = DigestValue(PackageBytes);
		Input.BulkIdentity = DigestValue(BulkBytes);
		const FAssetPackageCodec* Codec = nullptr;
		if (auto Result = ResolveAssetPackageReader(PackageBytes, Codec); !Result) return Fail(Result);
		FAssetPackageReadContext Context{.PackageBytes = PackageBytes, .PackagePath = Path,
			.PhysicalBulkBytes = BulkBytes.size(), .bResourceBackedBulk = true};
		if (auto Result = Codec->Inspect(Context, Input.Inspection); !Result) return Fail(Result);
		if (auto Result = Codec->ExtractReferences(Context, Input.References); !Result) return Fail(Result);
		// Schema discovery needs class identities, not retained serialized field payloads.
		for (auto& Object : Input.Inspection.Objects)
		{
			Object.Fields.clear(); Object.Fields.shrink_to_fit();
		}
		const auto& Header = Input.Inspection.Header;
		if (Header.BulkSegmentExtent != BulkBytes.size()
			|| (!BulkBytes.empty() && Header.BulkSegmentDigest != FXxHash128::HashBuffer(BulkBytes)))
			return Fail(EAssetError::CorruptFile, "Cook bulk segment identity is invalid.");
		Inputs.emplace(Path, std::move(Input));
		return {};
	}

	auto FCookDependencyDiscovery::Resolve(const FPackagePath& Requested, FPackagePath& Final) -> FAssetResult
	{
		const auto Resolution = Registry.ResolveAssetPath(Requested);
		if (auto Result = ValidateResolvedAssetForOperation(Registry, Resolution); !Result) return Fail(Result);
		for (const auto& Alias : Resolution.RedirectChain)
			if (auto Result = AcquirePackage(Alias); !Result) return Result;
		Final = Resolution.FinalPath;
		return AcquirePackage(Final);
	}

	auto FCookDependencyDiscovery::CaptureSchema(FInput& Input, FCookPackageBuildInputs& Node) -> FAssetResult
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
			if (Bytes.size() > MaximumDiscoveryBytes - RetainedBytes)
				return Fail(EAssetError::CorruptFile, "Cook schema storage limit exceeded.", ECookInputStatus::LimitExceeded);
			RetainedBytes += Bytes.size();
			SchemaValues.emplace(Name, Bytes);
			Node.Inputs.push_back({ECookBuildDependencyKind::SchemaProducerVersion, "schema/" + Name, std::move(Bytes)});
		}
		return {};
	}

	auto FCookDependencyDiscovery::Acquire(std::span<const FPackagePath> Roots,
		const FAssetReferenceStoreCapture& ExternalRoots) -> FAssetResult
	{
		std::string ShaderError;
		(void)GetShaderCookInputIdentity(ShaderBuildIdentity, ShaderError, Request.IsCancelled);
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
				if (auto Result = CheckCancellation(); !Result) return Result;
				Declared.insert(Path);
				auto& Input = Inputs.at(Path);
				FCookPackageBuildInputs Node{.Package = Path};
				Node.Inputs.push_back({ECookBuildDependencyKind::SourcePackage, Path.ToString(), Input.PackageIdentity});
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
							{Path, Request.TargetPlatform, Request.TargetProfile, Request.bRetainEditorOnlyData, ShaderBuildIdentity}, Declarations);
						if (auto Admission = CheckCancellation(); !Admission) return Admission;
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
								Input.DeclaredFiles.emplace(Declaration.LogicalName, Declaration.FilePath);
								continue;
							}
							else if (Declaration.Kind == ECookBuildDependencyKind::ConfigurationValue
								|| Declaration.Kind == ECookBuildDependencyKind::SchemaProducerVersion)
							{
								if (!Declaration.FilePath.empty() || Declaration.Value.size() > MaximumCookDependencyValueBytes)
									return Fail(EAssetError::CorruptFile, "Invalid Cook value declaration.");
								const uint64 RetainedValueBytes = Declaration.Value.size() * 2;
								if (RetainedValueBytes > MaximumDiscoveryBytes - RetainedBytes)
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
			if (auto Result = CheckCancellation(); !Result) return Result;
			if (!Dependencies.Expand(Path, Inputs.at(Path).Dependencies, &Error)) return Fail(EAssetError::CorruptFile, std::move(Error));
			for (const auto& Record : Inputs.at(Path).Dependencies)
			{
				const uint64 Size = Record.LogicalName.size() + Record.Value.size() + 9;
				if (Size > MaximumDiscoveryBytes - RetainedBytes) return Fail(EAssetError::CorruptFile,
					"Cook retained dependency limit exceeded.", ECookInputStatus::LimitExceeded);
					}
		}
		if (auto Result = CheckCancellation(); !Result) return Result;
		return {};
	}

	auto FCookDependencyDiscovery::IsReusable(const FPackagePath& Path) const -> bool
	{
		if (!Inputs.at(Path).bDeclared) return false;
		for (const auto& Dependency : Inputs.at(Path).Dependencies)
			if (Dependency.Kind == ECookBuildDependencyKind::SourcePackage)
				if (UnversionedPackages.contains(Dependency.LogicalName)) return false;
		return true;
	}

	auto FCookDependencyDiscovery::ReadInput(const FPackagePath& Path,
		ECookBuildDependencyKind Kind, std::string_view Name, FByteBuffer& Out) -> FAssetResult
	{
		Out.clear();
		const auto Found = Inputs.find(Path);
		if (Found == Inputs.end()) return Fail(EAssetError::MissingDependency, "Unknown Cook dependency package.");
		if (Kind == ECookBuildDependencyKind::ExternalFile)
		{
			const auto File = Found->second.DeclaredFiles.find(std::string(Name));
			if (File != Found->second.DeclaredFiles.end()) return ReadFile(File->second, Out);
		}
		const auto Value = Found->second.DeclaredValues.find({Kind, std::string(Name)});
		if (Value == Found->second.DeclaredValues.end()) return Fail(EAssetError::MissingDependency,
			std::format("UndeclaredInput: {}", Name), ECookInputStatus::UndeclaredInput);
		Out = Value->second;
		return {};
	}
}
