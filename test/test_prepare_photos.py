"""Run with: .venv/bin/python -m unittest discover -s test -p 'test_*.py'."""

from contextlib import redirect_stdout
from io import StringIO
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image

spec = importlib.util.spec_from_file_location("prepare_photos", Path(__file__).resolve().parents[1]
                                             / "tools" / "prepare_photos.py")
photos = importlib.util.module_from_spec(spec)
spec.loader.exec_module(photos)


class PhotoPreparationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "originals"
        self.output = self.root / "data" / "photos"
        self.source.mkdir()

    def prepare(self, **kwargs):
        with redirect_stdout(StringIO()):
            return photos.prepare(self.source, self.output, **kwargs)

    def test_ten_photos_baseline_size_order_and_originals_preserved(self):
        for i in range(10):
            Image.new("RGB", (1200, 800), (i * 20, 40, 60)).save(self.source / f"{i:02d}.png")
        original = (self.source / "00.png").read_bytes()
        self.assertEqual(self.prepare(), 10)
        files = sorted(self.output.glob("*.jpg"))
        self.assertEqual(len(files), 10)
        self.assertEqual(files[0].name, "001.jpg")
        self.assertLess(sum(p.stat().st_size for p in files), photos.MAX_DATA_BYTES)
        for p in files:
            with Image.open(p) as image:
                self.assertEqual(image.size, (480, 320))
                self.assertEqual(image.mode, "RGB")
                self.assertFalse(image.info.get("progressive", False))
        self.assertEqual((self.source / "00.png").read_bytes(), original)

    def test_transparency_letterbox_and_crop(self):
        Image.new("RGBA", (100, 200), (255, 0, 0, 128)).save(self.source / "portrait.png")
        self.prepare()
        with Image.open(self.output / "001.jpg") as image:
            self.assertTrue(all(abs(c - 18) <= 2 for c in image.getpixel((0, 0))))
            self.assertGreater(image.getpixel((240, 160))[0], 100)
        self.prepare(crop=True)
        with Image.open(self.output / "001.jpg") as image:
            self.assertGreater(image.getpixel((0, 0))[0], 100)

    def test_exif_rotation_and_metadata_removed(self):
        image = Image.new("RGB", (80, 40), (240, 20, 20))
        exif = Image.Exif()
        exif[274] = 6  # 90-degree rotation: becomes a portrait after transpose
        image.save(self.source / "rotated.jpg", exif=exif)
        self.prepare()
        with Image.open(self.output / "001.jpg") as result:
            self.assertFalse(result.getexif())
            self.assertLess(result.getpixel((100, 160))[0], 30)  # portrait border
            self.assertGreater(result.getpixel((240, 160))[0], 200)

    def test_failed_conversion_or_budget_check_preserves_previous_album(self):
        self.output.mkdir(parents=True)
        previous = self.output / "001.jpg"
        previous.write_bytes(b"previous album")
        (self.source / "broken.jpg").write_bytes(b"not an image")
        with self.assertRaises(OSError):
            self.prepare()
        self.assertEqual(previous.read_bytes(), b"previous album")
        (self.source / "broken.jpg").unlink()
        Image.new("RGB", (20, 20)).save(self.source / "good.png")
        with patch.object(photos, "MAX_DATA_BYTES", 1):
            with self.assertRaises(ValueError):
                self.prepare()
        self.assertEqual(previous.read_bytes(), b"previous album")

    def test_reprepare_removes_stale_jpegs_and_rejects_nested_paths(self):
        Image.new("RGB", (20, 20)).save(self.source / "a.png")
        Image.new("RGB", (20, 20)).save(self.source / "b.png")
        self.prepare()
        (self.source / "b.png").unlink()
        self.prepare()
        self.assertEqual(len(list(self.output.glob("*.jpg"))), 1)
        with self.assertRaises(ValueError):
            photos.prepare(self.source, self.source)
        with self.assertRaises(ValueError):
            photos.prepare(self.source, self.source / "generated")

    def test_empty_folder_and_too_many_images_are_actionable(self):
        with self.assertRaisesRegex(ValueError, "Add JPG"):
            self.prepare()
        for i in range(33):
            (self.source / f"{i}.jpg").write_bytes(b"unused")
        with self.assertRaisesRegex(ValueError, "maximum is 32"):
            self.prepare()


if __name__ == "__main__":
    unittest.main()
