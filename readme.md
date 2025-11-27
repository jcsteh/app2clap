# App2Clap

<!--beginDownload-->
[Downloads available on the App2Clap website](https://app2clap.jantrid.net/)
<!--endDownload-->

- Author: James Teh &lt;jamie@jantrid.net&gt;
- Copyright: 2025 James Teh
- License: GNU General Public License version 2.0

App2Clap provides three CLAP plug-ins:

1. App2Clap, which captures audio from a specific Windows application.
    It can be used, for example, to record audio from another application into a DAW.
2. Clap2App, which sends audio to a specific Windows audio device.
    It's not possible to send audio directly into another Windows application.
    However, if you combine this with a virtual audio cable driver, you can use Clap2App to send audio to the virtual output device and set an application to record from the corresponding virtual input device.
3. In2Clap, which captures audio from a specific Windows audio device.
    This can be used, for example, to capture input from a different audio device, since DAWs generally only support input from a single device.

This requires Windows 11, or Windows 10 22H2 or later.

## Download and Installation
For now, there is no installer.
You can download the latest build using the link at the top of this page.
Once downloaded, simply copy the `app2clap.clap` file out of the zip file you downloaded to the `%localappdata%\Programs\Common\CLAP` folder using Windows File Explorer.
The `CLAP` directory might not exist there yet, in which case you will need to create it under `%localappdata%\Programs\Common`.

## Usage with a DAW

### Capturing Audio from an Application
1. Add the `App2Clap` plug-in to a track in your DAW.
2. Choose whether you want to capture a specific process only, capture everything except a specific process or capture everything.
    Note that capturing everything could cause feedback, as it may capture the output of your DAW itself.
3. Select a process from the list.
    You can filter the list to show only certain processes by typing part of the executable name or process id into the Filter text box.
4. Press Capture to start capturing.
5. If you want to capture a different process, change the settings and press Capture again.
6. To capture multiple, separate processes, use separate instances of the plug-in on separate tracks.
7. If you want to record the captured audio, you will need to configure your DAW to record the output of the track, rather than the input.
    In REAPER, you do this by opening the track's recording context menu, choosing the Record: output sub-menu and then Record: output (stereo).
    You will most likely want to record the output pre-fader so that the track mute and volume don't affect the recorded audio.
    In REAPER, you do this by opening the Record: output menu and choosing Output mode: Pre-Fader (Post-FX).
8. In REAPER, you can decrease the delay (latency) before the live captured audio is played by your DAW by arming the track, regardless of whether you want to record the captured audio.
    If you want to arm without actually recording, you can set the track to "Record: disable (input monitoring only)".
    This may also be true in other DAWs, but this hasn't been confirmed.
9. Disabling input monitoring in your DAW will not prevent captured audio from being heard.
    This is because input monitoring only affects input via the DAW, not output from a plug-in.
    To prevent captured audio from being heard, you can mute the track or disable the track's send to the master output.
    If you want to hear audio previously recorded on that track instead of the live audio from the captured application, you can temporarily bypass the plug-in.

### Sending Audio to a Windows Audio Device
1. Add the `Clap2App` plug-in to a track in your DAW.
2. Select an output device from the list.
3. Press Send to start sending audio from the DAW track to the output device.
4. If you want to change the output device, change the settings and press Send again.
5. To output to multiple devices, use separate instances of the plug-in.

### Capturing Audio from a Windows Audio Device
1. Add the `In2Clap` plug-in to a track in your DAW.
2. Select an input device from the list.
3. Press Capture to start capturing.
4. If you want to change the input device, change the settings and press Capture again.
5. To capture multiple, separate devices, use separate instances of the plug-in on separate tracks.
6. If you want to record the captured audio, you will need to configure your DAW to record the output of the track, rather than the input.
    In REAPER, you do this by opening the track's recording context menu, choosing the Record: output sub-menu and then Record: output (stereo).
    You will most likely want to record the output pre-fader so that the track mute and volume don't affect the recorded audio.
    In REAPER, you do this by opening the Record: output menu and choosing Output mode: Pre-Fader (Post-FX).
7. In REAPER, you can decrease the delay (latency) before the live captured audio is played by your DAW by arming the track, regardless of whether you want to record the captured audio.
    If you want to arm without actually recording, you can set the track to "Record: disable (input monitoring only)".
    This may also be true in other DAWs, but this hasn't been confirmed.
8. Disabling input monitoring in your DAW will not prevent captured audio from being heard.
    This is because input monitoring only affects input via the DAW, not output from a plug-in.
    To prevent captured audio from being heard, you can mute the track or disable the track's send to the master output.
    If you want to hear audio previously recorded on that track instead of the live audio from the captured device, you can temporarily bypass the plug-in.

## Reporting Issues
Issues should be reported [on GitHub](https://github.com/jcsteh/app2clap/issues).

## Building
This section is for those interested in building App2Clap from source code.

### Getting the Source Code
The App2Clap Git repository is located at https://github.com/jcsteh/app2clap.git.
You can clone it with the following command, which will place files in a directory named app2clap:

```
git clone --recursive https://github.com/jcsteh/app2clap.git
```

The `--recursive` option is needed to retrieve Git submodules we use.
If you didn't pass this option to `git clone`, you will need to run `git submodule update --init`.
Whenever a required submodule commit changes (e.g. after git pull), you will need to run `git submodule update`.
If you aren't sure, run `git submodule update` after every git pull, merge or checkout.

### Dependencies
To build App2Clap, you will need:

- Several git submodules used by App2Clap.
	See the note about submodules in the previous section.
- [Build Tools for Visual Studio 2022](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)

	Visual Studio 2022 Community/Professional/Enterprise is also supported.
	However, Preview versions of Visual Studio will not be detected and cannot be used.

	Whether installing Build Tools or Visual Studio, you must enable the following:

	* In the list on the Workloads tab, in the Desktop & Mobile grouping: Desktop development with C++
	* Then in the Installation details tree view, under Desktop development with C++ > Optional:
		- C++ Clang tools for Windows
		- Windows 11 SDK (10.0.22000.0)

- [Python](https://www.python.org/downloads/), version 3.7 or later:
- [SCons](https://www.scons.org/), version 3.0.4 or later:
	* Once Python is installed, you should be able to install SCons by running this at the command line:

	`py -3 -m pip install scons`

### How to Build
To build App2Clap, from a command prompt, simply change to the App2Clap checkout directory and run `scons`.
The resulting plug-in can be found in the `build` directory.
