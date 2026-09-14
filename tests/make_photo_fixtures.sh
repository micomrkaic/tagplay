#!/bin/sh
# Photo fixtures for the tagview battery. exiftool WRITES the metadata
# my parser must READ: independent implementations meeting over the
# spec, the same adversarial trick as testing ID3 against LAME.
# Requires: python3-pil, libimage-exiftool-perl.
set -e
DEST="${1:-phototest}"
rm -rf "$DEST"
mkdir -p "$DEST/alps" "$DEST/city"
python3 - "$DEST" <<'PYEOF'
import sys, random
from PIL import Image, PngImagePlugin
D = sys.argv[1]
random.seed(7)
specs = [
 (f'{D}/alps/IMG_1001.jpg', (4032,3024), (110,140,190)),
 (f'{D}/alps/IMG_1002.jpg', (4032,3024), (120,150,180)),
 (f'{D}/alps/scan_old.jpg', (1200,800),  (160,140,110)),
 (f'{D}/city/IMG_2001.jpg', (3024,4032), (90,90,100)),
 (f'{D}/city/IMG_2002.jpg', (4032,3024), (80,85,95)),
]
for path,(w,h),base in specs:
    im = Image.new('RGB',(w//8,h//8))
    for y in range(h//8):
        for x in range(w//8):
            im.putpixel((x,y),tuple(min(255,c+random.randint(-30,30)) for c in base))
    im.resize((w,h)).save(path, quality=60)
im = Image.new('RGB',(1920,1080),(40,44,52))
meta = PngImagePlugin.PngInfo()
meta.add_text('Title','terminal at work')
meta.add_text('Keywords','city, screenshot; terminal')
im.save(f'{D}/city/screenshot.png', pnginfo=meta)
PYEOF
exiftool -q -overwrite_original \
  -Make=Apple -Model="iPhone 12 Pro" -DateTimeOriginal="2021:07:14 09:30:00" \
  -IPTC:Keywords=alps -IPTC:Keywords=hiking -IPTC:Keywords=triglav \
  -IPTC:ObjectName="Triglav from Kredarica" -IPTC:By-line=Mico \
  -XMP-dc:Subject=alps -XMP-dc:Subject=summer "$DEST/alps/IMG_1001.jpg"
exiftool -q -overwrite_original \
  -Make=Apple -Model="iPhone 12 Pro" -DateTimeOriginal="2021:07:15 17:05:00" \
  -IPTC:Keywords=alps -IPTC:Keywords=lake -IPTC:City=Bohinj \
  -IPTC:Country-PrimaryLocationName=Slovenia "$DEST/alps/IMG_1002.jpg"
exiftool -q -overwrite_original \
  -Make=NIKON -Model="NIKON F3 (scan)" -DateTimeOriginal="1987:08:02 12:00:00" \
  -IPTC:Keywords=alps -IPTC:Keywords=film -IPTC:Keywords=scanned \
  "$DEST/alps/scan_old.jpg"
exiftool -q -overwrite_original \
  -Make=Canon -Model="Canon EOS R6" -DateTimeOriginal="2023:11:03 21:12:00" \
  -XMP-dc:Subject=city -XMP-dc:Subject=night \
  -XMP-dc:Title="Ljubljana at night" "$DEST/city/IMG_2001.jpg"
exiftool -q -overwrite_original \
  -Make=Canon -Model="Canon EOS R6" -DateTimeOriginal="2023:11:04 08:44:00" \
  -IPTC:Keywords=city -IPTC:Keywords=morning "$DEST/city/IMG_2002.jpg"
echo "fixtures in $DEST"
