# Spatial for Windows

Extract the ZIP and open `Spatial.exe`. Keep `phonon.dll` in the same folder. The x64 build uses Windows Media Foundation and WASAPI and requires Windows 10 version 1703 or later. Windows N editions need the Media Feature Pack.

- **Add music** opens a file and creates a source. **Queue file** appends tracks to the queue. The first file source advances through the queue.
- Click a source to select it. Drag its marker or adjust Direction and Distance. Right-click a marker for source actions. **Recenter** sets its direction and distance to their defaults.
- **Add point** records the selected source's position. Place the source elsewhere and add another point. **Follow path** moves between points, spaced five seconds apart.
- **Save space** writes the file sources and positions to a `.spatial` file. **Open space** restores them with playback paused.
- **Device audio** adds one input from a different Windows playback endpoint. Use Windows Volume mixer to route the source app to that endpoint. Spatial sends the mix to the default audio output selected when you connect. A virtual audio cable can serve as the input endpoint. Protected content can block loopback. [Microsoft's WASAPI loopback reference](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)
- **Stop** closes the audio streams. The tray icon also offers Stop. Desktop playback needs no phone sensor.

Build from the workspace with `./scripts/build-windows.ps1`. The portable ZIP includes the Steam Audio licensing notices.
