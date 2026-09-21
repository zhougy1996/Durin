#include "Misc/PackageWriter.h"
#include "CookDependencyDiscovery.h"
#include "CookMemoryBudget.h"
#include "Shader/ShaderBuildProvider.h"
#include "Asset/RegistryOperations.h"
#include "Asset/References.h"
#include "Asset/EditorBulkDataStorage.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectGlobals.h"
#include "Misc/FileHelper.h"
#include "Serialization/BinaryFormat.h"
#include "Serialization/CustomVersion.h"

namespace Durin
{
	auto FormatCookInputError(const FCookInputFailure& Failure) -> std::string
	{
		switch (Failure.Error)
		{
		case ECookInputError::None: return {};
		case ECookInputError::Cancelled: return "Cook cancelled.";
		case ECookInputError::WriteConflict: return std::format("Cook input is being written: {}", Failure.File.string());
		case ECookInputError::FileIo: return Failure.FileCause ? Failure.FileCause->ToString() : "Cook input IO failed.";
		case ECookInputError::ByteLimit: return std::format("Cook input byte limit exceeded: file={}, size={}, maximum={}, retained={}, maximum retained={}", Failure.File.string(), Failure.Actual, Failure.Maximum, Failure.Retained, Failure.MaximumRetained);
		case ECookInputError::PackageLimit: return std::format("Cook input package limit exceeded: count={}, maximum={}", Failure.Actual, Failure.Maximum);
		case ECookInputError::UnknownPackage: return std::format("Cook input not found: {}", Failure.Package.ToString());
		case ECookInputError::UndeclaredInput: return std::format("UndeclaredInput: {}", Failure.Name);
		case ECookInputError::NoReader: return "No declared Cook inputs.";
		case ECookInputError::DeclarationCount: return std::format("Cook declaration limit exceeded: package={}, count={}, maximum={}", Failure.Package.ToString(), Failure.Actual, Failure.Maximum);
		case ECookInputError::DeclarationName: return std::format("Invalid Cook declaration name: package={}, name={}, bytes={}, maximum={}", Failure.Package.ToString(), Failure.Name, Failure.Actual, Failure.Maximum);
		case ECookInputError::DuplicateDeclaration: return std::format("Duplicate Cook declaration: package={}, kind={}, name={}", Failure.Package.ToString(), static_cast<uint32>(Failure.Kind), Failure.Name);
		case ECookInputError::PackageDeclaration: return std::format("Invalid declared build package '{}': {}", Failure.Name, Failure.PathCause ? ToString(*Failure.PathCause) : "Unexpected file or value payload");
		case ECookInputError::ExternalDeclaration: return std::format("Invalid external file declaration '{}': file={}, value bytes={}", Failure.Name, Failure.File.string(), Failure.Actual);
		case ECookInputError::ValueDeclaration: return std::format("Invalid Cook value declaration '{}': file={}, value bytes={}, maximum={}", Failure.Name, Failure.File.string(), Failure.Actual, Failure.Maximum);
		case ECookInputError::ValueStorage: return std::format("Cook declared value storage limit exceeded: name={}, requested={}, retained={}, maximum={}", Failure.Name, Failure.Actual, Failure.Retained, Failure.MaximumRetained);
		case ECookInputError::ReservedKind: return std::format("Contributor declared reserved Cook dependency kind {}: {}", static_cast<uint32>(Failure.Kind), Failure.Name);
		case ECookInputError::BulkIdentity: return std::format("Cook bulk segment identity is invalid: package={}, expected bytes={}, actual bytes={}", Failure.Package.ToString(), Failure.Expected, Failure.Actual);
		case ECookInputError::SchemaClass: return std::format("Cook schema unavailable: {}", Failure.Name);
		case ECookInputError::SchemaDepth: return std::format("Cook schema depth limit exceeded: class={}, member={}, depth={}, maximum={}", Failure.Name, Failure.Member, Failure.Actual, Failure.Maximum);
		case ECookInputError::SchemaFields: return std::format("Cook schema field limit exceeded: class={}, fields={}, maximum={}", Failure.Name, Failure.Actual, Failure.Maximum);
		case ECookInputError::SchemaField: return std::format("Cook schema contains a missing field: {}", Failure.Name);
		case ECookInputError::SchemaType: return std::format("Cook schema field type unavailable: class={}, member={}", Failure.Name, Failure.Member);
		case ECookInputError::SchemaEncoding: return std::format("Cook schema encoding limit exceeded: class={}, bytes={}, maximum={}", Failure.Name, Failure.Actual, Failure.Maximum);
		case ECookInputError::SchemaStorage: return std::format("Cook schema storage limit exceeded: class={}, requested={}, retained={}, maximum={}", Failure.Name, Failure.Actual, Failure.Retained, Failure.MaximumRetained);
		case ECookInputError::RootLimit: return std::format("Cook root limit exceeded: count={}, maximum={}", Failure.Actual, Failure.Maximum);
		case ECookInputError::RootClass: return std::format("Cook root class unavailable: package={}, class={}", Failure.Package.ToString(), Failure.Name);
		case ECookInputError::RuntimeEdgeLimit: return std::format("Cook runtime edge limit exceeded: package={}, edges={}, maximum={}", Failure.Package.ToString(), Failure.Actual, Failure.Maximum);
		case ECookInputError::ReferenceClass: return std::format("Cook reference class unavailable: package={}, class={}, member={}", Failure.Package.ToString(), Failure.Name, Failure.Member);
		case ECookInputError::NoRuntimePackages: return "Cook selected no runtime packages.";
		case ECookInputError::DependencyStorage: return std::format("Cook retained dependency limit exceeded: package={}, name={}, requested={}, retained={}, maximum={}", Failure.Package.ToString(), Failure.Name, Failure.Actual, Failure.Retained, Failure.MaximumRetained);
		}
		return "Unknown Cook input error.";
	}

	auto FCookInputFailure::ToInputResult() const -> FCookInputResult
	{
		if (Error == ECookInputError::None) return {};
		const auto Status = Error == ECookInputError::Cancelled ? ECookInputStatus::Cancelled
			: Error == ECookInputError::FileIo ? ECookInputStatus::IoError
			: Error == ECookInputError::ByteLimit || Error == ECookInputError::PackageLimit || Error == ECookInputError::ValueStorage
				|| Error == ECookInputError::SchemaDepth || Error == ECookInputError::SchemaFields
				|| Error == ECookInputError::SchemaField || Error == ECookInputError::SchemaType
				|| Error == ECookInputError::SchemaEncoding || Error == ECookInputError::SchemaStorage
				|| Error == ECookInputError::RootLimit || Error == ECookInputError::RuntimeEdgeLimit
				|| Error == ECookInputError::DependencyStorage ? ECookInputStatus::LimitExceeded
			: Error == ECookInputError::UndeclaredInput ? ECookInputStatus::UndeclaredInput : ECookInputStatus::InvalidDependency;
		return {Status, std::make_shared<const FCookInputFailure>(*this)};
	}

}

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

	auto FCookDependencyDiscovery::Fail(FCookInputFailure Cause) -> FCookInputResult
	{
		if (!Failure) return Failure;
		Failure = Cause.ToInputResult();

		return Failure;
	}

	auto FCookDependencyDiscovery::Fail(const FCookInputResult& Result) -> FCookInputResult
	{
		if (!Failure) return Failure;
		Failure = Result;
		return Failure;
	}

	auto FCookDependencyDiscovery::CheckCancellation() -> FCookInputResult
	{
		if (!Failure) return Failure;
		if (Request.IsCancelled && Request.IsCancelled())
			return Fail(FCookInputFailure{.Error = ECookInputError::Cancelled});
		return {};
	}

	auto FCookDependencyDiscovery::ReadFile(const std::filesystem::path& Path, FByteBuffer& Out) -> FCookInputResult
	{
		Out.clear();
		const std::array Paths{Path};
		auto Access = FPackageFileAccess::TryAcquire(Paths, false);
		if (!Access) return Fail(FCookInputFailure{.Error = ECookInputError::WriteConflict, .File = Path});
		auto File = FFileHelper::OpenRead(Path);
		if (!File) return Fail(FCookInputFailure{.Error = ECookInputError::FileIo, .FileCause = File.error()});
		const uint64 Size = (*File)->GetSize();
		if (Size > MaximumDiscoveryFileBytes || Size > MaximumDiscoveryBytes - RetainedBytes)
			return Fail(FCookInputFailure{.Error = ECookInputError::ByteLimit, .File = Path, .Actual = Size, .Maximum = MaximumDiscoveryFileBytes, .Retained = RetainedBytes, .MaximumRetained = MaximumDiscoveryBytes});
		FByteBuffer Candidate(static_cast<size_t>(Size));
		constexpr size_t Chunk = 4 * 1024 * 1024;
		for (size_t Offset = 0; Offset < Candidate.size(); Offset += Chunk)
		{
			if (auto Result = CheckCancellation(); !Result) return Result;
			if (auto Read = (*File)->ReadAt(Offset, std::span(Candidate).subspan(Offset, std::min(Chunk, Candidate.size() - Offset))); !Read)
				return Fail(FCookInputFailure{.Error = ECookInputError::FileIo, .FileCause = Read.error()});
		}
		Out = std::move(Candidate);
		return {};
	}

	auto FCookDependencyDiscovery::AcquirePackage(const FPackagePath& Path) -> FCookInputResult
	{
		if (Inputs.contains(Path)) return {};
		if (Inputs.size() >= MaximumCookDependencyRecords)
			return Fail(FCookInputFailure{.Error = ECookInputError::PackageLimit, .Package = Path, .Actual = Inputs.size(), .Maximum = MaximumCookDependencyRecords});
		if (Request.ReportProgress) Request.ReportProgress({ECookOperationStage::Discovery, Path, Inputs.size(), 0});
		if (auto Result = CheckCancellation(); !Result) return Result;
		const auto* Data = Registry.Catalog.FindExact(Path);
		if (!Data) return Fail(FCookInputFailure{.Error = ECookInputError::UnknownPackage, .Package = Path});
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
			return Fail(FCookInputFailure{.Error = ECookInputError::BulkIdentity, .Package = Path,
				.Actual = BulkBytes.size(), .Expected = Header.BulkSegmentExtent,
				.ExpectedDigest = Header.BulkSegmentDigest, .ActualDigest = FXxHash128::HashBuffer(BulkBytes)});
		Inputs.emplace(Path, std::move(Input));
		return {};
	}

	auto FCookDependencyDiscovery::Resolve(const FPackagePath& Requested, FPackagePath& Final) -> FCookInputResult
	{
		const auto Resolution = Registry.ResolveAssetPath(Requested);
		if (auto Result = ValidateResolvedAssetForOperation(Registry, Resolution); !Result) return Fail(Result);
		for (const auto& Alias : Resolution.RedirectChain)
			if (auto Result = AcquirePackage(Alias); !Result) return Result;
		Final = Resolution.FinalPath;
		return AcquirePackage(Final);
	}

	auto FCookDependencyDiscovery::CaptureSchema(FInput& Input, FCookPackageBuildInputs& Node) -> FCookInputResult
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
			if (!Class || !LiveClass) return Fail(FCookInputFailure{.Error = ECookInputError::SchemaClass, .Package = Node.Package, .Name = Name});
			FBinaryWriter Writer({MaximumCookDependencyValueBytes, 4096});
			Writer.WriteString(Class->QualifiedName);
			Writer.WriteU64(static_cast<uint64>(LiveClass->GetClassFlags()));
			Writer.WriteU8(Class->bConstructible);
			Writer.WriteU32(static_cast<uint32>(Class->Ancestry.size()));
			std::unordered_set<std::string> SeenTypes{Name};
			for (const auto& Parent : Class->Ancestry) { Writer.WriteString(Parent); SeenTypes.insert(Parent); }
			FCookInputFailure SchemaFailure;
			auto RejectSchema = [&](ECookInputError Code, uint64 Actual, uint64 Maximum, const FProperty* Field = nullptr) {
				if (SchemaFailure.Error != ECookInputError::None) return;
				SchemaFailure = {.Error = Code, .Package = Node.Package, .Name = Name, .Actual = Actual, .Maximum = Maximum,
					.Member = Field ? Field->NamePrivate.ToString() : std::string{}};
			};
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
				if (Depth > 64) { RejectSchema(ECookInputError::SchemaDepth, Depth, 64); return; }
				std::vector<FProperty*> Properties;
				Type->ForEachProperty([&](FProperty* Field) { Properties.push_back(Field); });
				std::ranges::sort(Properties, [&](const auto* A, const auto* B) {
					return std::pair(DeclaringType(A), A->NamePrivate.ToString()) < std::pair(DeclaringType(B), B->NamePrivate.ToString());
				});
				Writer.WriteU32(static_cast<uint32>(Properties.size()));
				for (auto* Field : Properties) Property(Field, Depth + 1);
			};
			Property = [&](FProperty* Field, uint32 Depth) {
				if (!Field) { RejectSchema(ECookInputError::SchemaField, 0, 0); return; }
				if (Depth > 64) { RejectSchema(ECookInputError::SchemaDepth, Depth, 64, Field); return; }
				if (++Fields > MaximumCookDependencyRecords) { RejectSchema(ECookInputError::SchemaFields, Fields, MaximumCookDependencyRecords, Field); return; }
				using K = DurinCodeGen::EPropertyGenFlags;
				Writer.WriteString(DeclaringType(Field));
				Writer.WriteString(Field->NamePrivate.ToString());
				Writer.WriteU32(static_cast<uint32>(Field->GetKind()));
				Writer.WriteU64(static_cast<uint64>(Field->GetPropertyFlags()));
				Writer.WriteU16(Field->GetArrayDim());
				if (Field->GetKind() == K::Struct)
				{
					auto* Type = static_cast<FStructProperty*>(Field)->GetStruct();
					if (!Type) { RejectSchema(ECookInputError::SchemaType, 0, 0, Field); return; }
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
					if (!Enum) { RejectSchema(ECookInputError::SchemaType, 0, 0, Field); return; }
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
			if (SchemaFailure.Error != ECookInputError::None) return Fail(std::move(SchemaFailure));
			if (Writer.HasError()) return Fail(FCookInputFailure{.Error = ECookInputError::SchemaEncoding,
				.Package = Node.Package, .Name = Name, .Actual = Writer.Tell(), .Maximum = MaximumCookDependencyValueBytes, .Expected = 4096});
			auto Bytes = Writer.TakeBytes();
			if (!TryRetainCookBytes(Bytes.size(), RetainedBytes, MaximumDiscoveryBytes))
				return Fail(FCookInputFailure{.Error = ECookInputError::SchemaStorage, .Package = Node.Package, .Name = Name, .Actual = Bytes.size(), .Retained = RetainedBytes, .MaximumRetained = MaximumDiscoveryBytes});
			SchemaValues.emplace(Name, Bytes);
			Node.Inputs.push_back({ECookBuildDependencyKind::SchemaProducerVersion, "schema/" + Name, std::move(Bytes)});
		}
		return {};
	}

	auto FCookDependencyDiscovery::Acquire(std::span<const FPackagePath> Roots,
		const FAssetReferenceStoreCapture& ExternalRoots) -> FCookInputResult
	{
		FShaderOperationResult ShaderError;
		(void)(ShaderError = GetShaderCookInputIdentity(ShaderBuildIdentity, Request.IsCancelled));
		if (Roots.size() > MaximumCookDependencyRecords)
			return Fail(FCookInputFailure{.Error = ECookInputError::RootLimit, .Actual = Roots.size(), .Maximum = MaximumCookDependencyRecords});
		std::vector<FPackagePath> Pending(Roots.begin(), Roots.end());
		for (const auto& Store : ExternalRoots.Stores)
			for (const auto& Root : Store.Occurrences)
				if (Root.bCookRoot)
				{
					const DClass* Expected = Root.ExpectedClass.empty() ? nullptr : FindClassByQualifiedName(FName(Root.ExpectedClass));
					if (!Root.ExpectedClass.empty() && !Expected) return Fail(FCookInputFailure{.Error = ECookInputError::RootClass, .Package = Root.TargetPath, .Name = Root.ExpectedClass});
					if (auto Result = ValidateResolvedAssetForOperation(Registry, Registry.ResolveAssetPath(Root.TargetPath), Expected); !Result)
						return Fail(Result);
					if (Pending.size() >= MaximumCookDependencyRecords)
						return Fail(FCookInputFailure{.Error = ECookInputError::RootLimit, .Package = Root.TargetPath, .Actual = Pending.size() + 1, .Maximum = MaximumCookDependencyRecords});
					Pending.push_back(Root.TargetPath);
				}
		std::unordered_set<FPackagePath> Runtime;
		const auto IsRuntimeReference = [&](const FAssetReferenceEdge& Reference) {
			if (Request.bRetainEditorOnlyData) return true;
			const auto* Class = FindClassByQualifiedName(FName(Reference.SourceClass));
			const auto* Property = Class ? Class->FindPropertyByName(FName(Reference.FieldName)) : nullptr;
			// Custom archive fields without a reflected policy remain runtime dependencies.
			return !Property || !Property->HasAnyPropertyFlags(EPropertyFlags::EditorOnly);
		};
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
				return Fail(FCookInputFailure{.Error = ECookInputError::RuntimeEdgeLimit, .Package = Path, .Actual = RuntimeEdges, .Maximum = MaximumCookDependencyRecords});
			std::unordered_map<FPackagePath, bool> ReferencedPackages;
			for (const auto& Reference : Input.References)
				ReferencedPackages[Reference.TargetPath.GetPackagePath()] |= IsRuntimeReference(Reference);
			for (const auto& Dependency : Input.Inspection.Header.Dependencies)
			{
				const auto Reference = ReferencedPackages.find(Dependency);
				if (Reference == ReferencedPackages.end() || Reference->second) Pending.push_back(Dependency);
			}
			for (const auto& Reference : Input.References)
			{
				if (Reference.Kind == EAssetReferenceKind::Redirect) continue;
				const auto Resolution = Registry.ResolveAssetObjectPath(Reference.TargetPath);
				const DClass* Expected = Reference.ExpectedClass.empty() ? nullptr : FindClassByQualifiedName(FName(Reference.ExpectedClass));
				if (!Reference.ExpectedClass.empty() && !Expected) return Fail(FCookInputFailure{.Error = ECookInputError::ReferenceClass, .Package = Path, .Name = Reference.ExpectedClass, .Member = Reference.FieldName});
				if (auto Result = ValidateResolvedAssetForOperation(Registry, Resolution, Expected); !Result)
					{
					if (Result.Error == EAssetReadError::NotFound) Result.Error = EAssetReadError::MissingDependency;
					return Fail(Result);
				}
				for (const auto& Alias : Resolution.RedirectChain)
					if (auto Result = AcquirePackage(Alias.GetPackagePath()); !Result) return Result;
				if (auto Result = AcquirePackage(Resolution.FinalPath.GetPackagePath()); !Result) return Result;
				if (IsRuntimeReference(Reference)) Pending.push_back(Resolution.FinalPath.GetPackagePath());
			}
		}
		RuntimePackages.assign(Runtime.begin(), Runtime.end());
		std::ranges::sort(RuntimePackages, [](const auto& A, const auto& B) { return A.GetView() < B.GetView(); });
		if (RuntimePackages.empty()) return Fail(FCookInputFailure{.Error = ECookInputError::NoRuntimePackages});

		const auto CustomVersionDefinitions = FCustomVersionRegistry::GetAll();
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
				// Include local format definitions before reuse is considered, including cooked-only formats.
				for (const auto& Version : CustomVersionDefinitions)
				{
					FBinaryWriter Value;
					Value.WriteU32(static_cast<uint32>(Version.CurrentVersion));
					Node.Inputs.push_back({ECookBuildDependencyKind::SchemaProducerVersion,
						"custom-version/" + Version.Guid.ToString(), Value.TakeBytes()});
				}
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
				if (!ContributorResult && Runtime.contains(Path)) return Fail(ContributorResult);
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
						if (Declarations.size() > MaximumCookDependencyRecords) return Fail(FCookInputFailure{.Error = ECookInputError::DeclarationCount, .Package = Path, .Actual = Declarations.size(), .Maximum = MaximumCookDependencyRecords});
						std::set<std::pair<ECookBuildDependencyKind, std::string>> Unique;
						for (const auto& Declaration : Declarations)
						{
							auto Reject = [&](ECookInputError Error) -> FCookInputFailure {
								return {.Error = Error, .Package = Path, .Kind = Declaration.Kind, .Name = Declaration.LogicalName,
									.File = Declaration.FilePath, .Actual = Declaration.Value.size(), .Maximum = MaximumCookDependencyValueBytes,
									.Retained = RetainedBytes, .MaximumRetained = MaximumDiscoveryBytes};
							};
							if (Declaration.LogicalName.empty() || Declaration.LogicalName.size() > 4096
								|| Declaration.LogicalName.find('\0') != std::string::npos)
							{
								auto Cause = Reject(ECookInputError::DeclarationName);
								Cause.Actual = Declaration.LogicalName.size();
								Cause.Maximum = 4096;
								return Fail(std::move(Cause));
							}
							if (!Unique.emplace(Declaration.Kind, Declaration.LogicalName).second)
								return Fail(Reject(ECookInputError::DuplicateDeclaration));
							if (Declaration.Kind == ECookBuildDependencyKind::DirectPackage
								|| Declaration.Kind == ECookBuildDependencyKind::TransitivePackage)
							{
								FPackagePath Dependency, Final;
								if (!Declaration.FilePath.empty() || !Declaration.Value.empty())
									return Fail(Reject(ECookInputError::PackageDeclaration));
								if (const auto Validated = FPackagePath::TryCreateWithDiagnostic(Declaration.LogicalName, Dependency); !Validated)
								{
									auto Cause = Reject(ECookInputError::PackageDeclaration);
									Cause.PathCause = std::make_shared<FObjectPathError>(Validated.error());
									return Fail(std::move(Cause));
								}
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
								if (!Declaration.Value.empty() || Declaration.FilePath.empty()) return Fail(Reject(ECookInputError::ExternalDeclaration));
								if (auto Result = ReadFile(Declaration.FilePath, Value); !Result) return Result;
								Node.Inputs.push_back({Declaration.Kind, Declaration.LogicalName, DigestValue(Value)});
								Input.DeclaredFiles.emplace(Declaration.LogicalName, Declaration.FilePath);
								continue;
							}
							else if (Declaration.Kind == ECookBuildDependencyKind::ConfigurationValue
								|| Declaration.Kind == ECookBuildDependencyKind::SchemaProducerVersion)
							{
								if (!Declaration.FilePath.empty() || Declaration.Value.size() > MaximumCookDependencyValueBytes)
									return Fail(Reject(ECookInputError::ValueDeclaration));
								const uint64 RetainedValueBytes = Declaration.Value.size() * 2;
								if (!TryRetainCookBytes(RetainedValueBytes, RetainedBytes, MaximumDiscoveryBytes))
								{
									auto Cause = Reject(ECookInputError::ValueStorage);
									Cause.Actual = RetainedValueBytes;
									return Fail(std::move(Cause));
								}
								Value = Declaration.Value;
								Node.Inputs.push_back({Declaration.Kind, Declaration.LogicalName, Value});
							}
							else return Fail(Reject(ECookInputError::ReservedKind));
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
		if (const auto Initialized = Dependencies.Initialize(Graph); !Initialized)
			return Fail(Initialized.ToInputResult());
		for (const auto& Path : RuntimePackages)
		{
			if (auto Result = CheckCancellation(); !Result) return Result;
			if (const auto Expanded = Dependencies.Expand(Path, Inputs.at(Path).Dependencies); !Expanded)
				return Fail(Expanded.ToInputResult());
			for (const auto& Record : Inputs.at(Path).Dependencies)
			{
				const uint64 Size = Record.LogicalName.size() + Record.Value.size() + 9;
				if (!TryRetainCookBytes(Size, RetainedBytes, MaximumDiscoveryBytes)) return Fail(FCookInputFailure{.Error = ECookInputError::DependencyStorage, .Package = Path, .Kind = Record.Kind, .Name = Record.LogicalName, .Actual = Size, .Retained = RetainedBytes, .MaximumRetained = MaximumDiscoveryBytes});
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
		ECookBuildDependencyKind Kind, std::string_view Name, FByteBuffer& Out) -> FCookInputResult
	{
		Out.clear();
		const auto Found = Inputs.find(Path);
		if (Found == Inputs.end()) return Fail(FCookInputFailure{.Error = ECookInputError::UnknownPackage, .Package = Path, .Kind = Kind, .Name = std::string(Name)});
		if (Kind == ECookBuildDependencyKind::ExternalFile)
		{
			const auto File = Found->second.DeclaredFiles.find(std::string(Name));
			if (File != Found->second.DeclaredFiles.end()) return ReadFile(File->second, Out);
		}
		const auto Value = Found->second.DeclaredValues.find({Kind, std::string(Name)});
		if (Value == Found->second.DeclaredValues.end()) return Fail(FCookInputFailure{.Error = ECookInputError::UndeclaredInput, .Package = Path, .Kind = Kind, .Name = std::string(Name)});
		Out = Value->second;
		return {};
	}
}
