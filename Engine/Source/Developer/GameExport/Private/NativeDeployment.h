#pragma once
#include "CoreMinimal.h"
#include "Json/Json.h"
#include "Plugins/PluginResolution.h"

namespace Durin
{
    struct FNativeReceiptRoots
    {
        std::filesystem::path Engine, Project, Artifacts, Dependencies;
        bool Prebuilt = false;
        std::filesystem::path ProjectProducts;
    };
    struct FNativeReceiptModule
    {
        std::string Name, Owner, Binary, Phase;
        std::vector<std::string> Linked, Dynamic, RuntimeFiles;
        size_t Record = 0;
    };
    struct FNativeReceiptProduct
    {
        std::string Module, Destination, Kind, Sha256;
        std::filesystem::path Source;
        size_t Record = 0;
    };
    // Owns the single parsed availability document; decoded values do not borrow views.
    struct FNativeReceipt
    {
        FJsonDocument Document;
        std::string Bytes, EngineBuild;
        std::map<std::string,FNativeReceiptModule> Modules;
        std::vector<FNativeReceiptProduct> Products;
    };
    struct FNativeDeploymentPlugin
    {
        FResolvedPluginModule Module;
        std::string Binary;
        std::vector<std::string> Dependencies;
    };
    struct FNativeDeployment
    {
        FJsonDocument SelectedReceipt;
        std::vector<FNativeReceiptProduct> Products;
        std::vector<FNativeDeploymentPlugin> Plugins;
        std::string EngineBuild;
    };
    auto ReadNativeReceipt(const std::filesystem::path& File,const FNativeReceiptRoots& Roots,
        FJsonNodeView Snapshot,FJsonNodeView Selection) -> FNativeReceipt;
    // Pure graph selection/projection: no filesystem access or publication.
    auto SelectNativeDeployment(const FNativeReceipt& Receipt,FJsonNodeView Snapshot,
        const FPluginResolution& Selection) -> FNativeDeployment;
    auto ValidateNativeDeployment(const FNativeDeployment& Deployment,FJsonNodeView Snapshot) -> void;
}
