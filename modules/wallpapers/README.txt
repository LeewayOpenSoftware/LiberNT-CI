Wallpapers
===========

The JPEG wallpapers are resampled to 1920 px wide (Lanczos) and re-encoded as
progressive JPEG at quality 82 with metadata stripped. "Sea Bridge" ships
additionally as a PNG. See LICENSE.txt for the governing terms, current
clearance status, and required provenance record.

|-------------------------|--------------|-----------------|-------------------------|
| Filename                | License      | Author          | Origin                  |
|-------------------------|--------------|-----------------|-------------------------|
| Calm Lake.jpg           | Restricted*  | Unrecorded      | magnific.com (Freepik)  |
| Feather Droplets.jpg    | Restricted*  | Unrecorded      | magnific.com (Freepik)  |
| Forest Canopy.jpg       | Restricted*  | Unrecorded      | magnific.com (Freepik)  |
| Lake Mountains.jpg      | Restricted*  | Unrecorded      | magnific.com (Freepik)  |
| Lugano Reflections.jpg  | Restricted*  | Unrecorded      | magnific.com (Freepik)  |
| Modern Villa.jpg        | Restricted*  | Unrecorded      | magnific.com (Freepik)  |
| Mountain Lake.jpg       | Restricted*  | Unrecorded      | magnific.com (Freepik)  |
| Night Sky.jpg           | Restricted*  | Unrecorded      | magnific.com (Freepik)  |
| Oil and Water.jpg       | Restricted*  | Unrecorded      | magnific.com (Freepik)  |
| Plant Textures.jpg      | Restricted*  | Unrecorded      | magnific.com (Freepik)  |
| Rain Lights.jpg         | Restricted*  | Unrecorded      | magnific.com (Freepik)  |
| Sea Bridge.jpg          | Restricted*  | Unrecorded      | magnific.com (Freepik)  |
| Sea Bridge.png          | Restricted*  | Unrecorded      | magnific.com (Freepik)  |
| Sunlit Meadow.jpg        | Unrecorded   | Unrecorded      | User supplied          |
| Misty Forest.jpg        | Unsplash     | Daniil Silantev | unsplash.com            |
| Layered Glass 16x9.png  | Unrecorded   | Unrecorded      | Owner supplied          |
| Layered Glass.png       | Unrecorded   | Unrecorded      | Owner supplied          |
| Persian Windows.png     | Unrecorded   | Unrecorded      | Owner supplied          |
| BlueHorizon.png         | Unrecorded   | Unrecorded      | Unrecorded              |
|-------------------------|--------------|-----------------|-------------------------|

* The exact content type and account tier are not recorded. These images must
not be pushed to upstream ReactOS or shipped in a public release until the
per-file redistribution rights required by LICENSE.txt are documented.

"Sunlit Meadow.jpg" was supplied as "8220trolley.jpg" and resampled to
1920 x 1239 using the same JPEG settings as the other wallpapers.
Its size is 321,869 bytes, down from 2,274,011 bytes. Its author and license
have not been supplied; the provenance record is in LICENSE.txt.

"Misty Forest.jpg" is the Unsplash photo "A bird flying over a forest at
night" by Daniil Silantev, free to use under the Unsplash License. It was
resampled from 4297 x 2792 to 1920 x 1248 using the same JPEG settings as the
other wallpapers. Its provenance record is in LICENSE.txt.

"Layered Glass 16x9.png" is the default wallpaper, drawn with WallpaperStyle
10 (Fill): scaled with its aspect ratio kept and cropped centred to the
screen, so it is not distorted at 16:10, 5:4 or 4:3. The three owner-supplied
PNGs ship as supplied (1672 x 941, 16:9), byte-identical to the checksums in
LICENSE.txt; no upscaled variants are shipped. Changing the default means
updating HKCU Control Panel\Desktop\Wallpaper in boot/bootdata/hivedef.inf.
That value is not overwritten when it already exists, so an upgrade keeps the
wallpaper set in the default user hive. BlueHorizon.png, the previous
default, stays installed and selectable.

The wallpapers are included in every built image.
