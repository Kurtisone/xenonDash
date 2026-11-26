// skate1 - native D3D layer (see docs/ngpu_core.md)

#pragma once

namespace rex {
class Runtime;
}

// Called once at startup; does nothing unless native_gpu is set.
void StartNativeGpu(rex::Runtime* runtime);
