1. What is Orosi?
-----------------
Orosi is a free operating system that aims to be compatible with the Windows NT family, built for stability and modern 64-bit (amd64) hardware. It targets Windows 11 (NT10) compatibility, with room for experimental features.

Orosi is based on ReactOS, the Open Source effort to develop an operating system that is compatible with applications and drivers written for the Microsoft Windows NT family of operating systems. It is a direct fork of ReactOS with extensive changes to kernel-space components and drivers, aimed at stability and modern hardware, and focused on the amd64 (x64) platform.

Our goal is a system that stays maintainable in the long run: stable, reliable and efficient, and able to support the features a modern operating system should have.

2. What Orosi Enables
---------------------

Orosi brings modern hardware support to the NT architecture. The current focus is:

    - amd64 (x64) platform support: kernel, HAL and FreeLoader
    - UEFI boot, GOP framebuffer, and early display support
    - PCIe, PCI, PnP, USB/xHCI, SD/eMMC, and storage/bus drivers
    - Windows 11 (NT10) compatibility and driver-model work

This means it can potentially work on modern hardware, but it's not recommended to use it as your main operating system, because it's still an experiment at this stage.


3. Differences Between Orosi and Upstream ReactOS
-------------------------------------------------

Most of the source tree is shared with ReactOS, because it's a direct fork of ReactOS.

However, there are some differences between the two:

    - Memory Management: a clean-room virtual memory manager and kernel pool implementation, accompanied by a new cache manager, replace the corresponding components inherited from ReactOS.
    - Application Compatibility: Orosi uses WoW64 to allow running 32-bit apps on 64-bit processors.
    - Graphics: a WDDM-style display driver model and desktop composition.


4. AI Usage in Orosi
--------------------

A lot of Orosi's code is generated and reviewed with the assistance of AI. However, the code that the AI writes is reviewed, run, and validated with end-to-end tests by a real person before it reaches the main branch. We care about this project too much to let a regression slip through.
It also helps us read documentation, extract definitions and fix bugs. For any given bug, it compares what the official Microsoft Learn documentation says with what our function actually does, so we can see exactly where our behavior differs.
