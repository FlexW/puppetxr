# Issues found running Meta's Unity Movement samples on a non-Meta OpenXR runtime

Found while running `oculus-samples/Unity-Movement` on puppetxr, an OpenXR simulator runtime
(https://github.com/FlexW/puppetxr), as automated tests.

## 1. Interaction SDK: a macOS player takes up to a minute to quit

Once a scene that uses the Interaction SDK has run, quitting a macOS Unity player blocks in a
static destructor of `InteractionSdk.dylib` for up to about 60 s. `isdk::telemetry::TelemetrySender`'s
destructor `join`s its sender thread, and that thread is in `std::this_thread::sleep_for` for its
send interval. Nothing wakes it, so `exit()` waits out the rest of the interval.

## Versions

- `com.meta.xr.sdk.interaction` 207.0.0, `com.meta.xr.sdk.interaction.ovr` 207.0.0
- Meta XR Core SDK 207, `com.meta.xr.sdk.movement` 207.0.0 (Unity-Movement v207.0.0)
- Unity 6000.0.66f2, Unity OpenXR plugin, macOS standalone player, arm64
- macOS 27.0 on Apple silicon

## Steps to reproduce

1. Build Unity-Movement's sample scene menu (`Samples~` of `com.meta.xr.sdk.movement`, which uses
   `RayInteractor` and `UIGazeAndPinch`) into a macOS standalone player.
2. Run the player and let the menu scene load.
3. Quit it. Observed with the OpenXR runtime requesting exit (session goes to EXITING, the app
   quits); since the hang is in static destruction at `exit()`, any quit path should hit it.

Expected: the process ends within a second or two.
Actual: the player log ends with several `Thread 0x… may have been prematurely finalized`
lines, and the process stays alive for up to about 60 s before it exits with code 0.
Scenes that don't use the Interaction SDK quit right away.

## Stacks while it hangs (`sample <pid>`)

Main thread:

```
exit  (libsystem_c.dylib)
  __cxa_finalize_ranges  (libsystem_c.dylib)
    std::unique_ptr<isdk::telemetry::TelemetrySender>::~unique_ptr()  (InteractionSdk.dylib)
      isdk::telemetry::TelemetrySender::~TelemetrySender()  (InteractionSdk.dylib)
        std::thread::join()  (libc++.1.dylib)
          _pthread_join -> __ulock_wait
```

Telemetry thread:

```
std::__thread_proxy<..., void (*)(isdk::telemetry::TelemetrySender&, float), ...>
  isdk::telemetry::(anonymous namespace)::sendTelemetryAtInterval(TelemetrySender&, float)  (InteractionSdk.dylib)
    std::this_thread::sleep_for(...)  (libc++.1.dylib)
      nanosleep -> __semwait_signal
```

## Suggested fix

Wait on a condition variable with a timeout (`wait_for`) instead of `sleep_for` in
`sendTelemetryAtInterval`, and have the destructor (or an explicit shutdown from
`Application.quitting`) set a stop flag and notify it before `join()`. Alternatively, shut the
sender down explicitly before static destruction, as `ISDKTelemetryInitializer.OnApplicationQuitting`
already does for `ISDKEngineTelemetryNative`.

## Impact

Every quit of an affected macOS player takes up to a minute. Automated tests that start and
quit the player wait that long each run, and the app looks hung to users after they close it.
