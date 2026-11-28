# Solar-system asset sources

Downloaded on 2026-11-13. The application treats these files as immutable baselines; saved projects contain design parameters, not edited copies of the maps.

## Surface and cloud maps

The following files come from [Solar System Scope's texture collection](https://www.solarsystemscope.com/textures/). The collection credits INOVE, describes the maps as composites based on NASA elevation and imagery, and distributes the textures under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Credit: **Solar System Scope / INOVE, CC BY 4.0**. The source page offers each named `download/` file. These are 2048 x 1024 equirectangular color maps; the ring image is a 2048 x 125 radial RGBA strip. Colors are artistically tuned and are not calibrated scientific measurements.

| Local file | Source file | Role |
| --- | --- | --- |
| `sun.jpg` | `2k_sun.jpg` | Visible photosphere baseline |
| `mercury.jpg` | `2k_mercury.jpg` | Mercury visible surface |
| `venus.jpg` | `2k_venus_atmosphere.jpg` | Venus visible cloud cover |
| `venus_surface.jpg` | `2k_venus_surface.jpg` | Radar-derived surface view beneath the visible cloud mesh; not visible-light color or measured elevation |
| `earth.jpg` | `2k_earth_daymap.jpg` | Earth land, ocean and ice |
| `earth_clouds.jpg` | `2k_earth_clouds.jpg` | Independent cloud overlay |
| `mars.jpg` | `2k_mars.jpg` | Mars color/albedo baseline |
| `jupiter.jpg` | `2k_jupiter.jpg` | Jupiter visible clouds |
| `saturn.jpg` | `2k_saturn.jpg` | Saturn visible clouds |
| `saturn_ring_alpha.png` | `2k_saturn_ring_alpha.png` | Radial ring color and alpha, including gaps |
| `uranus.jpg` | `2k_uranus.jpg` | Uranus visible clouds |
| `neptune.jpg` | `2k_neptune.jpg` | Neptune visible clouds |

Each source URL is `https://www.solarsystemscope.com/textures/download/<source file>`.

## Relief maps

`earth_height.jpg` and `mars_height.jpg` come from the `images/earthbump1k.jpg` and `images/marsbump1k.jpg` files in [threex.planets](https://github.com/jeromeetienne/threex.planets). That repository states an MIT license in its README, but it does not clearly identify the original authors or license of these two raster maps. Both are 1000 x 500 equirectangular grayscale bump maps. Their upstream scientific lineage and height units are not identified, so the renderer treats them as **relative relief only**. They are suitable for the current visual prototype, not quantitative elevation analysis or redistribution without a further source check. Replace them with documented NASA/USGS elevation products before publication.

The Mars bump map begins at a longitude approximately 180 degrees apart from the Solar System Scope color map. The renderer samples it at `(color_u + 0.5) mod 1` to align Olympus Mons and the surrounding major structures. Earth color and bump maps have matching geographic orientation at this resolution. The relief slider scales artistic mesh displacement; physical collision radius remains unchanged.

For the Sun, Mercury, Venus, Jupiter, Saturn, Uranus and Neptune, the renderer derives a fixed 256 x 128 grayscale shape map from each body's own color baseline. It averages image luminance into map cells, smooths neighboring cells with longitude wrapping, and scales local contrast before mesh displacement. This keeps visible features anchored to the source image. The resulting shape is an **artistic relative form**, not measured topography: it represents photosphere structure on the Sun, visible cloud tops on Venus and the giant planets, and image-based surface texture on Mercury. In particular, image brightness must not be interpreted as altitude, cloud pressure or a solid surface. Setting relief to zero restores a sphere; changing relief does not change the collision radius.

Mercury's crater-depth control depresses locally darker image centers relative to their surrounding neighborhoods. It is an image-guided artistic basin effect, not crater detection or measured depth. Venus uses a second shape map made by the same process from `venus_surface.jpg`; its inner radar-derived mesh and outer visible cloud mesh have independent relief controls, and cloud opacity reveals the inner mesh. Jupiter's belt relief uses the longitudinal average of its own shape map at each latitude. Its Great Red Spot relief is a local artistic form centered at the existing image anchor `(u=0.37, v=0.61)`; this position belongs to the selected image baseline and is not a time-evolving storm model. These additional controls affect only rendering, not physical collision radii.

## Image decoder

`third_party/stb_image.h` is from [nothings/stb](https://github.com/nothings/stb), distributed under its public-domain-or-MIT dual choice. It is used only to decode the baseline JPEG/PNG files into RGBA bytes.

## Interpretation

The optional dated orbital preset uses NASA/JPL Horizons geometric vectors for the Sun and planetary-system barycenters, with DE440 GM ratios. Original responses, exact queries and quarterly reference states are retained in `assets/ephemeris-year`; see [the observed-year documentation](OBSERVED_YEAR.md) for units, time scale and approximation limits. The default teaching preset remains schematic.

The default orbital radii and body sizes are schematic. Relative planetary masses and circular speeds provide a reproducible REBOUND scene, not a dated ephemeris. Gas and ice giants use their visible cloud maps and artistic cloud-top displacement; the prototype does not infer a solid rocky surface from those images. Storm and sunspot controls are image-anchored artistic overlays rather than scientific flow models. Uranus currently has a deliberately faint procedural ring whose axis follows its visual tilt; this is not a measured ring opacity profile. Advanced atmosphere, measured height-map provenance and ring particle models remain later work.
