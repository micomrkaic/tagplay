/* click probe: a 1 kHz sine runs through the chain while modes toggle
 * off -> tube -> off; the largest sample-to-sample jump must stay near
 * the signal's own slope. An unseamed switch steps the waveform. */
#include "dsp.c"
#include <stdio.h>
#include <math.h>
int main(void) {
    dsp_chain *c = dsp_create();
    dsp_on_format(c, 44100, 2);
    enum { BL = 1024, NBLK = 500 };
    static float buf[BL * 2];
    double ph = 0, w = 2 * M_PI * 1000.0 / 44100.0;
    double maxj = 0; float prev = 0; int have_prev = 0;
    for (int b = 0; b < NBLK; b++) {
        if (b == 120) dsp_set_mode(c, "tube", 0.8);
        if (b == 300) dsp_set_mode(c, "off", 0);
        for (int i = 0; i < BL; i++) {
            float s = (float)(0.5 * sin(ph)); ph += w;
            buf[2 * i] = buf[2 * i + 1] = s;
        }
        dsp_process(c, buf, BL);
        for (int i = 0; i < BL; i++) {
            if (have_prev) {
                double j = fabs((double)buf[2 * i] - prev);
                if (j > maxj) maxj = j;
            }
            prev = buf[2 * i]; have_prev = 1;
        }
    }
    printf("max jump %.4f\n", maxj);
    return maxj < 0.18 ? 0 : 1;
}
