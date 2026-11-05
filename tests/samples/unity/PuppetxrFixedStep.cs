// Added by puppetxr's sample tests (cmake/samples.cmake): with -puppetxr-fixed-step on the command line
// game time advances one display period per frame and Unity's random numbers start from a fixed
// seed, so animations follow the frames puppetxr steps rather than the wall clock and every run
// shows the same frames. Particle systems seed themselves anew each time they play, so each gets
// a fixed seed. UI colour fades (Selectable's transitions) run in unscaled time, which follows
// the wall clock even then, so they are made instant. -puppetxr-scene <name> loads that
// scene of the build instead of the first, for samples whose menus a test would otherwise have to
// click through.
using System;
using UnityEngine;
using UnityEngine.SceneManagement;
using UnityEngine.UI;

public static class PuppetxrFixedStep
{
    [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
    private static void Apply()
    {
        var args = Environment.GetCommandLineArgs();
        if (Array.IndexOf(args, "-puppetxr-fixed-step") >= 0)
        {
            Time.captureDeltaTime = 1.0f / 90.0f;
            UnityEngine.Random.InitState(1);
            var fades = new GameObject("PuppetxrInstantFades");
            fades.hideFlags = HideFlags.HideAndDontSave;
            UnityEngine.Object.DontDestroyOnLoad(fades);
            fades.AddComponent<PuppetxrInstantFades>();
            SceneManager.sceneLoaded += (loaded, mode) => SeedParticles();
        }
        var scene = Array.IndexOf(args, "-puppetxr-scene");
        if (scene >= 0 && scene + 1 < args.Length)
        {
            SceneManager.LoadScene(args[scene + 1]);
        }
    }

    private static void SeedParticles()
    {
        var systems = UnityEngine.Object.FindObjectsByType<ParticleSystem>(FindObjectsInactive.Include, FindObjectsSortMode.None);
        foreach (var system in systems)
        {
            if (!system.useAutoRandomSeed)
            {
                continue;
            }
            var playing = system.isPlaying;
            system.Stop(false, ParticleSystemStopBehavior.StopEmittingAndClear);
            system.useAutoRandomSeed = false;
            system.randomSeed       = 1;
            if (playing)
            {
                system.Play(false);
            }
        }
    }
}

// Before the event system moves the pointer, so a Selectable never starts a fade
[DefaultExecutionOrder(-10000)]
public class PuppetxrInstantFades : MonoBehaviour
{
    private Selectable[] selectables = new Selectable[64];

    private void Update()
    {
        if (selectables.Length < Selectable.allSelectableCount)
        {
            selectables = new Selectable[Selectable.allSelectableCount * 2];
        }
        var count = Selectable.AllSelectablesNoAlloc(selectables);
        for (var i = 0; i < count; i++)
        {
            var colors = selectables[i].colors;
            if (colors.fadeDuration != 0.0f)
            {
                colors.fadeDuration = 0.0f;
                selectables[i].colors = colors;
            }
        }
    }
}
