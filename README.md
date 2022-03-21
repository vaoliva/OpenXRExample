# OpenXR single file UWP project
This repository contains a Single File OpenXR Example Project that demonstrates the core concepts of the OpenXR API and how to use the Visual Studio toolchain to build and deploy an OpenXR app to the Hololens 2 device.

The example uses C++17 and D3D11. 

# Prepare, build and run the project
Update to Windows 10 May 2019 Update (1903) or later. If you will be deploying to a HoloLens 2, you should install `Visual Studio 2019 16.2` or later.

Prepare a HoloLens 2 device or a Hololens 2 Emulator.

Clone the example repo: git clone https://github.com/vaoliva/OpenXRExample

Open the `OpenXRExample.sln` file in Visual Studio. F5 to build and run the sample. You typically choose `ARM64` platform when running on HoloLens 2 devices, or choose x64 platform when running on a Windows Desktop PC with the HoloLens 2 Emulator.

# OpenXR sample code
The core OpenXR API usage patterns can be found in the `Main.cpp` file. The `wWinMain` function captures a typical OpenXR app code flow for session initialization, event handling, the frame loop and input actions.
