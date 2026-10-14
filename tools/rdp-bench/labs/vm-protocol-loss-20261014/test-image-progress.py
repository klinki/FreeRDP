"""Offline image-progress checks; never load Xlib or open a display."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('image_progress',Path(__file__).with_name('image-progress.py'))
image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(image)


def header(width=1,height=1,stride=4,byte_order=0,bits=32,depth=24,
           red=0xFF0000,green=0xFF00,blue=0xFF):
    return dict(width=width,height=height,xoffset=0,format=2,byte_order=byte_order,
                depth=depth,bits_per_pixel=bits,bytes_per_line=stride,
                red_mask=red,green_mask=green,blue_mask=blue)


class OfflineImageTests(unittest.TestCase):
    def test_bounded_schedule_and_configuration(self):
        self.assertEqual(image.validate_config(120,1,[448,280,384,240],2,.05),120)
        self.assertEqual(image.validate_config(1.2,.5,[0,0,1,1],2,.05),3)
        for seconds,interval,region in ((0,1,[0,0,1,1]),(601,1,[0,0,1,1]),
                (120,0,[0,0,1,1]),(120,float('nan'),[0,0,1,1]),
                (120,1,[-1,0,1,1]),(120,1,[0,0,2048,2048])):
            with self.subTest(seconds=seconds,interval=interval,region=region):
                with self.assertRaises(ValueError):
                    image.validate_config(seconds,interval,region,2,.05)

    def test_dimensions_stride_masks_and_truncation_validate_before_read(self):
        self.assertEqual(image.validate_header(header(),1,1),4)
        for changes in ({'width':2},{'height':2},{'xoffset':1},{'format':1},
                        {'bytes_per_line':3},{'bits_per_pixel':8},{'byte_order':2},
                        {'red_mask':0xFF00},{'red_mask':0x050000}):
            with self.subTest(changes=changes):
                with self.assertRaises(ValueError):
                    image.validate_header(dict(header(),**changes),1,1)
        with self.assertRaises(ValueError):
            image.canonical_rgb(b'\0'*3,header())

    def test_alpha_and_padding_do_not_create_false_content_changes(self):
        h=header(width=1,height=2,stride=8)
        one=bytes([3,2,1,99,5,6,7,8, 30,20,10,100,9,10,11,12])
        two=bytes([3,2,1,0,0,0,0,0, 30,20,10,255,0,0,0,0])
        a=image.canonical_rgb(one,h);b=image.canonical_rgb(two,h)
        self.assertEqual(a,bytes([1,2,3,10,20,30]))
        self.assertEqual(a,b)
        self.assertEqual(image.changed_pixels(a,b),0)

    def test_big_endian_and_rgb565_normalize_colours(self):
        self.assertEqual(image.canonical_rgb(bytes([99,1,2,3]),header(byte_order=1)),bytes([1,2,3]))
        h=header(stride=2,bits=16,depth=16,red=0xF800,green=0x7E0,blue=0x1F)
        self.assertEqual(image.canonical_rgb(b'\0\xF8',h),bytes([255,0,0]))
        self.assertEqual(image.canonical_rgb(b'\xE0\x07',h),bytes([0,255,0]))

    def test_pixel_count_is_not_changed_component_count(self):
        self.assertEqual(image.changed_pixels(b'\0'*6,bytes([1,2,3,0,0,0])),1)
        self.assertEqual(image.changed_pixels(b'\0'*6,bytes([1,0,0,0,0,1])),2)
        with self.assertRaises(ValueError):
            image.changed_pixels(b'\0'*3,b'\0'*6)

    def test_ppm_header_dimensions_and_overwrite_refusal(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'sample.ppm'
            image.write_ppm(path,1,2,b'\x01\x02\x03'*2)
            self.assertEqual(path.read_bytes(),b'P6\n1 2\n255\n'+b'\x01\x02\x03'*2)
            with self.assertRaises(FileExistsError):image.write_ppm(path,1,2,b'\0'*6)
            with self.assertRaises(ValueError):image.write_ppm(Path(directory)/'bad.ppm',1,2,b'\0')


if __name__ == '__main__':
    unittest.main()
