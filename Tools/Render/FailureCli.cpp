#include "EngineTestAccess.h"
#define main rendererMain
#include "Main.cpp"
#undef main

int main (int argc, char** argv)
{
    // The initial 32 groups succeed; the first real MIDI mode change fails.
    FluidSynthEngineTestAccess::failAfter (32);
    return rendererMain (argc, argv);
}
