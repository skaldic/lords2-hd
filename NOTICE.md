# Notice

This is an unofficial fan patch, not affiliated with or endorsed by the owners of Lords of the
Realm II. All rights in the game stay with them.

The art in the release archive is derived from the game's own pictures. It is free, is meant only for
use with a copy of the game you own, and is not covered by the license in LICENSE.md. Nobody may sell
it. It will be taken down on request from the rights holders.

## How the art was made

- The double-detail pictures, ground, buildings and troops are the game's own art enlarged with
  Real-ESRGAN, an AI upscaler.
- The new left and right parts of the widened full-screen paintings were painted by an AI image
  model (FLUX.1 Fill [dev], used under its non-commercial license) from the game's own picture. The
  original 4:3 picture is unchanged in the middle.

## How the voices were made

The dialogue and the unit voices (716 of the game's sound files) are the game's own recordings
restored with [Lords2Resounded](https://github.com/gqzomer/Lords2Resounded), used with its author's
permission: the LavaSR AI model rebuilds the high frequencies the game's 11 kHz files lost, and the
DPDFNet model removes hiss from the dialogue. The sound effects and the music are the game's own.

## The multiplayer layer

`dplayx.dll` is built from the LotR2 Fix multiplayer source in `multiplayer/`, by members of the
Lords of the Realm II community: Wolfe (the replacement DirectPlay layer, the per-turn sync check and
repair, the host checks and the game's multiplayer desync fixes) and Socius (GameRanger support). It
is included with Wolfe's and Socius's permission; see `multiplayer/CREDITS.md`.

## AMD FidelityFX Super Resolution 1

The FSR upscaling (`[Display] Upscale=fsr`) is a port of the two passes of AMD FidelityFX Super
Resolution 1.0 (`ffx_fsr1.h`), to C for the processor and to HLSL for the graphics card, under the
MIT license:

> Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
> associated documentation files (the "Software"), to deal in the Software without restriction,
> including without limitation the rights to use, copy, modify, merge, publish, distribute,
> sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all copies or
> substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
> NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
> NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
> OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
> CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## The videos

The disc-quality videos are the game's own, as on its original CD, unchanged: the GOG release has
smaller, interlaced copies. They were made available by a member of the community in the SMK_HIGH
thread on the GOG forum. Like the art, they are meant only for use with a copy of the game you own
and will be taken down on request from the rights holders.
