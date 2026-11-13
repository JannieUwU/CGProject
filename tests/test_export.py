import importlib.util
import struct
import tempfile
import unittest
import zlib
from pathlib import Path
spec = importlib.util.spec_from_file_location("export", Path(__file__).parents[1] / "tools/export.py")
export = importlib.util.module_from_spec(spec)
spec.loader.exec_module(export)
class ExportTests(unittest.TestCase):
    def test_png(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)/"test.png"
            pixels = bytes([255,0,0,255,0,255,0,255])
            export.write_png(p,1,2,pixels)
            data=p.read_bytes()
            self.assertEqual(data[:8],b"\x89PNG\r\n\x1a\n")
            offset=8
            while offset<len(data):
                n=struct.unpack(">I",data[offset:offset+4])[0]
                kind=data[offset+4:offset+8]; payload=data[offset+8:offset+8+n]
                self.assertEqual(struct.unpack(">I",data[offset+8+n:offset+12+n])[0], zlib.crc32(kind+payload)&0xffffffff)
                if kind==b"IDAT": self.assertEqual(zlib.decompress(payload),b"\0"+pixels[:4]+b"\0"+pixels[4:])
                offset+=12+n
    def test_invalid(self):
        with self.assertRaises(ValueError): export.write_png("unused",0,1,b"")
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(ValueError): export.write_results(d,[{}],{})
    def test_metadata(self):
        with tempfile.TemporaryDirectory() as d:
            export.write_results(d,[],{"machine":"test", "snapshot":{"time":42}})
            self.assertTrue((Path(d)/"parameters.json").exists())
            self.assertIn("kernel_ms",(Path(d)/"frames.csv").read_text(encoding="utf-8-sig"))
if __name__=="__main__": unittest.main()
