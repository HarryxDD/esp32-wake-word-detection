import os
import logging
import argparse
import tempfile
import shutil
from git import Repo


KISSFFT_URL = "https://github.com/mborgerding/kissfft.git"
KISSFFT_DIR = "kissfft"
KISSFFT_SRC = [
    "kiss_fft.h",
    "kiss_fft.c",
    "kiss_fftr.h",
    "kiss_fftr.c",
    "kiss_fft_log.h",
    "_kiss_fft_guts.h",
    "COPYING",
    "LICENSES"
]


def sync():
    root_dir = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
    kiss_dir = os.path.join(root_dir, KISSFFT_DIR)
    shutil.rmtree(kiss_dir)
    os.mkdir(kiss_dir)

    tmpdir = tempfile.TemporaryDirectory()
    Repo.clone_from(KISSFFT_URL, tmpdir.name)
    for src in KISSFFT_SRC:
        src_path = os.path.join(tmpdir.name, src)
        if os.path.isfile(src_path):
            shutil.copyfile(src_path, os.path.join(kiss_dir, src))
        else:
            shutil.copytree(src_path, os.path.join(kiss_dir, src))
    tmpdir.cleanup()


parser = argparse.ArgumentParser("ESP kissfft tools")
parser.add_argument('command', type=str, help='Command to perform')
args = parser.parse_args()

if args.command == 'sync':
    sync()
else:
    logging.error("Invalid command")