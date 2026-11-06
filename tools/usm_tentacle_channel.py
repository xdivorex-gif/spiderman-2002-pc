"""USM Venom's tentacle channel (animation channel 5, skeleton decoder 0x464A04D8
"Tentacles Compressed", sampled by USM.exe 0x5FE5A0 into 15 floats).

One bit of the channel mask per value; values in mask order. Read with a
period of 3 they are five tentacle slots of (activity, diameter, extra):
activity rises from 0 to ~0.5 while that tentacle is out (vententa_2: slot 1
from frame 3 to 23), diameter is 0.2 throughout. USM's renderer builds the
tentacle itself from these (tentacle engines); we use the activity as the
extension of our tentacle bones.
"""
import inspect
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'cap' / 'usm'))
import usm_decode_channels as dc      # noqa: E402
from emu import CPU, Mem              # noqa: E402
from exe import u32                   # noqa: E402

if len(dc.WIDTHS) < 6:
    dc.WIDTHS = dc.WIDTHS + ((1,), (1,) * 15)
_src = inspect.getsource(dc.decode_channel).replace(
    'rotation=index<3 or group<2', 'rotation=(index<3 or group<2) and index!=5')
_ns = {}
exec(_src, dc.__dict__, _ns)
_decode = _ns['decode_channel']
SLOTS = 5


def slots(record):
    """Per frame: [activity of slot 0..4] (0 where the mask leaves it out)."""
    r = _decode(record, 5, CPU, Mem, u32)
    present = [i for i in range(15) if r['mask'] >> i & 1]
    out = []
    for f in r['frames']:
        v = [0.0] * 15
        for i, x in zip(present, f):
            v[i] = x
        out.append([v[3 * k] for k in range(SLOTS)])
    return out
