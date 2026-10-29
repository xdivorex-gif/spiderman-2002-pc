r"""Upscale a Bink movie for movie_upscale.c: movies\<name>.mp4 beside the exe.

    py -3 tools/movie_upscale.py [name ...]     default: activisn

Each frame of game_files\data\movies\<name>.bik is upscaled x4 with
Real-ESRGAN (tools\realesrgan, realesrgan-x4plus: the general model -- the
Activision logo is CG, not drawn art), brought to 1440 lines and encoded
H.264 at the Bink's own frame rate, frame for frame (movie_upscale.c shows
the mp4's n-th frame for the Bink's n-th). Writes build\Release\movies\.
Work files go to cap\fmvup\<name>\.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOVIES = r"C:\Users\adub9\xboxrecomp\game_files\data\movies"
ESRGAN = os.path.join(ROOT, "tools", "realesrgan", "realesrgan-ncnn-vulkan.exe")
OUT = os.path.join(ROOT, "build", "Release", "movies")
HEIGHT = 1440


def run(*args, **kw):
    subprocess.run(list(args), check=True, **kw)


def rate(bik):
    r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                        "stream=r_frame_rate", "-of", "csv=p=0", bik],
                       check=True, capture_output=True, text=True).stdout.strip()
    return r


def upscale(name):
    bik = os.path.join(MOVIES, name + ".bik")
    work = os.path.join(ROOT, "cap", "fmvup", name)
    src, dst = os.path.join(work, "in"), os.path.join(work, "out")
    for d in (src, dst):
        os.makedirs(d, exist_ok=True)
    if not os.listdir(src):
        run("ffmpeg", "-v", "error", "-y", "-i", bik, "-fps_mode", "passthrough",
            os.path.join(src, "%04d.png"))
    if len(os.listdir(dst)) < len(os.listdir(src)):
        run(ESRGAN, "-i", src, "-o", dst, "-n", "realesrgan-x4plus", "-s", "4", "-f", "png",
            cwd=os.path.dirname(ESRGAN))
    os.makedirs(OUT, exist_ok=True)
    mp4 = os.path.join(OUT, name + ".mp4")
    run("ffmpeg", "-v", "error", "-y", "-framerate", rate(bik), "-i", os.path.join(dst, "%04d.png"),
        "-vf", "scale=-2:%d:flags=lanczos" % HEIGHT, "-c:v", "libx264", "-preset", "slow",
        "-crf", "14", "-pix_fmt", "yuv420p", "-bf", "0", "-movflags", "+faststart", mp4)
    print("%s: %d frames -> %s" % (name, len(os.listdir(dst)), mp4))
    return mp4


if __name__ == "__main__":
    for n in sys.argv[1:] or ["activisn"]:
        upscale(n)
