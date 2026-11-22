# Spatial for Windows

Run `Spatial-1.1.4-Windows-x64-Setup.exe` and open **Spatial** from the Start menu. If updating a tray version, choose **Quit Spatial** from the tray before running Setup. Setup installs for the current user and includes `phonon.dll`. For the portable ZIP, right-click it, choose **Extract All**, and open `Spatial.exe` in the extracted folder. Keep `phonon.dll` next to it if you move the app.

The x64 build uses Windows Media Foundation and WASAPI and requires Windows 10 version 1703 or later. Windows N editions need the Media Feature Pack.

- **Add music** opens a file and creates a source. **Queue file** appends tracks to the queue. The first file source advances through the queue.
- Click a source to select it. Drag its marker or adjust Direction and Distance. Right-click a marker for source actions. **Recenter** sets its direction and distance to their defaults.
- **Add point** records the selected source's position. Place the source elsewhere and add another point. **Follow path** starts from the first point. Points are spaced five seconds apart. External sources return along their route before repeating. **Hold path** keeps the current position.
- **Save space** records file sources, the external endpoint, and movement paths in a `.spatial` file. **Open space** restores them with playback paused. Older spaces still open.
- **Device audio → This PC audio** moves the stereo ambience when you drag its marker or enable a path. Centered and mono audio plays through the original source only. Spatial checks for independent audio before rendering and excludes the FxSound return stream when detected. Process loopback requires a supported Windows build, generally Windows 11. Protected playback can be unavailable. [Microsoft's application loopback documentation](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/)
- Choose **Route from** for a separate endpoint. Send the source app to it with Windows Volume mixer. Spatial sends the result to your default audio output. A virtual cable can keep the original stream out of your headphones when you want the processed audio alone.
- Closing the window leaves Spatial in the tray. **Stop Spatial** closes the audio streams and **Quit Spatial** exits the app. Idle PC audio has no active playback stream and wakes when audio arrives.

Build from the workspace with `./scripts/build-windows.ps1`. The script produces the portable ZIP and uses NSIS, when installed, to produce Setup. Both packages include the Steam Audio licensing notices.
