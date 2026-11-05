// Added by puppetxr's sample tests (tests/samples/unity/build.cmake, LOADER=oculus): makes Unity's
// Oculus XR plugin the Windows loader in place of OpenXR. Meta's Depth API has no provider for a
// Windows player on the OpenXR loader; on this one OVRPlugin drives the runtime itself.
using Unity.XR.Oculus;
using UnityEditor;
using UnityEditor.XR.Management;
using UnityEditor.XR.Management.Metadata;

public static class PuppetxrOculusLoader
{
    public static void Run()
    {
        var manager = XRGeneralSettingsPerBuildTarget.XRGeneralSettingsForBuildTarget(BuildTargetGroup.Standalone).AssignedSettings;
        XRPackageMetadataStore.RemoveLoader(manager, "UnityEngine.XR.OpenXR.OpenXRLoader", BuildTargetGroup.Standalone);
        XRPackageMetadataStore.AssignLoader(manager, "Unity.XR.Oculus.OculusLoader", BuildTargetGroup.Standalone);
        EditorUtility.SetDirty(manager);
        // The plugin reads its settings at run time only when they are the build's config object
        const string path = "Assets/XR/Settings/OculusSettings.asset";
        var settings = AssetDatabase.LoadAssetAtPath<OculusSettings>(path);
        if (settings == null)
        {
            settings = UnityEngine.ScriptableObject.CreateInstance<OculusSettings>();
            AssetDatabase.CreateAsset(settings, path);
        }
        EditorBuildSettings.AddConfigObject("Unity.XR.Oculus.Settings", settings, true);
        AssetDatabase.SaveAssets();
    }
}
