#include "Misc/PackageWriter.h"
#include "CookDependencyDiscovery.h"
#include "CookMemoryBudget.h"
#include "DObject/DObjectArray.h"
#include "DObject/Package.h"
#include "Shader/IShaderBuildModule.h"
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
		case ECookInputError::ShaderInput: return Failure.ShaderDiagnostic;
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
		case ECookInputError::ReservedKind: return std::format("Object declared reserved Cook dependency kind {}: {}", static_cast<uint32>(Failure.Kind), Failure.Name);
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
		FAssetRegistrySnapshot InRegistry, FAssetPackageLoadScope& InLoadScope, const FCookState* PreviousState)
		: Request(InRequest), Registry(std::move(InRegistry)), LoadScope(InLoadScope),
		Schema(FReflectionSchemaCatalog::Capture())
	{
		if (PreviousState) for (const auto& Input : PreviousState->Inputs)
			PreviousProjections.emplace(Input.VirtualPackagePath, &Input);
	}

	auto FCookDependencyDiscovery::Fail(FCookInputFailure Cause) -> FCookInputResult
	{
		if (!Failure) return Failure;
		Failure = Cause.ToInputResult();

		return Failure;
	}

	auto FCookDependencyDiscovery::MakeContext(const FPackagePath& Path) -> FObjectCookContext
	{
		FObjectCookContext Context;
		Context.Target = {std::string(GetTargetPlatformName(Request.TargetPlatform)), std::string(GetTargetProfileName(Request.TargetProfile))};
		Context.Package = Path.ToString();
		Context.bCooking = true;
		Context.bRetainEditorOnlyData = Request.bRetainEditorOnlyData;
		// The coordinator has already resolved and sorted the configuration set.
		FBinaryWriter Writer;
		Writer.WriteU32(static_cast<uint32>(Request.MaterialConfigurations.size()));
		for (const auto& Configuration : Request.MaterialConfigurations)
		{
			Writer.WriteU32(static_cast<uint32>(Configuration.Quality));
			Writer.WriteU32(static_cast<uint32>(Configuration.FeatureLevel));
		}
		Context.RequestValues.emplace("material/configurations", Writer.TakeBytes());
		Context.IsCancelled = Request.IsCancelled;
		Context.RequestReader = [this](std::string_view Name) -> std::expected<FByteBuffer, FObjectCookResult> {
			if (Name != "shader-build") return std::unexpected(FObjectCookResult{EObjectCookError::InvalidInput, "Unknown Cook request value: " + std::string(Name)});
			if (ShaderBuildIdentity.empty())
			{
				const auto Read = GetShaderCookInputIdentity(ShaderBuildIdentity, Request.IsCancelled);
				if (!Read) return std::unexpected(FObjectCookResult{
					Read.Status == EShaderBuildStatus::Canceled ? EObjectCookError::Cancelled : EObjectCookError::InvalidInput, std::string(Read.Error)});
				if (Request.TargetPlatform == ECookTargetPlatform::MacOS)
				{
					auto* Module = IShaderBuildModule::Get();
					const auto Producer = Module ? Module->GetTargetProducerIdentity(MetalLibraryShaderTarget)
						: std::expected<std::string, std::string>{std::unexpected("ShaderBuild is unavailable.")};
					if (!Producer) { ShaderBuildIdentity.clear(); return std::unexpected(FObjectCookResult{EObjectCookError::InvalidInput, Producer.error()}); }
					ShaderBuildIdentity += ";metal-library-v1=" + *Producer;
				}
			}
			const auto Bytes = std::as_bytes(std::span(ShaderBuildIdentity));
			return FByteBuffer(Bytes.begin(), Bytes.end());
		};
		return Context;
	}

	auto FCookDependencyDiscovery::CaptureProjection(const FPackagePath& Path, FInput& Input) -> FCookInputResult
	{
		FCookPackageBuildInputs SchemaNode{.Package = Path};
		for (const auto& Object : Input.Inspection.Objects) Input.SchemaClasses.insert(Object.ClassName);
		std::vector<std::string> Classes(Input.SchemaClasses.begin(), Input.SchemaClasses.end());
		bool PreciseVersions = true;
		std::set<FGuid> UsedVersions;
		for (const auto& Version : Input.Inspection.CustomVersions) UsedVersions.insert(Version.Guid);
		for (size_t ClassIndex = 0; ClassIndex < Classes.size(); ++ClassIndex)
		{
			const auto Name = Classes[ClassIndex];
			if (!NativeSchemas.contains(Name))
			{
				auto* Class = FindClassByQualifiedName(FName(Name));
				if (!Class) return Fail(FCookInputFailure{.Error = ECookInputError::SchemaClass, .Package = Path, .Name = Name});
				auto Context = MakeContext(Path);
				Context.Package.clear(); // Class schema values cannot depend on the first package encountered.
				Class->AppendCookSchema(Context);
				if (!Context.Result) return Fail(FCookInputResult{
					Context.Result.Error == EObjectCookError::Cancelled ? ECookInputStatus::Cancelled
						: Context.Result.Error == EObjectCookError::Limit ? ECookInputStatus::LimitExceeded : ECookInputStatus::InvalidDependency,
					Context.Result.Diagnostic});
				if (Context.Dependencies.size() > MaximumCookDependencyRecords)
					return Fail(FCookInputFailure{.Error = ECookInputError::DeclarationCount, .Package = Path,
						.Actual = Context.Dependencies.size(), .Maximum = MaximumCookDependencyRecords});
				for (const auto& Declaration : Context.Dependencies)
					if (Declaration.Kind != ECookBuildDependencyKind::ConfigurationValue
						&& Declaration.Kind != ECookBuildDependencyKind::SchemaProducerVersion)
						return Fail(FCookInputResult{ECookInputStatus::InvalidDependency,
							"Class Cook schemas accept only shared values; declare package/file dependencies in OnCookEvent: " + Name});
				if (Context.UsedCustomVersions.size() > MaximumCookDependencyRecords
					|| Context.ConstructedClasses.size() > MaximumCookDependencyRecords
					|| Context.VersionCoveredClasses.size() > MaximumCookDependencyRecords)
					return Fail(FCookInputResult{ECookInputStatus::LimitExceeded, "Cook class metadata exceeds its record bound."});
				uint64 SchemaBytes = Context.UsedCustomVersions.size() * sizeof(FGuid);
				for (const auto* Names : {&Context.ConstructedClasses, &Context.VersionCoveredClasses})
					for (const auto& Entry : *Names) {
						if (Entry.empty() || Entry.size() > 4096 || Entry.find('\0') != std::string::npos)
							return Fail(FCookInputResult{ECookInputStatus::InvalidDependency, "Invalid Cook class metadata name."});
						SchemaBytes += Entry.size();
					}
				for (const auto& Declaration : Context.Dependencies)
					SchemaBytes += Declaration.LogicalName.size() + Declaration.Value.size()
						+ Declaration.FilePath.native().size() * sizeof(std::filesystem::path::value_type);
				if (!TryRetainCookBytes(SchemaBytes, RetainedBytes, MaximumDiscoveryBytes))
					return Fail(FCookInputFailure{.Error = ECookInputError::DependencyStorage, .Package = Path, .Name = Name,
						.Actual = SchemaBytes, .Maximum = MaximumDiscoveryBytes, .Retained = RetainedBytes});
				Context.RequestValues.clear(); Context.RequestReader = {}; Context.IsCancelled = {};
				NativeSchemas.emplace(Name, std::move(Context));
			}
			const auto& Native = NativeSchemas.at(Name);
			PreciseVersions &= Native.VersionCoveredClasses.contains(Name);
			Input.bDependenciesComplete &= Native.bDependenciesComplete;
			UsedVersions.insert(Native.UsedCustomVersions.begin(), Native.UsedCustomVersions.end());
			for (const auto& Constructed : Native.ConstructedClasses)
				if (Input.SchemaClasses.insert(Constructed).second) Classes.push_back(Constructed);
			if (Classes.size() > MaximumCookDependencyRecords)
				return Fail(FCookInputFailure{.Error = ECookInputError::DeclarationCount, .Package = Path, .Actual = Classes.size(), .Maximum = MaximumCookDependencyRecords});
			const auto& Declarations = Native.Dependencies;
			Input.DeclarationGroups.push_back(Declarations);
			std::vector<FCookBuildDependency> Identity;
			for (const auto& Declaration : Declarations)
			{
				FByteBuffer Value = Declaration.Value;
				if (!Declaration.FilePath.empty())
				{
					const auto Locator = Declaration.FilePath.generic_u8string();
					const auto Bytes = std::as_bytes(std::span(Locator));
					Value.assign(Bytes.begin(), Bytes.end());
				}
				Identity.push_back({Declaration.Kind, Declaration.LogicalName, std::move(Value)});
			}
			FByteBuffer Encoded;
			if (auto Result = EncodeCookBuildDependencies(Identity, Encoded); !Result)
				return Fail(FCookInputFailure{.Error = Result.Error == ECookDependencyCodecError::Duplicate ? ECookInputError::DuplicateDeclaration : ECookInputError::ValueDeclaration,
					.Package = Path, .Kind = Result.Kind, .Name = Result.LogicalName, .Actual = Result.Actual, .Maximum = Result.Maximum});
			SchemaNode.Inputs.push_back({ECookBuildDependencyKind::SchemaProducerVersion, "native-schema/" + Name, DigestValue(Encoded)});
		}
		// A class hook may register a lazily constructed class after this run's
		// initial catalog snapshot. Refresh only when its immutable schema is absent.
		if (std::ranges::any_of(Input.SchemaClasses, [this](const auto& Name) { return !Schema.FindClass(Name); }))
			Schema = FReflectionSchemaCatalog::Capture();
		if (auto Result = CaptureSchema(Input, SchemaNode); !Result) return Result;
		for (const auto& Version : FCustomVersionRegistry::GetAll())
		{
			if (PreciseVersions && !UsedVersions.contains(Version.Guid)) continue;
			FBinaryWriter Value;
			Value.WriteU32(static_cast<uint32>(Version.CurrentVersion));
			Input.VersionInputs.push_back({ECookBuildDependencyKind::SchemaProducerVersion,
				"custom-version/" + Version.Guid.ToString(), Value.TakeBytes()});
			UsedVersions.erase(Version.Guid);
		}
		if (!UsedVersions.empty()) return Fail(FCookInputResult{ECookInputStatus::InvalidDependency, "Unknown declared Cook custom version: " + UsedVersions.begin()->ToString()});
		SchemaNode.Inputs.insert(SchemaNode.Inputs.end(), Input.VersionInputs.begin(), Input.VersionInputs.end());
		FBinaryWriter Settings;
		Settings.WriteU32(static_cast<uint32>(Request.TargetPlatform));
		Settings.WriteU32(static_cast<uint32>(Request.TargetProfile));
		Settings.WriteU8(Request.bRetainEditorOnlyData);
		if (!PreciseVersions) for (const auto& Configuration : Request.MaterialConfigurations)
		{
			Settings.WriteU32(static_cast<uint32>(Configuration.Quality));
			Settings.WriteU32(static_cast<uint32>(Configuration.FeatureLevel));
		}
		SchemaNode.Inputs.push_back({ECookBuildDependencyKind::ConfigurationValue, "projection/request", Settings.TakeBytes()});
		if (auto Result = FingerprintCookBuildDependencies(SchemaNode.Inputs, Input.Projection.SchemaFingerprint); !Result)
			return Fail(FCookInputResult{ECookInputStatus::InvalidDependency, FormatCookDependencyCodecError(Result)});
		const auto Previous = PreviousProjections.find(Path.ToString());
		bool bReuseProjection = Previous != PreviousProjections.end() && Previous->second->bCanReuseWithoutLoading && Input.bDependenciesComplete
			&& Previous->second->SourceFingerprint == Input.Projection.SourceFingerprint
			&& Previous->second->BulkFingerprint == Input.Projection.BulkFingerprint
			&& Previous->second->SchemaFingerprint == Input.Projection.SchemaFingerprint;
		if (bReuseProjection)
		{
			const auto& Records = Previous->second->Records;
			bReuseProjection = std::ranges::count(Records, ECookBuildDependencyKind::SourcePackage, &FCookBuildDependency::Kind) == Input.Inspection.Objects.size();
			for (const auto& Object : Input.Inspection.Objects)
				bReuseProjection &= std::ranges::any_of(Records, [&](const auto& Record) {
					return Record.Kind == ECookBuildDependencyKind::SourcePackage && Record.LogicalName == Object.ObjectPath && Record.Value.size() == 1;
				});
		}
		std::vector<FCookDependencyDeclaration> InstanceDeclarations;
		if (bReuseProjection)
		{
			Input.Projection = *Previous->second;
			for (const auto& Record : Input.Projection.Records)
			{
				if (Record.Kind == ECookBuildDependencyKind::SourcePackage)
				{
					if (Record.Value.front() == std::byte{0}) Input.ExcludedObjects.insert(Record.LogicalName);
					continue;
				}
				FCookDependencyDeclaration Declaration{Record.Kind, Record.LogicalName};
				if (Record.Kind == ECookBuildDependencyKind::ExternalFile)
				{
					const std::string Locator(reinterpret_cast<const char*>(Record.Value.data()), Record.Value.size());
					if (Locator.empty() || Locator.find('\0') != std::string::npos) return Fail(FCookInputFailure{.Error = ECookInputError::ExternalDeclaration, .Package = Path, .Name = Record.LogicalName});
					Declaration.FilePath = std::filesystem::path(std::u8string(Locator.begin(), Locator.end()));
				}
				else Declaration.Value = Record.Value;
				InstanceDeclarations.push_back(std::move(Declaration));
			}
			Input.DeclarationGroups.push_back(std::move(InstanceDeclarations));
		}
		else
		{
			const auto Loaded = LoadScope.LoadPackage(Path);
			if (!Loaded) return Fail(ToCookInputResult(AssetReadResultFromError(Loaded.error())));
			std::map<std::string, DObject*> Objects;
			std::vector<std::pair<std::string, DObject*>> Pending;
			for (auto* Asset : (*Loaded)->GetTopLevelAssets()) Pending.emplace_back(Asset->GetName(), Asset);
			for (size_t Index = 0; Index < Pending.size(); ++Index)
			{
				if (Pending.size() > MaximumCookDependencyRecords) return Fail(FCookInputFailure{.Error = ECookInputError::PackageLimit, .Package = Path, .Actual = Pending.size(), .Maximum = MaximumCookDependencyRecords});
				const auto [RelativePath, Object] = Pending[Index];
				Objects.emplace(RelativePath, Object);
				for (auto* Child : GDObjectArray.GetObjectsWithOuter(Object, EObjectQueryScope::LiveOnly)) Pending.emplace_back(RelativePath + "/" + Child->GetName(), Child);
			}
			for (const auto& Stored : Input.Inspection.Objects)
			{
				if (auto Result = CheckCancellation(); !Result) return Result;
				const auto Found = Objects.find(Stored.ObjectPath);
				if (Found == Objects.end()) return Fail(FCookInputFailure{.Error = ECookInputError::SchemaClass, .Package = Path, .Name = Stored.ObjectPath});
				auto Context = MakeContext(Path);
				auto* Object = Found->second;
				const bool Included = IsObjectIncludedForCook(Object, Context.Target);
				if (!Included) Input.ExcludedObjects.insert(Stored.ObjectPath);
				Input.Projection.Records.push_back({ECookBuildDependencyKind::SourcePackage, Stored.ObjectPath, {Included ? std::byte{1} : std::byte{0}}});
				Object->OnCookEvent(ECookEvent::PlatformCookDependencies, Context);
				Input.bDependenciesComplete &= Context.bDependenciesComplete;
				Input.Projection.bCanReuseWithoutLoading &= Context.bDependenciesComplete;
				if (!Context.Result) return Fail(FCookInputResult{Context.Result.Error == EObjectCookError::Cancelled ? ECookInputStatus::Cancelled : ECookInputStatus::InvalidDependency, Context.Result.Diagnostic});
				if (Context.Dependencies.size() > MaximumCookDependencyRecords) return Fail(FCookInputFailure{.Error = ECookInputError::DeclarationCount, .Package = Path, .Actual = Context.Dependencies.size(), .Maximum = MaximumCookDependencyRecords});
				for (const auto& Declaration : Context.Dependencies)
				{
					if (Declaration.Kind == ECookBuildDependencyKind::SourcePackage || Declaration.Kind == ECookBuildDependencyKind::OwnedBulk)
						return Fail(FCookInputFailure{.Error = ECookInputError::ReservedKind, .Package = Path, .Name = Declaration.LogicalName});
					FCookBuildDependency Record{Declaration.Kind, Declaration.LogicalName, Declaration.Value};
					if (Declaration.Kind == ECookBuildDependencyKind::ExternalFile)
					{
						const auto Locator = Declaration.FilePath.generic_u8string();
						const auto Bytes = std::as_bytes(std::span(Locator));
						Record.Value.assign(Bytes.begin(), Bytes.end());
					}
					else if (Declaration.Kind == ECookBuildDependencyKind::ConfigurationValue || Declaration.Kind == ECookBuildDependencyKind::SchemaProducerVersion)
						Input.Projection.bCanReuseWithoutLoading = false; // No reevaluation function for an arbitrary instance-computed value.
					Input.Projection.Records.push_back(std::move(Record));
				}
				Input.DeclarationGroups.push_back(std::move(Context.Dependencies));
			}
		}
		Input.Projection.bCanReuseWithoutLoading &= Input.bDependenciesComplete;
		uint64 Bytes = 0;
		if (Input.Projection.Records.size() > MaximumCookDependencyRecords)
			return Fail(FCookInputFailure{.Error = ECookInputError::DeclarationCount, .Package = Path, .Actual = Input.Projection.Records.size(), .Maximum = MaximumCookDependencyRecords});
		for (const auto& Group : Input.DeclarationGroups) for (const auto& Declaration : Group)
			Bytes += Declaration.LogicalName.size() + Declaration.Value.size() + Declaration.FilePath.native().size() * sizeof(std::filesystem::path::value_type);
		for (const auto& Record : Input.Projection.Records) Bytes += Record.LogicalName.size() + Record.Value.size();
		if (!TryRetainCookBytes(Bytes, RetainedBytes, MaximumDiscoveryBytes)) return Fail(FCookInputFailure{.Error = ECookInputError::DependencyStorage, .Package = Path, .Actual = Bytes, .Maximum = MaximumDiscoveryBytes, .Retained = RetainedBytes});
		return {};
	}

	auto FCookDependencyDiscovery::TakeProjections() -> std::vector<FCookInputProjection>
	{
		std::vector<FCookInputProjection> Result;
		for (auto& [Path, Input] : Inputs)
		{
			auto Projection = std::move(Input.Projection);
			std::ranges::sort(Projection.Records, [](const auto& A, const auto& B) { return std::pair(A.Kind, A.LogicalName) < std::pair(B.Kind, B.LogicalName); });
			Projection.Records.erase(std::unique(Projection.Records.begin(), Projection.Records.end(), [](const auto& A, const auto& B) {
				return A.Kind == B.Kind && A.LogicalName == B.LogicalName;
			}), Projection.Records.end());
			Result.push_back(std::move(Projection));
		}
		std::ranges::sort(Result, {}, &FCookInputProjection::VirtualPackagePath);
		return Result;
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
		Input.Projection.VirtualPackagePath = Path.ToString();
		Input.Projection.SourceFingerprint = FXxHash128::HashBuffer(PackageBytes);
		Input.Projection.BulkFingerprint = FXxHash128::HashBuffer(BulkBytes);
		Input.PackageIdentity = DigestValue(PackageBytes);
		Input.BulkIdentity = DigestValue(BulkBytes);
		const FAssetPackageCodec* Codec = nullptr;
		if (auto Result = ResolveAssetPackageReader(PackageBytes, Codec); !Result) return Fail(Result);
		FAssetPackageReadContext Context{.PackageBytes = PackageBytes, .PackagePath = Path,
			.PhysicalBulkBytes = BulkBytes.size(), .bResourceBackedBulk = true};
		if (auto Result = Codec->Inspect(Context, Input.Inspection); !Result) return Fail(Result);
		if (auto Result = ExtractAssetReferences(Path, Input.Inspection, Input.References, true); !Result) return Fail(Result);
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
		return CaptureProjection(Path, Inputs.at(Path));
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
		for (const auto& Name : Input.SchemaClasses)
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
				auto Owner = Field->GetOwnerVariant();
				while (auto* Parent = Owner.ToField()) Owner = Parent->GetOwnerVariant();
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
		const auto IsEditorReference = [&](const FAssetReferenceEdge& Reference) {
			const auto* Class = FindClassByQualifiedName(FName(Reference.SourceClass));
			const auto* Property = Class ? Class->FindPropertyByName(FName(Reference.FieldName)) : nullptr;
			return Reference.bEditorOnly || (Property && Property->HasAnyPropertyFlags(EPropertyFlags::EditorOnly));
		};
		const auto IsExcluded = [&](const FInput& Input, uint64 Id) {
			const auto It = std::ranges::find(Input.Inspection.Objects, Id, &FAssetPackageObjectInspection::Id);
			return It != Input.Inspection.Objects.end() && Input.ExcludedObjects.contains(It->ObjectPath);
		};
		std::unordered_set<FPackagePath> Explicit;
		for (const auto& Root : Pending) {
			FPackagePath Final;
			if (auto Result = Resolve(Root, Final); !Result) return Result;
			Explicit.insert(Final);
		}
		uint64 RuntimeEdges = 0;
		while (!Pending.empty())
		{
			const auto Requested = Pending.back(); Pending.pop_back();
			FPackagePath Path;
			if (auto Result = Resolve(Requested, Path); !Result) return Result;
			if (Runtime.contains(Path)) continue;
			const auto& Input = Inputs.at(Path);
			const bool HasRuntime = std::ranges::any_of(Input.Inspection.Objects, [&](const auto& Object) {
				return Object.OuterId == 0 && !IsExcluded(Input, Object.Id);
			});
			if (!Failure) return Failure;
			if (!HasRuntime) {
				OmittedPackages.push_back(Path);
				continue;
			}
			Runtime.insert(Path);
			RuntimeEdges += Input.Inspection.Header.Dependencies.size() + Input.References.size();
			if (RuntimeEdges > MaximumCookDependencyRecords)
				return Fail(FCookInputFailure{.Error = ECookInputError::RuntimeEdgeLimit, .Package = Path, .Actual = RuntimeEdges, .Maximum = MaximumCookDependencyRecords});
			std::unordered_set<FPackagePath> ReferencedPackages;
			for (const auto& Reference : Input.References)
				ReferencedPackages.insert(Reference.TargetPath.GetPackagePath());
			for (const auto& Dependency : Input.Inspection.Header.Dependencies)
				if (!ReferencedPackages.contains(Dependency)) Pending.push_back(Dependency);
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
				const auto& Target = Inputs.at(Resolution.FinalPath.GetPackagePath());
				std::string RelativeTarget(Resolution.FinalPath.GetAssetPath().GetAssetName());
				for (const auto Name : Resolution.FinalPath.GetSubobjectNames()) { RelativeTarget += '/'; RelativeTarget += Name; }
				const auto TargetObject = std::ranges::find(Target.Inspection.Objects, RelativeTarget, &FAssetPackageObjectInspection::ObjectPath);
				const bool ExcludedTarget = TargetObject != Target.Inspection.Objects.end() && IsExcluded(Target, TargetObject->Id);
				const bool ExcludedSource = IsExcluded(Input, Reference.SourceObjectId);
				if (!Failure) return Failure;
				if (!ExcludedTarget && !ExcludedSource && (Request.bRetainEditorOnlyData || !IsEditorReference(Reference)))
					Pending.push_back(Resolution.FinalPath.GetPackagePath());
			}
		}
		RuntimePackages.assign(Runtime.begin(), Runtime.end());
		std::ranges::sort(RuntimePackages, [](const auto& A, const auto& B) { return A.GetView() < B.GetView(); });
		std::ranges::sort(OmittedPackages);
		OmittedPackages.erase(std::unique(OmittedPackages.begin(), OmittedPackages.end()), OmittedPackages.end());

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
				Node.Inputs = Input.VersionInputs;
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

				std::map<std::pair<ECookBuildDependencyKind, std::string>, FCookDependencyDeclaration> PackageDeclarations;
				{
					for (const auto& Declarations : Input.DeclarationGroups)
					{
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
							const auto Key = std::pair(Declaration.Kind, Declaration.LogicalName);
							const auto [Existing, Fresh] = PackageDeclarations.emplace(Key, Declaration);
							if (!Fresh)
							{
								if (Existing->second.FilePath != Declaration.FilePath || Existing->second.Value != Declaration.Value)
									return Fail(Reject(ECookInputError::DuplicateDeclaration));
								continue;
							}
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
					}
				}

				for (const auto& Dependency : Input.Inspection.Header.BuildDependencies)
				{
					// Authored metadata is a scheduling aid. A Cook declaration owns
					// direct/transitive semantics; metadata alone observes source/bulk.
					if (PackageDeclarations.contains({ECookBuildDependencyKind::DirectPackage, Dependency.ToString()})
						|| PackageDeclarations.contains({ECookBuildDependencyKind::TransitivePackage, Dependency.ToString()})) continue;
					FPackagePath Final;
					if (auto Result = Resolve(Dependency, Final); !Result) return Result;
					Node.Packages.push_back({Dependency, false});
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
				for (const auto& Edge : Node.Packages) if (Edge.bTransitive) Input.TransitiveBuildPackages.push_back(Edge.Package);
				Graph.push_back(std::move(Node));
			}
		}
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

	auto FCookDependencyDiscovery::CanReuseOutput(const FPackagePath& Path) const -> bool
	{
		std::unordered_set<FPackagePath> Seen;
		std::vector<FPackagePath> Pending{Path};
		while (!Pending.empty()) {
			auto Current = Pending.back(); Pending.pop_back();
			if (!Seen.insert(Current).second) continue;
			const auto& Input = Inputs.at(Current);
			if (!Input.bDependenciesComplete) return false;
			// A direct edge observes stable source/bulk, not the target's native inputs.
			Pending.insert(Pending.end(), Input.TransitiveBuildPackages.begin(), Input.TransitiveBuildPackages.end());
		}
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
