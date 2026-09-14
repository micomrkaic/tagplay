#include <stdio.h>
#include "decoder.h"
int main(int argc, char **argv) {
    decoder *d = decoder_open(argv[1], FMT_RADIO);
    if (!d) { printf("OPEN-FAIL err='%s'\n", decoder_open_error()); return 1; }
    printf("rate=%d ch=%d\n", decoder_rate(d), decoder_channels(d));
    long total = 0; float buf[16384];
    for (int i = 0; i < 400 && total < 48000 * 4; i++) {
        long n = decoder_read(d, buf, 4096);
        if (n > 0) total += n;
        else if (n == 0) break;
    }
    printf("frames=%ld (%.1fs) %s\n", total, total / 48000.0,
           total >= 48000 * 3 ? "PASS" : "FAIL");
    return total >= 48000 * 3 ? 0 : 1;
}
