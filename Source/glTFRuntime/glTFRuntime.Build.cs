// Copyright 2020, Roberto De Ioris.

using UnrealBuildTool;

public class glTFRuntime : ModuleRules
{
    public glTFRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;

        PublicIncludePaths.AddRange(
            new string[] {
                "Niagara"
            }
            );


        PrivateIncludePaths.AddRange(
            new string[] {
            }
            );


        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "ProceduralMeshComponent",
                "Niagara"
            }
            );




        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "CoreUObject",
                "Engine",
                "Json",
                "RenderCore",
                "RHI",
                "ApplicationCore",
                "HTTP",
                "PhysicsCore",
                "Projects",
                "MeshDescription",
                "StaticMeshDescription",
                "AudioExtensions"
            }
            );


        if (Target.Type == TargetType.Editor)
        {
            PrivateDependencyModuleNames.Add("SkeletalMeshUtilitiesCommon");
            PrivateDependencyModuleNames.Add("UnrealEd");
            PrivateDependencyModuleNames.Add("AssetRegistry");
        }


        DynamicallyLoadedModuleNames.AddRange(
            new string[]
            {
            }
            );
    }
}
