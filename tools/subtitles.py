r"""Subtitles: transcribe the game's voice lines and movies with whisper.cpp.

    py -3 tools/subtitles.py extract       voice lines and movies to 16 kHz WAVs
    py -3 tools/subtitles.py transcribe    whisper-cli over them (CPU, resumable)
    py -3 tools/subtitles.py movies        the movies again
    py -3 tools/subtitles.py build         build\Release\subs.txt for subtitles.c

Voice lines are the sound-bank entries named speaker + scene + number
(smaiv063, krbzb012). The rest of the banks are effects and music. Work files go
to cap\subs\. subs.txt: one line per sound or movie line,

    v <sound name> <seconds> <text, "|" between the screens of a long line>
    m <movie name> <start s> <end s> <text>

Fix a wrong line by editing cap\subs\fixes.txt ("<name or name@start> <text>";
an empty text drops the line) and running build again.
"""
import glob
import json
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "xboxrecomp", "game_files"), "data")
WORK = os.path.join(ROOT, "cap", "subs")
VOICE, MOVIE = os.path.join(WORK, "voice"), os.path.join(WORK, "movie")
WHISPER = os.path.join(ROOT, "tools", "whisper")
MODEL = os.path.join(WHISPER, "ggml-large-v3.bin")
SKIP_MOVIES = {"activisn", "treyarch"}          # logos, no speech
VOICE_NAME = re.compile(r"^[a-z]{2}[a-z]{3}\d{2,3}[a-z]?$")
PROMPT = ("Spider-Man, Peter Parker, Norman Osborn, Harry Osborn, Green Goblin, Mary Jane, "
          "Uncle Ben, Aunt May, J. Jonah Jameson, Oscorp, Daily Bugle, Vulture, Shocker, "
          "Scorpion, Kraven, Bruce Campbell, Quest Aerospace, Stan Lee, Spidey.")


def extract():
    os.makedirs(VOICE, exist_ok=True)
    os.makedirs(MOVIE, exist_ok=True)
    seen = {}
    for xsh_path in sorted(glob.glob(os.path.join(DATA, "xboxsound", "ENGLISH", "*.XSH"))):
        xsd_path = xsh_path[:-4] + ".XSD"
        if not os.path.exists(xsd_path):
            continue
        xsh = open(xsh_path, "rb").read()
        xsd = None
        for i in range(struct.unpack_from("<I", xsh, 8)[0]):
            e = xsh[12 + i * 0x60:12 + (i + 1) * 0x60]
            name = e[:32].split(b"\0")[0].decode().lower()
            if not VOICE_NAME.match(name) or name in seen:
                continue
            off, size = struct.unpack_from("<II", e, 0x24)
            f = e.find(b"\x69\x00", 0x30)
            if f < 0 or size < 512:
                continue
            tag, ch, rate, avg, align, bits = struct.unpack_from("<HHIIHH", e, f)
            if xsd is None:
                xsd = open(xsd_path, "rb").read()
            fmt = struct.pack("<HHIIHHHH", tag, ch, rate, avg, align, bits, 2, 64)
            body = xsd[off:off + size]
            seen[name] = size / max(avg, 1)
            out = os.path.join(VOICE, name + ".wav")
            if os.path.exists(out):
                continue
            tmp = os.path.join(VOICE, name + ".adpcm.wav")
            open(tmp, "wb").write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(body))
                                  + b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt
                                  + b"data" + struct.pack("<I", len(body)) + body)
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", tmp, "-ar", "16000", "-ac", "1",
                            "-c:a", "pcm_s16le", out], check=True)
            os.remove(tmp)
    with open(os.path.join(WORK, "voice_seconds.json"), "w") as f:
        json.dump(seen, f)
    print("%d voice lines, %d s" % (len(seen), sum(seen.values())))
    for bik in sorted(glob.glob(os.path.join(DATA, "movies", "*.bik"))):
        name = os.path.splitext(os.path.basename(bik))[0].lower()
        out = os.path.join(MOVIE, name + ".wav")
        if name in SKIP_MOVIES or os.path.exists(out):
            continue
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", bik, "-vn", "-ar", "16000", "-ac", "1",
                        "-c:a", "pcm_s16le", out], check=True)
        print("movie", name)


def whisper_cli():
    hits = glob.glob(os.path.join(WHISPER, "**", "whisper-cli.exe"), recursive=True)
    if not hits:
        sys.exit("whisper-cli.exe not built yet under tools\\whisper")
    return hits[0]


def movies():
    """No carried context (-mc 0): with it, music makes whisper loop on one line."""
    cli = whisper_cli()
    threads = str(max(2, (os.cpu_count() or 4) - 4))
    args = [cli, "-m", MODEL, "-l", "en", "-t", threads, "-oj", "-np", "-mc", "0",
            "--prompt", PROMPT]
    for w in sorted(glob.glob(os.path.join(MOVIE, "*.wav"))):
        args += ["-f", w]
    subprocess.run(args, check=True)


def transcribe():
    cli = whisper_cli()
    threads = str(max(2, (os.cpu_count() or 4) - 4))    # leave the game some
    # Movies first: they are the most watched, and the quickest check of quality.
    todo = (sorted(glob.glob(os.path.join(MOVIE, "*.wav")))
            + sorted(glob.glob(os.path.join(VOICE, "*.wav"))))
    todo = [w for w in todo if not os.path.exists(w + ".json")]
    print(len(todo), "to do")
    for i in range(0, len(todo), 40):                   # one model load per 40 files
        batch = todo[i:i + 40]
        args = [cli, "-m", MODEL, "-l", "en", "-t", threads, "-oj", "-np", "--prompt", PROMPT]
        for w in batch:
            args += ["-f", w]
        subprocess.run(args, check=True)
        print("done", i + len(batch), "of", len(todo), flush=True)


# What whisper makes of music and silence: phrases from its training captions.
INVENTED = re.compile(r"thank(s| you) for watching|subscribe|subtitles? by|captions? by|translated by|amara\.org|see you (in the )?next|^you\.?$|^bye\.?$|^thank you\.?$", re.I)

NOISE = re.compile(r"^[\s\[\(\*♪.…-]*(music|applause|silence|blank_audio|laugh\w*|grunt\w*|"
                   r"sigh\w*|gasp\w*|groan\w*|breath\w*|inaudible|sound\w*)?[\s\]\)\*♪.…-]*$", re.I)

# A fight's grunts: captioning each would keep a bar on screen through every punch.
GRUNT = re.compile(r"^(ugh|oof|ah|huh|oh|ha|gah|augh|huah|pfft|hmm|arf)[.!?]*$", re.I)


def clean(t):
    t = re.sub(r"\[[^\]]*\]|\([^)]*\)|\*[^*]*\*|♪", " ", t)
    t = re.sub(r"\s+", " ", t).strip()
    # The font has no accents or curly quotes.
    t = t.replace("’", "'").replace("“", '"').replace("”", '"').replace("—", "-")
    t = t.encode("ascii", "ignore").decode().lstrip("- ")
    return "" if NOISE.match(t) or INVENTED.search(t) or GRUNT.match(t) else t


def pages(t, most=150):
    """A long line as screens of two lines or so, "|" between them, at sentence ends if it can."""
    if len(t) <= most:
        return t
    out, cur = [], ""
    for part in re.split(r"(?<=[.!?,;])\s+", t):
        while len(part) > most:
            cut = part.rfind(" ", 0, most)
            out.append((cur + " " + part[:cut]).strip() if cur else part[:cut])
            cur, part = "", part[cut + 1:]
        if cur and len(cur) + 1 + len(part) > most:
            out.append(cur)
            cur = ""
        cur = (cur + " " + part).strip()
    return "|".join(out + [cur] if cur else out)


def ms(ts):
    h, m, s = ts.replace(",", ".").split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def build():
    secs = json.load(open(os.path.join(WORK, "voice_seconds.json")))
    fixes = {}
    fx = os.path.join(WORK, "fixes.txt")
    if os.path.exists(fx):
        for line in open(fx, encoding="utf-8"):
            p = line.rstrip("\n").split(" ", 1)
            if p and p[0]:
                fixes[p[0]] = p[1] if len(p) > 1 else ""
    out = []
    for j in sorted(glob.glob(os.path.join(VOICE, "*.wav.json"))):
        name = os.path.basename(j)[:-9]
        d = json.load(open(j, encoding="utf-8", errors="replace"))
        text = clean(" ".join(s["text"] for s in d.get("transcription", [])))
        text = fixes.get(name, text)
        if text:
            out.append("v %s %.2f %s" % (name, secs.get(name, 3.0), pages(text)))
    for j in sorted(glob.glob(os.path.join(MOVIE, "*.wav.json"))):
        name = os.path.basename(j)[:-9]
        d = json.load(open(j, encoding="utf-8", errors="replace"))
        prev = ""
        for s in d.get("transcription", []):
            a, b = ms(s["timestamps"]["from"]), ms(s["timestamps"]["to"])
            text = fixes.get("%s@%d" % (name, a), clean(s["text"]))
            if text and text.lower() != prev:   # whisper looping on a line
                out.append("m %s %.2f %.2f %s" % (name, a, b, text))
            prev = text.lower()
    for d in ("Release", "Test"):
        path = os.path.join(ROOT, "build", d, "subs.txt")
        with open(path, "w", encoding="ascii", errors="ignore", newline="\n") as f:
            f.write("\n".join(out) + "\n")
    print(len(out), "lines")


if __name__ == "__main__":
    {"extract": extract, "transcribe": transcribe, "movies": movies, "build": build}[sys.argv[1]]()
