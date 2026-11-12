#include "PluginStartup.h"
#include "Json/Json.h"
#include "Misc/Paths.h"
#include "Misc/Build.h"
#include "Misc/Version.h"
#include "Misc/FileHelper.h"
#include "ProjectDescriptor.h"
#include "OrdinaryModules.gen.h"
#if defined(_WIN32)
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#endif

namespace Durin
{
	namespace
	{
		auto HostCoreHash() -> std::string
		{
			std::filesystem::path CorePath;
#if defined(_WIN32)
			HMODULE Handle = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&GetEngineVersionString), &Handle)) throw std::runtime_error("Cannot locate loaded Core");
			std::vector<wchar_t> Buffer(32768);
			const auto Length = GetModuleFileNameW(Handle, Buffer.data(), static_cast<DWORD>(Buffer.size()));
			if (!Length || Length >= Buffer.size()) throw std::runtime_error("Cannot locate loaded Core path");
			CorePath = std::wstring(Buffer.data(), Length);
#elif defined(__APPLE__)
			Dl_info Info{};
			if (!dladdr(reinterpret_cast<void*>(&GetEngineVersionString), &Info)) throw std::runtime_error("Cannot locate loaded Core");
			CorePath = Info.dli_fname;
#endif
			const auto Bytes = FFileHelper::LoadFileToArray(CorePath);
			if (!Bytes) throw std::runtime_error("Cannot hash loaded Core file");
			std::array<unsigned char, 32> Digest{};
#if defined(_WIN32)
			// The SHA-256 pseudo-handle has no owned provider/hash resources.
			if (Bytes->size() > std::numeric_limits<ULONG>::max()
				|| BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
					reinterpret_cast<PUCHAR>(const_cast<std::byte*>(Bytes->data())),
					static_cast<ULONG>(Bytes->size()), Digest.data(), static_cast<ULONG>(Digest.size())) < 0)
				throw std::runtime_error("Cannot hash loaded Core file");
#elif defined(__APPLE__)
			if (Bytes->size() > std::numeric_limits<CC_LONG>::max()) throw std::runtime_error("Core file exceeds hash limit");
			CC_SHA256(Bytes->data(), static_cast<CC_LONG>(Bytes->size()), Digest.data());
#endif
			std::string Result;
			for (auto Byte : Digest) Result += std::format("{:02x}", Byte);
			return Result;
		}
	}
	auto ResolveProcessPlugins(std::string_view ProjectFile, FPluginResolution& OutSelection,
		std::vector<FAdmittedPluginBinary>& OutBinaries, std::string& OutReason) -> bool
	{
		try
		{
			FPluginResolveRequest Request;
			Request.EngineRoot = FPaths::EngineDir();
			Request.bProjectBrowser = ProjectFile.empty();
			Request.ProjectFile = ProjectFile.empty() ? Request.EngineRoot / "Engine.dproject" : std::filesystem::path(ProjectFile);
			Request.Platform = DURIN_BUILD_PLATFORM_STRING;
			Request.RuntimeVariant = DURIN_RUNTIME_VARIANT;
			const auto WorkspaceRoot = std::filesystem::path(FPaths::RootDir());
			// Browser is independent of plugin metadata. Existing no-plugin staged
			// Games also retain startup without source workspace descriptors.
			if (ProjectFile.empty() || !std::filesystem::is_regular_file(WorkspaceRoot / "Durin.dworkspace"))
			{
				if (!ProjectFile.empty())
				{
					FProjectDescriptor Descriptor;
					if (!Descriptor.Load(ProjectFile, OutReason)) return false;
					for (const auto& [Name, Enabled] : Descriptor.PluginOverrides)
						if (Enabled) throw std::runtime_error("Source plugin admission requires workspace descriptors; staged plugin deployment is not available");
				}
				FPluginResolution Empty;
				for (const auto* Name : GOrdinaryModuleNames) Empty.OrdinaryModules.emplace_back(Name);
				OutSelection = std::move(Empty);
				OutBinaries.clear();
				OutReason.clear();
				return true;
			}
			FJsonDocument Workspace;
			const auto WorkspaceFile = WorkspaceRoot / "Durin.dworkspace";
			if (!Workspace.LoadFromFile(WorkspaceFile)) throw std::runtime_error("Cannot read workspace: " + WorkspaceFile.generic_string());
			const auto Projects = Workspace.GetRootView().GetView("Projects");
			if (!Projects.IsArray()) throw std::runtime_error("Invalid workspace Projects");
			for (size_t I = 0; I < Projects.Num(); ++I)
			{
				if (!Projects.GetView(I).IsString()) throw std::runtime_error("Invalid workspace project path");
				Request.ProjectFiles.push_back(WorkspaceRoot / Projects.GetView(I).GetString());
			}
			auto Selected = ResolvePlugins(Request);
			if (!Selected) { OutReason = Selected.error(); return false; }
			std::vector<FAdmittedPluginBinary> Binaries;
			if (!ProjectFile.empty() && !Selected->Plugins.empty())
			{
				FJsonDocument Project;
				if (!Project.LoadFromFile(Request.ProjectFile)) throw std::runtime_error("Cannot read project");
				const auto Name = Project.GetRootView().GetView("ProjectName").GetString();
				FPluginBuildIdentity Identity{std::string(GetEngineVersionString()) + ":" + HostCoreHash(),
					Request.Platform, DURIN_PLUGIN_ARCHITECTURE, DURIN_BUILD_TYPE_STRING,
					Request.RuntimeVariant, DURIN_PLUGIN_TOOLCHAIN};
				const auto Inventory = std::filesystem::path(DURIN_PLUGIN_BUILD_ROOT) / Name / DURIN_BUILD_TYPE_STRING / "PluginBuild.json";
				if (!AdmitPluginBuild(*Selected, Identity, Inventory, Binaries, OutReason)) return false;
			}
			// Browser never starts project plugins, including engine-project overrides.
			if (ProjectFile.empty()) { Selected->Plugins.clear(); Selected->Modules.clear(); }
			OutSelection = std::move(*Selected);
			OutBinaries = std::move(Binaries);
			OutReason.clear();
			return true;
		}
		catch (const std::exception& Error)
		{
			OutReason = std::string(Error.what()) + "; configure/build required before restart";
			return false;
		}
	}
}
