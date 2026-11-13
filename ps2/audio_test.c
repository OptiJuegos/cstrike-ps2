/* Standalone manual audsrv/SPU2 verification.
 * This ELF does not need game textures, songs, memory-card files or ROMFS.
 * Listen for two short tones; see the PCSX2/ps2link console for errors.
 */
#include <3ds.h>
#include <delaythread.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define TONE_RATE 44100
#define TONE_FRAMES 44100

static s16 first[TONE_FRAMES] __attribute__((aligned(64)));
static s16 second[TONE_FRAMES] __attribute__((aligned(64)));
static ndspWaveBuf a, b;

static void make_tone(s16 *dest, float frequency, float gain)
{
    for (int i=0; i<TONE_FRAMES; ++i) {
        float ramp = i < 400 ? (float)i / 400.f :
                     i > TONE_FRAMES - 400 ? (float)(TONE_FRAMES-i) / 400.f : 1.f;
        dest[i] = (s16)(sinf((float)(6.283185307179586 * frequency *
                                   i / TONE_RATE)) * 32767.f * gain * ramp);
    }
}

int main(void)
{
    puts("GDPS2 AUDIO TEST: initializing NDSP->audsrv->SPU2");
    int status = ndspInit();
    if (status) {
        printf("GDPS2 AUDIO TEST: ndspInit failed (%d)\n", status);
        return 1;
    }
    make_tone(first, 440.f, 0.23f);
    make_tone(second, 660.f, 0.18f);
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.data_pcm16 = first;
    b.data_pcm16 = second;
    a.nsamples = b.nsamples = TONE_FRAMES;
    float pan_left[12] = {1.f, 0.2f};
    float pan_right[12] = {0.2f, 1.f};

    ndspChnReset(0);
    ndspChnReset(1);
    ndspChnSetRate(0, TONE_RATE);
    ndspChnSetRate(1, TONE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
    ndspChnSetFormat(1, NDSP_FORMAT_MONO_PCM16);
    ndspChnSetMix(0, pan_left);
    ndspChnSetMix(1, pan_right);
    ndspChnWaveBufAdd(0, &a);
    ndspChnWaveBufAdd(1, &b);
    puts("GDPS2 AUDIO TEST: expected 440Hz left + 660Hz right for one second");
    int checks = 0;
    while ((a.status != NDSP_WBUF_DONE || b.status != NDSP_WBUF_DONE) &&
           checks++ < 200) {
        DelayThread(10000);
    }
    if (a.status != NDSP_WBUF_DONE || b.status != NDSP_WBUF_DONE) {
        printf("GDPS2 AUDIO TEST: playback timeout, status=%u/%u\n",
               a.status, b.status);
        ndspExit();
        return 2;
    }
    DelayThread(200000); /* Let SPU2 finish its already submitted PCM. */
    ndspExit();
    puts("GDPS2 AUDIO TEST: finished; did you hear both tones?");
    return 0;
}
