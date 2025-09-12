# App2Clap

<!--beginDownload-->
[Downloads available on the App2Clap website](https://app2clap.jantrid.net/)
<!--endDownload-->

- Author: James Teh &lt;jamie@jantrid.net&gt;
- Copyright: 2025 James Teh
- License: GNU General Public License version 2.0

App2Clap is a CLAP plug-in that captures audio from a specific Windows application.
It can be used, for example, to record audio from another application into a DAW.
Windows 11 is required.


## Download and Installation
For now, there is no installer.
You can download the latest build using the link at the top of this page.
Once downloaded, simply copy the `app2clap.clap` file out of the zip file you downloaded to the `%localappdata%\Programs\Common\CLAP` folder using Windows File Explorer.
The `CLAP` directory might not exist there yet, in which case you will need to create it under `%localappdata%\Programs\Common`.

## Usage with a DAW
1. Add the `App2Clap` plug-in to a track in your DAW.
2. Choose whether you want to capture a specific process only, capture everything except a specific process or capture everything.
    Note that capturing everything could cause feedback, as it may capture the output of your DAW itself.
3. Select a process from the list.
4. Press Apply to start capturing.
5. If you want to capture a different process, change the settings and press Apply again.
6. To capture multiple, separate processes, use separate instances of the plug-in on separate tracks.
7. If you want to record the captured audio, you will need to configure your DAW to record the output of the track, rather than the input.

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
