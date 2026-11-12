#include "ProjectDescriptor.h"
#include "Misc/Paths.h"
#include "ProjectContextInternal.h"

namespace Durin
{
    auto BuildProjectMountDefinitions(std::string_view File, const std::vector<FMountPoint>& Additional) -> std::vector<FMountPoint>
    {
        std::vector<FMountPoint> Result{{.VirtualRoot="/Engine/", .Owner=EMountOwner::Engine,
            .Root=FPaths::EngineDir(), .ContentPath="Content", .bAutoScan=true, .bContentWritable=true}};
        const auto ProjectRoot = std::filesystem::path(File).parent_path();
        const bool bEngine = std::filesystem::weakly_canonical(ProjectRoot) == std::filesystem::weakly_canonical(FPaths::EngineDir());
        if (!bEngine)
        {
            FMountPoint Game{.VirtualRoot="/Game/", .Owner=EMountOwner::ActiveProject, .Root=ProjectRoot,
                .ContentPath="Content", .bAutoScan=true, .bContentWritable=true, .Dependencies={"/Engine/"}};
            for (const auto& Mount : Additional) Game.Dependencies.push_back(Mount.VirtualRoot);
            Result.push_back(std::move(Game));
        }
        Result.insert(Result.end(), Additional.begin(), Additional.end());
        return Result;
    }
    namespace
    {
        auto ValidateRelativeDefinitionPath(std::string_view Text) -> bool
        {
            const std::filesystem::path Path(Text);
            return !Text.empty() && !Path.is_absolute() && !Path.has_root_name()
                && std::ranges::none_of(Path, [](const auto& Segment) { return Segment == ".." || Segment.empty(); });
        }
    }
    auto ReadProjectMounts(FJsonNodeView Root, std::string_view File,
        std::vector<FMountPoint>& Definitions, std::string* OutError) -> bool
    {
			const FJsonNodeView Mounts = Root.GetView("Mounts");
			if (!Mounts.IsValid()) return true;
			if (!Mounts.IsArray())
			{
				if (OutError) *OutError = "Project descriptor Mounts must be an array.";
				return false;
			}

			const std::filesystem::path ProjectRoot = std::filesystem::path(File).parent_path();
			for (size_t Index = 0; Index < Mounts.Num(); ++Index)
			{
				const FJsonNodeView Entry = Mounts.GetView(Index);
				if (!Entry.IsObject())
				{
					if (OutError) *OutError = std::format("Mounts[{}] must be an object.", Index);
					return false;
				}
				const bool bHasContentWritable = Entry.GetView("ContentWritable").IsValid();
				const bool bHasLegacyAuthoringWritable = Entry.GetView("AuthoringWritable").IsValid();
				if (bHasContentWritable && bHasLegacyAuthoringWritable)
				{
					if (OutError) *OutError = std::format(
						"Mounts[{}] cannot contain both ContentWritable and legacy AuthoringWritable.",
						Index);
					return false;
				}
				if (Entry.Num() != 7)
				{
					if (OutError) *OutError = std::format("Mounts[{}] must contain exactly seven fields.", Index);
					return false;
				}
				constexpr std::array<std::string_view, 8> Fields{
					"VirtualRoot", "Owner", "Root", "ContentPath", "AutoScan",
					"ContentWritable", "AuthoringWritable", "Dependencies"};
				bool bUnknownField = false;
				Entry.ForEachObjectMember([&](const std::string_view Key, FJsonNodeView) {
					if (!std::ranges::contains(Fields, Key)) bUnknownField = true;
				});
				if (bUnknownField)
				{
					if (OutError) *OutError = std::format("Mounts[{}] contains an unknown field.", Index);
					return false;
				}

				std::string VirtualRoot;
				std::string OwnerText;
				std::string RootText;
				std::string ContentPathText;
				bool bAutoScan = false;
				bool bContentWritable = false;
				const FJsonNodeView Dependencies = Entry.GetView("Dependencies");
				const std::string_view ContentWritableKey = bHasContentWritable
					? "ContentWritable" : "AuthoringWritable";
				if (!Entry.GetChildValue("VirtualRoot", VirtualRoot)
					|| !Entry.GetChildValue("Owner", OwnerText)
					|| !Entry.GetChildValue("Root", RootText)
					|| !Entry.GetChildValue("ContentPath", ContentPathText)
					|| !Entry.GetChildValue("AutoScan", bAutoScan)
					|| !Entry.GetChildValue(ContentWritableKey, bContentWritable)
					|| !Dependencies.IsArray()
					|| !ValidateRelativeDefinitionPath(RootText)
					|| !ValidateRelativeDefinitionPath(ContentPathText))
				{
					if (OutError) *OutError = std::format("Mounts[{}] has invalid field types or paths.", Index);
					return false;
				}
				EMountOwner Owner;
				if (OwnerText == "Extension") Owner = EMountOwner::Extension;
				else if (OwnerText == "ExternalSources") Owner = EMountOwner::ExternalSources;
				else
				{
					if (OutError) *OutError = std::format("Mounts[{}].Owner is invalid.", Index);
					return false;
				}
				FMountPoint Definition{
					.VirtualRoot = std::move(VirtualRoot),
					.Owner = Owner,
					.Root = (ProjectRoot / RootText).lexically_normal(),
					.ContentPath = std::filesystem::path(ContentPathText).lexically_normal(),
					.bAutoScan = bAutoScan,
					.bContentWritable = bContentWritable};
				for (size_t DependencyIndex = 0; DependencyIndex < Dependencies.Num(); ++DependencyIndex)
				{
					const FJsonNodeView Dependency = Dependencies.GetView(DependencyIndex);
					if (!Dependency.IsString())
					{
						if (OutError) *OutError = std::format("Mounts[{}].Dependencies must contain strings.", Index);
						return false;
					}
					Definition.Dependencies.push_back(Dependency.GetString());
				}
				Definitions.push_back(std::move(Definition));
			}
			return true;
    }
}
