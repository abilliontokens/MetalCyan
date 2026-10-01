# MetalCyan

Metal acceleration on macOS for the ASRock BC-250 (AMD Cyan Skillfish, GC 10.1.3). A Lilu plugin that runs Apple's
Navi 10 drivers on the BC-250's GPU. Based on [NootedRed](https://github.com/ChefKissInc/NootedRed).

Tested on macOS Tahoe 26.7.1 and Sonoma 14.8 with a 4 GB UMA frame buffer in the BIOS.

## Build

    git submodule update --init
    make CONFIG=Release zip

Needs clang 19 or newer and ld64 (cctools-port on Linux).

## License

Thou Shalt Not Profit License 1.5, as NootedRed. See `LICENSE`.
