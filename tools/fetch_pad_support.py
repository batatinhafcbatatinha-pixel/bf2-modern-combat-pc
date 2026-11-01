"""Controller support files next to the executable (downloaded and checked; never committed).

SDL_GameControllerDB (zlib license) names the buttons of PS2/PS3/PS4/PS5 pads, USB adapters and other generic
controllers for SDL. With `sdl3=True`, also SDL3.dll (zlib license), which the original-Xbox ports load at run time
for every pad that is not an XInput device (the Xbox 360 ports have SDL inside the SDK).
"""
import hashlib
import io
from pathlib import Path
import urllib.request
import zipfile

SDL3_URL = 'https://github.com/libsdl-org/SDL/releases/download/release-3.2.26/SDL3-3.2.26-win32-x64.zip'
SDL3_SHA256 = '866bd702f16cc767420ed401e8c5663679833a0de56892f03e4ef3e9c0a5a333'
DB_URL = ('https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/'
          '22a98f6339abf4b6ca6cb061443a70e4f7ab252d/gamecontrollerdb.txt')
DB_SHA256 = '0f17831b6a9e0e9fc2f52bf8ab1bef1d30ffd307d966c2118e556db345ea7e96'


def fetch(url, sha256):
    with urllib.request.urlopen(url, timeout=120) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != sha256:
        raise ValueError('Unexpected download: ' + url)
    return data


def install(destination, sdl3=False):
    destination = Path(destination)
    if not (destination / 'gamecontrollerdb.txt').is_file():
        (destination / 'gamecontrollerdb.txt').write_bytes(fetch(DB_URL, DB_SHA256))
    if sdl3 and not (destination / 'SDL3.dll').is_file():
        with zipfile.ZipFile(io.BytesIO(fetch(SDL3_URL, SDL3_SHA256))) as archive:
            (destination / 'SDL3.dll').write_bytes(archive.read('SDL3.dll'))
            (destination / 'SDL3-LICENSE.txt').write_bytes(archive.read('LICENSE.txt'))
