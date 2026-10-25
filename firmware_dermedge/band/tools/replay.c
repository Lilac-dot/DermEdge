// Runs the band's scratch detector (main/scratch.c, unchanged) over a raw recording, so tune.py
// trains on exactly the features the band computes. Reads int16 x, y, z triples (little-endian,
// 256 counts per g, 800 Hz) on stdin; prints one CSV row per one-second window.
//   cc -O2 -o replay tools/replay.c main/scratch.c -lm   (tune.py builds it)
#include <stdio.h>

#include "../main/scratch.h"

int main(void) {
    static scratch_t s;
    scratch_reset(&s);
    printf("t,p,scratchy,activity,vibration");
    for (int i = 0; i < SCRATCH_N_FEATURES; i++) printf(",f%d", i);
    printf("\n");
    short xyz[3];
    while (fread(xyz, sizeof(short), 3, stdin) == 3) {
        scratch_motion_t m;
        scratch_event_t e;
        if (scratch_feed(&s, xyz[0] / 256.0f, xyz[1] / 256.0f, xyz[2] / 256.0f, &m, &e) & SCRATCH_GOT_MOTION) {
            printf("%.2f,%.4f,%d,%.5f,%.6f", m.t, m.p, m.scratchy, m.activity, m.scratch_energy);
            for (int i = 0; i < SCRATCH_N_FEATURES; i++) printf(",%.5f", m.f[i]);
            printf("\n");
        }
    }
    return 0;
}
