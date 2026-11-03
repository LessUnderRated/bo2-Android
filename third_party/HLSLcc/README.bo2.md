# HLSLcc — vendored

Unity's DirectX-bytecode cross compiler, used here to turn BO2's DXBC shaders
into Metal Shading Language. Upstream: https://github.com/Unity-Technologies/HLSLcc
(license.txt is upstream's; unmodified sources).

## Why this and not DXVK / DXMT

DXVK (DXBC -> SPIR-V -> MoltenVK) and DXMT (DXBC -> AIR) both build as **PE DLLs
that reach their native backend through Wine's unixlib mechanism**. This port has
no Wine: it has its own intercepted fake-COM D3D11 vtables over a 32-bit guest
memory map, so neither can drop in — using one means linking it natively for arm64
and forwarding our intercepted calls into its D3D11 implementation.

HLSLcc solves only the part we actually cannot do ourselves — translating the
bytecode — and it is a plain C++11 library with no Wine, no Vulkan and no
MoltenVK. We keep our own D3D11 interception (already written) and compile the
translated MSL with the Metal API at runtime.

## Measured on BO2's real shaders

Extracted with `BO2_SHADER_DUMP=1` (2518 distinct shaders created, 314 bound by
the menu alone — hand-translation was never viable):

    2185 / 2185 shaders translated to MSL, zero failures.

The first one translated is the frontend's font shader — it samples the glyph
atlas and multiplies by the vertex colour, and it even carries Treyarch's own
spelling of `FontTextutre`. That is precisely the shader the menu's text needs.

## Build

    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -S . -B build
    cmake --build build -j

Builds clean for arm64 macOS as-is.
