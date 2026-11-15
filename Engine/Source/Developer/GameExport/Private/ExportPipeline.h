#pragma once
#include "CoreMinimal.h"
namespace Durin {
    struct FExportTarget
    {
        std::filesystem::path Engine, Project, Cook, Output, Receipt, Artifacts, Dependencies, Policy, ProjectProducts;
        std::string ProjectName, Configuration, Platform, BackendJson, ProjectDescriptorJson;
        bool Prebuilt = false;
    };
    using FStageProgress = std::function<void(std::string_view, std::string_view, uint64, uint64)>;
    void StageGame(const FExportTarget& Target, const std::function<bool()>& Cancel,
        const FStageProgress& Progress, std::string& Category, std::string& Fingerprint);
}
