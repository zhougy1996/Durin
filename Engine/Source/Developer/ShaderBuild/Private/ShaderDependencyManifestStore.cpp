#include "ShaderDependencyManifestStore.h"

#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Misc/StringConvert.h"
#include "ShaderBuild/ShaderPaths.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 GShaderManifestVersion = 6;
		constexpr size_t GMaximumShaderDependencies = 4096;
		constexpr size_t GMaximumDependencyPathLength = 32768;

		auto IsValidKey(std::string_view Key) -> bool
		{
			return StringUtils::IsHex(Key, 32);
		}
	}

	auto FShaderDependencyManifestStore::Load(
		std::string_view VirtualShaderPath,
		const FShaderDependencyKey& DependencyKey,
		FShaderMetaData& OutMetaData) -> bool
	{
		if (!IsValidKey(DependencyKey.Hex)) return false;
		const std::string Path = FShaderPaths::MetaPath(
			VirtualShaderPath, DependencyKey.Hex);
		if (!FFileHelper::FileExists(Path)) return false;

		FJsonDocument Document;
		if (!Document.LoadFromFile(Path)) return false;
		const FJsonNodeView Root = Document.GetRootView();
		if (!Root.IsObject()
			|| Root.GetView("Version").GetUInt() != GShaderManifestVersion)
			return false;

		FShaderMetaData Candidate;
		const std::string Signature =
			Root.GetView("SourceTreeSignature").GetString();
		if (!StringUtils::IsHex(Signature, 32)) return false;
		Candidate.SourceTreeSignature = FXxHash128::FromString(Signature);

		const FJsonNodeView Dependencies = Root.GetView("Dependencies");
		if (!Dependencies.IsArray()
			|| Dependencies.Num() > GMaximumShaderDependencies) return false;
		Candidate.Dependencies.reserve(Dependencies.Num());
		Candidate.PortableDependencies.reserve(Dependencies.Num());
		for (size_t Index = 0; Index < Dependencies.Num(); ++Index)
		{
			const FJsonNodeView Node = Dependencies.GetView(Index);
			const FJsonNodeView PathNode = Node.GetView("Path");
			const FJsonNodeView VirtualPathNode = Node.GetView("VirtualPath");
			const FJsonNodeView TimeNode = Node.GetView("LastWriteTime");
			const FJsonNodeView SizeNode = Node.GetView("FileSize");
			const FJsonNodeView HashNode = Node.GetView("ContentHash");
			if (!Node.IsObject() || !PathNode.IsString()
				|| !VirtualPathNode.IsString() || !TimeNode.IsInt()
				|| !SizeNode.IsUInt() || !HashNode.IsString()) return false;
			const std::string PhysicalPath = PathNode.GetString();
			const std::string PortablePath = VirtualPathNode.GetString();
			const std::string Hash = HashNode.GetString();
			if (PhysicalPath.empty() || PortablePath.empty()
				|| PhysicalPath.size() > GMaximumDependencyPathLength
				|| PortablePath.size() > GMaximumDependencyPathLength
				|| !PortablePath.starts_with('/')
				|| !StringUtils::IsHex(Hash, 16)) return false;

			FFileFingerprint Fingerprint;
			Fingerprint.NormalizedPath = PhysicalPath;
			using FRep = std::filesystem::file_time_type::duration::rep;
			Fingerprint.LastWriteTime = std::filesystem::file_time_type(
				std::filesystem::file_time_type::duration(
					static_cast<FRep>(TimeNode.GetInt())));
			Fingerprint.FileSize = SizeNode.GetUInt();
			Fingerprint.ContentHash = FXxHash64::FromString(Hash);
			Candidate.Dependencies.push_back(std::move(Fingerprint));
			Candidate.PortableDependencies.push_back({
				.VirtualPath = PortablePath,
				.ContentHash = FXxHash64::FromString(Hash)});
		}
		if (!std::ranges::is_sorted(Candidate.PortableDependencies, {},
			&FShaderPortableDependency::VirtualPath)) return false;
		OutMetaData = std::move(Candidate);
		return true;
	}

	auto FShaderDependencyManifestStore::Save(
		std::string_view VirtualShaderPath,
		const FShaderDependencyKey& DependencyKey,
		const FShaderMetaData& MetaData) -> bool
	{
		if (!IsValidKey(DependencyKey.Hex)
			|| MetaData.SourceTreeSignature.IsZero()
			|| MetaData.Dependencies.size() != MetaData.PortableDependencies.size()
			|| MetaData.Dependencies.size() > GMaximumShaderDependencies) return false;

		FJsonDocument Document;
		FJsonNodeRef Root = Document.GetMutableRoot();
		Root.EnsureObject();
		Root.SetChildValue("Version", GShaderManifestVersion);
		Root.SetChildValue("SourceTreeSignature",
			MetaData.SourceTreeSignature.ToString());
		FJsonNodeRef Dependencies = Root.AddArray("Dependencies");
		for (size_t Index = 0; Index < MetaData.Dependencies.size(); ++Index)
		{
			const FFileFingerprint& Fingerprint = MetaData.Dependencies[Index];
			const FShaderPortableDependency& Portable =
				MetaData.PortableDependencies[Index];
			if (Portable.ContentHash != Fingerprint.ContentHash) return false;
			FJsonNodeRef Node = Dependencies.AppendObject();
			Node.SetChildValue("Path", Fingerprint.NormalizedPath);
			Node.SetChildValue("VirtualPath", Portable.VirtualPath);
			Node.SetChildValue("LastWriteTime", static_cast<int64>(
				Fingerprint.LastWriteTime.time_since_epoch().count()));
			Node.SetChildValue("FileSize", Fingerprint.FileSize);
			Node.SetChildValue("ContentHash", Fingerprint.ContentHash.ToString());
		}
		const std::string Json = Document.ToString();
		return !Json.empty() && FFileHelper::SaveArrayToFileAtomically(
			std::as_bytes(std::span(Json)),
			FShaderPaths::MetaPath(VirtualShaderPath, DependencyKey.Hex));
	}
}
