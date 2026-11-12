# HD art: how it is made, what worked, what failed

The owner judges art by eye and was unhappy with early results. This file records what survived.

## Upscaling (works)

- Real-ESRGAN `realesrgan-x4plus` at 4x, reduced to 2x. Run on Windows with
  `realesrgan-ncnn-vulkan.exe`; a Mac has the same tool (ncnn, Metal through MoltenVK).
- **Sprites:** fill the transparent area with colours bled outward from the sprite before
  upscaling, so edges stay clean, and take the shape back from the original mask afterwards
  (`tools/build_sprites.ps1`, `Bleed`). Upscaling a sprite on a flat key colour gives halos.
- **Map tiles:** upscale each tile inside a block of its neighbours and cut the middle, or seams
  show between tiles.
- **Paintings:** upscale whole. Dithered 256-colour gradients come out clean.
- **Small battle units (about 30 pixels tall):** the upscaler is still clearly better than
  pixel-art scaling (Scale2x), which looks almost like the original. This was tested side by side.
- **Battle water:** the opposite. Scale2x on the water tiles kept the original look; the upscaler
  smeared it. The owner chose pixel-art for water.
- **Portraits:** the upscaler sharpens faces but shifts their light and colour (waxy skin, eyes
  turned bright blue). Fix: keep the upscaled detail, put the original's own tones back
  (`hd + blur(original) - blur(hd)`, blur radius 4 at 2x), and mix 25% of the pixel-doubled original
  back in as grain (`tools/likeness.ps1`). The owner picked 25% from a sheet of four strengths.

## Widening 4:3 paintings to 16:9

The original picture always stays untouched in the middle. What fills the sides, by kind of screen:

| Kind of screen | What worked | What failed |
|---|---|---|
| A scene (merchant yard) | FLUX.1 Fill given the **whole** picture with empty sides and a plain description of the scene | SDXL inpainting on close-ups of each edge: it invented a second cow and a doubled door. Mirroring: visibly symmetric |
| A room in perspective (armoury) | FLUX.1 Fill, whole picture | Copying wall columns: hard vertical cuts through shadows |
| Flat stone or brick with panels (menus, castle builder) | FLUX.1 Fill told to continue "the same plain stone"; or copying the screen's own bricks at the same scale | Repainted texture at another scale or colour |
| A regular structure (battlement of the greatest-nobles screen) | Repeating the original's own structure at its real spacing, with only the sky blended | Any image model: it turned the wall's ends into towers |
| Dark pictures on black (title, main menu) | Fading into their own shadow | FLUX drew grey panels or glowing patches |
| Faint, busy relief (score screens) | A dim blurred copy of the picture behind it, or FLUX for one of them | Mirroring |

Rules that came out of it:

- **Whole-picture context beats close-ups.** Painting each side as a close-up at full resolution
  gave sharp but unrelated panels. Painting the whole picture gave the right scene, slightly soft.
- **Keep prompts plain.** Naming objects ("fence posts and rope") makes the model produce a lot of
  them. Describe the surface and the light, and say "continues".
- **Colour-match and fade at the join.** Shift each row of the new part to the original's colour
  next to the join (smoothed over neighbouring rows), then fade over about 64 columns
  (`Room.PaintedRaw`).
- **Enlarge the new parts the same way as the middle** (Real-ESRGAN), or they look softer.
- **If the picture has regular geometry, don't use a model.** Repeat the original.
- **Look at every result at full size against the original before installing it.** One bad screen
  was installed overnight unreviewed and the owner found it.

## Running FLUX.1 Fill locally (Windows, 16 GB AMD card)

- stable-diffusion.cpp `sd-cli`, Vulkan. Files: `flux1-fill-dev-Q6_K.gguf` (9.9 GB),
  `t5-v1_1-xxl-encoder-Q8_0.gguf`, `clip_l.safetensors`, `ae.safetensors` (the image decoder; the
  official copy needs a Hugging Face login, ungated copies exist).
- The Q8 model (12.7 GB) does not leave room to run on 16 GB. Q6_K fits at 1136x640, not at
  1712x960: paint at 1136x640 and enlarge.
- Settings: `--cfg-scale 1 --guidance 30 --steps 30 --sampling-method euler --strength 1`, text
  encoders on the CPU.
- Time: about 10 minutes to load the model from a hard disk, then about 4 minutes per picture. Paint
  several variants per run (`-b`) so it loads once.
- Redirect its output to a file: piping it through PowerShell hides progress and errors.
- FLUX.1 Fill [dev] is for non-commercial use.

On a Mac, the same model runs through other front ends; none of that was tried here.

## Showing HD art over a 256-colour game

Only relevant if the Swift game draws the original art itself: draw from the 2x files directly and
none of the patch's machinery is needed. The patch has to guess what the game drew, so it keeps a
2x layer beside the game's frame and shows an HD pixel only while the 1x pixel under it still holds
what the sprite put there. A Swift game that owns its renderer should simply draw the HD frame.
