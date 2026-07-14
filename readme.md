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
1. Add the `App2Clap` plug-in to the input FX chain of a track in your DAW.
2. If you want to capture mono audio, ensure that a mono input is selected for that track.
    Similarly, if you want to capture stereo audio, ensure a stereo input is selected.
    The specific input doesn't matter; the plug-in will replace the input with the captured audio.
3. Choose whether you want to capture a specific process only, capture everything except a specific process or capture everything.
    Note that capturing everything could cause feedback, as it may capture the output of your DAW itself.
4. Select a process from the list.
    You can filter the list to show only certain processes by typing part of the executable name or process id into the Filter text box.
    Processes are sorted so that processes that were created earlier appear first, which means that parent processes appear before their child processes.
    This is useful when dealing with applications that create multiple processes.
5. Press Capture to start capturing.
    Capture stays pressed while you are capturing.
    Press it again to stop.
    The settings are disabled while you are capturing, as they can't be changed for a capture which is already running.
6. To prevent the captured audio from being echoed by your DAW, you can disable input monitoring in your DAW.
7. If you want to capture a different process, press Capture to stop, change the settings in the plug-in, then press Capture again to start the new capture.
8. If you want to automatically capture a process when the plug-in is reloaded, enter the appropriate text into the Filter text box and enable the Capture first matching process when reloaded check box.
    When the plug-in is reloaded, such as when opening a saved project, the first process matching the filter will be automatically captured.
    This is useful for saving and quickly applying commonly used configurations.
9. To capture multiple, separate processes, use separate instances of the plug-in on separate tracks.

### Sending Audio to a Windows Audio Device
1. Add the `Clap2App` plug-in to a track in your DAW.
2. Select an output device from the list.
3. Press Send to start sending audio from the DAW track to the output device.
    Send stays pressed while you are sending.
    Press it again to stop.
    The device list is disabled while you are sending, as the output device can't be changed for a send which is already running.
4. If you want to change the output device, press Send to stop, select the new device, then press Send again to start sending to it.
5. To output to multiple devices, use separate instances of the plug-in.

### Capturing Audio from a Windows Audio Device
1. Add the `In2Clap` plug-in to the input FX chain of a track in your DAW.
2. If you want to capture mono audio, ensure that a mono input is selected for that track.
    Similarly, if you want to capture stereo audio, ensure a stereo input is selected.
    The specific input doesn't matter; the plug-in will replace the input with the captured audio.
3. Select an input device from the list.
4. Press Capture to start capturing.
    Capture stays pressed while you are capturing.
    Press it again to stop.
    The device list is disabled while you are capturing, as the input device can't be changed for a capture which is already running.
5. To prevent the captured audio from being echoed by your DAW, you can disable input monitoring in your DAW.
6. If you want to change the input device, press Capture to stop, select the new device, then press Capture again to start the new capture.
7. To capture multiple, separate devices, use separate instances of the plug-in on separate tracks.

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
