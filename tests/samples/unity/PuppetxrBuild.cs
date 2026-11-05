// Added by puppetxr's sample tests (tests/samples/unity/build.cmake): builds the Windows player, or
// on macOS the Apple silicon one, with Mono, whatever backend the project picked (IL2CPP needs a
// module the editor may lack), and the project's own scene list; -puppetxrScenes <folder> appends
// the scenes under that folder. A macOS player asking for the microphone without a usage
// description is killed by the system.
using System;
using System.Linq;
using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

public static class PuppetxrBuild
{
    public static void Build()
    {
        var args = Environment.GetCommandLineArgs();
        var output = args[Array.IndexOf(args, "-puppetxrOutput") + 1];
        PlayerSettings.SetScriptingBackend(NamedBuildTarget.Standalone, ScriptingImplementation.Mono2x);
        var scenes = EditorBuildSettings.scenes.Where(scene => scene.enabled).Select(scene => scene.path).ToList();
        var extra  = Array.IndexOf(args, "-puppetxrScenes");
        if (extra >= 0 && extra + 1 < args.Length)
        {
            var found = AssetDatabase.FindAssets("t:Scene", new[] { args[extra + 1] }).Select(AssetDatabase.GUIDToAssetPath).OrderBy(path => path, StringComparer.Ordinal);
            scenes.AddRange(found.Where(path => !scenes.Contains(path)));
        }
        var macos = Application.platform == RuntimePlatform.OSXEditor;
        if (macos)
        {
            // The type is in the macOS build support only, which a Windows editor may lack
#if UNITY_EDITOR_OSX
            UnityEditor.OSXStandalone.UserBuildSettings.architecture = UnityEditor.Build.OSArchitecture.ARM64;
#endif
            PlayerSettings.macOS.microphoneUsageDescription = "Voice commands of the sample";
        }
        var options = new BuildPlayerOptions
        {
            scenes           = scenes.ToArray(),
            locationPathName = output,
            target           = macos ? BuildTarget.StandaloneOSX : BuildTarget.StandaloneWindows64,
        };
        var report = BuildPipeline.BuildPlayer(options);
        // Unity's OpenXR plugin loads "openxr_loader" plus the platform's extension from the
        // player's plugins, but its macOS loader is named libopenxr_loader.dylib
        if (macos && report.summary.result == BuildResult.Succeeded)
        {
            var plugins = System.IO.Path.Combine(output, "Contents", "PlugIns");
            var loader  = System.IO.Path.Combine(plugins, "libopenxr_loader.dylib");
            if (System.IO.File.Exists(loader))
            {
                System.IO.File.Copy(loader, System.IO.Path.Combine(plugins, "openxr_loader.dylib"), true);
            }
        }
        EditorApplication.Exit(report.summary.result == BuildResult.Succeeded ? 0 : 1);
    }
}
