"""Adapt Vita artwork to PS5 shell formats. Requires Pillow and DirectXTex texconv."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
from PIL import Image, ImageOps


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--texconv', default='texconv')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    vita = root / 'platforms/vita/livearea'
    output = root / 'platforms/ps5/sce_sys'
    with Image.open(vita / 'icon0.png') as image:
        image.convert('RGBA').resize((512, 512), Image.Resampling.LANCZOS).save(output / 'icon0.png')
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        # Selected-game background and launch splash, respectively.
        for name, source in (('pic0', 'bg.png'), ('pic1', 'pic0.png')):
            with Image.open(vita / source) as image:
                image = ImageOps.contain(image.convert('RGBA'), (3840, 2160), Image.Resampling.LANCZOS)
                canvas = Image.new('RGBA', (3840, 2160), (0, 0, 0, 255))
                canvas.paste(image, ((3840 - image.width) // 2, (2160 - image.height) // 2))
                canvas.save(temp / (name + '.png'))
            subprocess.run([args.texconv, '-f', 'BC7_UNORM', '-m', '1', '-y', '-o', str(temp), str(temp / (name + '.png'))], check=True)
            shutil.copyfile(temp / (name + '.dds'), output / (name + '.dds'))


if __name__ == '__main__':
    main()
