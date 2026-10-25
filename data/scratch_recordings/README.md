# DermEdge scratch recordings

Labelled wrist-band accelerometer clips for training and testing the DermEdge band's scratch
detector (`firmware_dermedge/band/tools/tune.py`). Recorded on 25 October 2026 with
`tools/record.py session`, by volunteers who agreed to this anonymised data being published.

## What's here

| Person | Clips | Hand wearing the board | Notes |
|---|---|---|---|
| P01 | 16 | right | the first session |
| P02 | 4 | right | round 1 rest, nails, fingers and waving only: the band restarted mid-session and the other clips came out empty, so they're left out |
| P03 | 16 | right | |
| P04 | 16 | right | |
| P05 | 16 | right | |
| P06 | 15 | right | `rest_2` left out: she scratched during that "rest" clip |

Each person did 8 activities for 20 seconds, twice (`<activity>_<round>.npz`):

| Activity | Scratching? | What they did |
|---|---|---|
| `nails` | yes | scratched the forearm with the nails, like an itch |
| `fingers` | yes | scratched with the fingertips only, wrist as still as possible |
| `rest` | no | hand still on the table |
| `waving` | no | waved side to side without touching anything (round 2 often gentler) |
| `typing` | no | typed on a keyboard |
| `phone` | no | tapped and swiped on a phone |
| `rubbing` | no | rubbed or stroked the forearm with the palm, like putting on moisturiser |
| `marching` | no | marched on the spot, swinging the arms |

## Format

Each `.npz` (numpy, `np.load`) holds:

| Field | |
|---|---|
| `xyz` | int16, shape (n, 3): the ADXL345's x, y, z at 800 Hz |
| `fs` | 800 (Hz) |
| `lsb_per_g` | 256: divide `xyz` by this for g (full resolution, ±2 g) |
| `label` | the activity |
| `scratch` | true for `nails` and `fingers` |
| `person` | P01-P06 |
| `hand` | `right`, `left` or `unrecorded` |

The board (an ADXL345 on an add-on board) was strapped flat to the **back of the hand**, the same
way round for everyone. On a still hand its noise is about 14-22 mg RMS, flat across 0-400 Hz.

## Privacy

There are no names, dates, times or anything else about the volunteers, only an ID per person;
the list matching IDs to people is not kept with the data. The data is hand movement only: no
audio, video or photos. Anyone who took part can ask Tishya to have their clips removed.

## Using it

```
cd firmware_dermedge/band
python3 tools/tune.py ../../data/scratch_recordings/P0*          # each person tested with a model trained on the others
python3 tools/tune.py ../../data/scratch_recordings/P0* --write  # train on everyone, write main/scratch_model.h
```
