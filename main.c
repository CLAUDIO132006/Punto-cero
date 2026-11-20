/* PUNTO CERO para PS2 - main.c (juego completo en un solo archivo)
 * Une: sound.c, game.c, ps2_platform.c (aqui esta main) y net_ps2.c, con todos los .h incluidos.
 * Los datos generados (assets_data.c, models_data.c, *_irx.c) siguen aparte: los crea el Makefile.
 */

/* ################ sound.c ################ */
/* Sintesis de efectos "tacticos" (estampido grave + chasquido agudo + eco de sala)
 * y mezclador estereo con distancia y paneo. Todo se genera en tiempo de ejecucion. */
#include <math.h>
#include <string.h>
#include <stdlib.h>
/* ===== inicio sound.h ===== */
/* Sonido: sintesis de efectos y mezclador estereo (independiente de la plataforma). */
#ifndef SOUND_H
#define SOUND_H

#define SND_RATE 44100

enum {
    SND_PISTOL, SND_RIFLE, SND_SHOTGUN, SND_BOTSHOT,
    SND_HIT, SND_HEAD, SND_DEATH0, SND_DEATH1, SND_HURT,
    SND_RELOAD, SND_EMPTY, SND_STEP, SND_TICK, SND_WIN, SND_LOSE, SND_PDEATH,
    SND_BOOM, SND_PIN, SND_BOUNCE,
    SND_PLATING, SND_ARMORBREAK,
    SND_COUNT
};

/* Los sonidos viven como archivos .wav en assets/sounds (incrustados en el ejecutable por tools/embed_assets.py). */
typedef struct { const char *name; const unsigned char *data; unsigned size; } SndAsset;
extern const SndAsset snd_assets[];            /* generado: src/assets_data.c */
extern const int snd_assets_n;
extern const char *const snd_names[SND_COUNT]; /* nombre de archivo de cada efecto */

int  snd_gen(void);                                   /* carga los .wav incrustados (una vez, al inicio); devuelve cuantos efectos se cargaron */
void snd_synth(void);                                 /* sintetiza los efectos (respaldo y herramienta que crea los .wav) */
const short *snd_sample_data(int id, int *len);       /* muestras de un efecto (para exportarlas) */
void snd_music(int on);                               /* musica del menu: 1 = suena en bucle, 0 = se apaga suavemente */
int  snd_music_ready(void);
void snd_volume(int level);                           /* 0..10 */
void snd_listener(float x, float z, float yaw);       /* posicion y orientacion del jugador */
void snd_play(int id, float vol);                     /* sonido centrado */
void snd_play3d(int id, float x, float z, float vol); /* sonido en el mapa: volumen, paneo y apagado por distancia */
void snd_mix(short *out, int frames);                 /* mezcla `frames` pares L/R de 16 bits */

#endif

/* ===== fin sound.h ===== */


#define SR ((float)SND_RATE)
#define PI 3.14159265f
#define MAXLEN (SND_RATE * 2)

typedef struct { short *data; int len; } Sample;
static Sample samples[SND_COUNT];
static float work[MAXLEN], dry[MAXLEN], echo[MAXLEN];
static unsigned rngs = 0x1badf00d;

static float nz(void) { rngs ^= rngs << 13; rngs ^= rngs >> 17; rngs ^= rngs << 5; return (rngs & 0xFFFF) / 32768.0f - 1.0f; }
static float snd_clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

/* Ruido filtrado con envolvente exponencial. lpk: pasabajos (0..1, mayor = mas agudo), hpk: pasaaltos (0 = apagado). */
static void layer_noise(float *b, int n, float t0, float dur, float tau, float lpk, float hpk, float gain) {
    int s = (int)(t0 * SR), len = (int)(dur * SR);
    float lp = 0, slow = 0, env = gain, dec = expf(-1.0f / (tau * SR));
    for (int i = 0; i < len && s + i < n; i++) {
        float a = nz();
        lp += lpk * (a - lp); a = lp;
        if (hpk > 0) { slow += hpk * (a - slow); a -= slow; }
        b[s + i] += a * env; env *= dec;
    }
}
/* Golpe grave con caida de tono (el "cuerpo" del disparo). */
static void layer_thump(float *b, int n, float t0, float dur, float f0, float f1, float tau, float gain) {
    int s = (int)(t0 * SR), len = (int)(dur * SR);
    float ph = 0, env = gain, dec = expf(-1.0f / (tau * SR)), fd = expf(-1.0f / (tau * 0.45f * SR)), f = f0 - f1;
    for (int i = 0; i < len && s + i < n; i++) {
        ph += (f1 + f) / SR; f *= fd;
        b[s + i] += sinf(2 * PI * ph) * env; env *= dec;
    }
}
/* Tono puro con caida (campanilla, clic metalico). */
static void layer_ping(float *b, int n, float t0, float dur, float freq, float tau, float gain) {
    int s = (int)(t0 * SR), len = (int)(dur * SR);
    float ph = 0, env = gain, dec = expf(-1.0f / (tau * SR));
    for (int i = 0; i < len && s + i < n; i++) { ph += freq / SR; b[s + i] += sinf(2 * PI * ph) * env; env *= dec; }
}
/* Resonador de dos polos (formante). */
typedef struct { float y1, y2, g, a1, a2; } Res;
static void res_set(Res *r, float freq, float bw) {
    float rr = expf(-PI * bw / SR);
    r->g = 1 - rr; r->a1 = 2 * rr * cosf(2 * PI * freq / SR); r->a2 = -rr * rr;
}
static float res_run(Res *r, float x) {
    float y = r->g * x + r->a1 * r->y1 + r->a2 * r->y2;
    r->y2 = r->y1; r->y1 = y; return y;
}
/* Eco de sala: tres peines en paralelo. */
static void reverb(float *b, int n, float fb, float wet) {
    static const int D[3] = { 1301, 1783, 2503 };
    memcpy(dry, b, sizeof(float) * n);
    memset(echo, 0, sizeof(float) * n);
    static float comb[MAXLEN];
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < n; i++) {
            float v = dry[i] + (i >= D[k] ? comb[i - D[k]] * fb : 0);
            comb[i] = v; echo[i] += v - dry[i];
        }
    }
    for (int i = 0; i < n; i++) b[i] = dry[i] + echo[i] * wet * 0.5f;
}

static void store(int id, float *b, int n, float peak) {
    float m = 1e-6f;
    for (int i = 0; i < n; i++) { float a = fabsf(b[i]); if (a > m) m = a; }
    short *d = (short *)malloc(sizeof(short) * n);
    float k = peak * 32767.0f / m;
    /* fundido final para evitar chasquidos */
    int fade = n < 400 ? n : 400;
    for (int i = 0; i < n; i++) {
        float v = b[i] * k;
        if (i > n - fade) v *= (float)(n - i) / fade;
        d[i] = (short)snd_clampf(v, -32767, 32767);
    }
    samples[id].data = d; samples[id].len = n;
}

static void gen_gun(int id, float dur, float thumpF0, float thumpF1, float thumpTau, float bodyLp, float bodyTau, float crackTau, float crackGain, float bodyGain, float thumpGain, float fb, float wet, float peak) {
    int n = (int)(dur * SR); memset(work, 0, sizeof(float) * n);
    layer_noise(work, n, 0, 0.05f, crackTau, 0.95f, 0.18f, crackGain);      /* chasquido agudo */
    layer_noise(work, n, 0, 0.30f, bodyTau, bodyLp, 0, bodyGain);           /* cuerpo de ruido */
    layer_thump(work, n, 0, 0.35f, thumpF0, thumpF1, thumpTau, thumpGain);       /* golpe grave */
    layer_noise(work, n, 0.002f, 0.012f, 0.004f, 0.5f, 0.05f, 0.7f);        /* transitorio inicial */
    reverb(work, n, fb, wet);
    store(id, work, n, peak);
}

/* Grito / quejido: fuente glotal + dos formantes con barrido, y golpe del cuerpo al caer. */
static void gen_voice(int id, float dur, float base, float drop, float f1a, float f1b, float f2a, float f2b, float tau, int thud, float peak) {
    int n = (int)(dur * SR); memset(work, 0, sizeof(float) * n);
    Res r1 = { 0 }, r2 = { 0 }; float ph = 0, vib = 0, lpn = 0;
    float f1 = f1a, f2 = f2a;
    for (int i = 0; i < n; i++) {
        float t = i / SR, u = snd_clampf(t / (dur * 0.7f), 0, 1);
        if ((i & 31) == 0) { vib = sinf(2 * PI * 6.0f * t) * 0.03f; f1 = f1a + (f1b - f1a) * u; f2 = f2a + (f2b - f2a) * u; res_set(&r1, f1, 120); res_set(&r2, f2, 160); }
        float f0 = base * (1.0f - drop * u) * (1.0f + vib);
        ph += f0 / SR; float saw = 2 * (ph - floorf(ph)) - 1;
        lpn += 0.3f * (nz() - lpn);
        float src = saw * 0.9f + lpn * 0.35f;
        float att = snd_clampf(t / 0.012f, 0, 1), env = att * expf(-t / tau);
        float y = (res_run(&r1, src) * 1.0f + res_run(&r2, src) * 0.65f) * env;
        work[i] += y * 6.0f;
    }
    if (thud) {
        layer_noise(work, n, 0.30f, 0.20f, 0.07f, 0.12f, 0, 0.55f);
        layer_thump(work, n, 0.30f, 0.25f, 95, 45, 0.09f, 0.9f);
    }
    reverb(work, n, 0.35f, 0.5f);
    store(id, work, n, peak);
}

static void gen_click(int id, float f, float peak) {
    int n = (int)(0.06f * SR); memset(work, 0, sizeof(float) * n);
    layer_noise(work, n, 0, 0.03f, 0.004f, 0.8f, 0.2f, 1.0f);
    layer_ping(work, n, 0, 0.05f, f, 0.01f, 0.6f);
    store(id, work, n, peak);
}

void snd_synth(void) {
    /* Pistola (tipo USP/Glock): seco, nitido, cola corta. */
    gen_gun(SND_PISTOL, 0.50f, 190, 70, 0.060f, 0.55f, 0.050f, 0.012f, 1.0f, 0.55f, 0.16f, 0.42f, 0.8f, 0.85f);
    /* Rifle (tipo AK-47): cuerpo mas grueso, mucho medio grave, eco de sala. */
    gen_gun(SND_RIFLE, 0.70f, 150, 50, 0.110f, 0.40f, 0.095f, 0.016f, 1.0f, 0.85f, 0.26f, 0.50f, 1.0f, 0.95f);
    /* Escopeta: estampido enorme y cola larga. */
    gen_gun(SND_SHOTGUN, 1.10f, 120, 36, 0.200f, 0.28f, 0.190f, 0.022f, 0.9f, 1.20f, 0.50f, 0.58f, 1.2f, 1.00f);
    /* Disparo lejano de bot: el rifle con otro caracter (mas grave y redondo). */
    gen_gun(SND_BOTSHOT, 0.65f, 135, 45, 0.100f, 0.30f, 0.085f, 0.014f, 0.8f, 0.9f, 0.30f, 0.52f, 1.1f, 0.90f);
    /* Impacto en cuerpo: golpe sordo. */
    { int n = (int)(0.14f * SR); memset(work, 0, sizeof(float) * n);
      layer_noise(work, n, 0, 0.05f, 0.012f, 0.35f, 0, 0.8f); layer_thump(work, n, 0, 0.12f, 210, 80, 0.035f, 1.0f); store(SND_HIT, work, n, 0.6f); }
    /* Impacto en la cabeza: golpe + "tink" metalico (casco). */
    { int n = (int)(0.30f * SR); memset(work, 0, sizeof(float) * n);
      layer_noise(work, n, 0, 0.03f, 0.008f, 0.7f, 0.1f, 0.7f); layer_thump(work, n, 0, 0.10f, 240, 90, 0.03f, 0.8f);
      layer_ping(work, n, 0, 0.28f, 2350, 0.07f, 0.9f); layer_ping(work, n, 0, 0.20f, 3560, 0.045f, 0.4f); store(SND_HEAD, work, n, 0.8f); }
    /* Muerte de bot: dos voces distintas. */
    gen_voice(SND_DEATH0, 0.95f, 150, 0.45f, 700, 420, 1150, 800, 0.30f, 1, 0.95f);
    gen_voice(SND_DEATH1, 0.85f, 195, 0.55f, 620, 380, 1300, 900, 0.26f, 1, 0.95f);
    /* El jugador recibe dano. */
    gen_voice(SND_HURT, 0.30f, 130, 0.30f, 600, 480, 1000, 900, 0.12f, 0, 0.8f);
    /* Recarga: cargador fuera, cargador dentro, cerrojo. */
    { int n = (int)(1.1f * SR); memset(work, 0, sizeof(float) * n);
      float T[3] = { 0.00f, 0.55f, 0.90f }, F[3] = { 1700, 2100, 1300 };
      for (int k = 0; k < 3; k++) { layer_noise(work, n, T[k], 0.04f, 0.006f, 0.7f, 0.15f, 0.9f); layer_ping(work, n, T[k], 0.06f, F[k], 0.012f, 0.5f); layer_thump(work, n, T[k], 0.05f, 260, 120, 0.015f, 0.4f); }
      store(SND_RELOAD, work, n, 0.55f); }
    gen_click(SND_EMPTY, 1500, 0.5f);
    /* Paso: golpe seco y sordo. */
    { int n = (int)(0.10f * SR); memset(work, 0, sizeof(float) * n);
      layer_noise(work, n, 0, 0.06f, 0.018f, 0.16f, 0, 0.9f); layer_thump(work, n, 0, 0.08f, 110, 60, 0.025f, 0.6f); store(SND_STEP, work, n, 0.4f); }
    gen_click(SND_TICK, 2400, 0.35f);
    /* Ronda ganada / perdida. */
    { int n = (int)(0.9f * SR); memset(work, 0, sizeof(float) * n);
      float N[3] = { 523.25f, 659.25f, 783.99f };
      for (int k = 0; k < 3; k++) layer_ping(work, n, k * 0.12f, 0.7f, N[k], 0.25f, 0.7f);
      store(SND_WIN, work, n, 0.5f); }
    { int n = (int)(1.0f * SR); memset(work, 0, sizeof(float) * n);
      layer_ping(work, n, 0.00f, 0.6f, 330, 0.22f, 0.7f); layer_ping(work, n, 0.18f, 0.8f, 247, 0.30f, 0.7f); layer_ping(work, n, 0.18f, 0.8f, 123, 0.30f, 0.5f);
      store(SND_LOSE, work, n, 0.5f); }
    /* Muerte del jugador: golpe sordo, quejido grave que cae y aire que se apaga. */
    { int n = (int)(1.3f * SR); memset(work, 0, sizeof(float) * n);
      layer_thump(work, n, 0.00f, 0.40f, 150, 38, 0.10f, 1.0f);
      layer_noise(work, n, 0.00f, 0.20f, 0.04f, 0.10f, 0.0f, 0.8f);
      { Res r1, r2; memset(&r1, 0, sizeof r1); memset(&r2, 0, sizeof r2); res_set(&r1, 420, 160); res_set(&r2, 900, 260);
        int s0 = (int)(0.05f * SR), len = (int)(0.9f * SR); float env = 1.0f, dec = expf(-1.0f / (0.30f * SR)), ph = 0, f = 150;
        for (int i = 0; i < len && s0 + i < n; i++) { f *= 0.99995f; ph += f / SR; float src = (ph - floorf(ph)) * 2 - 1; src += nz() * 0.25f;
          work[s0 + i] += (res_run(&r1, src) * 0.9f + res_run(&r2, src) * 0.4f) * env * 0.9f; env *= dec; } }
      layer_noise(work, n, 0.35f, 0.9f, 0.30f, 0.05f, 0.0f, 0.25f);
      reverb(work, n, 0.35f, 0.5f);
      store(SND_PDEATH, work, n, 0.7f); }
    /* Explosion de granada: estallido grave, onda de choque, escombros y cola larga con eco. */
    { int n = (int)(1.9f * SR); memset(work, 0, sizeof(float) * n);
      layer_noise(work, n, 0, 0.10f, 0.020f, 0.95f, 0.10f, 1.0f);
      layer_noise(work, n, 0, 0.90f, 0.220f, 0.22f, 0, 1.3f);
      layer_thump(work, n, 0, 0.90f, 95, 28, 0.30f, 1.6f);
      layer_noise(work, n, 0.18f, 0.80f, 0.25f, 0.10f, 0, 0.6f);
      layer_noise(work, n, 0.45f, 0.35f, 0.08f, 0.55f, 0.2f, 0.25f);     /* escombros */
      reverb(work, n, 0.62f, 1.4f);
      store(SND_BOOM, work, n, 1.0f); }
    /* Seguro de granada: clic metalico y "ping" del anillo. */
    { int n = (int)(0.35f * SR); memset(work, 0, sizeof(float) * n);
      layer_noise(work, n, 0, 0.03f, 0.005f, 0.8f, 0.2f, 0.9f); layer_ping(work, n, 0.00f, 0.05f, 1900, 0.012f, 0.6f);
      layer_ping(work, n, 0.10f, 0.22f, 3300, 0.06f, 0.5f); layer_thump(work, n, 0.00f, 0.05f, 220, 110, 0.015f, 0.4f);
      store(SND_PIN, work, n, 0.55f); }
    /* Rebote de granada: golpe metalico sordo. */
    { int n = (int)(0.18f * SR); memset(work, 0, sizeof(float) * n);
      layer_noise(work, n, 0, 0.05f, 0.012f, 0.5f, 0.1f, 0.7f); layer_thump(work, n, 0, 0.10f, 300, 120, 0.03f, 0.8f);
      layer_ping(work, n, 0, 0.14f, 1200, 0.04f, 0.4f); store(SND_BOUNCE, work, n, 0.5f); }
}

const char *const snd_names[SND_COUNT] = {
    "pistola.wav", "rifle.wav", "escopeta.wav", "disparo_bot.wav",
    "impacto.wav", "impacto_cabeza.wav", "muerte_bot1.wav", "muerte_bot2.wav", "herido.wav",
    "recarga.wav", "sin_balas.wav", "paso.wav", "tic_menu.wav", "ronda_ganada.wav", "ronda_perdida.wav", "muerte_jugador.wav",
    "explosion.wav", "granada_seguro.wav", "granada_rebote.wav",
    "colocar_placa.wav", "escudo_roto.wav"
};

const short *snd_sample_data(int id, int *len) { if (id < 0 || id >= SND_COUNT || !samples[id].data) return 0; *len = samples[id].len; return samples[id].data; }

/* ---------- Carga de archivos WAV (PCM 16 bits mono) ---------- */
static unsigned snd_rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static unsigned snd_rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
/* Devuelve el puntero a las muestras (dentro del propio archivo, sin copiar) o 0 si el WAV no es valido. */
static const short *wav_parse(const unsigned char *d, unsigned size, int *len, int *rate) {
    if (size < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return 0;
    unsigned p = 12; int ok = 0, ch = 0, bits = 0;
    while (p + 8 <= size) {
        unsigned csz = snd_rd32(d + p + 4);
        if (!memcmp(d + p, "fmt ", 4) && csz >= 16) { ok = snd_rd16(d + p + 8) == 1; ch = (int)snd_rd16(d + p + 10); *rate = (int)snd_rd32(d + p + 12); bits = (int)snd_rd16(d + p + 22); }
        else if (!memcmp(d + p, "data", 4)) {
            if (!ok || ch != 1 || bits != 16) return 0;
            if (p + 8 + csz > size) csz = size - p - 8;
            *len = (int)(csz / 2); return (const short *)(d + p + 8);
        }
        p += 8 + csz + (csz & 1);
    }
    return 0;
}
static const SndAsset *find_asset(const char *name) {
    for (int i = 0; i < snd_assets_n; i++) if (!strcmp(snd_assets[i].name, name)) return &snd_assets[i];
    return 0;
}

static const short *musData; static int musLen, musRate = 22050;
static float musPos, musGain; static int musOn;
int snd_music_ready(void) { return musData != 0; }
void snd_music(int on) { musOn = on; if (on && musGain <= 0.0001f) musPos = 0; }

/* ---------- Disparo de la pistola VP70 ----------
 * Recorte limpio de pv70.mp3 (un solo disparo, sin la voz): 0,52 s, mono, 44100 Hz, 16 bits, incrustado en base64.
 * Reemplaza al sonido SND_PISTOL (pistola.wav) del jugador y de los bots. */
#define PV70_N 22932
static short pv70Buf[PV70_N];
static const char PV70_B64[] =
"AAADAAYACgAPABIAFgAbABwAHAAkACYAKQAvAC8AMAAyADcAPABCAEoATgBKAFoAYABWAF4AWQBiAG4AZwBnAHgAeABvAHQAjACH"
"AIoAnQCMAJcAiwCXAIwAggCWAK8AtQCUAK4ApADDAKwAvQDDAKUAtQClALsAnQC5AKAAqgCPAJ8A0wCuALMAyADRAKgA7AC7AOYA"
"zQDlAPUA0AAGAZkA9AD8ABQBxwDhAN8A7gD5AKQACwGsAAcB1wDjAMcA5ADxAOYANQHjAAcBugD6AMcA+ADyADAB6wCnAKsAcAD4"
"AEoAuAB6ALcAfgBzAIQAOwCSAEMAtAAtANEAfQCjAIkAZgC6AJYA3gCGAAABUgDgAGoAwQC/AJ8AzAB1AAQBMADZADsApwCAAM8A"
"jwCFAOoATADhAGIAJAHv/5YAGgBhAIYAfgDFAAAA+QD9/4AB3P/pANf/jABiAF4ArAKuA80XqxGw/Ez9dv4RAT8E+weMDsgWdh77"
"IUIeVAu323DEnMlSy5nYz+mxFAM2Lzf0M0YuSyWsFgv/6dcGxofGjcWtx/LHJMnryazKFMvXy2/M1M+m0fjljO5mzwjI8MyjzqXV"
"ptmY5KzxOwizI4JJLFP9OVBCejDyB+IIyBKQLtNay2x2WYFTSl8yVq9FhiZRF1UjKhCn8NHpjugN2Na9Qq2Xqomw+bImrfKraarp"
"rdm+Vsr2wR2rv5/xo8SmEql/sAfBrtiK7LQCTBoSJ4YztkMcWUZk21x+TLAy+DEpM6wHWNhIyt3Q9tAuyhTFcs5w4+H4+xCaGSkh"
"MCtPJL0bLhYSF4cT0/Mv0oHIY8WNwpm9prlVv3vR8eTT0wa+7cEzxn7ONNNt1Hvfb/fNBdcKWCrRQmxAcTg5LYYZVfqE6CrhtMsh"
"uta+rspU2FPmDevP6Lflpe8T9Z3mz9644l7cXsfnuKC6ystQ2W7WJ9BJznvY2d1o1qPLdMYXzUrMWMh00fTn8fNq8ZMBuRA7ERAM"
"OQVYBkUE4PrF5pHctN/F2vTWONR12AfeJ9+n4P3jg+9c+JT3AfDa7qryN/35Db0KWPxd+ET+1ATMCNIEWP+DBXARhRZfELsVjCL4"
"IzEjbiNoJl0poyZkJ9At7y8EM8Iy7C3UM2g63TYxKSAcPyCoKZ8p9SiXK1MlihZlDYYJ3gYsBG7/WgK+DWcV3RTeEx8XWBkNGika"
"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"
"T/nM+LH4Gfkd+tD7av1P/rn+2f67/qv+m/6Y/l7/5gASAkECDALoAQACXAKNAk8C1wFlAewASwCp/2b/uv+cAI8BRwIcAwkEjwRC"
"BGQDhwLPAXABkQGfAQwBcQCOABwBTQEhARsBWwGPAZYBsQETAtoCwgNkBOUEcgXQBcgF1AVdBt4GhAZ3BX0EwgMMAxUCBwE8ALv/"
"K/+H/i/+kv6z/+YAmQEaAgID6gNIBGIEnASqBMIDHQKYAHb/yP6i/qr+j/5a/lL+uf4i/yL/6v7u/uj+Hv7k/G38Gf3k/eP9J/0s"
"/ED7ifre+fz4XPii+NP5L/sy/FL90P4MAMsAfwEkAooClAJKAqwBsQCL/8r+v/4f/8v/uQCsAVgCpALwAqUDewSgBOAD2AJEAj4C"
"lQIGA2IDmANuAxkDHQNyA4IDQQMSA64CngEtAF7/jv/z/8//ov/z/4oANgHIASgCYAKVAqAC6gFDAH3+XP2k/ML7mfrm+Sb6v/rT"
"+kj6g/mw+Af4lPcA90L2E/aq9l73svfd93r4oPm8+mz76/sr/Pn7rfuF+177JvsE+/H6v/qL+uL61fuo/An9cf1d/r//WwHNAr8D"
"LQRFBIoESgWFBgUIJwllCf0IPAh5BwQHawatBUcFFAWwBGAEgATcBBgFPgWuBXAGHwc3B7cG1wWwBG0DIQL5AB8AkP8Y/8X+1f4l"
"/3X/yP8nAGwAwAAOAQoB0wCYAEUA5f+w/6b/ef8K/7L+i/57/n7+nf7q/lf/uv/b/9b/p/9G/w7/K/8w/8L+m/4s/+P/aADlAGoB"
"8gGDAtgCwwJAAnEBdQBm/1X+L/1B/Nn7vfuc+5H7+Psq/d3+QgDTALkAngDHANMAawDV/8f/fwBGAWEBLQFyAVcCOAOiA8oDKQS6"
"BB0FSQUzBekEpASCBJoE8QRpBesFVQYwBqAFHAXABHgEHgSqAz4DKwO+A9gEnwWrBWoFaAUHBgcHgAcUBxMGggSHAt4AKgAqAA8A"
"m//1/lT+2f2S/Z39SP5U/yYAngC3AGYA3/9P/3z+UP0X/PL64vn7+GD4K/gz+Pv3SPeD9iL2OfZJ9uT1DvUK9DHzm/JT8p3yT/PX"
"8zX07PRP9hv4C/rH+/z8yP2H/n3/lgCBASwC/QJIBLkFzgZjB6IHfwc+B2cH1QfrB4gHtwacBbsESwRMBIUEowSPBGgEmQTvBPcE"
"tAQ9BL0DSwPUAvwBoQBO/0f+Uf1U/IL7zvoq+vH5Uvr9+mT7ovuc/HD+QAAtAW4BhQFcAbQAd/8C/pX8UftJ+n75z/j690L35PYS"
"9/P3bPkQ+4T8CP6O//gAbAKqA38EAQWKBXMGoQfNCI8JjAntCCwIrQc4B7AGAwa+BGIDxALrAnsD+wNBBEcEuwO/AtYBQAG8ALH/"
"gv6G/Yv8yPug+0D8Sv10/lT/uv/q/+n/Wf8p/hb9Tvxr+1r6L/lE+Oj3EPhH+Gj4Evlr+g78gf2W/on/SgDlAKMBpALQA9MEQQXv"
"BGUEBQTVA8QD0wP4A+QD7wMFBNkDmANoA3wDrAP4AwkEkAMsAzMDWwNfAwkDhQJFAlECbwJzAkMC0AFJAcQABQAe/3b+Iv7k/bv9"
"qv13/SD9/vzk/K/8xPwL/T79TP06/WH9Pv5p/18ABAEqAVMBvgEXAh4CxgFgARgBEAFAAVoBNAHzAMgAqABTAPT/2f/b/9v/zf+j"
"/1H/+/7f/vj+B/8t/73/YwCqAIEAJgC7/33/bv85/wX/Af84/2v/V/8q/+7+kf40/hP+QP67/lT/vv/G/2D/2v6C/lr+bP7W/m7/"
"+f99ACEB+wHVAhoDyAJdAhMC2QFRAYwAv//x/hr+Vv0U/Tz9bv3K/ar+q/9hAPgAVwFjAWMBvgEMArcB6wBUAI0ARwGTAQEBOADF"
"/1n/Af+//gb+9fxE/Lr7B/vc+oD7gvx6/Vz+7P4M/0b/dP9E/8z+Lf7p/Rr+Zf4x/rL9b/1n/ab9KP69/ur+8f5W/+//lAAKATAB"
"EwEmAbYBaQIaA3cDSQP1AucCSwPYAzoEcQRyBCAEYwM1AgMBPgCC/6D+yf0r/f78Jf25/aT+Uv9V//j+Av9e/3L//P4A/sn8+vu/"
"+8b7/vt//HP9ff6F/6QAegHnAcYBRgHKAEMArf8i//L+EP8B//H+MP98/4b/ev+i/+b/HAAZANL/Rv+t/mL+U/5b/jL+2f2t/cf9"
"Cf4W/hr+Of4l/hX+Vv7y/qr/zv9O/37+iv3M/Jj8JP0C/pL++f6V/zkATQDX/3b//v73/ff8p/z2/Ij9Tv5e/5AA5AE/A0YEugSs"
"BDwEzAOwA2sDuALzAUkBgABs/4f+ev40/y0A/gB8Ae0BlAJTA+wDRQRQBAsEoQP/AiECWAGvADEAAQDs//7/ggBlAewBhgHRAH8A"
"jQDKAB0BXgFGAeUAdwANANT/zP/R/xkAbwC5ABUBigEdAlEC8QEzAVMAj/8D/5z+Af5s/SD9Ev1g/Sr+YP9rALwAHwBk/0j/l//r"
"/+P/uP+4/9P/5P/0/1AA8wB7AawBqgGvAeIBDgK4ARwBiwAcAL3/Iv9Z/qL9ov1G/t3+SP9t/2r/Uv90/y0A/wBfAfsARAALAB0A"
"+P+m/5X/1P8QAFQA1gCKARgC2AHfANj//f5p/u39dv0r/Qn9K/16/Qb+2v6m/z0AYQBQABEAZP+B/sH9e/1j/Wr9lf13/Uf9Xf0n"
"/oz/uQB6AegB8AHgARIChAIYA30DbgPjAg8CPQG/AKsAyQDPAKgAdQBUAGEAcABoAA0Aav/q/pL+Rf7+/Rn+kv5M/xEAhACbAGgA"
"EADS/77/ov+u/ykApADNALkAgABhAIwAGAHaATACxwELAa8AuACcADsAwv8x/6b+av5x/oz+5v7W/wwBBwKOAusCZwOhA6IDOgOW"
"AvkBTAEYAQ8BAgEKAfYA+wD0ABMBVgGHAd8B4gGPAWIBigH1AeYBcgEDAboA4gBFAbIBCgIFAtUBnwF1AWsBTgEmAdYAUgDa/4z/"
"nv/e/+P/f/8L//T+L/9r/1v/Fv/K/q7+Av+r/y4AOQAgABwAQQCBAGwADACR/z3/Ev/a/rb+cP40/hP+J/6R/gr/z/98AMcAyACd"
"ANUAZQE/AgwDRgMDA44CLwIAAvAB6wHQAXYB+AB8AEMAhAD0AHcB+gFvAskC8QL1At4C9QL/ApkC5wEnAXsAAwDD/6P/tf+9/6T/"
"eP9B/z3/K//T/mT+F/7r/Zv9G/2B/C78dfzl/DD9i/3H/Z/9L/2f/BH84fs8/Nv8Wf3K/Sb+Fv7+/d/9l/0n/Yz8NfwA/OP7vPus"
"+/v7kPw+/cb9nf6S/z8AxAAkAWgBpQHbAdcBlQE1AfUAywB1APn/bv8h/zf/lP/Z/wAABwCu/xD/Tv69/X79gv39/Zv+Mv/o/5sA"
"KgFmAVoBQwFIAVYBRAEOAY4A1f8q/7X+jv7I/jj/ov/7/zIAOwAfAAAA6P/T//D/EgDN/0r/8f4S/3b/3P9EAIoA0gBHAc0BSwKh"
"Aq4CbwI3Ai0COwIfArsBgwFvAYsB0wEPAnYCuwLKAsUCywL5AtUCjgI9Aq4BLAGzAFMALwAXAAQA1f99/xL/sf54/kz+Jf4E/h7+"
"RP5h/oL+bv5U/gj+nv0p/b/8mfyi/N78Bv0H/dv8ufyZ/J/8+/w//T794fyP/HP8hfy+/Nn85fzu/Dv96P2W/jD/mP+O/4r/nf+u"
"/9T/CAB2AK8AnwCUAKsA1wDzANsAlwCHAMMAYwEwArMC3wL9Ah0D3gJHAsgBjgF2AW8BigHIAQcCMAJHAk4CYAJUAvoBZwHXAFEA"
"mf/j/n/+cf7h/n3/2v/Z/6H/rP/s/ykAeADvAFUBaQFYATEBJwF4Af8BdgKiAmcC4QE4AdsA2wD7ACcBDAHNAIAASQBEAE4AggDR"
"AC0BoQEjAm8CbAIRApsBQAHKAGAA6/+C/0z/W//W/2kA0gBIAfsBhAK9ApgCKALEAYgBoQGcATABjADj/4D/TP8o/9z+d/4Y/sD9"
"jP0l/Yb8LPxQ/Pj84f23/kf/pv/O/6r/f/9X/wf/4/7z/s/+bf4R/gD+N/6N/uT+C//o/oj+SP5V/o/+7/5M/4X/q/9//x//2P6y"
"/rH+qP6M/k7+Af6o/UT9Jv1s/Q7+mf7u/hn/Gf8i/zH/ef/W//z/9v/Y/6b/YP8N/+H+MP95/zb/mv4B/ov9+PyS/MP8b/1R/hr/"
"sf81AMUAdQEjAqcC5wK3AisCYQG/AKkA+QATAaYAagDdAKUBLQJhAn8CqAIRA44D3gP7AwME8gO1A3cDZwNpAzcD2wKDAj4CFQIx"
"Al4CVQIJAowBJAHpALIAewBiAF8AiwDCANsAxQBSANb/ef9T/0H/MP83/yH///78/kf/4/9OAFAA6f9l/zD/EP8U/w//3f7d/gX/"
"Wf/D/wwAJwAkABIAGwA8AFQAQwDp/7b/qP/N/w0ALQAYAMD/aP8W//X+4v7P/mr+u/1a/Ub9r/1a/uv+F/8B/wP/K/97/7f/y/+n"
"/5P/xv/9//X/zv/H/8b/lf8//x3/I/8j/9f+a/5Y/pD+tv6X/nP+bP6k/gz/cP/K/yIAgQCmAHUAFgCr/0z/+/6J/tv9OP3w/Nr8"
"6fw0/cj9df7J/iH/eP+X/7P/sf+1/6D/c/9k/0z/G/+0/jT++v0+/sb+Hf8Y/wL/ZP/3/3QA7AAnAWABfAFWAScB8AC+AJcAlACi"
"AMAA+wBIAa4BJgKDArICywKiAkwC7AGIATgBAwH4ACEBSQE7ASUBMQFjAX0BdAFYASUByQBEAPD/6/83AIkA2AAwAXsBzAEAAhgC"
"4QF8AUgBKAHgAHoAVgBdAGQAYgBhAFgAagC2AK4APADG/3v/Zv9x/23/Tv8q/wD/9v7s/sv+uP7P/hf/Wv9X//b+p/7n/mf/9/+A"
"AMEA6ADNAJYAxQAkAXYBiQFiATAB4QCZAJEAqABoAAQAGADJAIAB2AHYAaIBqQHMAdUBzQG7AaABTgHqAJgAYgCJAA0BmgH5ATkC"
"ewJ4AkUCKAK7AQUBUQC//0j/Bv9L/+f/ggDxABQBzgBMAO//3f8RAEkAMQCy/yf/2v7z/k//l/+t/5//wv/q/+H/tv+B/yv/nv4z"
"/hb+Qv6R/sf+5v7S/qf+y/4t/4v/1f/l/8v/w/+9/3v/J//H/oL+lf7a/jP/h//S//n/JAA8AFMAawAkAMX/Pf/W/rb+s/7e/s/+"
"s/7e/k3/0v9JAKsAxAClAHwASQA5AGQApADsAAoB8ADLAL4AogAnAGz/sv4e/vP9LP6s/nH/BQBYAHwAkgCaAGIAJwDY/5r/hv+8"
"/yQAagB0AEwASwA+ACwAOgBiAJsAkABUAAAA1f/9/0oAgACXAN4AYQGnAbMBqwGuAfIBBgKnAUUBZQHLAScCYAJRAkECHALLAU8B"
"1QCdALkAxgB7AB8A4v/K/8D/r/+i/8b/AQA9AG8AaQBpAJEAmwAbADv/Vf7B/Zn9lf2A/YX9I/4K/wYA5QBtAa0BoAGAAXEBnQG+"
"AaQBKgF4AP//mP9M/yf/Hv8M/+T+uP54/lT+Tv5s/nj+Qv4j/i7+VP57/qf+6/5I/7X/1v/H/8v/1v/u/+7/wf9+/0D/Lv8u/yr/"
"Fv/S/pr+dv6C/t/+U/+d/4L/S/9e/4X/e/9C//T+z/7Q/gD/U/+e/+X/CgABAML/Zf8C/8j+2P4M/1r/jv93/y7/7P7j/hD/X/+v"
"/9P/t/9W//L+oP5L/hj+Dv4i/kL+lv4K/2b/qP+j/6T/r/+D/y//1v6w/sr+9P7S/nD+Jf4l/oj+BP9G/1z/bv92/2D/TP8Y/+b+"
"v/6f/rj+zf4O/3P/+/+tACwBmAG8AYQBFgGGAPD/aP8B/8H+tf7R/gj/Y//W/yMAdQDGAOIApgAqANT/l/9f/xn/3P74/nv/JwDO"
"AG4B8gEyAmoCugLxAs0CSwLaAXoBMgH+AIsAJAAcACcALACKADwB7wEWAuEBtgGQAWoBQgFCAVYBngECAkUCPALWASoBlgBjABEA"
"hP/p/nL+C/7S/db95/0C/gX+K/5Z/qX+Dv9Z/4T/Xv8O/9H+x/7x/iL/Pv9x/5n/uP/Y/9r/uP+w/wcAVwCoANwAHwGJAcQBuwFz"
"AUABEQHwAN8AuACIADMA9v8cAFkAgACkANQACQHtAKMAhADAABMBNAEQAcgAuwC3AKEAdQB4AJUAXAAuAEoAbQBZAD4ATgBqAGkA"
"WgA6ABQA3f+O/1r/cf/P/0EAuQDZAMYAiQBAADoANQAxABgAAgD9/wgABADX/9b/6v8gAGgAqgDxAPAAhADZ/0r/5f7H/ir/sv9I"
"AK8AfQAcAMf/ev8e/8f+pv5//n/+v/7h/u3+Cf9N/7P/DQCJABwBXQElAZAAxf8z/y7/iv8sALkAIQEzARcBFQEAAd0AsQChAI8A"
"lQC9AAgBcAGnAbsBowF8AWIBMwERATgBeAGiAcMB7gEMAggCDgL6AcYBgwEaAYIADQDI/3L/+f59/jL+5f29/d79Tv6x/vX+LP8k"
"/w3/1P6w/rX+6f4p/1P/p/8GADMAIgAYADwAlgDdAPIAGgFlAYsBcQEiAa0ARgAJAP7/GwBRAGYAcQCHAKIAwgC9AKoAogC7AAYB"
"NAEtAREB2gDZAAwBTQFqASAB6ADZAOEA+gAeAUQBQAEWAfkA8QDFAJYAWQA8ABwAnv8b/7n+lf6v/u3+Gv81/z3/Dv/X/t7+G/9f"
"/4L/jf+t/7D/wP/k/+z/2v+A/wb/sv6t/vj+VP9+/4X/Vv8F/57+Lf4S/ib+cP71/lD/YP8w/yf/dP/N/wcA9f/I/8z/CgBqAJ0A"
"uQCuAIEAWgA9ADYAQQBXAF8AXwAyAAYADwApAGEAmwDKAOwAIgFUAXgBkgFyAS4B1gBpABAAEAAgAAwAGQAyAEAAOgA6AGIAmQC9"
"ANAA4gDYANIAzACCAEgAPgA1AN7/Rf/q/ur+Jf9l/2n/Df+F/jn+M/43/vP9d/07/VL9bv12/ZP93v01/of+8f5y/73/yP+4/7X/"
"tf+Q/4j/df9h/27/Wf8x/+3+ov6M/rz+Av8l/yL/Gf8O/x3/Sf9e/1b/Ff/B/oX+Uf5a/sD+Yf/s/yQALwBLAHsApwDEAMMAqwCS"
"AGwAZgCGAJoAjwCLALYA6gARAQ0BFgEXAfUA7gC3AFIA3P+x////ZgCxALMAjgBiACcA+P/F/5D/j/+l/+T/LAAnAOP/c//1/lX+"
"uP1c/Wn93P1V/rH+3/7I/pn+kf6j/rP+yf7g/uz+0f5o/hj+JP5//iP/5f+DAKEAfgByAHUAPgDc/4H/LP/w/tX+JP+p/zUAoQDb"
"ACIBdQG7AewBMgJ1ApoCYgLcAUUBugBXANf/hf9p/2X/ff+w/wYAVgCJALIA6wArAUoBTQF2AXcBJQHFAIsAeQBpAG8AfgB9ADcA"
"2P+f/5j/mv9f/yj/If9I/1z/av9+/4v/qP/g/z8AjQCdAH8AXwBRADEA9////0IAdgCqANcA+gD6AM0AfQAeAMj/ZP8Q/+/+/v4P"
"//f+6P7X/pz+XP45/mH+4/6r/0MAagBTAA4A8v8fAFIAZgAhAJD/KP/v/tf+vv57/nz+5v6L/xkAkQAtAbIB5wHMAaQB7AGnAiAD"
"QAM3AzgDUgMdA7QCPwIEAukB1QHJAZABZwFJAQsB0QCdAHYAjwCnAJEAdQBfAGEAiwCfAJwAoACYAJUAoADdAAQB0ABtACUAPQBn"
"AIwAfABQAFAAVAB0AJkAtACzAKcAnwB0AFcAZACUALoAxwD4AEcBowHZAa8BYgE4AV0BigF+AScBkAASALD/Zf8s/xf/Ef/3/g//"
"U/+//y8AqQAXAVUBggGLAYYBZQEZAaoATQARAKb/Sv8c/w7/Lv9S/1v/YP+W/+P/CQASAA4A3f98/zL/PP+2/1wA5AA5AWgBmwG4"
"AY8BSAHTAEUA7v/a/+P/5f/u//P/0f+f/5//9f+AAPMAAQGEALL/z/4B/lr94PzG/BL9kf0J/k/+Wv5L/nb+lv5w/i/+C/42/m3+"
"pf7N/pr+Mv7m/fT9eP4q/6D/3/8YAIsAAwFzAeoBJgJCAh0C1wHRAeEBzwF2Ad8AUgD9/wsANwAQAMT/VP8N//X+3P7R/qz+x/4N"
"/0X/if/b/yYABwCj/03/6P57/if+uv1d/TL9EP3S/I38ffyl/BL9ff26/fX9MP6H/sv+8P4j/2n/uf+s/zT/vf5t/mz+o/7p/j3/"
"i//n//L/yP/F/+D/PgDCAEEBiwGyAdIB0wHHAbsBngFdARIB4QDdAAEBIwFDAUoBRQFVAUoBLgFCAYcB8wFDAjUC5QGFAUkBHQHr"
"AKsAVgDg/zf/jf4J/rT9mf2x/dH97P0m/mj+lv6Y/n7+dP5t/n7+ef5J/gT+qP1h/TX9M/1i/aH97f08/mL+NP7r/dr9B/5O/rD+"
"IP+O//H/AwDo/+j/JACfAAgBUwGOAbYB0wGwAUcB2AB1ADgALwBZAJwA0wAQAUQBZAF1AZEBywExArsCHgMoA+gCdAL1AbgBtwHZ"
"Ad0BsQGMAU4BKAH4AJ4AWQAeAPb/vv+M/37/bP+I/5D/XP8m/9D+p/59/iD+3/2p/YT9Z/1P/U79hP3I/SX+rP4f/4f/mP+A/4T/"
"jP+c/4z/rP/h//3/BwDM/3v/Gf/i/uL+uv6g/rj+EP9s/7T/DABkAMoAHAFYAYsBrAG7AbYBpgF0AS8B6wDJANcA/wApAUkBVwEn"
"AeMAnQBNAPD/ov+h/8//HABZAH8AqACjAIEAZABaAHYAggB4AHAAbABwAG0AdACRALQA1ADmAMwAkgBSACkAKwAbAPj/2P+y/5L/"
"f/+G/5P/g/9T/zj/aP+y/+f/+P/s/7z/dP9z/5n/mf9Z/+X+gP5F/jX+Lv45/mP+mP7w/lH/m//B/9n/6/8AADMAbwCOAJYApwC/"
"APQAFAEeAScBAAHmAM4ArACWAG8AbACGAJ4AuADQAAgBOgFHAV4BTgEDAboAgwBRACEA/v/U/6X/eP92/3b/df+S/8H/HABTAEkA"
"BwDE/47/XP9l/2X/b/9x/3b/pP/N/9r/xv/F/9L/9f8jAEkAWwBYAGcAjQCbAKAAtQC9APsATAGUAc8BxQG3AZ8BbwE9ARMBBAEU"
"AScBNAEzAR0BKQEvARsBBQHhALkAjABHAO7/if9C/z//av+l/8X/3//1/9//wf+d/27/Kv/v/sr+m/5r/kf+Of4p/h/+If4q/jX+"
"N/5X/o/+5f5y/xcAxwBUAaEB3wH+AQQC/wHfAcQBvgHWAfsBHgIvAhMC3wGSAUsB+wCkAFMA8P/G/8n/7f8rAG0AsQDDAMUAzADj"
"AN4AjwD5/zD/k/4v/vb90/3G/bf9uv3b/ef9G/50/tL+Iv9p/6L/ov+s/7D/mP+K/5j/r/+g/5H/jf9x/yr/9P4S/0H/cf+D/2//"
"d/+Z/+j/OgCbAA0BXQGpAfIBMQJiAnECfQKoAt0C4gK0AogCbgJ2ApkCoAJkAu0BgQFlAYUBdgE4AQEB1ACoAHIAegCdAKgAjQA+"
"AOf/fv80/wP/0v6y/p7+vP7X/ub++f7x/tX+i/43/gv+/v0b/lL+hf69/vf+Hv8r/zL/Uf+C/7r/5P8kAKsANwGfAcYBwAHCAdQB"
"6gH8AQwC9gHWAbMBZwEEAaQAgACuAOgAEAErATEBFQHuAM8AqQCJAHUAawBhAFMATwBdAGcATgAfANr/xP/r/wcABQDw/+T/3P/v"
"//b/4v+8/6f/yP/X/8L/bP8G/5/+Iv60/W/9Zv2H/aL9oP2U/Wf9HP3A/Fj8A/zQ+8/77Psa/D78Zvyw/Ar9Rv1j/YH9yf1K/s7+"
"MP94/7j/7f80AFwAVABvALIALgGQAbQBuwGqAYwBggGCAYAB0QFGArECAQM6A4EDzQMqBFwEWgRhBG0EZgQ1BNIDZQMIA44CEwK9"
"AZsBuQG6AaoBlQFhAToBIwEJAd0AtgC6ALcAgQAvAM//e/8u/9n+mf5p/nH+Y/4P/rX9bP1R/VP9a/17/YD9rf34/U/+X/42/hz+"
"AP7U/ZT9VP1b/Yz9o/2T/Vj9Mf0n/T39oP0U/mv+sv7r/gr/Ff8G/9j+qv6i/tL+J/9b/2b/af9u/4X/bv9G/zL/If8P//7+Ff8t"
"/03/gv++/93/4/8RAE0AkwC1ALQAwQDMAOQA6QDDAIcAUAAOANX/wv+9/8f/7/8UABgABQD9/xUAMQA6ACYACgAGAOr/zv/L/+H/"
"BQAqAGkAvQAfAU8BWAFTAUABVQFvAYUBuAHPAdgBwwF0ASEBxQCCAEkAJAAoADgAUQBRAEYAPgB2ALgA2wDRAKUAqgDIAAkBMAE8"
"AVABZAFpASMBwABpACwAzv85/6j+W/5n/oz+k/57/mP+av6C/ov+tP7b/gj/J//7/tj+4P4u/6T/CQBAAEoAQAAmAOb/b//w/pr+"
"kP6j/tf+GP9T/5T/wP/H/8f/3//U/8b/lv9W/zz/KP8n/zL/N/83/1f/cv+Q/9r/KQBzAJYAqQDGANsA6QDiAN0A2wDXANMArgCG"
"AHsAjgC3AMcAwgDOAAcBYwGzAdUBqgFXAQMB1gC9AI4AcwBeADoAEwDn/8v/qf9//0j/Af/N/pz+gf6N/o3+Yv4Y/sH9i/1p/Vv9"
"kf3b/TH+ov4c/3X/wf/9/z8AlAC1ANgA/wAnAVcBkgHcARsCMAIHAt8BuAG2AecBJAJTAmICcAJsAlYCMwIQAv8B7wHcAcgBwQGw"
"AaUBqAGZAXkBPwH3AKkAVQDy/4X/Nf/u/sD+r/6c/pb+r/7i/iT/TP9G/1b/aP+M/6v/nP+P/1z/Fv/U/rT+qv67/uT+Hv+A/+D/"
"IgBCAF8AfgC/AAcBIQEOAfYABwEPARcBCwHxAPoALgFTAXkBywEXAl8CUgIdAgUC4gHiAeMB0QG9AYYBOQHcAFoA1f9B/8H+fP5c"
"/jL+4f2X/Vv9Lv0Z/Rf9Kf06/T39O/1D/Vf9U/0y/Q79+Pz//OL8svx7/Ef8Qvxc/If8wvwf/U79W/1k/Xj9mv2v/dz9+P0u/nf+"
"3v4z/1D/ef+u/+P/BAAxAEQAWQB0AHAAfgBxAH8AmQCjAMIAvQCmAJcAlQCtAMYAyQCRAEcATgB2AJsA0gARAUkBWwFBAR0BIQEu"
"ASkB+gDLAOoALgFgAVYB+ACEAC4A4f+F/zr/Bv/F/pz+pP7B/vn+Pv9i/1j/Wf95/7H/AAAxADUAGwAHAPT//f89AGcAZAAsAOX/"
"1v8EADYAIgDq/+b/IgBYAGwAZQB4ALkA6ADpAKoAZQBYAFQAOQAxAEIApAAlAUoBNgEJAcEAeQBBAAoA7P/w/+r/zP/W/xMANgBN"
"AG8AwQAlAYIB7gFAAp4C0gK5AlECxgG7AeIB6gHeAakBfwFHAfoAsgBaAO3/f/8r/8n+i/6F/qP+wf65/pD+Kv7e/bz9xv3Z/d39"
"/f3k/bz9xP0Y/mL+VP40/jL+Tf5c/mb+iP7B/v7+S/+c/9j/5//o/woAIwBIALQAYwEBAoUC5QIuA2gDTAMNA84CvQLPAtYCuQKV"
"AqUC3QIcAz0DSgNgA24DVAMNA38C4gF6AR8BwwBEAM3/nf+o/7P/iP9g/1T/QP8M/7D+ZP4//kn+e/6n/sj+8/4r/0j/QP8Y/93+"
"uP67/sn+5f4O/0T/rP/a/9T/vP+I/2//V/9O/1P/Vv9G/1//oP/Z/yQAhQDlACgBOgEcAQ4B1QB5ADwAPACVAMsA8QAIATMBfQG9"
"AQYCJAIgAtsBqAGqAaoBgwEwAeYA0ADkAM4AtACMAFsAGwC//5j/u/8GAE8AvAAuAWMBXgFZAX0BdgEJAX0ADQCo/5j/0P8/ANUA"
"QgGMAZ0BfgFhAVoBaAF4AYEBhAGAAY0BoAGhAWgBJQErAf8AqwBgAEUAWQA0APH/wf/M/wUAWACfAK4AjgBvADoA6f/H/7//0//M"
"/8//3f/Q/9f/y//G/5f/XP9P/2L/iP+S/2j/+/6U/k7+IP4M/hr+R/6j/jL/h//C//n/NwBVACwADAALABEAMAB4ALcA1wDvACwB"
"bgFjARsBwAB4AG8AkACDAFYAJwAPAGsA/QBxAbYBwAG5AXwB6AA7ALL/Y/8y/wr/2/4D/5b/IQBhACoAtf8p/8P+c/4Z/sP9hv2W"
"/Y39Zf1H/VL9ov0G/l7+dP5R/ir+Lv5B/lz+mv7t/jj/b/+V/6j/z//z/wwA6/90/xH//P43/3f/pv+4/7L/rv+T/3z/eP9v/37/"
"lv+S/5P/gP9z/4b/rv/V/+7/JgBaAF4AIgDG/23/W/96/4b/mv+y//n/PQBkAJEA0QAdATMBGwEPAUQBlgHXAfMB7wHTAXwB/gBt"
"ACMACwDc/4X/5v54/nz+hf50/kr+IP44/jT+8f2a/WP9Vv0B/Yr8R/xh/LX8/vwq/Vr9nP3J/fj9Cf7+/eX99f0k/i7+Vv55/o7+"
"if5g/kD+Mf5a/qj+4f74/gn/Ff8b/yD/LP9T/5P/p/+D/2D/X/+b/8j/yf+3/63/0f8WAIsA8wBKAZIBsgHJAdsBAwIPAs4BdgFJ"
"AWcBvgH1Af4BCgIvAkkCBQJ6AeEAcAAvANT/Qf+6/o7+tP7//mD/7f9qAKgAvgCwAH8AHgDH/4f/TP8W/wH/Nv+T/9j/5P8TAHYA"
"twC6AFsAwP8x/+z+2f7m/vT+yf68/s7+5f4U/0L/Wf91/7z/DQBwAKYAcgAJANj/5f/u/xAARwCfAOoADwFQAZQB2wH6AcoBlwGT"
"AYgBeAFcATYBLgEuASsB8wDEAMMA7QAhAekArAB5ADUADAD9/+X/rv+l/8b/AQAxAEAARAAvAOb/kf9u/2n/af9a/2L/af9n/3n/"
"cv99/6//4/8EAA0A7v/J/77/tf+L/2n/hv+X/4b/gf+h/8//7v8EACUAZgC8AAAB3QCRAGUAWgBJAP7/sf+D/57/3//+/xoANABG"
"AFUASAA0ABoA9v/S/7//3//3/xcARABJADgA///q/w4ANwBJAB8A1P+o/8r/BgAxAFcAawCBALIAwgDlACYBWgGJAYoBcgFgAWgB"
"YgEwAf0A9AAeAWIBhgE6AaoAHQDj/x0AagCyAM0AwQDDAMYAzgCrAEoAw/9m/2r/h/+i/37/KP8A/wP/Fv8w/3T/yv8hAEMAHQDM"
"/0r/7f7H/vj+Vv+P/5r/Z/80/xL/JP9I/3v/0/89AKkA4gALATIBagFiAQoBzQCVAHoAUgAXAO//1P/e//T/IgBaAKIA9gAwATEB"
"BQHZALkAvwC6ALoAzgDOAMIAsAC4ALoA7AA0AUYBGgGdADEA5//A/5//gv+G/4b/g/94/6T/1P+y/0j/8v7//kD/Wv9D/zv/aP/O"
"/w0AHQAeAC0AVABxAMkAOQGKAakBZgEKASoB5wHVAnoDmAOGA38DbgMjA8sCeALZAfEA8/9t/2P/Xv8//xr/G/9d/83/SABJALv/"
"L/+9/oP+Yv5W/pv+8v4h/zv/o/8IADEAPAACAJL/Ef/H/tX+/f7P/o3+pf7t/g//5/6r/pH+0/53/2UANwGMAbABywHTAcABlgFe"
"ASAB0QA0AHb/8f6v/rX+yf7u/kD/eP+h/6z/qv++/9D/DAA4AOr/Zf8a/zr/df9u/2b/cv94/5X/AACvAEQBdwFEAQYB1wCwAJcA"
"hgB8ADEAuv9Y/zr/nf+GAHcB2wHaAeQBBwLjAVABtgByAJgAzgCaACwAxP+M/47/bv8+/zT/cf/x/28ArAB6AOn/Gv9G/vf9Qf68"
"/hb/FP/Q/mn+E/7X/Zr9Zf1e/c39R/6e/sr+uf59/vf9Uv25/IL8qfwT/Zv9Iv6y/hz/Xv+J/93/TQDIAAkB2ABIAHj/5v6J/kn+"
"7/1+/Wr9t/1b/iL/DwCrAJ0AcwCIAN8AKwFdAaMB4AG5AR0BQgB//9/+bP5b/ob+1/4e/1v/2f+FAG8BRwKXAqQC6AJzA/gDKASl"
"AzkCPgBA/rn8DvwQ/HD84fwE/fX8yvzi/E398/0t/70AiQI/BD8FXwWpBIwDHQKNADn/PP6u/Zn9rv1Z/W38QvtU+uP5J/pG+xv9"
"Nv/8AC8C6gJNA3UDewM9A94ClAKTApoC9gGDAJ/++vzc+3z7zvt4/EP9GP4+/6MA8AHnApMDwAOMA3sDjAN1A3QCegAg/v/7rfpF"
"+pb6Qvvm+5H8w/2f/6gBIgPsAxgE3AOQAyoD4AJtAq4B8QAqAID//f7a/gL/Jf9N/4L/yP/0/8b/OP+d/jH+Fv5O/sn+kv+VAMsB"
"xAI4A2gDVwMrA8ICBAJDAWgAVf8X/vL8Mfy3+137QPuV+338k/1c/sr+Qv/6/8EAcAGkAX0BFAGLAEkACwCy/zn/1P6b/on+vP4P"
"/1X/VP8l/yj/Xf+c//P/VwCrAMcAywArAcIBNgJOAiMC9AGoARQBXADM/0H/1/6h/qn+//5a/6P/8f87AFUAXABoAG4AZABBAHEA"
"/QCPAfMBLQJJAicCsQE2AfYA1ADMAN4AFAFNAT0B+QDgABkBhAHtARkC8QGYAWwBoAHgAeABlQFSAVEBdgGrAZ8BSQHGAD8A+//J"
"/8f/0f/c/+H/g/8c/9n+0v76/hr/b//Y/w4A4P9d/8D+Uf4m/kX+jP6S/ob+mv7V/gL/7f7O/uj+Qv+W/9D/6v/q/8n/kv94/1z/"
"VP89/w//Ef89/7H/RwDAAP8APwHbAY4CKgNXA+ACPwK7AUUB/wDZAKwAbQA1ABcADAAZABEA/f/2/wAARQChAAcBMQEFAdQAxwAT"
"AV8BXQErAfkAAQEoATQBCAHBAF4A/f/e/wIASgCYAMMAzwDYANAA4ADrAKsAEgBu/zP/Sv92/3//g//H/yAAdQCoAKwAkQACADX/"
"u/6F/k/+8/23/cj9EP6Z/i3/pf/m//X/yv+Z/4H/Y/82/+L+bf4G/hv+kf4H/27/yv///woAKQA8AFcAbwBfABgAuf9//1f/Mf//"
"/uH+3v4R/23/wP8UAFQAeQBsAGQAaQB1AIkAUwDF///+o/7R/lH/AACJAAoBdAHPAQwCBgLwAcQBfwFAAf8A6AAHARcBCAHWAIAA"
"YABnAG8ApwDUAN8AygCfAI8AeABWADsAQABzAKsAyACnAFcAEgDb/6z/ef9J/xv/1P6Y/n/+kP6z/uX+Qf+Y/8D/5P8VAB4AGgAa"
"ADIAogA1AZgBigE4Ae4AlgAVAL//m/+L/3f/TP9E/0z/cv+K/4P/pv/K/+r///8eADoAJQDr/3//Gv/q/tb+4v4P/1//2/8sACwA"
"5v+m/5//lv+h/9X/LQCJAMcAxwCIAEwAMQA9AFAAfACBAFEALAD8/+n/7f8dAEwAOAA3AEQAXwB4AI4AtAC7AKMAfwA7ANT/b/83"
"/1f/x/8oAHMAbgALAI3/GP8J/0X/of8GAFEAfwCaAKMAgQBdAEIAUQBlADMAFwA1AFgANQDH/y7/e/7P/Uv96Pzf/F79Mv4O/4X/"
"ov+r/8H/0//r/xAAPQCIAOMAMQEoAR0BSQGEAZ8BRwHYAJMAhAB9AHAAhgCaALIArgCeAIwAcgBbADsAEwD3/xMAeAAKAWUBbgFZ"
"ASYB8QCyAGYAIQDc/5j/Of/X/oj+W/5x/q3+CP9u/8//PwC9ACwBdAGNAWABFgHcAOUABAHoAKoAiAC0AOYA/AAdAXsBGQKwAvMC"
"0gKEAiMClwHZAEUAEwAsACEA2P+h/4r/nf94/zn/PP9S/6b/JABPAPT/dv8C/53+If6c/Yn90f0i/k7+gv70/on/GACJAMEAvwCW"
"AFYACgCY/zD/4/6L/jX+M/67/nb/BQA2ACIA4/+e/3T/h//L/9j//v8xACkAFQDb/4j/Hv/b/vj+Pv+e/+v/KQBxALYA1gDKAM0A"
"rQB0AEQA9/+5/3L/Jf/u/rb+qv7Z/gr/Fv8K/xv/av/U/wcAFAAVABQAFQD1/9T/pP9p/yL/nv4L/qn9yv2M/o7/eAAcAX8BzAEL"
"AhsC3AFeAbMA6v9I//X+8P4W/0z/Zv9W/1n/o/9KAP8AcAFUAbYA+P9A/8r+tP7Y/u3+4P66/pD+gP6x/i//j//b/zYAngAfAYMB"
"oQFqAUYBQwFVAWUBMgHGABYARf9o/sL9lf2+/Rz+Uf6M/hT/6f/RADUBOwE9AXkB3QEnAk0CCwKaASIB2QCnAHIAgwCfAL0AuwC5"
"AMsAxQCrAEsAu/9g/2n/iP9w/wz/nf51/o7+yP7Z/sT+of5e/hL+pf0Z/c781fwZ/ZL9Kf7x/pH/yf+h/z3/8f7w/jL/VP8s/9X+"
"Zf4l/jr+bf62/vr+LP9W/5D/DgCkADMBbgFcAUQBWAG6AQYCLwILArgBXAH/AMoA0gAsAWoBaAEyAf8ACwE6AWoBZAFCASABNQFr"
"AYcBYwElAd0AXwDd/2X/Nv9m/5//m/9P//L+of5x/k7+M/5I/pr+Cv9h/5X/qf+z/7L/V/+4/iP+yv2n/Yz9k/3P/Sb+e/6v/sD+"
"5/4s/4T/3P8BAOD/m/9e/y7/F/8E//3+Cf8Z/yX/Kf9h/5f/kP9s/0P/PP9f/6X/9/9jAMcAGgFaAXEBcgFPATIBFgHrAJ8ATwAu"
"AFMAugA7AagBxgHAAacBiAF+AW4BYgEzAcUAMwDE/5T/gf9T/xX/9/7x/hL/Yv/R/yYAQAA8ABgA4f/B/7X/wf/t/xwATQB2AJoA"
"uACtAKoAtQC5ANAA2ADVAOEAzACYAFsAHQDb/5T/e/+c/+L/EgATABUAFQD6/7P/ZP9L/2X/df9v/2f/Q/83/0T/I/8P/zz/j//g"
"/wsABgAKACoALwAYAPn/DwBnAJYAgAA+APb/2f/E/5D/fP+k//H/OwBfAGsAcABrAGUAfQCbAJ8AkQCFAKAA0wAoAZ8B9AH7AcUB"
"kwF1AXMBcAFTATUBIQEYAQgB9ADQAIAAIQDm/+X/+v8eAEgAZAB9AH4AegCFAJAAkQB+AHEAZQBVADsA9v+2/4b/fv+n/8P/yv+i"
"/4v/mP/M/ysAZQBnACYA1/+U/2j/X/9b/2D/Zf9n/2T/aP9s/4D/xf83AMoAPwFfAUYBHQHuAM0AvwCyAH4APgAWAAoAMABTAGoA"
"ZwA4APn/u/99/0//UP95/7H/xP+j/4n/p/8LAH8AwAC/AIYAPwAaAAoAGAAYAPT/0P/A/9P/+f83AFgANADc/3r/OP8W/yD/QP9I"
"/1b/gv+O/5L/hf9s/53/+f9OAH8AmwCkAJQAewBbAC0AAADd/8D/vv+t/7D/uf+d/4n/Zf9A/yj/JP82/3D/xv8oAIMAtADmAOwA"
"8gDrALgAgABFAF0AhQChALIAoACfAJAAcwBTAEQAOQAvABgA5v/Y//H/KwBNADsACwD0/xEAOABgAHgAiwCtAMQA1wDQAKcAZAAN"
"AM3/xP/c//r/+P/B/37/Sv9i/6L/xf/a/9r/2f/V/9H/xf+5/8L/0v/5/w0AHwAsAC0ASAA8ADsAVABjAFEAFwDo/8T/vP++/67/"
"hf+F/6L/nf+i/6H/uP/R/9D/zf/O//r/PwB3AJIAngCQAH8AgQB/AIsAgwBgAEQAGwDq/8T/tP+q/5X/ef9c/z3/HP8F//b+1f7M"
"/uD+7P4I/0T/hP/B//z/HQAsADEAPABMAGcAbwA7AOr/pP+C/47/vv/p/9r/vv+2/8j/8P/w/9r/zf/O/8z/pf90/0X/PP8x/zL/"
"Sf9h/5z/pf+B/yz/2v7E/tT+Cf8x/2z/qP/t/w4A6//C/5D/if+C/43/wP8AAF4AgABvAE4AOwAyAAoA/P/5/xoAagCaAJwApwDU"
"AAQBCgHUAMQA6gAqAWwBewGFAXUBUgEiAdQArACmALwAxwCoAJQAdgBGACsAFAASAAwABwADANH/o/+T/6X/wP/R/9T/tf+Y/3X/"
"Uf85/xn/4f6+/rn+uP7L/gD/S/9t/4z/sf/R////FAAUAAoA/f/h/7j/lP90/0P/K/8l/yX/PP9E/2j/gP97/33/mP/b/yYAbwCi"
"AMkAzwCrAHYANwD8/+H/5f/6/yEARgB1AJ0AkgBzAHkAewBqAGMAWQBWAFMAXgB1AJEAkwCTAK4AyQDvAOEApwBmAC0AOQBZAI4A"
"vwCrAJUAhwBqAEkAIwD6/8L/rv+7/8b/wv+e/4T/a/9i/1j/Uv9f/2L/jv/Q/w4AHgDs/6//b/9D/zT/Ov8o/xb/G/8g/1T/m//Q"
"//H/6//t/wkAJAA2ACQA8//O/8L/4/8dAFYAkgDBAOIABAEmATwBSQFEAUQBQQEXAQAB1gC5AMkA4AAjAVEBVwE3ARwBFQEUARQB"
"8ADNAKEAgQB7AIMAiABsAFUAMQALAAAA9v8EAC0AMAAXAAwAGABBAFMARwBAAC8AEAD9//z/EQAoACMADAABADgAgADDAPMA7ADf"
"ALgAjwBjAB8A5f+q/33/bP9x/4L/lf+M/3X/Wv9M/2L/lP/f/woAHAAcAAMA7f/V/7j/m/98/3H/ff+B/2j/Mf8Z/yX/N/9d/3j/"
"hf+L/3z/Wf9V/3v/g/+B/4j/p//X/+z/9f/n/8z/rP+c/7//8f8ZACEADwACAPb/8/8FAB0ACwDi/8v/vv/P//z/HwAVAOP/rv+q"
"/+j/HQA6AEcAQgBKAFkAUQBHADEA6/++/87//P8hAB8AAwDr/woAZQCyAMgAmgBGAB8ADgD6//P/5P/a/7j/sP/b/xUAVgBuAG0A"
"WgBGAC8AHQAhAAAA3P/L/9z/AQAlAFkAVgA9AAoA3v/q/9//r/9K/+b+p/5r/nP+2P5e/8T/BQApADwAQgAdAMv/SP/c/sP+7P40"
"/2f/iP+e/7j/1//W/+z/EAAeABUA8//k/9L/p/+G/3j/kP+///L/HQA1AEwAWgBBABYA8//M/7b/sv/I/+f/7P/m/8j/s/+o/6T/"
"nv9w/1z/WP9T/4v/yv/8/xsABgDP/3//T/9a/4f/2v8bADkAKgDo/6f/g/+U/6z/jP9//4b/k/+z/8v/7v/c/4j/K//d/tD+4/4M"
"/zn/Of8z/xT/JP9x/8f/FgAeAPz/wP+Y/5P/mP+k/6H/tP/D/+X/GwAvABwA/P/z//j/AwAeACsAFgDl/6z/l/+X/6f/2P8iAHsA"
"1gAzAWMBSwHzAKAAagAbAKb/7v5k/iz+Ff4N/gX+Gv49/qb+Uv8UAM4AVgGyAZQBDgGAABcA4P+d/zX/1v6K/lv+bf6i/s/+1/7r"
"/kb/6f+lADgBkAGtAawBqwHgAWAC0wLrApYCBAKFAT8BGAHTAG0ACQC//5//mv+V/4X/U/8R/9v+7/5t/yUA4ACHAScCgwKdAnkC"
"9QFOAZQA+v+e/2f/UP8G/0j+N/0g/Fb7EvtS+wX8FP0r/iL/z/9DALYAPgHgAVQCrQILA1gDeAM1A4oCjAFSACb/H/5E/bb8hPyo"
"/Ab9o/1P/sv+NP+L//T/gQA5ATECLAPsA1EETQT/A3IDpQLSARcBkwBGAAAA2P+t/43/iP9Z/wj/w/7d/oj/oQDZAboCDQO1At8B"
"6QAiAM//w//P//T/JwBkAKkAyQCNACAAuP+A/5//DwCqAAUB4gBHAHP/rf4e/t79zf35/WX+zP4N/xT/0f54/jj+FP4R/iD+Zf4M"
"/8v/iAAyAaEB4QHvAekB7QESAlsCpgLFAqgCRgK6AVEB/QDCAHoAMwBKAKMAHwGNAc0BzgGXAVcBJQEfAT0BZQGLAa0BvQHeAesB"
"2QGmATkBzgBRAMz/SP/n/r3+kP5j/hT+yf2k/bz9BP4//n/+1/5b/9//RQCXAM8A6gABATQBdgGSAV4B6gBOAKf/CP+d/lX+//24"
"/aj92f09/tr+gv/r//z/2v/Y/wIAUQCeANAAGQFQAVwBYAFUAUgBGAHBAF4ACQDh/8b/lv84/8P+Yv5v/uP+W/+//+f/HQB8AOUA"
"gQEPAmECVALtAYEBPAH9AJEAEQCf/3n/vv8jAFkAOQDa/3H/Mv8t/13/ov/Q/9j/xP+6/7z/pP90/0H/Ev8Y/03/iP/G/7r/bf8W"
"/7L+f/6Q/tL+Hv9i/4n/jP+l//T/WgCrALAAgABqAFwAXABQACcA3/95/xT/sP51/lX+Vv6F/tn+SP+r/wwAMgAcAP//xf+g/63/"
"1/8OABwA///L/3X/LP/v/tX+7P4h/1H/WP9b/0//T/9o/53/6P83AJUA2QAVATMBLgEfAfMAwQC3ANsAFwExAQ0BxgCOAIUAigCj"
"ALYAwQDRAOEABQHzAL4AewBfAIkAzQAOARUB7gCpAGIALwD+/9P/rv+k/6v/kv9+/3v/c/9o/1D/UP9m/5r/z//f/97/rP+G/1z/"
"OP8u/xr/IP8o/0f/hP/N/xEAOABJAEAAFQDT/6v/s//P/9X/vf+t/53/pf+//8v/6/8YAEIAaACAAKQAzgDuAPEAyQCJAFgAQAAx"
"ACkALAA5AGwAkgCkAMYAxQDBAL4AtACdAHIASgAWAPr/yv+5/6r/if+N/4v/rf/B/9v/5v/b/83/nf9+/0//Nv83/0r/Uv9J/z3/"
"Jv8i/xr/Jf8l/xr/Af/c/sP+vv7E/rf+r/6c/qP+p/6f/pz+mP6y/uP+L/9m/4//s//X/9r/vv+r/57/u//y/zYAZQBoAE0ANgAq"
"AC0AVQCIALgAzQDEALEAoQCcAKwAvgDcAPYA8QDqANgAugCOAFEAIAATAB0ANwBaAIoA3AAOARUB9QDJAJgAaQBFAAkAy/96/0D/"
"Q/9k/4f/lP+O/3T/d/+K/4T/a/9B//j+q/5p/kz+YP6D/rj+xf7Q/vX+Hf9Z/23/a/97/4n/jP+X/4z/fP91/23/cP9Z/0D/FP/x"
"/vT+/P4J/y3/Zf+n//3/MgBDADsAOgBLAFIAeACfALoAxgDPAO8AJgFgAYoBhwFYAUQBPAFUAV4BMgHcAGEADQDG/5L/ff+J/7D/"
"7v8/AHgAfQBJABwAEAA7AI8A6QAwAUUBJQHXAIMAOQAVAP3/5f/L/67/q/+q/43/Mf/T/q3+rv7T/gP/Lv9L/1P/Uf9V/3P/tv/n"
"//b/+//3/+P/yv+L/xD/i/4Y/vP9G/5X/rv+L/9r/3//j/+o/8r/4P/y//r/FgBHAHUAjwCSAI0AbQBLAEoAVwBiAGsAggCyAPcA"
"JAEnAQAB0gDQAPAADgEaARMB/gDeAMwAzADiAAcBKAFDAVcBYwFfAVsBVAFVAWUBeAGEAVMB6QBfAMf/X/8Z/+j+4P7P/rP+r/63"
"/tn+AP8t/3H/wP8XAGAAmAC4ANAA4QDZAKkAYwAkAPL/yP+V/1r/Lf8V/xH/GP8g/xH//f4M/zz/gf/U/xQAaQCxAOoAOwFqAYYB"
"jAFuAT4B/QC/AJEAXgAsAAAAyf+R/17/QP9I/2z/nP/I//H/9//p/+n/z/++/9f/+f8+AIoAzgD3APgAAAEAARIBHwEiAQIBkgAB"
"AGr/AP/R/sv+2f77/i3/WP+R/+H/QQCcAAMBVgGBAZkBggFQARUBzACbAJcAlAB/AFUAJQAJAAoAJgBPAHcAjACkAKsAqACmAHYA"
"PQAGAMf/pf+1/+P//f8GABoAGQAlACMAHwAfAA4ABwDi/7D/cP8//yv/Hv8u/yj/I/8z/3v/1/8fAFcAVABBAAoAz/+A/yb/7v7A"
"/rr+0P4T/0//gf/E//X/RgBmAGMAaABEABMA5v+1/4f/W/9C/1L/gP+o/8X/2v/X/8X/vf/G/97/4f/S/8X/rf+t/8L/yP/Q/83/"
"2f8PAEYAiwC1ALEApQCLAG0AQADp/4P/Nf/w/q7+kf6E/qL+//5i/9L/LgBaAHkAkACaAKAAqgC5ANUA7ADOAKIAgwB0AHkAYAA1"
"AA0A8//u//L/4P+l/2n/Rv9Q/5T/5f8dAC8AIgAfAEAAigDQAO0A4gC1AHcAJgDZ/37/EP+4/of+c/6D/rP+4P4L/1H/iv+V/57/"
"i/9f/0v/T/9+/7z/AQA3AGcAowDLAA0BKAEvAUABMwFLAVcBYgFgAToBFQHdANAA4AAJATIBPAEnAQ8BFgESAQMByQB4ADEA7//E"
"/7X/xP/k//7/8//g/8z/2/8LABQA7P+J/yv/8/7w/vn+7v7s/tf+wP7A/q7+jf6J/pX+pf6d/qD+0P4T/13/o//k/wYAMgBBACgA"
"OgBcAJoA6QAhAUIBUgE3AQsBzgCLAGwARQAuAPf/k/9O/zX/Zf+7/xgAhAD2AD4BWQFZATwBMQEhAfwA2wDbAPcAEgEtASsBHAEF"
"AfQA4gCjAF4AHQDk/6//Zv9I/1T/Xv90/3r/jf++//v/OQBgAHkAgAB3AGIAKwDy/8j/of91/1H/Pv85/0P/Rv85/zP/Of9G/2X/"
"jP+4/9///P8bADUAQAA3ADQAOQA1AFMAawBeAE0AGQDO/4//T/8f/wj/BP8V/zL/cf/J/xUAXwCjANcA9ADtANcAxgCrAKIAowCJ"
"AGMASgA3ABkABQAJABQAOQB5ALgA7QD2AOsA7gD0APoA+gD0ANwAxgCkAGgAIADg/7f/iv9j/zz/Pv9i/4z/tf+6/9P/8v8gAFsA"
"dABzAFsARQA/AC8ADgDS/4//aP9S/1//bP9w/1H/Dv/r/uv+J/9p/5v/sf+5/8X/rv+k/6T/q/+i/3//WP86/zr/Tv9g/2T/d/+U"
"/8T/GABaAG8AXwA6ABIA3P+y/5f/j//F/wUAQACPANMACAEtAUMBWgF1AXcBRAHUAFAA5v+c/4T/iv+x/woAbgC7AOMA0QCYAFQA"
"HgARACUANgA2ABEA0/+s/5P/hf+F/5D/l/+e/7//2v/5/xAADgAMABIALAAtACMAFgD4/+H/zP+3/57/k/96/2L/V/8x/x7/KP9R"
"/6X/BQB4AO4AUgGjAd4B3AGoAUwB5AClAIwAigBmACkAz/9v/0H/Pv+S/+H/BAAUAOv/1P/S/8D/lP8w/67+Ov7d/bz9xv3j/Qr+"
"G/4//pP+Ef9z/4z/Vf/m/l/+xf0+/c/8nfyT/MX8Y/1Q/m3/hwCNAUICqALcAvwCHwMfA/cCeAKqAbsA2v8s/9D+3v4w/7H/RgDK"
"AEcBvwESAmsCzwIlA1ADOQPzAoMC8gFbAb8ALgDD/3j/UP87/xH/xv6F/mH+QP4W/tX9vv3O/ST+uf5E/9b/IgA5ABQAuv9Z/xD/"
"Df8P//7+zf5w/gr+n/06/Q39Gf1O/a39Gf5y/qz+5f4s/4L/zP8AABkALgBzALAA4AASARgB8QC/AHQAIwDy/8r/zv/h//f/HwBl"
"AM8AOAGRAdABBwIzAjoCIwIBAsABggFTAQABwQCaAHEATgAMAN//z//I/7f/j/9o/zj/Dv/m/rn+sP7K/gj/VP+j/93/3v+7/3D/"
"Hv/p/tn+Av9Z/8P/PACYALgAuQCxALkAwQC8AJwAgQCIAHoAUgAEALb/e/9i/3L/jv/A//H/MgBlAHUAgACJALcA3wABASkBRAE0"
"AfMApAA7AM7/bf8v/yH/Cv/0/uX+0v7l/tf+zf76/hj/Uv9//5X/nf+n/9X//P8+AIIAlgB/AEIA8v/L/9r/7f/0/9//v/+t/63/"
"rP+8/8j/0P/3/xUALAAqACEADgD2/+z/1//R/9X/5f/2/xYARwBkAGsAWwA6ABQA7//N/8n/1//x/yQAZQCpANwA+wDwAMQAmwB3"
"AGcAUgAiAMT/Mf+f/jD+7/3u/RT+Sf6U/uP+Mf92/7//DABAAFoASwAhAPb/1P+y/3j/TP8y/yD/G/8G/+j+0P6z/qH+hv5m/lv+"
"Yv6V/rL+xv4K/2r/9P+DAPoAJwEiAQwB1gChAE8A8P+j/0T/z/5X/vP9z/34/VT+vv4X/3r/6P9TAMcAHQFiAXkBUQH4AJwAeACK"
"ALYA3QD0ABsBZAHIAToCgQKrAswC0wLmAtkCrwJ6AkICPgJFAmUCjAKyAuEC1AKjAmICLQIsAiACCwLwAbUBdwE6AfEAqwBvADUA"
"EADw/8n/u/+1/6v/df8w/+3+of5l/i3+IP44/ln+ev6W/pz+hv5p/mz+af5p/mj+Rv42/gv+Bv4W/ib+Zf6O/s3+/f4U/xf/Df8a"
"/yf/Ov87/1T/iP/J/wQANwCFANYADQEeAQwB8wDjAMwA0wDwAP0A7ADBAJ8AmADBAPIAFAEmASkBMQExATgBOwE1AR4B8wDPAKIA"
"kgCjAKcAtQC3ALMApAB8AEwA7v+b/2n/Nv8S/wX/F/9G/5D/3P81AI0A1QAEAfIAwAB8ADMA/v/N/6b/jP9u/1T/W/+F/7X/5//4"
"/+H/t/+A/1f/Q/82/x3//v7a/sL+4P4Z/1b/jP+5/+b/AgAPACcAPgBcAG0AXABXAGMAgACbAKgAlwCSAKsAxADWALoAoQCZAKMA"
"pgCCAGAATQBOAFgAewCUAKQAsADMAP8AIgEyARIB1QCCAEkAKQAEAOv/yf+8/8L/4/8WAEUAawB3AGwAYABFAA4A0v+R/4n/u//v"
"/xUAFQAJAB8AUACEAKsAxQDvAP0A7QDHAJQAcwBgAGMAWQA8ABYA/f/4/xAAPAA/ADUALAAhACgAPwBfAH8AigCAAFcAFQDa/7H/"
"lv+O/4f/eP+J/6n/xv/N/8T/u/+c/5r/oP+i/47/X/85/yT/Hf8P/wT/7v7t/vf+/P4B/+X+wP6s/qr+sP7L/t3+A/9J/4H/jv+A"
"/3X/Zf9//6f/uv/G/7v/nv+j/83/+v86AGsAhACTALAA2QD0AOwAqQBaAAAAqv+I/5z/0v8MAEUAewC5APYAKwFDAS8BDAHrAN0A"
"2gDKALgAqQCuAJsAewBxAGIAXQBoAIQAkACPAIIAagBOACMA///k/93/3//h/93/6/8TADMARwAwAAMA1P+3/7b/u//I/9T/1//U"
"/7f/jv95/1//P/9C/1j/df+a/57/lf+F/2//fv9+/5D/tf/k/xkAOwBcAHEAmQC4ALYAnwBwAEAAEwD2/+j/yf+Z/4f/d/9v/4P/"
"m//P/w4ATQCEAKIAqACeAH8AVQAxABkAGAAuAFQAawBpAF4ATABeAH4AfwCJAH4AeQCKAIwAiQBxAE8AIQDw/7//kP9l/zj/C//F"
"/ov+Xf42/hb+Cv4U/iz+TP5r/or+qP7g/gn/MP9K/1v/k//P/xUASABcAGAAWwBSAEQAQgAuAB0AKAA8AGEAhAChAL4A4AATAUAB"
"cQGYAZMBbgEkAcMAbAAjAOj/vv+L/0z/I/8L/wX/Dv8i/yj/NP9R/2j/if+d/6T/qP+c/47/dv87//n+yv6l/pP+jv6R/oz+i/6m"
"/rf+vf6p/pv+nv6Z/qL+rP7F/uz+E/85/zf/Hv8e/z//d/+///L/IQBDAF0AdQBvAG0AYgBlAHoAmwC8AM4A2QDOAM0AywDfAAUB"
"HwFBAVUBXgFgAWkBfAGOAZYBjQF/AWIBOgEDAcYApACQAH4AggCDAHkAcgBxAHgAhwCbAJMAgwBzAFoAPwAqAB0AJQA5AEoAWgBk"
"AG8AcQBTACgA/f/O/57/aP89/yv/Kv88/0b/Rf9E/0D/Lv8N/+P+vP64/sT+1/7o/vL+7P7U/sb+xv7V/u3+Ff8t/zf/Pf86/0T/"
"Sv9c/37/pf/D/8z/uf+o/6z/sP+//8v/y//G/8T/xf/Q/+L/0f+i/4r/kv+u/+T/AAABABQAJQBCAHcAogCpAJsAjgCNAJoAqgCr"
"AKwAowCVAJgAnwCvAKoAkgBmADwAQQBOAF8AZwBVAEAANgBNAH4AtADTAMwAxADTAOEA5ADSAK4AkgCDAIAAeABhADUACADu/+f/"
"8P/w/+P/2P/E/73/uv+4/8b/6f8gAFUAigCsAMsA2wDJALMAkgByAGMAdACGAIsAgQBfAEoAJwAPAAkADgAuADYAIAD4/8X/nP9/"
"/3j/g/+L/57/rv+5/8j/uv+Z/2f/PP8n/x7/K/9E/2f/jP/E//r/JwBVAGIAWABBACsAGAAUAA8A/P/t/+H/4//g/9r/3f/q/xgA"
"WgCYALIAswCPAEwAMwBIAHIAjACKAG8APAD7/7D/jf+K/5L/sP/N/9n/4v8AAEMAkwDTAAIBDwEIAf4A3ACeAGYASgBYAHsAmwCw"
"AK8AogB7AEwANAAwADQAQgBlAIUAjgCMAJwArQDDANoAzADLAMoAyADHAKkAjQB8AHIAaQBgAEoAIgAAAN3/x//L/8//4f/5/xIA"
"LAA/AE0AVQBwAIYAlACKAF4AMADy/77/lv+G/5z/t/+3/5L/YP8o/wj/5f7I/sD+zf7r/gH/C//y/s7+sP6z/s7+1f7C/qX+kP5/"
"/nT+WP47/jL+Of5W/nb+kP60/uH+Bv8+/3P/i/+D/27/U/89/zz/QP9D/zD/Of9O/2T/mP/C//7/OwBoAJQAtwDgAO0A2wDFAKMA"
"pAC/AN0AAwEUATUBQwFKAVgBVQFTATkBIQEQAREBMQFYAXsBjQGRAZ4BpgGbAZkBfgFhAV0BXQFSAUIBNwEXAfcAzgChAHcAZQBw"
"AHoAjACKAHQATgAmAAIA2v/H/8b/xf/A/7r/o/+L/3H/U/82/yD/K/8+/1n/c/97/3//ef9u/2H/Sf8X/+P+x/66/rv+x/7j/uz+"
"5P7a/tL+0v7I/sX+0/71/iX/Rv9e/2f/d/+K/5L/mf+a/53/mf+Q/6H/wv/n/xIAOABLAE8ANgADAN3/zP/I/93//v8RAAwABwAC"
"AAkAOABbAHAAbgBJACIABQAAAAQACgAbADAAQgBYAG4AhQCaAJkAkAB3AFYAMQAIAOr/1f/Q/8f/wv/n/x8ASQBeAFsARQBEAFAA"
"YQB8AIMAhgCFAHYAWgA1AAoA2f+6/5r/kP+p/83/8//+//b/4f/M/8b/xf/a/+X/6f/o/+//AgAHABQADAD+//b/4P/T/9D/1P/W"
"/93/3P/a/+f//P8lAE4AXgBUADEA///Z/87/1//y////AQADAPf/9P/9/wUAIgBQAIgAxADiAOEAtgBsAC4ADQADAAwAJgA5AEcA"
"TQBHAEYAPQA7ADMANQA9AEAARgAtABkADwAFAP3/+/8GAAsAEgAgADIAOQAvAAYA1P+r/4b/cv9l/2j/bf9+/5n/t//s/xgANABD"
"ACoA7v/F/6//t//B/67/mv+C/3j/bf9j/2z/g/+a/6n/tf/G/+b/CgAgABoAEgD///j/AAAGAAEA3v/N/7b/qP+r/6z/u//J/9z/"
"3//c/+L/4f/i/+L/4v/s/+//+P8XACwAQQBNAFEATwA9ACsAEgABAPv/8f/r/+r/3P/d/+X/5P/i/9j/2f/g//f/CgAiADMASwBp"
"AGwAawBeAFkAVgBfAGAASAAvAA0ABQDr/9P/vP+m/7H/uf/e//T/AgANAPf/7v/t//P/+P/2//D/6f/h/9n/zP+5/63/n/+k/63/"
"uP/K/9v/6v/s/9n/uf+o/53/p/+0/7f/uf+q/7H/0f/7/ykATQBpAIAAnwCtAKwAmQBxAEoAKAASAAYABADy/9L/tP+m/6X/tP/M"
"/8//x/+6/7X/p/+E/2T/S/9J/1f/ev+V/7f/8f8WADIAOAAlAA4A/v/8/wYAHgBEAHQAnQC0ALYAmwBxAE4AQwBRAGIAfgCcAKsA"
"sgCaAGgARwA8ADEANgBCAEgAYgB6AIkAkQBzAEoAJwATABMAFAAIAPb/4f/M/8j/wP/B/8v/0P/N/7f/pv+j/7L/s/+o/5n/hP+A"
"/4L/mP+6/+3/HgA/AE8ATABJADoAMQArACkAHgATABgAGgAaABAA/P/W/8H/vv/G/9v/5v/w//P///8FABMAHQAiACwAIgANAOv/"
"2f/k////DQAEAOf/2v/m/9//1//N/8X/1v8DAC4ATwBqAHoAgQCIAI0AfwBrAD8ADgD2/+v/8f/4/wMA///8//r/9v/x/+3//P8W"
"ADkATgBaAGQAawB0AHIAVAAsAAcA5P/N/73/uP/B/+L//P8UACIAIQAlACEAIgAIAO7/2//U/+H/3f/r//X//f8TACUAMQA2ADUA"
"MAAgAAgA6f/E/6X/lP+h/7L/zv/z/xYALQAmACAACwD0/+//+v8TAB8AGgAHAPj/1/+9/8H/1P/w/wsAHwAbAAcA8P/X/8P/wf/g"
"/w8ARAB6AJYAoQCfAIgAegBjAFsAVwBHAEAALAAdAB0AKAA0AEYAYAB4AIkAhgB+AGUAUwBGADwAPQBMAGYAdACBAI4AkwCRAHsA"
"QAADAND/uP/A/8f/1f/O/7n/mP9x/1b/Qv87/zj/R/9m/4f/mP+f/7T/zP/s//n////4/93/x/+j/4P/bv9s/3D/ZP9i/23/kf/L"
"//3/CgD4/9L/o/+M/4b/iP+H/5T/rP/L/+7/BgAlADYASgBuAJIAuADAALwApwCLAH0AdwCAAIwAjACNAIUAbQBhAFgAVABbAGQA"
"aABWAEgANQAaAAcA9v/w/+v/8v8MAB8AMgBGAFIAYgBWAEUAPQAkAAYA5//A/57/mP+k/8H/4v/7/yEANQAzACYA/v/c/9r/7P8H"
"ABkAIwASAPP/5P/I/7r/x//j//3/BQD6/97/z//C/7X/k/9w/2H/Yv+A/5b/p/+a/3b/Sv8J/9v+wf67/q7+of6b/oj+dv5c/kf+"
"Nv4y/jX+Rv6C/sX+Cv9D/2L/f/+P/6P/v//O/9H/2f/x/woAJQAqAB4AFwAXADIAZwCwAPsAPwFsAYcBngGxAc4B6QENAi8CUgJ8"
"ApECjwJrAiYC6QG5AZIBcwFKARsB4wCwAIMAVgA0ABYABwD1/+L/5f/0/xAAKgAyACIAAADQ/4j/Ov/6/sj+rf6V/n3+YP5A/jr+"
"Lf4k/if+Mv48/kv+a/6S/tH+FP9Y/4r/nv+l/53/lP+S/4b/c/9o/1z/W/9k/37/m/+k/6z/pf+f/7T/0//z/xEAKQBEAGAAewCR"
"AKkAzADkAPYAAwEDAfsA4wC+AJ4AfQBbAEMALQAsAD0ASwBjAHIAdQB0AGQAWgBQAEAAOgBGAGIAdgCHAI8AgwByAFMANAAVAAgA"
"CQAAAAYA/P/p/+n/6//7/wAA/v/+/wAACgAIAA4ACQD7/wAAAQAQACsAOQA3ACEACwDz/97/1v/U/9X/1f/Z/+P/7f/y//L/4f/E"
"/6n/l/+W/5//tf/R/+v/AQATABsAIAAdABcAJQApACwAMgA2AD4AOAAoABMA/P/m/9f/z//Z/+H/8P8IAAoAFgAhADoAVABfAGsA"
"ZQBhAFMASwBEADoAOgAxACcAEAD8//z//v/7/+v/2v/G/6z/of+l/77/6P8ZAEwAcwCNAI4AhQBwAE4ALAAAANP/of9//2v/af+C"
"/57/vf/G/7X/qP+Y/5T/nv+q/77/0f/s/w4AKAA5AD0AQwBFADsAKgANAOv/y/+v/5//lP+Z/7P/0f/t/wkAJwA2AEYAVgBdAFkA"
"TABAADoAPABKAG0AjAChAKUAkQB7AGEAUgBQAEcAMwAkABEAAwDy/93/1//H/8H/uf+9/9v/9/8XADQAMwAnAB0AEgABAOz/4v/d"
"/+7/AAAMABoAGQAdABoAFQALAPb/5P/O/7r/s/+1/7j/xf/S/9//6v/v//b/7v/d/9P/zv/V/+L/6f/q//b/DgAiADwATABVAFMA"
"RgAvAA4AAQDu/9v/yv+2/7L/sP+z/77/vf+x/7j/wf/W/+z//P8XACcAPQBRAFgATQBAAC0AHAAPAPb/5//V/9D/2P/i/+//8f/u"
"/+X/5f/b/8r/wf+y/6r/rf+1/8n/2//i/+b/6f/u//7/GQA8AFEAUQBEACwAEgD0/97/z/+8/7b/s/+0/7r/wv/O/8j/wP+1/6f/"
"nP+Z/6P/rv/G/9v/5f/y//T/+P8BABAAJAAyAEAATwBRAEwAQwA1ADMALgAoACoAKgAvACcAHwAZABkAJQAyAEkAWgBoAHsAigCK"
"AIIAdgBkAE0AOgAoAAMA6P/b/9n/2//V/9P/0f/R/8v/x//G/9j/8v8AAA0ADgAVABgAHAAgABkAEQACAP3/9P/t/+3/8v8BAAAA"
"9//v/+n/9P8CAA4AFgAYAA4AAQABAAIACgAZACAAKAAhABMADAAGAAYAAwAAAP//+/8AAAQABwAOABAADQALAAgADAAdACgAKAAo"
"ABoADgARAA8AFQAQAAQA+f/t/+T/zf/O/9P/2P/j/9n/z//J/8b/xP/E/8f/0P/W/9//4//j/+L/1v/L/7//sv+n/6T/rP+3/8v/"
"5P/w//H/7v/c/9L/2P/j//H/8v/r/+P/4P/W/9b/3P/e/+z/9f/+/wwAFQAZAB0AGAAKAP7/9v///wwAGQAqADsATQBYAFkAVwBW"
"AF4AYABSADkAHQAKAPj/8P/z//j//v8FAA0AFwAhAC4APwBPAGcAdQCAAIUAdQBZADIACQDs/9v/zf/J/8f/yf/P/9P/3P/h/+P/"
"5v/w//v/BQAUABsAFAAJAPn/6f/c/9P/1v/a/+L/5P/i/9T/xv++/63/rv+r/6v/s//B/9b/6P8GABgAHAAUAAcABgAOABsAIAAi"
"AB0AFwATABEADgAIAAAA+f/6/wAAEQAnADkARABIADwAKwAdABAACQAAAPr/9//u/+f/5v/q//f/AQAHAAQA/v/5/+3/5//l/+j/"
"7f/u//j/BAARABoAGQAHAPf/7f/l/+P/2v/J/8L/xP/L/9n/5//3/w8AIwAsADYANwAzADUAPQBNAGAAcAB6AIkAlQCcAJ8AlwCN"
"AHYAXAA8ABoAAgDk/9H/wf+1/7T/tf+1/7j/vP+9/8T/zv/S/9v/7f/7/wMADQAQAA0ADAAAAO7/3f/G/7D/nv+R/4P/cv9o/1z/"
"Vv9Y/2f/gf+n/9T//P8jAD8AVwBrAGwAcQBqAGAAXABXAFEAUgBaAFgAWwBYAFYAWABUAFcATwBGAD8AOQA3ADgAQQBPAF0AZQBl"
"AGEAVwBNAEQAOgA4ADMAJQASAPz/7f/l/93/0v/B/63/m/+Q/5H/mv+v/73/uv+p/4b/X/9E/zz/Pv9H/1X/Wv9h/2v/d/+J/57/"
"sP/D/9T/2//c/9f/0//U/9j/4f/j/+P/7P/3/wIACwAVACIAMAA7AEQAUgBhAG8AdAB0AHAAagBlAGAAYABmAGsAZwBZAD4AJAAN"
"APv/8P/m/+r/7P/w//b/9P/3//n/+//z/+b/2v/U/9n/2f/a/9n/0f/U/9j/2v/h/97/6f/5/wEACQAMAA8ADQAMAAMAAAAEAAsA"
"FgAZABcAEwARABAAGQAnADsAVgBuAIoAlACWAI4AfABrAFcASgA6ACwAIwAbABQADgAMAAQA8f/V/7L/kf96/2z/a/9n/1r/Uv9I"
"/z7/Ov8//0b/Uv9c/2L/bf9y/3X/eP9z/3H/dP93/4L/k/+u/9D/8P8HABcAIwAtADoASABZAGwAdAB4AH8AhQCbAL0A3gD/ABYB"
"JAEuATIBMAEkARIB9QDYAMUAuACxAKkAnQCQAIcAfgB1AG4AZgBeAFQASwBEADkALAAhAA4AAADz/+L/2//P/8H/sP+Y/3//Z/9a"
"/1P/Uf9Q/1D/Uv9Q/1L/Uv9Q/1r/Y/9v/3//i/+X/5//q/+2/7v/u/+v/6P/mf+V/5n/nf+t/8P/1v/u/wQAGQAyAEcAWABiAGEA"
"VwBFADIAIAAQAAQA/f/0/+v/4P/X/9r/6P/9/xkAOQBVAG8AhACOAI8AiwCAAGsATgAyABYA+//q/9//0f/I/8f/yf/Q/97/5v/1"
"/wUADgAaACIALAA2ADEAKAAiACAAIAAhACIAJAAiABsAEAAKAAUABQAEAP//AAACAAQACQAKAAoAEwAfACkAMwA2ADQAKwAoACYA"
"HAAQAAQA+v/u/+b/3f/a/9j/0//Q/9X/4v/y/wkAJwA7AEUASQBIAD8AKwASAPf/3P/M/8P/tv+i/47/ef9p/17/VP9T/1z/a/99"
"/5b/qv+4/8r/3f/u//f/+f/7/wAABAAGAP//9f/r/+D/3v/S/8b/wf+5/7f/wP/K/9j/6v/9/w8AHwAyAEQAUgBeAGcAawBtAGsA"
"ZABiAGAAXABXAEwAPQArABwADQAFAAAA+//z/+T/1v/M/83/0//Y/9//4v/l/+f/7P/x//L/+P/9//3//v8AAAgADwAUABAACAD/"
"//j/+f/7/wAABQAEAAMAAwAAAAcADQATABYAEwASAA0ACgAFAAEA/f/5//z///8HABAAFQAbABsAHAAZABgAGwAdAB8AIgAoACwA"
"LQAxADQANQA1ADIAKQAdAA4A/v/1/+r/5P/d/9P/zP/H/8L/vf+6/7b/t/+8/73/xf/G/8L/xP+9/7n/tf+t/6r/qf+r/63/sf+1"
"/7f/t/+2/7n/vv/I/9D/3P/q//L/+f///wIAAgABAAAA//8BAAUADQARABAAEAAMAAwADQARABQAGQAlACwAMAA1ADkAPwBCAEAA"
"PAA5ADcAPABBAEQASQBQAFcAXwBjAF4AUgBCACwAFQAGAPr/7//p/+T/4v/d/9r/2f/Y/9n/1//X/9P/0v/W/9f/3//i/+L/3v/d"
"/+L/5v/s/+z/7v/w//T/+P/6//f/7//o/9//3//f/+T/7v/1/wAABwANABMAGwAhACYAKwAvADUAOgA6ADUALwAlABwAGQAXABoA"
"GwAdABwAHAAhACQAKgApACQAJAAgABoAEgAHAP3/9v/y/+z/6P/k/+T/6P/q/+3/7f/y//P/9f/3//L/7v/p/+n/7//1//3/AwAN"
"ABcAGwAgABsAFQAPAAMA///1//H/8v/w//H/8//4//r/AAADAAQABQABAAEAAgAEAAUABgAGAAEAAQADAAkADgAOAA0ADwAOAA0A"
"EAAMAAgABgAGAAkADAATABgAGwAcABkAEwANAAgAAwAAAPv/9//y/+7/6f/j/+P/4f/h/+D/4f/m/+r/8v/1//f/9v/z//P/9P/8"
"/wAABAAIAAQAAwABAAIABgALAA0ACwAQAA4ADQAMAAQAAAD5//P/9P/1//n///8FAAYABQAGAAQABQAJAAwADAAJAAcABgAEAAMA"
"BQAIAAoADAAJAAIA+//0/+//6//r/+n/6v/t/+//9f/5//v///8DAAoADgAPAA0ACgAHAAcACQALAA0ADAALAAUAAwAEAAgADgAT"
"ABYAEwAPAAgABgAHAAYABgACAAIABQAJAAsACAADAAAA///7//z/+//9/wIABQAHAAUAAAD7//r//f/+/wEAAAD9//3/+//9/wAA"
"AQACAAAAAAABAAYACwANABAAEwATABQAEwATABMAEgASABEAEgATABAACQACAPn/8P/q/+b/6P/s//H/8//2//j/9f/z//D/7f/t"
"/+3/6v/s/+j/5//q/+7/8f/z//b/9v/3//n//v8CAAIAAgD+//r/9//5//v//v8BAAQACAAJAAwACAAHAAUAAQABAAAA/v/+/wAA"
"BgANABMAFwAZABwAHgAhACMAIQAgAB4AGQAUABMAEQANAA8ADgAOAA0ADgAPABAAFQAZAB0AHQAbABcADgAFAAAA+f/3//T/8f/w"
"//L/9f/4//3/AAADAAEA//8AAP3/+v/5//j/9//3//X/8f/x//H/8v/0//X/9f/y/+//7f/s/+3/7//y//P/9P/z//L/8//0//n/"
"//8CAAUABAADAAAAAAAAAP///v/6//r/+f/7////AAAAAAMABAAEAAgADQASABIAEQARABEAEQASABYAFwAXABgAGAAVABEADgAK"
"AAkACAAGAAYAAwABAAAA/P/5//f/8f/t/+n/6v/s//L/+P/9//7/+v/3//H/7v/s/+3/7//w//P/8//1//f/+v/8//7/AAAAAAMA"
"BgAJAAkACQAJAAcABgAHAAcABgAHAAYABAACAAAA/v/8//v/+//9/////f/8//v/+f/5//r//P/8//z/+//4//f/9P/x/+//6//q"
"/+z/7v/w//P/9//5//r/+//5//j/+P/4//n/+////wAA///9//z//v8BAAMABAAEAAQAAgD///7///8AAAMABQAGAAYACAALAA0A"
"DwAQABAADwAQABIAEgASAA4ACwAIAAMAAwAEAAQABQAGAAYABwAJAAkACAAEAAAA/f/7//r/+v/5//f/9//1//X/9v/2//j/+v/8"
"///////+//v/+P/4//j/+f/6//j/9//3//n//P///wAAAQAAAP///v/9//7/AAAAAP7//P/6//n/+P/1//f/+f/6//z//v/+////"
"AAAAAAAA/v/9//3//P/+/wAAAgADAAEA///+//3//f/9//v/+v/4//f/+P/5//n/+//8//3//////wAAAAABAAQABgAJAAoACQAH"
"AAMAAQD+//v/+v/5//r/+v/7//7//v8AAP7//f/8//n/+P/3//j/+f/6//v/+//6//z//v///wIABQAFAAUABAADAAMAAwADAAMA"
"AQAAAAIABAAFAAUABQAFAAUABAAFAAUABQAGAAYABwAHAAUABAADAAMABAAFAAYABgAFAAQAAgAAAP3/+//7//3//v/+//3//P/6"
"//j/+P/4//n/+v/6//v//P/+/wAAAQACAAEAAQAAAAAA/v/8//z//P/9/////v/7//n/9//3//n/+////wAAAAAAAP//AAAAAAAA"
"/v/+//7//v/+///////+///////9//z/+v/5//j/+f/5//r/+//7//z//f/9//7///8AAAEAAgAEAAYABwAIAAgACgAJAAcABwAG"
"AAcACQAIAAcABAAAAP3/+v/6//n/+P/3//f/9v/2//f/9//2//f/9//5//v//f/9//7//v///////v/+//7//f/+//7//v/+//3/"
"+//6//j/+f/4//f/+f/7//v//f///wAAAgAEAAUABgAFAAQAAwACAAEAAAAAAP///f/8//v/+//8//z//f/9//3//v//////AAAA"
"AAAAAAAAAP//AAD////////+/////v/+//z/+//8//z//P/7//v//P/8//3//v/+//3//P/8//3//v/+/wAAAAAAAP7//P/5//f/"
"9v/1//X/9v/4//r/+//9//7///////7//v/+//3//v/9//z//P/8//z//f/+//7//v/+////AAABAAMABAAFAAYABgAFAAUAAwAC"
"AAEAAAAAAAAAAAAAAAAAAAAAAAAAAQACAAMABAADAAMAAQABAAAAAAD///7//////wAAAAABAAEAAgADAAMABAACAAEAAAAAAAAA"
"AAD///7//f/7//v/+//7//3//v/+////AAAAAAAAAAD//wAA/////wAAAAAAAAAAAAAAAAAA///+//7//f/+////AAABAAIAAwAD"
"AAMABAADAAMABAAEAAQAAwADAAIAAgABAAEAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
"AAD//////v/+////AAAAAAAAAAAAAAAAAAD//wAA/////////////wAAAAAAAAAAAAAAAAAA//////7//v/+//7//v/+//3//f/+"
"////AAAAAAAAAAAAAAEAAgADAAMAAwADAAIAAgABAAIAAgACAAIAAQABAAEAAQACAAMAAwADAAMAAwACAAIAAgADAAMAAwADAAMA"
"AgACAAIAAgACAAIAAgACAAIAAgACAAIAAAAAAAAA////////AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP////8A"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQABAAAAAAAAAAAAAAAAAAEAAQACAAEAAQABAAEAAAAAAAAAAAAAAAAA////////"
"///+//7//v/+//7///////7//v/+///////////////////////+//7///////////8AAAAAAAAAAAAA////////AAAAAAAAAAAA"
"AAAAAAAAAAAAAAAAAP///////wAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAA/////////v/+//7//v/+//7//////wAA"
"AAD//wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD////////+//7/////"
"/////////wAAAAD//wAAAAD//wAAAAAAAAAAAAAAAAAA/////////////////////////////////////wAAAAAAAAAAAAAAAAAA"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
#define SX_PLATING_N 49392
static short sx_platingBuf[SX_PLATING_N];
static const char SX_PLATING_B64[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQABAAEAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAA//////////8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD/////////////"
    "/////wAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAQABAAEAAQABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA////////"
    "////////AAAAAAAAAAAAAAEAAQABAAEAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAQABAAEAAQABAAEAAQAAAAAAAAAAAAAA/////////////wAAAAAAAP//////////////////AAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAAEAAQABAAEAAQABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAP//////////////////"
    "/////wAAAAAAAAEAAQABAAEAAQAAAAAAAAAAAAAAAAABAAEAAQABAAEAAQABAAAAAAAAAAAAAAAAAAAAAAAAAAAA/////wAAAAAAAAAAAAAAAAAA/////////////////////wAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAEAAQABAAEAAQABAAEAAQAAAAAAAAAAAP//////////AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA//////////8AAAAAAAAAAAAAAAABAAEAAQAAAAAAAAAAAP//////////AAAAAAAAAAAAAAAA"
    "AAAAAAAA////////////////AAAAAAEAAQABAAIAAgACAAIAAgABAAEAAQAAAAAA//////////////////8AAAAAAAAAAAAAAAD//////////////v/+//7//v//////AAAAAAEAAQABAAEAAgACAAIAAgACAAIA"
    "AgACAAEAAQABAAAA//////////////////8AAAAAAAABAAEAAQABAAAAAAAAAAAAAAAAAAAA/////////////wAAAAD//////////wAAAAD/////AAAAAAAAAQABAAEAAQAAAAAAAAAAAAAA///+//7//v/+////"
    "/////wAAAQACAAQABAACAAIAAgACAAEA/////wAAAAD+///////+//7///8AAAAAAQABAAEAAAABAAEAAAD//wAAAAAAAP//AAABAP////8BAAEAAAAAAAAAAAABAAEAAQABAAAA//8AAP//AAABAAAAAAACAAIA"
    "AAAAAAEAAAD+////AAD/////AQAAAAEAAgAAAAAAAQAAAAEAAQD///7//v/8/////v/8//7////+//7/AAABAAAAAQAEAAIA//8BAAIAAgABAAAAAQABAP////8BAAIAAQABAAAAAAABAAIAAQAAAAIAAQD//wAA"
    "AgAAAP7//v/+//7///////7/AAABAAAA/v/+/////v///wEAAQABAAQAAQAAAAIABAACAAEAAQABAAAAAAACAAIAAAD+//z//P/8//z//v/+//7///8AAAAAAQACAAIAAQAAAAAAAgABAAAAAAAAAP7///8AAAEA"
    "/////wIAAQD//wAAAAD///7//v/8//z///8BAAQABQAEAAAA//8AAP7//v//////AAAAAP////8AAP7//v8CAAUABwAHAAUABAACAAEA//8AAAEA///+/wAAAgAAAAAABAAEAAAA//8AAP////8AAAAA///+////"
    "AAABAAAAAAAAAP////8AAAAA//////7/+//7//z/AAAAAP////8AAAAAAAACAAcABAAAAAEAAQAAAAIABAABAAEAAgABAP//AQAEAAEA/v////7//P/+//z/+f/+/wAA/P/7//7////+/wAABQAHAAUACAAIAAUA"
    "BwAEAPv/+f/7//j/+P/7////AgD///z/AAABAAAAAAACAAIAAAAAAAUAAgAAAAIAAgD//////P/+/wIABAAEAAQAAAAAAAQAAQAEAAUAAAABAAQA///+/wAAAAD8//z/AgAEAP7//P/+//n/+f/4//P/+P//////"
    "BAAHAAQABAABAP//BAAAAPj//P8AAAEAAgAEAAcABwAAAP//AgD///n/+////////v8AAAAA+f/7/wAA/P/+/wcACgAHAAgACwANAAgAAQAAAAEA/v/+//7//v///wQABQAEAAQAAgD+//v//P/8//j/9f/5//7/"
    "AQAFAAgACAAIAAcAAQD//wIABQABAP///P/7//z//P/5//n/+P/1//X//v8EAAQABQAFAAEAAgAFAAEA///+//j/9f/7//z/+//8/wAAAgD8//v/CAAKAAAABwAQAAoABQAHAAUAAAD8//n/+P/7//z//P///wQA"
    "BAABAAEA/v/5//z/+f/7/wQAAgD//wgACgACAAEA+f/1//v//P/8/wQABQABAAcACAABAAIACgAEAPv/+//7//v/AAAEAAUADQARAAUA/v8BAP7/8//1//j/+P8AAAUACAAKAAQAAgAFAAEA//8CAAIA/v/8//v/"
    "AAAFAAQA///4//X/9v/5/wIADQAHAP//BAACAPn/+//7//j//P/8//v///8FAAsADQAQABcADgD+//z//P/4////BAAFAAsABQD+/wEA/////wQA/v/y//X/+f/5//v///8IAAUA/v8BAP7/8P/2//7/9v/4/wAA"
    "/v/4//v/BAACAPz/BAAEAPz/BAAFAPv/CAAZAAcAAgAQAA0AAgAAAPj/8v/w/+3/8v///wgAEAAXABcAEQAKAAIAAQAKAAUAAAALABEABQD///j/6v/v//j/5//b/+z//v/+/wIADQAFAPn//v/5//P//P/7//L/"
    "/P8CAAUAFAAWAAcA///7//b//P8EAAIAAQAFAAUAAAAAAAcACAAOABoAGQAUABQABwD+/////v/y/93/3f/5/wQAAQARACAAHQAQAPn/7f/w/+n/4//d/8v/uv+z/8L/+P8fAA4A9v/7/x8ARABDAC4AGQD//wgA"
    "LgAsAAsA4f/A/93//v/1/wIAHQAQAAQA+P/Y/9v/7//5/xcAKAAXAAoA9f/X/97/+/8OABwAFAAEAAoAFwAWABYALABAACgA/P/v//n/DQArAEEAPQAfAPv/2P+w/57/uf/M/8P/w//J/8P/zv/s/wAABQAIAAgA"
    "CwAiADEAEwD//woABAABABkAFwAHAAsAAAD4/xMAKAAoADcAPQAWANv/wP/M/+D/BAA4AEMALwApABoA/P/h/8//1//2/wsADgAHAPz////+/+z/7P/8//L/3f/j/+//4//j//X/7f/q/wEA///5/xAACwD8/xMA"
    "JgAdAAoA7P/q/wQAAgAAABEAFAAOAAEA/P8XACAACgATABcAAAD2/9j/tP/P//P//P8KABMAFAArADsAMgAQAPb////7/+H/9f8BAOf/9v8OAO3/0v/p////CAATACYAKwAaABwAHwD5/+n/BAABAOT/7P8BAAEA"
    "9f/s/+z/2//I/9j/AAAZACYAMQAvACkAIgAAANr/1//Y/8b/3f8NABMAEQAuAC4AEAD5/+b/1P/O/9L/6f/8/wQAGQAoACgALwAuABwAGQAfABoAEAACAAAACAAHAPD/0f+//8X/0f/b//X/FAAiABoADgAAAPL/"
    "/P8dACkAHQAfABkA+P/n/+//4P/D/7r/wP+//8b/6f8KABYADgDy/9r/2P/b/+r/CAAIAPX/8v/t//X/EQAUABEAMgBDADQAJgAiACsALgAQAPP/7P/k/97/5P/8/x0AMQA3AD0ANQAyACwABwDz/wQAFAAuAC8A"
    "+P/j////6v/C/8b/xv+2/7n/yP/n/wAA/P/1/+//4//q/wcAHAAiACAAJgAmAAsA8/8CACsAOgAjABAABADe/7z/w//Y//j/HwAcAA4AJQArABYAEwANAAUAFgAZAP//6f/v/xcAPgAxAA0AAADy/8//w//1/zIA"
    "NwAWAP//5P/I/8v/1f/R/+P/BQAFAPL/8//4/+f/4//t//j/AQAAAPz/GQAxAA4AAAAjACUACwATAAgA0f/G//7/HwAZAB0AKAAaAAQA/v8HABoAMQA1ABYA4f/F/7T/j/+A/6r/z//O/9H/8P8UADQARwBMADoA"
    "FgAEAAUADgAyAEwAHQDp/+f/3f/I/+z/HQAcABoAMgAmAPX/1f/b/+f/6v/m/+P/5v/w////AgABAB8ANQAUAAsAMgAiAOf/1f/A/5n/pP/I/93/7f8CABoAIgANAA4AKQAoABYAJgAuABAACAAoAB0A6f/O/9j/"
    "AQA1ACkACgAZAAEAzP/d/+//7f8QABAAAAAuACwA4P+9/7r/s/+5/8D/7/80ADgAIwA1ADgAFgDv/9v/8P8CAPX/9v8IAAAA5P/d/+z/9f/1/wUAFAANAA4ACADm/+D/CgAmACsAQwBTACgA6f/d/+//AQAaACIA"
    "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"
    "BwAoAFwAhQB9ADQA/P8UADgANQA4ADUABAC0/4b/qP/d/+b//v8CAK7/X/9N/0r/k/8fAFgADgCY/2z/pf/I/8n/9v/e/3L/Sv9s/7n/+f/G/6j//P/j/1//Rf94/87/KAAaAPL/HQBwALIAmABEAGUAlABAACAA"
    "UgBnALYA9QCzAH8AbQBEAF8AfAA1APv/IwCIAJoAFADj/0kALwCl/4z/q//j/3EA6QABAcQAAgBH/zz/XP8+/4T/PQCSAC4Atv+5/9H/n/+o//D/vP8+/xX/Qv+8/zIAFwDV/87/j/9u/+//agCDAHoAPgABAOf/"
    "uv+0/9X/z//q/w0A9f8WAD0A8v/D/7r/kP/J/xcA//8OADoAIAAXAAoAAABSAHYAMQDv/5j/bP+c/4P/Vv+Y/+H/RgC1AF8A2P8XAKMA/gAzASUB5gBqAN7/3f8RAO//0v/V/6L/Xf8j/xX/Zf+e/2b/A/+j/m/+"
    "nf4V/7H/OgBNAAgA6f/8/yMAXgCdANkA7gCsAGUAlwDRAGcA8v8cADoA4f/L/x0APgArADgAMgDs/7b/1/8rAGEANwDk//z/SQAUANr/OgBlAAcABAAuAN3/p//P/7P/ff/I/xkA6v+b/4T/e/90/3v/lf/b//P/"
    "X//q/m7/AgDj/9X/JQALAJv/vf9JAGgARwBoAHoAOgARAFwA+gBdAT8BDwHdAFgABQBrABABkAHvARAC6QGLAU4BlAHZAZEBUgF/AYQBTgFdAaoBygG5Ad0BHgI3AnsCxAJSAogBUQFsAVcBDwGqAJIAvABeALH/"
    "j/+H/xH/4v41/1n/U/+w/zsAXwApABoAJQAIAPv/FwBAAKkATwG1AacBRgHBAGsAQQArAHQA7gANAf4AIgF4AcoBsgFRATcBJQHOAMIA+gDyAPgANwFLAU8BbwEVARQAgP/a/wAAlv+P/8j/n/+E/4//bv9a/zz/"
    "Bv9K/57/If97/pD+vf5Q/vP9CP7S/ZT9J/6Q/j/+bf4C/wn/2f6L/hT+Ff5X/mr+rv7e/pD+Wv5+/qb+jv5R/kL+Cf6Q/bT9FP60/XP94f1B/o7+6v4q/3T/Wf/h/v3+MP/T/tL+R/9m/2P/h/+i/6f/dP8g/xr/"
    "aP+x/5b/C/+j/uj+g/9O/+L94vy9/Qj/Hv/t/lf/X/++/p/+Ff9a/5v/EQBKAPv/GP9X/qb+7f4d/tj9FP9kANAAfAAZAF4A6wBCAfoAw/9K/5sAXQGyAHoAKwEPAv0BUwBB/7H/m/8b/13/0f8+ABEA9P6r/i3/"
    "MP+e/0cAJgD1/0v/DP4v/if/G/92/vr9MP4O/yb/iv4A/jT9KP0D/vP9k/3t/dD9Wv0T/bH8Wv3H/uL+Uf6U/v3+1v5s/lz+1v4j/w//6v6Z/rX+bP+x/6v/3v+t/1D/cf+T/3X/XP85/zb/cv+n/73/AAB9AJ0A"
    "LgD+/38AagElAgYCPQG8AKEAnQCDABEAnv9s/yH/zP6H/jD+Qf6a/q7+ov5w/lz+6P4+/yT/lf/2//7/owAkAfoACgHmAGgAhgB6AK7/bP/4/4sA7wD3AHcAqP8v/33/5v/1/xwAVQB8ALMArQB8AHwAdACFAOIA"
    "9wCjAH8A3AAtAbkAQwDIADoB8gDlAB4BJwEKAdEA5QAbAcEAgwDLAN0A5QDXAEYA/v8FAOn/UgC8AE8AUADoANoAdgBAADIAhgDRALwArQCtAKcAfwATAOD/8v/R/9X/CgDh/9j/dwAYARwBtgB5AH0AaAApAOD/"
    "zP83AIkAWwBlAKMAlACMAIMAVQBuAKAAowC7ANoA1ACYACYA2v+W/z7/d//k/7z/Zv8Y/8/+6/76/uX+G//6/tb+n/9SAGEAXAD+/5//0v/n/wIAUgATAL//pf8g/9n+Nf+M//b/UgCCAA8BVAHaAJ0AkgBNAIAA"
    "HAFtAX8BrQEKAicC8QH1ARkCKAJgAnkCowILA5kCuQH7AZwCqQKaAogClgK9AnsCLgL+AakBxwEoAloCxgLcAqICGgNFA9ACJANqAxQDUQN7AycDLQPYAksCYQIwAtkB9AGmAbYB0wJOAwYD3AJ1AjECbAKRAtgC"
    "FAPEApMCygIOAzUDCwPYAscCmQLZAjwDwwJPAjcCfgHpAC4BlwEoAooCQwIZAugBTAEMAekApADuAEMBKAEbAQAB9AAKAZoAAgC2/0T/S//k/+//nP+M/3L/RP/f/ov+9/5U/9n+Vv4m/iD+MP4R/gz+P/4m/gz+"
    "Ef6p/UD9K/1U/RX+pf5B/hH+h/7M/rj+oP4a/wQAKQABAKQAKwHxAHYAEQBHAJ0AJQD5/5EAnQA4AAUAyf+//9T/4f9nAMsAgABnAKMAyAC8AE8AQADoANMAKwCAAEIBZwE0Ae8AswBeANX/ov/5/3oA1wDBAJQA"
    "rACOAK0AWwFvARYBKAHOAF8AswB/AK7/of/R/7f/AABJADsAQwAOAFr/1f7M/qz+Qv4S/lD+q/7o/rH+Jv4+/rL+hf5c/s3+DP/r/tL+n/5T/h7+Kv54/pT+YP45/gz+8/0s/nX+3P5T/wn/Wv5F/mf+V/5i/nX+"
    "0v6i/+T/Vv8M/wP/jf4X/hT+If4O/ib+i/75/hH/C/85/2D/jP/h/9r/gP9U/x3/If+2/9X/kv/s/wcAuf8FAOb/0P5w/pr+e/7M/g//sv66/t/+YP4J/g/+7f3w/Tj+i/60/mL+9v3t/Qj+LP4z/g/+bf4J/yb/"
    "X/+f/yP/0/4w/1r/ev+8/2v/6v7l/vn+/P5r/wsADgCi/57/v/+k/5L/bP9j/7//uf+N/w4ARgDw/zcAzgDsAN0A1AC1AFUADQA3ADIAHwCJAHcAMgDEAMoARwB6AB0Aq/+SAMQAFgCDAJ4A7f8gAHkAeQDXANMA"
    "hgCVAIsA3wCIASoBNwDM/6j/dP8s/yf/gf97/1f/m/96/yr/aP9p/x3/Dv/T/sn+XP/D/9v////q/4r/8P4+/t/90/0S/pn+uP5R/jv+af4k/q79Bf76/mL/Tv+J/8b/4/8sAFkAdgCXAEoANwCdAGgAEQBJACAA"
    "6v8rAOn/if/1/x8A0f/g//7/GgCLAKAAKwD7//7/p/+G/xQAKwCA/7r/XgCW/7T+M/+b/0f//P7K/gz/gf8X/7f+Qf9C/5H+qP41/0r/YP/S/zEALgC8/w//u/4d/7b/z/+T/2P/Ef++/sf+5/4A/1f/g/91/9T/"
    "PgA4AEQATAAcAG4AGQFsAXABJAEBAW8BOQERAGn/jP8QAIYAUgA6AKAATADO/+f/b//8/qT/8v+l/+n/QQBcAHkARAD//+b/kv+A/+T/5//A/xAAJQCV/xT/Dv91/wUALwD1/6X/9v5k/gv/1f+B/1f/9f8WAKH/"
    "bv+z/xMAQAB2AKAAdADEAHgBZwFOAc0B6AHcAfgB3QFjAj8DVAOOA9UDLQMIA6gDbwMLAzwDGwPbAggDSANgAwYDIgJzAUgBKgHoAMsAygCvAO4AnQGjAdkAfADdAEMBEwFiAPL////b//D+kP3s/FH9hf0K/XP8"
    "P/yy/OD8Lfzs+x78vvvH+xj8xfvv+w/8JPsv+w/8GPxa/FT8Jfvr+jf7kfrN+qT7kfvl+1H8zvsD/Af9jv23/Xn9df1B/nD+9v0w/mT+6v2o/Rj++v6l//7/ZQCYAC0BQgJUAf3+Ff+/AA0BsACGACsA2v+3/wsA"
    "9wCsAXwBkQAKAGUAaABQAAQB9wA3AGoAUwDF/1wA4ACPAHEA4P+A/3kA4ACVACcBSwF2AYgDqgQyAyQDeAVTBr0ElAJ2AHb+Ff2L/IX8APz9+t/6Vftt+wz84/zl/HL9tP7z/k3/zgCkAWQBCgGJAGQA2gBVAHb+"
    "0Pwx/IT8Zv3S/aX9xP3k/cb96P2r/aD9U//FAKMA+wBOAYsAFwBE/+f9zf2O/ZP8jfxI/Ov7BP1e/dn8h/3x/QP+Nf8UAGQAXgGlAtIDhgQDBcwF4AU0BuwHZQiVB98HfwgBCRMJDwiPCB4KnAmQCcMKFgqWCXMK"
    "DQq/CZYKUgpjCfQIkghfCE0IIghHCJgIagi1B4sHvwfABnoFNQW9BGMEUATQAioBowA1AJQAIgFGANH/EACT/xj/df6o/YL9n/zd+4X8p/sW+o76Q/rY+Mj4kPjp9xr4Avh99xL3q/YJ92z3M/eW9+f34ffs+MX5"
    "RPnp+Eb5yPmn+f74dvnp+r/7B/zi+8L7i/x8/WP+mf8dAGcA6QA4AGX/8v+bAC4B3AHKAakBzgHHAWEC4QJCAukBxQH9AOwA4AHWAk4DwwJXAu4CugJsAsAD/wNsAvQBDQKkASUCxAJzAloCiAKfAo0CmQGSAN8A"
    "oQGWAd8AlwD0AHEAdf/X/44AjgCGAEkAXADfAEoA7f+AAJ//Wf4m/kb97vzt/Yr9lPzA/Ir8LfyR/ED9LP6B/hr+0/33/ED8BP1m/Qn9+/xV/Ar8/vxD/RD9Tv0t/Uj9ZP2u/N38u/1b/fL8cv0j/kr/sAC+AUUC"
    "ygGRAaMCogJDAYIByQLVAtUChQO7A8cDCwT2AxQEQgTEA7EDAAR5A3IDfQTZBI0EPAQABJgESAU5BVYFYAXLBC0ETwOiAv0CSAPoAj8CSAE9AXUCFQPxAg4DzQLLAbAB+gK9AscAGAFgAgABGgA2AZkBNALGAn8B"
    "MAEWAqcBcwFPARAAGQB0AIz/5/96AM7/DAHqAtEBmwBqAXYBJgCx/0MAjABnAA0Ah//Y/0UBJwLIARUBBwG4AdQBFgGzAFUAFgAHAXkBggBoACgBXQGkAfoBlAHoAM0AKwGpAC3/Av8QAAIAbP+b/8z/FwC+AHoA"
    "rv/m/ygAa/8P/4z/wv+D/0f/jP/7/5z/y/8rAfEA6f/uAGYBKwAEALT/qf6J/3oA2v96AEIC4gKXAvcBrwFPAoECsgFCAY0B0AEAAnICWgKCAX4BYALVAj8DkwMjA+ECkAKFAXwBaQI6AqEBzgEMAvIB5gEHArgB"
    "zQC2AJABhwG7ABcAJ/9n/m/+G/6N/QL+pf5N/qP9VP11/bL93Px9++n7Mf3v/Pr7YvtE+xX8WPzE+8380v2a/Bn8w/wf/IP7Ffy+/DP9sfyR+837ovy6/AH9tf1E/mP+pv2X/Bn8B/xC/Mr8NP1j/fH93v7V/tn9"
    "wf1i/i3+Tv3E/Dz9P/7z/rn/7P/w/hf/MgDh/9j/tQBYAB0AtgCdALsA+wCaAPcAVQEAAYcBEwI/AiYDRgPJAnADsQPoAl0C/QHiAYIBsgA0ARUC9QFvAjwCVQCt/5L/O/7k/S3+dv1w/Qv+vf06/T390v1i/t79"
    "E/0S/S79H/0Y/SH9cP3b/e79cP39/LX9jf5F/oj+Nv9Z/o39I/5c/uv9wP0R/qX+Z/7H/Vn+GP8g/y//tf7f/bj9Rf2Q/A39mv3g/PX76PuH/CL9hf0F/m3+x/5Q/xH/Jv5C/u7+pv4h/iP+Gv4F/mr+C/90/5X/"
    "SP/Q/rf+cv7i/UX+G/82/33/9f8BAMUAjQFGAcUBcwKpAcsB6gI0AhIBDAHpAAwBuAEbAqUCEQMJA4gD7QORA4cDeAPVAngCiAIAA/QDZQQUBNkDmQMIA8oCHQMAA/4BSQFgATwBBwFUAWMBLgEiAesA4AASAXMA"
    "mf+W/4H/5/5m/pn9nfzD/JH9bP2u/IH8pvx//B787/tL/Kj8APzU+qb6Q/tb+xD7NPt5+9D7pfwV/Wr8oPtG+zH7fPu++877OfyI/IH85fxL/Rn9Mf0D/s3+q/66/Z39tP4w/97+5/4M/9/+vv7c/iH/gf8OAIYA"
    "egB6AH0A5v+6/wAAZf93/7MAdgDg/y4BKAK+AX4BOgHxACUB9QBxALIAfAHTAWwBHwF4AdEB+AG8ATkBqgH7AX0Arf83AJn/L/8gADsAOwDNABEAp//KAK8Ap/89AGMBhAFVAW0BcAE9AWwBzgGnAd8B3wIUA6UC"
    "AwOEAzADjgJdApYCkAI3Au8BrwH+AfQCQwPoApQC5gGBAUACTwOTA0ICiAABASICeQFbARMCdQGBAYgCewEgANEARgG8AFsALACAAM0AZQBBACkA4f/EAKABzQAgAGUAhgC/AL4AEQBAAGYBWwE7AKMAcAJtAv0A"
    "igHJApEBuv+//6cArwB+/+H+kv+n/9f/lgEQAosAv/9N///+DgCeAAoAmACEAdwAlf87/7r/6v/1/8cAMAHQAO8AuwB9AN0BxgIKAqwBeQFkAfoByAEMAsYDQwPlAF4AjwCSAOwAxv9m/qL/MwEqAQ0BFQHQABMB"
    "zgHZAW8B4gE3AjwB+ADIAYcBSQEJAvoBagEkAYYAagAGAZcBNAK5AXMAoACgAFn/mP+wAH8AUAAAAAL/R/92ALkAQADp/4IAgQFOAU4ByQKoA8kDtwQ7BZIEVAQpBUMGrQaGBroG5Aa/BqcHyAgiCEQHggdZBxQH"
    "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"
    "AQCjACEBOQEbAc4AoACbAEQA0v+Z/2P/UP+E/8//OwC2AN8AngBMAEYAKAB9//z+If9I/03/gf/O/xEARwCPAOMA0QCVALMAoAAXALb/aP/3/v3+5v+hANf/sv5Q/2oAHwC6/z0AuAABAW0BuwGjAdwA4P/G/2UA"
    "wQCzAJIAXwDa/0T/Y/8OAFAADQDY//D/LgA+APn/q/+u/8n/tP/Y/0YALwC2/67///9nALkAagAAACMAEQCe/13/Lf8S///+1v5x/4sAowA3AEAAGgDX//P/2v9y/0j/L/8h/4f/JQBYACkAXwAKAfUA1/81/6T/"
    "+f/q/yUAcQB8AIYAPgCt/43/hv8+/5D/XgDpADwB6QAQAOz/GgC0/1r/Lf+7/qn+S//2////M/+K/uv+kv/a/xAAAQDS/xMADQBi/1T/BAAAADn/Bv+r/1MAiACLALYAvABKAP7/KQACAFH/7f4b/03/MP9K/x8A"
    "2gDCAOMAggGTAW8BlgEfATcA5v/I/2L/Qv+B/5n/aP84/2v/BQBWANL/Nv9Z/3v/MP+x//UAEwHk/z//j//G/6T/kv9p/x3/Tf/4/20AowCjADgABQB5AIgAzP9g/43/jf9g/4H/zv/s/93/0v8FAG4ApACaAJcA"
    "rQC2AIUAPQBYAKwAsgChAI4ARABGAHAA9v+V/+b/3f+c/zQA0wCjAFwAXgBtAI8AegAAAKf/wv8NADsATwB/AH8ABQDm/48ADwH7AN8AswBVAN7/XP9d/+f/4f9p/3T/nP+t/yIAVgDk/6r/nP9j/6L/DgDm/8X/"
    "8v+w/27/z//s/2P/O/+E/0T/mf6o/mD/hP8b/y//jf+H/2j/lv8aAJcAiQCIAB8B+ACt/zP/+/9SAAcAGQBTADUAFwAOAK3/Qf9R/33/e//G/zUAHQDI/9j/EwA0AEQA8P9I/wn/Nv+S/zUApAB5ADQACAAQAIYA"
    "0AB0ABEA5v/O/y4A/QCQAZQBJQHKAN0A5gCzAKkAfwAQAPv/GQAxAL4AMwHTAGsAZABPADgAMgBnAHQAMv/t/c/+MgA6AMv/Lf91/r3+1f/ZADQBPgAU/3H/cQCbADcA2v9m/9X+Yv4+/qj+uv9VAHH/Yv6g/nL/"
    "zP+J/wv/AP+c/6kAwQHxATkBsgCCAFMABABj/0L/KwDBAH0AUgBSAFAAMQCh/5D/hQC7APv/RAA8AQ0BTABcAOkADwHuAE4B2gFYASIAn//I/zEAxwDcAJIAxQDTAFkAQwA0AMD/9v99AFwAbgDlAJoACwAWAPX/"
    "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"
    "DQABADUAWQA6ACMATABRAB0AIAA9AC4AJAAXAAoAIQAvACgAEADR/8r/9//b/9//KQARANT/8P8CAO3/z/+u/9f/JgA2AC8AGwDy/+z/5P+2/7b/3P/k/+b/5//M/7r/xf+z/4v/jf+f/7P/7f/z/6//rf/V/9H/"
    "4f/1/+P/+f/6/63/tf8VABEAvP+v/+T//f/6//r/+v8XADgAJwAvAFoAOAACAAoAKgBTAFoALQArAC0A/f8CACYADwDv/87/vv/p//3/+/8iADAALgBOADgA8v/o//L/x/+2//j/HgDy/+D/9f/i/+X////N/7L/"
    "4f/S/7H/7v8iAAsA9P///xwAIgALAAoABADv/wsALQAgABUA+//S/+r/DAADAAoAFgAaADcAJQD8/xEAGwADAAEADQBEAEYA1//Q/wwA0f/6/2QACQDZ/w0Ar/+P//P/5//b//n/3/8nAFwA7//j/woAxf/k/04A"
    "UgBeAGoAKgAcAD8AGQDc/+b/EAATACgAZgBVAPr/xv+///H/IQDw/8f/4//m//L/DADm/83/6f/s/97/6/8HAAIAyP+w/9H/zv/C/9f/4v/m//P/AwAtAEMAJwAXACgANwAxACEAHAASAAwAJgAmABAAIAAuAB8A"
    "AwDn//z/JAAkAC0ACwCY/4v/3P/v/wUAGQDu/wAAIwDo/+L/MQA3ABgAIQAXAPX/4P/j//z/EQAPAPv/6f/0/wIA7//U/97/8f/j/9v/BgAuABoA8v/i//D/AAD1/+//CQATAAoAFAASAP//8//h/9L/5P/z/wUA"
    "HwAEANn/8v8YABMADAAQABcAKgA0ADkANAAHAPb/FwAJANn/8v8YAAYA6f/z/xUADADW/9T//v8PACUALwAZACYALgAOACQAOQAEAAoAOAASAP//IQADANL/5/8AAOz/3P/x/wUA3f+o/7D/zv/q/wYAAAAHAD8A"
    "RgAeADEARQAbAA8AOABOADQAAwDv//f//f8GAAUAAQAaABIA5P/u//X/4P8FAAsAx//L/9j/pv/W/yoAFgAiAEIA9f/B//H/AAABACwANQAZABgAJwAUAOj/9v88AEAAEwAuAEoAEAD3/xIA+P/j/xYALQAUABcA"
    "DwDk/83/1P/p//X/7f/x/wYAEQAQAAIA8//8//z/9v8OABcA+//z//j/+P8IAAsA6P/Q/9z/5v/e/+D/9//x/+D/9/8HAPf/9/8AAPD/3//r/woAFQAHAPv/5v/f//X/4P/K//P//P/Z/93/1v/M/+T/0P+2/+L/"
    "8//e/+//9//2/xoAKQAEAO3/AAAaAA4A+f/+/wkAFgAmAB4AEAAVACQANwA3ACUAFQAEABEAIADy/+H/AADh/8//9v/c/7v/1f/S/+H/HwAdAP3/BAACAPv/AAD0//n/EwARAPj/8/8LABsADwAcADoAHQD0/wMA"
    "DADy//r/EgAHAAAACwD9/+//AQAFAPT/8v8AAB4AJwD2/87/3f/5/wwABAD7/xgAFwDu/wQAFQD4/xIAIQDt//X/DQDq//P/DAD2/zgArwCVAEAAJQACAPr/SwB9AFQAHQDx/9r/z/+f/43/uf+8/7r/+P8cAA0A"
    "9P/T/8v/yv/E//v/KgD6/9T/1//R/93/5//o/wEA+v/e//X/AwAAABoAHwAPABIAHAAQAOb/5P8yAE8AHwAVABQADwA6ADQA5P/S/wUAJgAbAP///f8LAAwADQADAPj/EgAqAB8AGAAQAA0AFgABAO7/AQANAA4A"
    "HwAsACsACQDN/7v/3//q/8r/uf/B/8H/2/8JAOb/nP+2//L/6f/O/8//3f/U/7D/tP/M/8D/2f8BAOf/2//5/+X/zf/V/8j/yv/4/xUADwAMABkABADH/7//7P8IACEAIAD3/+3/8v/p/wEACQD3/xkAPgApABgA"
    "FgAlAD4APQBFAFIAOwBAAFAAGQD4/w8ABQD0/wMABwAKABkACwDj/87/4P/w/+P/7f8QABUADgASABYADQDu//n/PQA9ACEAWQBZAAYAFABIADUAOABFACIADgARABMAIgAaAAUACwAPAAIA7v/T/+L/+P/T/8X/"
    "5//G/5L/sv/f/9j/1f/s//L/1//Z/wcAGgAOAB8ALgAfACYAPgAxABQAFAAlACcAFwAVACAAEwABAA4AHwASAP7/9//5//7/AwD//+r/2v/a/9X/z//V/9r/2//l//T///8GAAYAAADw/+D/7f8AAO7/4v/2//n/"
    "7v/x/+//7v/5//r/AAATAAgA8f/0/+r/2//w//f/5v/q/+j/4P/8/xMACwAKABoAJwApACEAGgALAPf/8//u/+r/BgAhACkALAARAAAAHgAbAPD/+v8VAP7/6v/7/wMA7P/Z//H/DAAOAA0ABQD8/w0AHwAbABoA"
    "EAAEAAEA8f/s//j/7v/r//f/7v/4/xQABQD0//b/7//3/w4ADgD8/+j/7v8DAAAA+/8HAAwAGQAkAAkA9P/8////AAADAP3/+f/w/9//0v/Q/9//9P/6/wEABgD9//T/6v/i/+n/5v/t/w4AEQD+//7/9//s//L/"
    "7f/r//T/7//w//L/5P/l/+j/4f/6/xoAGgAbABcAAQAAAAgAAwAGAAoA/P/2//n/6//j/+7/6f/o/wAABAD+/w4ABwD0/wgADQD2/+//5f/k/wAA+f/j/+3/4v/Y//X/9//m//b/AQADAA8ACgD+/wEAAgD/////"
    "+v/z//7/DwAAAOj/7P/3////AADv//z/JAAOAO3/AgD+/9v/5P/1//j////6//n//f/m/+P/+v/7//b/+//t/+P/8////wEACAAOAAwACQAJAA0ADgAIAAMACAANAAcAAAD7/wAAEAABAOD/4v/x//n/AwD0/+T/"
    "//8EAPD/9//0/+b/BQAeAAwACgARAAwAEQAPAP7//P8HAAgABAADAAYACAABAP7/BgAKAAAAAAALAAgADAAVAAAA6//s/+v/+P8QAAYA9P8BAAYA9P/t//T/+f/9/wQABwAAAAQAEwADAOv/9v8CAAAACAANAAQA"
    "BAAFAP3/+P/2//b///8GAAYABwALAAYA+v/0//b/8v/8/xQAFQAAAP3/AgAGAAYA+v/8/woAAAD3/wYABADx/+v/6v/z/wIA9f/l//P/+v/1//v////4//b/AAAJAAYABAAMAAgA//8AAPz/9//+//7//f8DAP7/"
    "9v8AAAIA+v/3//z//v/+/wIABgD+//j/AwAIAAIAAAAAAPv//f8EAAkABwD9//r/BQAEAPD/6v/8/wAA9//6/wIAAAADAAkAAQD9/wUAEQANAP////8OAAUA9v8GABYAAQD3/wsADgD2//n/FgATAPX/8f////7/"
    "+/8FAAUA+f/9/wkAAAD5/wQACQD///v///8AAAAAAAD8//v/AAAAAPn//v8AAPj//P8LAAUA+////wEAAAD///n/+/8DAAMAAgACAPv/9v/7/wAAAQABAAQABgADAAEA///8/wEABQACAAkACgD8//f//f/7//v/"
    "AAABAAEAAAD7//r//P///wEAAgAAAP7//v8BAAIAAAD+/wAAAwADAAAAAAAAAP//AQAGAAIA//8AAAAA/v////////8CAAQAAQAAAAAAAAD///////8AAAIAAwABAAIAAgAAAAAAAAAAAAAA//8AAAEAAQAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
#define SX_ARMOR_N 35280
static short sx_armorBuf[SX_ARMOR_N];
static const char SX_ARMOR_B64[] =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAD9/wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD9/wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/f8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD9/wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAP3/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP3/AAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/f8AAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD9/wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP3/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP3/AAAAAAAAAAAAAAAAAAAAAAAAAAD9/wAAAAAAAAAAAAAAAAAA"
    "AwADAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD9//3/AAAAAAAAAAAAAAAAAAAAAAAAAwADAAAAAAAAAAAAAwADAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/f/9/wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/f8AAAAAAAAAAAAAAAAAAAAAAAD9/wAAAAADAAMAAAADAAAAAAAAAAAAAAD9/wAAAAAAAAAA"
    "AAAAAAAAAwADAAMAAwD9//3//f8AAAAAAAAAAAAA/f8AAAAA/f/9/wAAAAAAAAMAAAAAAAAA/f/5//3//f/9//3/AAD9//3//f/9//3//f8AAAAAAAAAAAAA/f/9//3/AAAAAP3//f8DAAMAAAAAAAAAAAD9//3/"
    "AAAAAAMAAAAAAAAAAAAAAAAAAAAAAAAA/f/9/wAAAAAAAAAAAAAAAAAAAAD9//3/AAAAAAAAAAAAAAAAAAAAAPn//f/9//3//f8AAP3/AAAAAAAAAAD9//n//f8DAP3//f8AAAAA/f/9/wAAAwD9//3/AAAAAAAA"
    "AAAAAAAA/f/9//3/AwAAAP3/AAADAAMAAwAAAAAAAAADAAMAAAD9/wMAAAD9/wAAAAD9/wAAAwAAAAAAAAD9//3/AAD9/wAAAAD9//3/AAAAAAAAAAAAAAMA/f8AAAMAAwADAAMAAAD9/wAA/f8DAAcA/f8AAAMA"
    "AAD9/wMAAAD9/wAAAAAAAAAA/f/9//3/AAADAAAA/f8DAAMAAAAAAAAA/f8AAAAAAAADAAMA/f8AAAcAAAAAAAAAAwAAAAAAAwADAAMAAwAAAP3/AAAAAAAABwAHAAMAAwADAAAAAAAAAAAAAAAAAAAAAwADAAMA"
    "AAAAAAAAAAAAAAAAAAD9/wMAAwAAAAAAAwAAAAAAAwAAAP3//f8AAAAAAAADAAMAAwADAAMA/f/5/wAABwAAAAAAAwAHAAsABwADAAMAAAAAAAMAAwADAAcACwAHAAMAAwADAAMAAwADAAcABwADAAAA/f/9/wAA"
    "/f/9/wMABwADAAAAAAAAAAAAAAAAAAAAAAAHAAcAAAD9//3/AAADAAAA/f8DAAAA/f/9/wAAAAAAAAAAAAAAAAAA+f/1//n//f8DAAMAAwAAAAAAAwADAAAAAAADAAMAAAAAAAMAAAD9/wAAAwAAAAAAAAAAAAAA"
    "AwADAAcABwAAAP3/AwAAAP3/AwAAAP3/AAADAAAA/f8AAAAAAAD9//3/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAwD9//X//f8AAP3/AAAAAP3//f/9/wAAAwD9//3/AAAAAAAAAwAHAAcAAwAAAP3//f/9/wAA"
    "AwAAAP3/AAAAAAAAAwAAAAMABwADAAAAAAAAAAAAAwAHAAAA/f/9//n//f8DAP3//f8AAAAABwALAAAA/f8HAAcAAAAAAP3/AAADAAcABwADAAMAAwADAAAAAwADAAAAAAAAAP3/AAADAAAAAwALAAsAAwD9//3/"
    "AAADAAAA/f8AAAMAAAD9/wAAAAD9//n//f8DAAMAAAAAAP3/+f/5//3/AAAAAP3//f8DAAMAAAD9//3//f/9/wAABwAHAAMAAAAAAAAAAAADAAMAAwAAAP3//f8AAAMAAAD5//n//f8AAAAA/f/9//3//f/9//n/"
    "/f8DAP3/9f/1//3/AwADAAAABwADAP3/AAADAP3//f8AAP3//f8HAAAA/f8AAP3/+f8DAAcAAwAAAP3//f/9//X/+f8AAPn/+f8AAPn/8f/5//3/+f8DAAcA/f/9/wMAAwAAAAAAAAADAAAAAAADAAAAAAADAAAA"
    "/f/9//n//f8AAAMAAAD5//3/BwAAAAAAAwD9/wAAAwD9//n/AwAHAAMA/f/9/wAA/f/9/wAA+f/5/wcAAwD5/wAABwAAAAAAAwAAAAAAAwADAAcABwD9//3/AwAHAAAA+f/5/wAAAAD5//n/+f/1//3/AAD9//3/"
    "+f/5/wsACwD9/wMABwADABMADwAAAAMACwAHAA8ADwAAAAsACwD9//n/BwADAPn/9f8AAAcAAwAAAPn/9f/9//n/+f8LAAcA5v/t/wcA+f/T/9r/AwALAPX/6v/x//3/AADq/+b/9f/q//H/CwD9/+r/AAAAAPX/"
    "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"
    "uPsM/TgFbgVv/SP7KQHVA8f+4vsLAV8FHgFG/Nr/NQRR/xz4J/qFAmYGcgLa/M/81QAPANP57vjH//gCaP/A/mcCUwM1AZL+Pvyl/a8A2v6W/cIClASt/if8fgBIA9P/mvrA/IEF0QZR/pL6Pv+zASf+aPsb/9ED"
    "bgA++/n/ygZIAyf8ofx9AhYFnAFk/qgARAO3ACf9b/70Am4DRf3f+Kn8XwFY/3v7d/19A5gFzgBd+zr8XwHhAqH/WP9TA6cC4vxV/JgBMQPH/vX6G/0PAvgBqf3i/BD/WP68+537sP31/dv68vjA+xf/0/6t/Jr7"
    "c/yO/WT8jvoc+978c/17/eL+9ACs/zb7xPrL/zUCtP6H+9P9+AHZABD80/s+/+r+uPsr+5392v+8/mT7+frP/M/79fmD+03+Qv9z/gj9Nv3e/u79d/pC+hD+kv+8/P36e/1UAJb/x/wM/Pn9f/42/P37gv+rADb9"
    "HPtR/Sf/YP0f+zb8FP8y/4r8h/sQ/cD9g/yd+5L8jv75/tf88ftY/kn/OvwB+ub7Nv9nAAH/J/7L/1AAxP2l/LD++f/e/sv98f74ADEAc/2O/bz/b//H/Rj+b//P/x//g/42/24A8f/m/Sv9+f7GADUAH/+s/3IA"
    "JgDW/wcAVwBbAMP/mf+vAEgBCwCl/jb//AA5AaX/Sf/CABoB2v9z/4kAoAE9Ae3/8f9QAaQBnAAWAI0AzgAtAMv/4QDKAdIA6v+gAGcBPQHCAJQAJgHoAWoBPQAqAPgAOQHpANkAHgE5AWoB3QHSAVsBWwFfAfwA"
    "UwH/AcYBlAESAg8CpAGkAasB0gELApgBUwEpAqAC/AF2AVcBXwGNAXoBZwGcAVMB8ABjAaABDwGvAKgA5QCJAa8BNQEWAfQAcgCgAHYB1QG+AV8BswByANIAAwHCALcAFgFMATkBIgHGAFAAjQDsAFQAvP8mAKsA"
    "lAByAHYAVwBbAMoAEwGkACYAXwC7AKQAcgBfAF8AgQB+AEAAVwCzAIUA3v/T/3IAkQA5AIEA5QBjAMD/1v9MAJEAVADx/yoAkQBIALz/gv/D/2oAswBbAC0ASAA1AP3/uP+l//n/PQAmAOL/lv+w/yoAKgDe/+3/"
    "0/+O/8f/AADt/w8AMQD5/7z/y/8mACYArP/a/1cA8f+l/xMA5v9R/3//z//t/0wAXwADAPX/QACBAGMA5v/A/+b/4v8AADEAAADm/w8A7f+4/7z/pf+W/+b/RABIAPX/5v85ABMAqf8DAIkASADa/8P/0//9/+b/"
    "f/93//X/PQAiAA8AGgAeABYAGgDx/6z/y/8HAOL/wP/5/zkAVwA5AAAAHgBMAAAAy//x/+b/y/8AABMA9f8HABYA9f/x/yYASAAaAMv/2v9MAFcAy/+W/w8AZwAtAOL//f9AACoAx//P/0gAXwALAPH/BwAPABYA"
    "DwADAA8AAwD9/zkAagA9AO3/4v8AAAMA7f/9/wcAAwAxAEAA5v/D/xMAMQAqAFQAWwAeAAMAEwAmADEAEwDq/xMAbgB+AEAAAADW/+L/IgA1AAcABwA1ADUAHgAaAAMA8f/9//H/+f8qADEAAwAAAAsA9f/e//n/"
    "HgAiACoAJgDm/7D/5v8mACoAOQA5AP3/3v/1/wAAGgATANr/7f85ADEABwAPABMABwD1/8v/6v9EADkA3v/W/xYAOQAiAP3/AAAmAB4A9f/i/9b/4v8AAAMAAwAWAAcA6v8AAB4ACwDi/8D/uP/W/+3/6v/t//n/"
    "AAAHAAAA8f/5/wMA9f+8/47/wP8PABYA/f/9/wcAHgAiANr/uP8AACIA5v/D/9r/6v/m/97/3v/t/wsABwDm/+L/BwAAANr/2v/q/+L/9f8aABoAAwDt/9r/3v/q/+b/4v/P/9P/FgAqAPH/7f8PAOr/1v/9/wMA"
    "6v/m//H//f/9//H/4v/W/97/7f/q//H/BwDx/8v/2v/t/+b/8f/5/+r/8f8DAAcA8f/T/+L/AADx/8//1v/x/xMADwDi/97/BwAPAPX/5v/m/wMAGgAHAPn/AAD9//H/8f/9/wsAAwDx//3/BwD1/97/5v8AAAcA"
    "8f/e//n/CwD5/+3/+f8AAAAABwALAP3/7f8AABoAEwAHAA8ADwADAP3/+f/9//X/6v/5/wcAAAD9/wsABwD1/+r/7f/9//n/7f/1//3/8f/1//3/AAADAAMA/f8AAAMA+f/1/wMACwAHAAsACwADAP3/AAAAAAMA"
    "BwAHAAMAAwALAA8ABwADAAcACwAHAAMABwALAAcABwALABMADwADAAMABwAPAA8ABwAHAA8ADwALABMAFgAPAAsAEwATABMAFgAWABYAEwAPABMAEwATAA8ADwATABoAGgATAA8AEwAPAA8ADwAPAA8ADwAPABMA"
    "EwAPAAsABwALABMADwALAA8AEwAPAA8ABwADABMAFgAPAA8AEwALAAcABwALAAsACwAHAAcADwAWABMABwAHAAsAAwADAAAAAAADAAMAAAADAAMA/f8AAAcABwAHAAsAAAADAA8ACwADAAAAAAADAAMAAAADAAsA"
    "BwAAAAAAAwADAP3//f8AAAAAAAAHAAcAAAD9/wAA/f/9/wMAAAD1//n/AwADAAAA/f/5//X//f/9//3/AAAAAP3//f/9//n/+f/9//3/AAADAAMA/f/9//3/+f/9/wAA9f/x//3//f/9//3//f8AAAMAAAD5/wAA"
    "AwADAP3/+f/1//n/+f/9/wMAAAD9//n/+f/5/wAA/f/1//n//f/9//n/9f/1//n/9f/x//n/AAD9//n/9f/x//n/9f/x//n/+f/x//X/+f/1//X//f/1//H/8f/t/+r/8f/1//H/9f/1//H//f/9//n/+f/1//H/"
    "9f/1//H/9f/1//H/9f/9//n/9f/1//X/+f/1//X/9f/x//H/+f/5//n//f/9//3//f/9//3//f8AAAAA/f/9/wAAAAAAAP3/+f/5/wAA/f/5//n/+f/1//H/+f/9//3//f/9//3/AAAAAP3/AAADAP3/+f/5//n/"
    "/f8DAAAA+f/9//3//f8AAAAA/f/9/wMAAAD9//3/AAD9/wAAAwADAAAA/f/5//3/AAAAAAAAAAAAAAMABwAHAAAA/f/9//n/AAADAP3/AAADAAAAAAAAAAAAAAAAAP3/+f/5//n//f/5//X/+f/5//X/9f/9/wAA"
    "AAD9//3//f/9//n/+f/5//X/9f/5//3//f/9//3//f/5//X/+f/9//X/9f/9/wAA/f/9//X/9f/1//n/+f/9//3/+f/1//n//f/9//n//f/9//n//f/9//3//f/9//3/+f/1//H/+f/9//X/+f8AAAMAAwADAPn/"
    "9f/1//X/9f/5//X/9f/5//X//f8AAPX/8f/9//3//f/5//H/9f/5//H/9f/9//n/+f/9//n/+f/5//X//f/9//n//f/5//H/+f/9//3/+f/5//n//f8AAAAAAAAAAPn/+f/5//n/+f/9/wAAAAAAAAAAAAAAAP3/"
    "/f8DAAcAAwD9//n/+f/1//H/7f/1//X/9f/5//3/+f/9//3/+f/9/wMAAAD5//3/AwAAAAAAAwADAAAA+f/x//n/AAD5//X/+f/9//n/7f/t//X/+f/5//X/8f/1//3/9f/x//X//f8AAP3/9f/x//n/+f/5//3/"
    "AAD9//X/+f/9//3/AwADAAsADwALAPn/9f/5//X//f8DAAAA/f/9//n/8f/x/+3/7f/1//n/9f/5//3/9f/5//3/9f/t//H//f8DAAMAAAADAAAA+f/1//H/8f/1//H/+f8DAAMAAAAAAPn/+f/9/wAAAAD9//n/"
    "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"
    "/AAHAJEACwJmA84D4QO3BIEFJQWvBPQE1QQSBBoEgQXKBkAGWwQpAzUDgQPCAxIERASrBJwF1QbsBxYI3QZMBRoEGgOkAlADowTGBbYGSwgaCjwK/whXCIkHfQVTBDEFhQa6BwcJ1QlHCpgKCgoSCR0J8Al5CrYK"
    "CgsOC8UK/wrRC0AMgQu6CUAIWwhXCUsKrwvcDIQM3AtHDPcMCg1TDEQL5ArzCogKHQoGChIK8wpXDNEMkAwKDIwL+wvNDMkMrgyYDJQLUwpLCZwI4AlADLYMvgshC3UKBgrFCjAM1Q37DlMOcQwhC+AKlAunDIgM"
    "mApXCJAH6Af3B80HDghbCJAIzQiYCJQIfQktCikKlAoKCykKSAiQBnkFXwUDBk8G+wWUBQsFQAT4AxIEdgOUAnYC3QLZA+gFcgenBroEfQPlApQCSAJ9Aa8A1QDZATEDHgSzA/8BXwALAOkAagFEAVMCbgTVBFcD"
    "HgKNAZwAz/64/JL7rftR/FH9Rf4n/tf8nfvu+2j9bP7m/lz/8f7m/aH9x/1Z/Yb8wPuO+6X7J/sn+3P8GP0n/FX7J/sf+0b7Bfvy+Rj5RvnT+bT5y/j598j3z/cB+FX5OvuK+1n6vPkn+gj7H/wb/Cv6Cfhd9xz4"
    "OvkJ+V33OvbA9rz30/dZ93T39vdd+Nf59fzT/+UA+f97/d76I/n998D3kvie+QH7GP0j/1cAAwCl/X/6vPgz+Qj7sPyZ/Un+y/4j/3v/sP6G/FX7z/vT/I7+GgBs/5b9x/wM/T7+I/8U/lH9rf6S/1z/of9o/2z+"
    "3v0j/cv8jv7KAIkBJgEmAOr/cgHhAokCbgHlADUBygHKAUwBDwFqAeUB5QEPAqsCcgKJAWcBZwJXBMYFnASnApwCTAP7A8oEzgP4AWMCMQTZBVsHqwZXA40Aqf+BABYDMQVABRoFzQWUBi0HMQcpBokEcgPsA3IF"
    "pwZbB80G0QOzAD0A/AAaAfgAOQHKAjwFtwXlAzUCTAEeATUCTANTA0wDswPZA4kDZgNIBAMFxgPZAUABWwF9Aa8BJgELAKX/PQBfATkCEgLGAQMC+AE5Aq8DkARyA7cB7AAPARYBNQBg/0X/DP8M/wcA+ACvAcoC"
    "4QJnAR4AXP/D/tr+Rf9g/2j/1v8iAVcClAG0/3/+aP3a/AH+Sf9o/yv/2v7A/lX/f/+d/h/+0/6s/7z/Rf/e/rj+YP/8AB4CMQIaAmcBNv8v/fH8b/0r/Zr8SfyS+3D6HPry+t77L/w6/Jr8I/1//eb9aP6O/lj+"
    "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"
    "pfhh9of7QAG4/an2YPmgAX4ADPh791X/BwGh+Z71xPkv/XD6wPZ/+AX9vPrf8NjswPTT+rX0M+7y8+r8cPsY9NPynvY69lLxrfFG+qn/wPmx8VXzf/sv/Zr0mu9/+X0E2v7I88j2Nv+d+2z0xPnKA6sBYfYB83P9"
    "TAY+/prxCfUU/3v9mvj5+0wB3QKs/9/48vZG+ur4ufYn/MoC3QOW/3v5DPtTAkAAM/pIAEsJygXT+wz6IgXYDhoGkvi4/ewJagiNAVMBlAHH/Zr7tP7oAjUCJ/75/TUCSAN//pb6CPzT/40DTAWBAY79LQEpBcYD"
    "lAanDV4LygC0/fsGAg8aCZL/6APNDS0MkAWJBQ4HRATZAPsCiAvBEEAK+ADdAHkFfQWcAvsDxgmyDCkJmATZA0AF6AWcBBYDtgayDKMHaPpC+2YKJQ/0BWMB6ASnBvQD0gESBUQIPANz/5QGUwyMB0QAx/3RAyUM"
    "EgfP/B4B1QaZ/vn4pf6BAxYDhv93/b4CVwZv/6n6twDwBkAEmvyt+tP/QABR/MD/VwVyA53/Af4j/3kEQAQ6+p73mAF9BrcA5vx+AEgDEP9k+m/95QNXBasBF//T/978zPR09GT++AREBA8APvqW+gH/3/lZ84f5"
    "agCoAP8CFgP5/bD99f3y90L29fql/D7+RARfB40AYfTy8e78qwDA9cTyTf3GACv5rfNK9aH5f/jI8mH2AABz/P7xtfO4/M4BMQGe+Sv0GPsxAQz8DPmS/C/7IPX28q31QvfQ8T/rEe+x93/3EfDv613xaPtz+ovt"
    "dekv9C/8K/nA8xz0Nvhd9m3uue259a35zPfI9tv1zPOL8v7wePDf83D0IO3J6d/wfPNK7NDrg/TX9Wnu7+p47Ubv2OyL6cXrL/Fh8Zftwex08Jb1/fTj7Xjuy/lN/Q325vVs/JL5+u9K7134J/0v9sXs2Owk9K34"
    "qfg6+FH2vfCH7G3uXfO895r6UflZ9MzylvZZ9+rxwe+D9fX5J/fQ8zr28fuO/WT5z/l2Aa8BH/y3AmYLBwMr+P35of3m/Ob6Rvg6+vX9SvnI9bj8Lv86+Xf53v2Z/fn9agELAxIE2QY4Bvn9xPYv/asJKQoxAkX/"
    "Sf/E/U39lv0I/s/9rfpG/PQF9AZN/HP6zgEpA/QCUwbRBFsAAf9v/RT/qwUtBE37d/vCAkQFBwThAQcAoAP3CcUKjAfdBgoKkAwaCkgHpwinBiP/sP0aBLoGUACi92H4nAKQBS/9I/vOA0sJUwf0A2oEHQpDDQoJ"
    "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"
    "7AiUBq8F+ASkAm4DUwU8BGYEJQfVB7YG9AbVBwsH+wQ8BZQIyQmYBq8DkAM1BLoEMQWzBWIGZga6BnII+wdbBcoGDgoeCMIEMQU1Bl8GNQe6B/AHcghyCKsHZgU5A74E7AanBpQIhAsOCskI5AmMB5wE5AWcBrcF"
    "PAefCIUHZgbkBPgC/wT/Ca8KUwe6BkQIswf/BsoG6ATGAwMELQTwB64MVwroBV8G8AarBp8ISwl5CHkI9AYtBugIGgl9BcIDiQOBAsoBIgLGAwcFHgT4AyUFgQTdA5wE2QMHA90DEgSUBMIFMQQmAvQCuwJQACoA"
    "HgLRAtkBUACG/wcA4QC3AQcCLQFMAYEDfQRAA18CMQF//6AA7AKoAbD/EwEiA/8DvgNMAO77g/wLAaAEugUaBFcB4QCRATkAe/4I/jb94vwX/2cBWwGNAPn/Rf8HABIC2QGt/vn7TfzX/kQASf82/hD+GP5s/p39"
    "2/vL/Cf/K//P/tf+G/79/qn/K/2p/E3+nfzm+1z+Uf3y+iP8Sfya+tf6uPsM/SP/Nv/m/bD8HPsU+9v7ofqa+ub8bP2w/HP8EPxV/TL/Qv4Q/WD9sPw+/GT95v39/Z3+Af4b/En7EPx3/C/8FP3u/KX5cPkQ/lj/"
    "lv1k/jL+lvwI/kL+aPvb+2z+bP4+/cf7ZPu0/WD+EPxV+8v7FPxo/df8XfpC+5L95vxV/Fz9+fwB/Mv8qf1R/Yr9I/5F/fH8rf5s/97+Rf/T/nP94v02/mT9c/75//n+4v3D/n//Qv4F/Sv+Sf/9/nIAMQHT/QX9"
    "ygBEAS7/nf+K/z7+kv/hAZgB3v4U/SYAAwXVBCIBTf8y/2z/mACJAgcDRADE/ZL/JgLlAnIExgRIAs4BRALW/yv+of68/uEAHgXdBqMFzgIHAMf/of9o/Zb8I/4mAKQBhQD9/kQBOQMSAmMCrwKoAFsC8AZIBnoC"
    "uwFfAq8BYwCW/zUAiQF6AnkEZgYeBKn/Tf5Y/yIB1QOvA53/sP02/3f/5v02/UAAjAYhCZgGMQWBAo781/zdAewAZP7x/y0BfQI1A7z/z/0X//n99f0qAKz/oAC+At7/Nv7CAAMBiQEeA7MA0/7a/+r/VwFMA7MC"
    "HgN2Arz+YP6d/z7+y/7H/3/+Uf8PASYBQAH5/xT//wA1AVQAOQFQAFH/nAB7/wX9/fzi/SP/FP/A/er+g/5R+xT90/+D/vn/7ADE/JL8gv/1/mz/tP8v/Zn9jv0j+8f99f4Q+gz7lv4M+xz68f1J/ioAKQTZAbT9"
    "Rf31/Lz8Qv3q/Or8tPxd+wn61/gM+mD9y/06/j0B4v9k+7z5g/gB+QH+TAOgBhIFBP4F+kL5yPbf9x/7mvtR/gAAc/02/+kA2vwM/Dr8d/ml+7D+J//0AgsCCPuh+lz8ivtg/oL/BP79/1X/Rf3x/gX8UfhN/Cv+"
    "Pvxk/HD6/fk6/lH/rP8eAdP96vz4AB4A8fwy++L4wPvlAC0AEP8U/u75AfpN/XP+7ABTAZn9qfyt/MD8of9c/wz/6AI5AVj/nAOZ/1H5BP4TAEL/1QJ7/jP6egDa/+r8LQJjAI7/Ygb9/d/0of+nBKQCwgirBQz/"
    "HgR2AWz7gQFjART+xgVfBub/NQCl/5gCpwjCACv7RAIj/y/8GgdbBoP8YP6JAEwAtwPOAOL+FgWUA1n9y/1F/kAAVwXOBLMCTAKBAJQCYwPe/Sf/3QQDBLoDswJR/cv/wgX4Atr+z/0f/wsF+wb7AnIC/wETAJwB"
    "nAAj/jkCdgZ5BugDMv3T+pEBxgQHAf39+f3oAY0Ey/8B+976d/zlAJAD0/9v/fX+kQD/AvgBd/zA/KQCVwT0AFz8yPo2/jEBHgCW/Xv8Pv/sACf7e/ep/KgAHgGBAWz+Uf1EAkwCH/2W+5L8YwD4A7z+I/p3/dP8"
    "RvtEACIAuPy4/9P/mf1+AG//3vzCAFH/5vkF/e3+mvvx/TUBQv9N/a37wPrE+2j7qfyNAJL/Sfw+/E37K/rP/LcAUAONAUn6YfTu83/3Lv7dA6cFlATt/tP3QvYg+er9wgA6/Kn5qf/1/z751/lg/zEC5QDX+P7y"
    "y/mUA5gG6AOh/T77bP+oAJL9f/tN+xv/eQTwBOEC+ADX/nf/FgBs/bD9fQG+AoEBlv/X/WT8yPnT+TL+bP8n/dr8bP3e/ioABf1N+y7/NQAM/pb+of68/UX/c/97/fX8G/ye+nP7sPx7/E38e/ue+vn76vyx+rz4"
    "bPpF/lsAw/6S/PH8ZP7T/o79tPsB/UAAAwBR/hD/mf8E/0L/kv9C/z7+of0u/yIA6v7q/i0AlAFyAi7/xPsb/ykDGgOUAuUA2v+7AjEDPQDP/7D/Pv8PASIBsP92ALsAbgAaATEAYP8tAJgAWwJ9A/AAzgBAA2cC"
    "OQKJA18CZgMxBS0CdgGcA4UBVACrALz+vgGBBaQBDwFqA/3//AA8BBoAiQFXB68FDgZABRf/VwUDDMYCpf/CAs4BugkhCsv9agYlD8f+AfyUBPH+GgRxC5n+Vf/kDEQJMQWYBH/+NQOjB6sCpwleDIL/uwJWDhIK"
    "1QKS/cT93AnVCsoAugSvBx4BagT3BxYDoAO+BW4FjAYiAub+cgNMAg8Bnwj0B18A2QKrCE8IUwPt/pwDkAnKBSIElAZQAsv/qwIpA2YF2QWG/zUA+wUiBAcDEgTGAAsDtghABZgAYwHsAroEZgN+AFsEnwcaBFcC"
    "9AGUAFMDJQZMBZQDw/8f/zEF7AWBAMD/mABQALsCqwMWAer+Vf74AOgDXwL/AeEDEwHq/lcCkQKK/mD+3QBUAFj+sP/oApECz/+K/tP96v0I/2D+EP4LACYA1/7i/mz+Rf7D/s/8nfw1AXoChv5c/Z3+DP2a/LMA"
    "QAJN/sf89f6K/hD9Qv3A/Hf9qf+p/u78G/57/18AHgBo/cv8e/71/VX+TADi/mj9rf5R/SP6z/lN+5n9KgBjAG/+PvwB+9P7pfzA+2j8c/7E/df8iv46/kL7+frA/Nf8tPt/+jL7hv6C/3/80/kY+Iv3qfl/+sv5"
    "+fuS/HD5Wfqw/RD8uPnH+7D+aP7e+tf4Vfsb/Yr8wP6zAHf8IPd39/36H/xR+xT8CPyp+P33/foU+i/4QvsB/fH7Tf2S/O740/nA+9P6+fwHAAz/y/0U/N/5Ufxc/t76f/k6+9v6x/s+/or9G/zx+9f74v23AEX/"
    "PvuO+rj9FP9C/B/7Lv6W/9P97vx7/Yr+Tf8B/lz8CP0+/ur+b/8I/lH8y/0X//H9WP4X/yv+PQB2AsP+sPykAOwBvP9+ANkC2QNjA6ABsP+d/cf85v/4An0CSAKFAgsBJgC7AI0CFgTCAaz/agFMABj+QAI8BkgF"
    "6AQSBcIEZgUHBEQC+AOUBNECRAMWBKACWwHwASIDXwO6A/AEcgNEAUAECwfoBAcFpwc8Bq8D6ANjBA8E4QOrBPcFMQWUA6AEjAYLB0wGjQQeBIEFiQUSBf8EgQMxBOwHCghIBX0EvgPRA7YGrwezBVcEXwO6A68F"
    "0QS7AgsEHgX0AyUF1QcOBzwEMQQOBoEFMQR5BbMFjQONA24DuwDZAfQF/wRMApADtwQDBCUFagYeBKABcgNbBZQCDwEaBHoDOv+RAKsDkQC4/ZgAUALx//H/JgN9Amz+6v6UAlAC8ADwApgDYwDi/rsBoAPwAFj/"
    "gQKkAob9qfwmAIr/pf0tAEgBL/x/+OL85QH1/xD+SAHdAFX7aPsDAUQC/f6w/TL+WfyH+vH9pAEY/oP6y/4xAXP72/hF/YL/x//CApECg/0y/PX+J/+Z/Sv+tP8M/yv8DPvP/OL9DP/hAdUAZPvI+oUAJgNV/1z9"
    "jQEDA1X9M/ob/Qj/FP9o/wH//f6C/wH/2v2h+yP7K/+Z/zP6NvuRAVMBH/5R//8Ax/9o/m4A2QN9AnP/iQF2ArT9FPzP/8oANv68/cP/vP8r/iYAegO3AlsB5QLdA8oCNQGzAHICfQPlAa8BUwOrAyUEHgVuA5gA"
    "+ACkAzwEOQJyAVADagXwBWYEWwJ9AuUDFgSgAsoA8AGrBcIFnAMeBSEG0QPCA1cFfQRfA8YDVwV1B0QHMQXCBB4F5AQ1BXIFFgX7BJgFqwacBl8FmAVmBgcG9AavCE8H9AN6At0DLQYaB9kGgQZfBv8H8AnsB68E"
    "pwUpCG4HvgQpBAsGswYaBvcGnAZIBB4FhQfCBq8FHgavBSkFdgWjBUQF+APVA2IGMQfRBPgDYgXZBXkEOQNbBOEFeQWBBmYIKQblA+EFWwZjBKMEFgX/A6METwbsBdkD7ALOBBYGSAMxAbMDeQVQA5wB0QJABKAD"
    "agItA1cEZgPRAhIEYwMeAfABqwMHAn4A5QF9AuUBnAL0ASf/Uf9EAnoCQADq/0QBqADE/Wz97AA1Aor/CP4f/oP96v3L/vH9YP35/SP+H/4Y/S/7YPxC/8v+Sf1//XP8y/rL+2z9lv3E/c/9vPzu+0L8bPz5+8/7"
    "Afyx+4P6lvnm+q39EP7q+636f/po+nv7J/zy+vX6xPza/HP7BftJ+8D6lvnA+f36YPpR+Qz7kvyp+9v7Rf3i/K36EPh/92D5Bfmi9rz37vlk+df5Rvvu+Wj44vgv+XT58vlw+bH4rfcN9+r5Nv2a+kL1xPQF+YP8"
    "2/sU+Sv4Xfie91n4z/rb+rX4vPji+rH7nvqS+Tr5M/mW+df58vgY+PX4WfoI++L79fx7/S/9nftR+v37EP6O/I76GPsr+1H6g/sF/QH8PvrI+Wz6cPsj/JL88fya/LT7Wfs6+9v6OvtZ/O77Nvri+oP9rf0f/Az8"
    "8fsf+xD8H/31+0n73vwr/t79b/0r/cD7jvrE+/n8xPxc/lQAf/5z/H/9GP6w/RT+y/0r/bD9Mv5C/tf9wPxc/cv/6v9g/Sf8ZP3q/h//ZP/0AGoBpf+t/nf/CwAaACYA+f/P/xoA7ADCAIr9d/oy/Q8ChQHX/jb+"
    "YPt7+tUCQAizAokAjQLq/cf9rwXwAvn5Rvtv/c/5OvpR/ikBjQIHAvsD7AZXA/8BOAdqBC7+twJiBkQBw/5XAF8AaP5Z/Aj/9AEQ/RD8ygOUA7j+FgOQBl8DJQSYB8YGCwNQAPQB1QL1/aH9xgJAAcP/qwThAvn8"
    "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"
    "sP+0/x//aP+C/0n/J//H/sv+vP/OAIUB1QFQAZ3/XP1o/IP9vP5F/lz9K/5qAGoB/f9c/lz+Tf4Y/f38rf4WAGoALQDx/xYAAAA5APgBoAKvAAAAfQE5AkgCUwLGARMBrP/m/Wj+wP+K/i/9z/4iAZwBWwGUATUB"
    "IgDD/w8AVAByAMv/Mv/W//n/BP9C/24AegBv/8v+tP89AVsBhQCcAIkBOQKgAuUC6AFv/53+RADsAJEAegEHAmMBVwF6Aa8A5v/a/wcA1v/t/wcBOQLhApgDoAOFAqgBuwH0AVsB0/9c/tf91/5nAR4DIgKRAHIA"
    "JgFEAlADfQMHA2cCtwGRAVACCwN2AvgAOQBnAK8AMQHKAbsBTAEeAbsAFgAAAIkAWwEiAvQB/AC3AB4BfQHKAlcEHgQWA+wCqwKgAa8AZwCkAC0B3QHoAQsBqwCBAYECTAOUAx4D9AJTA4UD9APKA+wB5v9J/8v/"
    "VwDA/6n+vP6p/74AqAG3Ac4BCwImAa8A6AFqArsBswExAqACHgNqA4kDUAN9AtUBqAGUAdIBNQIDApwBagGoAHP/Ov/L/xMA0gDwAcYB4QCRAGMAhQAxAUwB1QB2ACIATAATAdkBUAIDAjkB6QCFALT/vP/CANUB"
    "OQJbAT0AlACRARYClAKnAgcC9AFjAuUBGgFTAVsB5v9s/pn+b/93/yf/XP+8/0gAQAFfATUAvP+gADkBMQFIATkBzgCgAMYAjQBEAOwAuwGNAT0BBwEtAOL/3QB9ARMBoACUAGoAuP+G/24AxgAqAPX/8f+w/+b/"
    "lAAmAfgAw//L/vX+4v8TAXoB7ADZAL4Apf9F/7T/G/9s/sP+Tf8TAAMBNQEeASIBfgDL//3//f9J/x//b/9R/5L/qAAiAVQANv/D/lX/CwC4/+L+wP4y/3P/c/+4/y0ATAALAJ3/CP+w/mD+CP6Z/lz/x/5z/qX/"
    "LQB3/47/zgCkAT0Akv24/Q8Aw/6t+479KQF6ANP+qf5F/e78y/+3AM/9rfvm+x/90/7e/6H/Tf9F/1H/rP/L/1X/Uf/x/3IAJgHoAewAsP7m/Vz+I/5C/qn/LQCW/p380/tG/Bj9ZP2d/bD+EwCrALMAqwCcAGcA"
    "4v9R/1z/c/+h/vX9Nv53/v3+WwDVAFX/sP0+/YP9Lv5N/y0A9f8B/0X+GP6l/u3/DwH4ALj/mf7m/uL/HgBQAMYAHgAn/2//uP+l/74A2QEiAe3/rP8TAK8A4QCkAKgApABbAHYAYwCh/3//FgA9AGcA/wAxARoB"
    "LQHZAFcA/ACcAlADUwKNAS0COQKvAOL/agCUANUAFgIpAnoAFgC3AdUClALZATEBegHOAqsDRAMHApwARAA1AYkB4QDsAOUBpAISAkgAQv/i/0gAZwCUATUCqwESAq8C5QFTAW4B+ABfAR4DkANnAgsCmAJbArcB"
    "HgLRAnICJgJXA4UEFgTVAsoBJgEpAUQC4QN5BCUEHgRIA+UA3v/dANUAGgDVANIBWwK6A/8EfQT/AiICjQKNA0QEIgULBrcFoASgA1MCYwG3AVMCIgLwAOL/SAFPBKsFRAVXBMoCGgLsAhIDDwOnBLoFugQLA/AA"
    "x/7H/hoBXwMaBGYEMQbZBxYGhQJ+AEn/H/9MAvAF9wXKBOgEVwThAmcCZwIpAqcCAwREBYkFOQSBAtUBMQFXAA8BJgKzAXYCoAXVBsIENQNIA1cDkAMeBKAD2QI1BHIGpwV2AvwAzgH4AiIElATGAxYEZgaMB/AF"
    "2QNMA9EDCwQ8BK8EOQOFAFsBBwWUBXoD6AL0AvgBEgK+A30E+AOFBPQFkAUiBHYE2QSnA/QD1QU8BeUCGgIeAvQBCwMOBdUEAwK+AZgFugXL/+79TAM4B58H2QegBegBRAJ2BEQDEwGRAcoDqwUWBsIFIQYLBroE"
    "JQQDBGcCOQGgAl8EagTwA6QDPAIPAAAAXwKgA1AD4QSzB9kHzQVuBKsDSAOFBF8GSAbZBOEEUwZXBi0EwgGUAF8B3QONBacE0QLwAQcCgQK3AnYCuwKkA1MEqwRjBG4DcgMHBTUGmAXGAyYCAwI8Ay0ExgNbAkQB"
    "XwFyAfAAiQESA4EDZgP0AxoEgQM8A4UDIgSBBN0DGgNyAy0ESASrAyYCMQA6/+3/agFjAoECjQKrAnoCgQL0AuwCrwIeA5QDJgNAApEBdgHGAaQBrwD1/y0AtwAtAdIBDwJQAc4AdgEDArMBmAHoAfgB/AE8AiIC"
    "xgHGAeUBhQHwAM4ANQFjAbsAy/+S/9r/AABIAM4AxgAqAO3/EwAiAHoAMQGUAZgBhQEPAXYAcgDSAP8A7ACvAEQADwAeAP3/w//i//H/x/8xAP8AKQEDAfgAOQAU/+3+jv8DAK8AhQGUARMBzgB+AOL/mf+8/8v/"
    "qf9//0X/DP/L/lH+y/3a/Xv+9f4I/03/8f9nAJgAqAA5AIb/f//W/7z/wP8WAAsAEwBUAKX/bP42/qX+BP9c/xv/YP6Z/ub/+ADpAKX/qf4U/8v/2v/T/4L/EP+K/xYAc/97/qX9nfzH/Ib+jv/x/gz+Qv2S/P38"
    "b/79/kL+H/4b/3//+f7H/gj/K/9c/53/Rf8j/kX9tP20/lz+jvxZ+/H7sP1o//X/2v4Y/bT8K/6C/0X/b/4I/rz9rf1C/m/+iv3H/DL9DP6G/rT+of4I/or92v1R/h/+lv0j/e78DP06/Sv9Mv17/QT+uP7T/hT+"
    "of0Q/sf+Ov8E/xv+Tf0r/WT9pf2w/Yr9ZP0B/bz8b/0j/rz9pf1v/pb+Ov42/sv9EP0Y/VH9L/0+/Un9Af0U/YP9pf0b/fH7H/vH+0L9GP4f/tf9kv2p/fX9Af7T/aX9f/2w/UL+XP6t/U39FP4B/9f+xP13/Jb7"
    "0/vP/I797v3m/Vn9Bf0r/dP8Ufy0/JL9Ov7A/uL+Qv5z/TL9bP13/T79Wf2O/UL9H/2O/Yb9z/xk/GD8e/zX/Fn9wP3u/cv9jv0+/bj8pfxC/ZL9g/3e/Sf+xP0j/cf82vw2/Sf9sPyK/H/8f/wr/er9qf0Q/er8"
    "Bf0r/UX9H/35/B/9g/3H/W/9rfw6/En8g/y8/N78GP1Z/Tb9Bf1F/Tb9hvw2/Fn8hvxF/Rj+0/0F/Zr8PvwU/LD8mf0B/vH9DP5g/lH+qf0j/R/9Uf2O/a39g/1k/bT9Uf60/kL+Ov2l/Nr8qf0M/y0Ay/9z/sv9"
    "Mv7L/sD++f0b/bD81/yl/Wz+Lv53/Vz9rf0B/o7+3v6D/lH+3v5V/xD/of6S/pb+kv7X/vX+Sf7e/aH+Uf/L/iv+Vf7D/h//c/93/1H/b/+d/2//Lv8Q/5n+1/2K/er9nf57//X/pf8B/5b+bP5c/gT+lv06/qH/"
    "KgDe/3//z/6D/nv/JgBs/97+Lv9g/2z/qf/a/+r/x/8y/5n+Vf4I/ur9d/4y/3v/lv+h/2T/Pv+G//n/egDlAIkAd/+p/m/+f/6Z/lX+tP2w/Y7+YP+K/2z/YP97/53/iv8j/9r+YP9yAAcBmAB//63+3v6s/y0A"
    "JgC0/x//K//5/34ARADa/5L/kv/W/w8A/f+d/yv/+f4I/yv/WP9//4b/mf+w/2z/w/5z/hD/3v/P/2D/gv/D/8f/+f/W/wj/lv7q/lz/uP/m/6H/Uf9Y/3P/x/92AJgA0/9J/4r/0//e//H/2v+p/97/RABQADUA"
    "NQA5ACYA3v+h/+r/VABnAIUAfgD1/6n/z/+8/53/0//q/yYAHgEWAg8CBwHD/2T/NQATAT0BEwHlALcAfgD1/1z/f/9UAAsBRAEDAUwA1v9XACkBOQHCAG4AbgCrAOUAxgBfAEQA0gBjATEBtwC+ALsARAAqAN0A"
    "mAGvAUgBIgFTAUABAwEeATEBMQGvAf8BfQH8AOkAzgCrAKQAzgBIAZQBUAETARYBJgGgAUACGgKgAbcB7AG7AY0BqAHKAd0B8AE5AqsCpAIPAsYBEgJjAokCqwK7At0CGgP4AmMC/AESAn0CxgKUAl8CrwL4ApwC"
    "DwLlAQsCHgIWAh4CQAJ2AqsCbgLGAagBPAKBApgCXwNTBK8EagRXA90BbgFEAkwD/wMSBG4D6ALhAsYCvgIWA0gDZgPdAyIE7APZA8IDegN6A7MDrwOzA90DtwOkA0AEvgRMBKQDpwMSBF8ESASgA9ECuwJ6Ay0E"
    "KQSgA0ADYwO3A/AD/wOgA+wCxgJbA68DiQNuAzwDrwJEAlMCagJQAmMCxgL7AsYCegJAAiICWwLZAi0DQANTA5QDzgOzA2MDMQMWA/8CFgM5A0wDegNbA8YCWwJIAkQCbgLlAlsDnANjA9ECmALhAg8DHgOBAw8E"
    "SAQPBIUDzgJAAjkCmAIDA2MDnANqA6cCswEpASkBQAF2ATUCLQOFAxIDKQJyAYEB6AEHAi0CkQKnAmoCUAJAAgcCBwKJAtUCOQJbAUABmAHlATwCbgItArMBRAHZAJgAlADZAF8B7AEWAsoBgQFjAQcBpAAiAV8C"
    "bgPZA18DUAKzAd0BJgJuAuUCOQMiA6QCJgIWAh4C0gGFAYEB1QF9At0CfQLhAWcBDwELASIB3QDGAIUBpwI5A84CCwLKAdUBlAFEAXYBLQLsAh4D2QJTArsBSAH0AI0AlAAxAXoBHgHSAPQAfQESAg8CgQEWAQcB"
    "JgFnAbcB0gGBARoBGgELAXIA8f/m/9P/rP/e/0QAtwBuAf8BuwHwAIkAfgCNAPQAYwFMARoBKQEWAQsBJgHhAIkAuwDGADEA3v89AMoA7ACBAPX/wP/t/1sAqwCkAH4AVwBAAHYA2QD4ANkAnABQACoAFgAWAHoA"
    "4QDCAI0ApACzAKsAcgAAAN7/MQCBAN0AOQE5AfQAuwCYAI0AagADALz/vP/D/8v/wP9g/yf/c/+w/3f/EP/T/t7+Lv93/4b/sP/1/9b/Nv+8/tf+Rf+S/6X/of+Z/7T/AwAeAMD/b/9k/2T/sP8aAN7/b/+G/2T/"
    "w/7a/rD/IgADAM//of97/xv/hv5R/q3+ZP81AKsATABg/7j+z/4y/xD/kv6p/kX/iv9//6z/y/9v/9r+jv6t/h//c/9V/xv/Vf+8/9r/rP86/4b+/f3x/RD+Qv7T/or/lv/P/u79lv3X/R/+K/5s/hD/WP8U//X+"
    "7f5c/s/9G/7A/tf+mf7T/mj/tP9//03/nf/q/53/Pv93/8//mf9J/2//x//a/5n/WP9C/yv/7f7t/mz/8f/x/7T/y/8HAAsA7f/T/7D/rP/e/xYAPQBnAH4AbgBjAIEArwB+AO3/sP8eALMA5QDpABMBSAEpAb4A"
    "pAD/ACIBtwB+AOkAYwGJAZEBegE5AQ8BOQGFAZwBUwETAUQBnAGkAYEBmAH0AQsClAEeASkBbgGJAWoBKQETAT0BYwFuAYkBqwHGAb4BlAF2AXIBkQHoATECUwKYAugCxgJMAuwBAwJuAqACkQJ9Am4CZwJqAkAC"
    "/wHhAeUBHgJ9AqACagL/AaQBuwExApwCygK7ApgCpAK3As4CUAPwA/gDgQMaA/sC8ALVAuECHgMtA/8C2QKrAmoCUAKJAv8CTAMtA/gC4QK+AoUCgQLhAlcDfQNfA1cDdgN2A0gDDwMSA3YD/wNMBPgDQAPGAtEC"
    "AwPoAqACpAL0AgcDxgKkAqsCmAJnAm4CwgLlAnYC7AHKASkC7AKrA9kDcgMHAxIDhQO+A5wDjQOUA0gD1QK7AugC6AKvArMCAwPZAikC+AFfAlsC0gGrASkCjQJTAikCtwI8A/ACegKgAiYDlAOrA3YDKQPdAqAC"
    "mAKkAr4CDwNMAzwDIgP4AugCSAOgA3IDAwOUAm4CtwL7AuwC1QLdAugCiQK+AYUBIgKkAsYCoAIHApQBqwHlARYCUALdAsoDBwRyA0QDbgNmA3YDGgNAAgMCUwKRAnYCygF9AeUBrwFqAd0BoAEiAbMB/wG3Ac4B"
    "qAGBAfgBLQJXAtUCDwNXA0ADUAJTAv8CfQI1Al8CbgEHAb4BegHSAM4A3QBTAbMBFgGRAI0A1QDOATECuwFAAhIDmALGAaABSAL4AgsCiQCJADEBXwF2AYUBnAGJAfgA1QADAZQAXwC3ANIAGgGJAX0BHgGJAJgA"
    "wgEHAiYBPQGkASIBvgALASkC+wKcATEAHgG3AcoAPQD5/wAAmAA9AMD/oAD/AGcAkQDlAJEANQADACoAGgAn/+L+0/+W/+79Nv35/f3+jv/W/+r/pf9J/+r+e/4f/zEAPv+h/Tb+YP8+/wT/7f6Z/rz+Qv+K/wT/"
    "1/3A/bz+jv4E/sf+Af8+/hv+uP2p/K38ZP2h/QH+mf7X/kX+9fyh/H/9of3q/S7/DP8+/rD+4v3q++77Ufwn/MD9Nv+W/v39nf3q/OL8pfz9+1X8Ov1C/gT/DP7E/Hf8f/tk+4r9I/+RANIBuP5R+8/86vmP7/ru"
    "F/8CDVcKZwHW/9kAmvrj8bXxcPrKAU3/lvky/jQIdgYM/Fn69ADGAV37pfj1/VMEeQX7AsYAXP9o/eb72vzE/fX6Afh/+Z38Nv6O/5gAHgDA/j79hvxV/cf+YwD/AUwCRAKrAyYDsP2t+Ob5bP28/TL9d/7a/p39"
    "Mvz1+Y74Wfre/Db/XwKnAnv/DP7a/Sf8c/ud+976nfwaAAj/Tfsv/OkAzgL9/zr9hv0j/mj99fyO/MT81v81Ag8Ab/6kAAMBDPzX9YvzxPUN9/3z0PL29/X8pf3a/zUGLQr0B1cFYgbZBcIB8ADZAyIDf/7L+qX4"
    "z/cJ+bH70/7SAOL/GP0Q+xj8IgBXA9EDYwPsAe3/8f5o/CD5aPkc+Ur1dPT99Zr0ovQv+B/6yPmK+dv72v9Z/PbyDfSD/UAC6AgRFLYVLBCnDnELMQPL/e78vP0E/or7c/rx/bD/z/vq9BXvWfBk97T75v16AfAD"
    "HgZ9Btr/pfrL/lcC2QD/AHYB1/7L+i/2VfRk9gz4I/vm/2T9qfWi9D76b/8xAan/WP71/kX/BP+K/fH7Lv9uA+wBiQDGAkACJ/9o/Rj9J/8HAtUC/wKkAe78Yfia9qL30/nI+fn5NvzP+QX1Dfah+C/66v1v/qX7"
    "Tf3SADUCGgIX/03+RAEM/5L9CwW+CNEGEgp5C30IBgt5C68BSvW96eDmEPinDX0QLQP68lrqxe3A8/32XP7oBHYCPv+W/GXzL/BN/ZAKEg4tDbMCh+8Z5ijopusQ+F4OLBqjEjkBGPCu6QX08wgCHLkhXhcWBXzz"
    "rulx61bxOvenAgYMtwW59gLtIOqe7Fn1UAGfCAsGL/0z89jq3+p79QMCBgrCCfn9g+8K6frnQ+rb8nf+9AaQCDkCwPbQ6vfm3/ExAvsLbQ9qChv8h/CE7e7uzPYxBB0NQwx5BQ8ArfxO9hTzkvrsA4kI1QjT/rnu"
    "Eefv6IPyiQRSEQ4O6AJC+NvwSvCh+EgI4xUCFwYOIgDB8P7rQvY1A+8Lrg9aC5ECUfqH8mXvN/UPAWYNthM8EW4ITftp8dfzuPz4AkgHjAjCBfwBqfsB9XD2Sf44BV4ICwVk/JL06vEB9hj8FP8HBOAJPAYM/Zb2"
    "P/Hy8vX/vgpuDIEKfQQ6+yT1BfXe/EsJSw+yDS0LzQXi+3T09vQF/eQHzQ2nCyEGpADi+Zb0g/cxARIKpw8dEOAIpABc/o7+2v++BHIJsgo8CasCkvhl9FX6jQREC9ELsgm6B3YE2QCgAFMD4QXkB2YKqwvwBwMA"
    "+fpJ/X0DowgKDAoO/wsxBfH+ZP0B/90CrwgKDPMJDgXD/0L8Vf6QBCkJ6Av3DhkPKQqrA7D+vP1yA+8LPBDRD4QMPActAzkB6v+RAl4J2AzvC+gKXwd2AWoAVwQWCIwLEg7zDUcNVwujBY0BUwQ0Cl4OQw8GDb4K"
    "hQnoBjUEFgSYBd0Irgx5C/AFgQJQAVMB1QXkDEMQ6A5mCzEI0Qa6BcIDQARbCLoLzQtAChYIjQVbBIUFnwh1DHkNPAqzB24HoASkAQsF8wqUDIgK2QaJA18DtwRmBWoHhQo8DNEMvgzJC14KPAi+BewEeQZ5CPAH"
    "IgXGA7cDygLVA1cHugiBCHIJ4AlmCVcJJQlTCfcJowl5CZgJhQf0BBoFeQZPBiIFfQUOB0wG+wPOA0gEagSBBlMIiQegBg4GSAVfBUQFhQRMBFsEcgaMCg4LvgegBYUEBwTGBE8ERATKBikHrwSrA0wD0QIaBKcF"
    "LQb7BboFtgYtBkgCvgBqAhoDBwVPB1sFrwONBHYD+wJEBUQGAwboBEgCXwE1Afn/kQGYA4ECQAOrBAcCrP+S/47/8ABIAiIDcgX4BKgBCwFfADL/zgEiA74B5QImAxICnAN9As//uwFMAgcB8AP0BGoCNQJfAasA"
    "UAPOApEAOQL4Abz/9f9s/4kAGgQiAn//ZwLCAoUAiQBz/lH8wP1//or/+AD5/lX+1/5s+6n7nAADATEBFgQtAur+b/6D/B/87f5///3/HgL/AUQBiQA+/nP+qwBQAKH/y//q/mz+G/1k+hD7e/4tAEwAuP5w/G/9"
    "mf9z/5L/bgA9AKsAdgGNAEL/bP4Y/Tb8mvzL/UL/z/9V/1z/ZP8y/rj9F/8iAA8Aqf8n/0n/jv9z/mT9b/3L/Kn8rf5IAH4ADwAr/gz9Lv7q/W/9mf+8/4b9Sf4Q/978ivyw/gcAagB//zb+1/42/9r9Pv0y/cv8"
    "f/3L/nv/8f6d/En7H/0n/y0AfQGzAfwAnADL/hT9hv5s/xv95vs6/Zn+ZP9J/2/+Tf6W/s/+tP8iAN7/UADe/zL+4v7/ABMBZwDH/zL+YP1V/ZL8Ov3x/sf+z/41AAMARf+h/wH/K/53/nP+aP8HAsoCWwELAMD+"
    "Ov4n/5n/f/+d/+3+Lv5o/m/+qf6Z/2z/ZP5z/h//z/+oAJwAlv/X/or+w/7P/0wAOv9N/rz+rP9MAB4AOv/1/hv/Nv53/eb9z/2D/U3+aP4r/rj/qwBR/63+CP8+/j79GP2O/cf+gv/L/p3+gv+G/9f+4v4U/7z+"
    "6v7i/+b/z/4f/5QAdgDL/3IAVwDA/hj+Qv6h/S/9lv3E/WD9DP1g/UX+wP68/uL+/f64/kX+x/3x/dP+2v5g/vH+H/8y/qX+EwDi/x//K/+d/sD9sP2W/bj9CP+8/z7/aP+S/7D+kv7a/v39EP6C/4L/Sf9QAKn/"
    "y/0f/tf+GP4U/nv+f/3A/BT9aP35/df++f6K/j7+aP4r/x4AwgD0AHYAd//5/i7/ZP9//1z/jv7e/cT9XP2K/R//bP/5/c/9K/57/fX9F//A/gT/7f9R/xf/e/9N/sv9mf4r/qX+zgDCAMv/mABqAMv+aP7L/iv/"
    "lv9c/1j/wP8I/63+OQC7AFX/K//W/z7/6v6p/3//Tf++AF8B+f9Y/9P/2v8PAFAA+f85ALMAGgC4/9b/XP9k//n/gv8Q/5n/4v89AP8AnAB7/zb/Tf9Y//X/mAAPAfgBFgLSAHoADwF6AA8A8AAtASIB0gFAAQsA"
    "LQD9/8v/xgHZAmoBBwEtAd7/iv9XAAcAIgALAagA9f/D/wz//f7x/+3/3v/lAMYAwP/5/1sAPQD/AGcBcgAqAJgAZwB+APAAhQBIAIkAtP/A/iv/c/9z/74AegH1/yP/8f/T/8//RAGBAcoAWwEDAbT/dgATAf3/"
    "cgBuAQsB2QGzAn0BWwGrAdP/Nv+YAMoAOQE5AhYB4v9QAGMArwByAW4Af/+oAOwAof/D/8IApACJALMA8f+w/40A+f/P/qn//f/e/tr/ZwGvAG4A8AAAAJb/w/+w/hT/FgH0ACoABwE1AbMALQEaATkAUABUAFX/"
    "gv9EAdIB+ACRAGcAYwAxAUgBuwB2AUQBLv8n/18AYP/H/j0A8AAtAcYBRAGoANUAUACs/xoAegBuAKsAzgCNAJwA/wCoAAcAcgCoAPn/egBTAcIAwgALAd7/0//OAIb/hv7A//X/5v8pAQcBPQBuASkCSAFnAQsC"
    "UAGJALsANQBC/1sAGgK7AT0BoAGJAKH/lACNAPn/lABuAGoA5QF6ARMArwA5AMD+AwA9AaAAagEWApwA9f9MAAAAmACkAQsBNQBuAFsAz//q/3IAqwDZACkBDwEWAZEBMQGrAGoBSAHT//X/jQCK/8P/TAHwAC0A"
    "qABMANb//wBIAS0AfgBfAb4ASAC+AFcA8f8DAVsBZwD4ACkCiQHpACIBWwAAADUBiQExAcoBmAGoANUAjQBk//n/VwH0AHoAXwHZAUQBqwATAKH/YwCrAd0BtwHoAfgArP/t/zUA7f/ZAI0BrwBbAG4Aof/H/9IA"
    "pABXAL4AhQBfAOUAoAAWALcAHgFuAHoAswH8AUQBYwGBAXIAAABUAO3/VAASAgsCvgCvAEQAF/+8/8YAwP/t/pn/uP+h/68ALQGkAOwASAGoAMoARAE5AJ3/UADT/yP/AwAiAKz/HgEmAvAAbgAWAfQA2QA5AaAA"
    "wP8DAFcA6v8tACIB8ABuAM4AFgC4/pb/egBR/4r/4QD5/2D/vgB+AI7/ygBqAX4A4QB9AZwAgQAaARMAG/8HAMIAzgDKAVsCOQGUANkAWwDP//X/tP9Y/6H/rP+C/53/YP/1/gz/c//D/7j/jv/e/xMA6v/x/6z/"
    "Sf+w/53/DP/9/6sAkv+G/yIAF//P/ioAfgByAL4AwP8I//n/EwBC/6H/MQDA/4b/JgCrAK8AuwCNAJL/+f6C/47/WP9bAOkAJgAaAF8ArP+w/2MA0//X/t7+5v60/jL/qf9o/6H/cgAmAEL/hv+l/7z+DP/1/+r+"
    "FP4U/03/7f7i/1QAmf/H/8v/d/5F/mz/hv8f/3//Tf8u/tf9Nv4r/i7+iv4y/rj9kv64/7D/YP9C/3v+5v2p/lj/H/+C/yIAVf9N/pL+1/6t/k3/y/8y/wz/e//1/nP+8f7T/tr9wP0j/t792v1c/lX+G/79/Qz9"
    "ZPyd/bD+Qv6O/lj/Tf4I/Yb9DP4M/ur+z/9//+b+7f53/+3/9f/1/yoAHgDt/wMAGgAWADEALQD9/+b/9f/1/+b/6v8HABMA7f/e/+3/AwAiACIAAwD9/wAA2v/T/wMAFgD1/9r/3v/a/8//9f8mACYALQBMABoA"
    "y//L/+r/7f/5/wMA9f/9/xMAEwADAPH/7f/t/8//x//t//X/1v/i/wMABwAAAP3/+f/9/wMAAAD5/+3/4v/5/x4AGgDx/+r/FgAmAAcA/f8AAPH/9f/5/+3/BwAtABoACwAaAAcA+f8WABYAAAALAB4AGgD5/9P/"
    "5v/1/8P/z/8eABYA9f8eACYA6v/e/wAA/f/1/wcA/f/P/8D/2v/m//X/FgAWAAcADwADAOL/5v8AAAcAHgAxABYACwAeABoAEwAiAA8A2v/e//X/0//P/w8AGgD9/x4AMQADAOL/3v/i//H/5v/e/wAAAADx/w8A"
    "BwDe/+3/AwD9/wAAAwAHABoAAwDi//n/EwAHAPn/6v/t/wcAAADt//X/9f/x//n//f8AAPn/7f/9//n/2v/t/wsA+f/5/wcAAAD9//n/6v8DAAsA5v/t//n/6v8TADUACwDx/wAA8f/e//H/AAAAAAcAEwAAAOb/"
    "8f8HAP3/9f8HAAcA+f/x//H//f8AAPH/7f/x//X//f8AAAAADwADAO3/AwADAOb/8f8HAAcACwD1/9b/AAAWAP3/CwAWAOr/4v8TAA8A7f/9/wsA/f/1/+r/0//m/w8ABwDx//3/9f/q/wcAEwD1/wAADwD1//X/"
    "FgAPAAAAAwAAAPH/+f8HAAMAAwALAP3/5v/t/wAAAAAAAA8ADwATABYACwD5/+L/2v/1//3/8f/5//X/4v/t//3/+f/9/wAA+f8HAA8AAwALAP3/3v/q//n/5v/t/wMAAAADAAsA/f/1//3/AwALAAsAAwADAP3/"
    "3v/W/+r//f8AAPn/7f/x/wAAAwD9/wAABwAAAPX//f/9/wAABwD1/+b//f8LAPX/9f8DAAMAAwADAPH/6v8DAA8A/f/1/wMAAwD1//n/AwAHAAsABwD5//X/9f/t//X/BwALAAcAAwADAAcAAwAAAP3/+f8AAAMA"
    "8f/t//X//f/9//n/9f8AAAsAAAD5//n/AwAHAAMAAAAPAA8AAAD1//X/+f8HABMACwAHAAsABwAAAAAA/f/x//H/+f/9/wAAAAD9//3/AwADAAcADwADAPn/AAADAP3/BwAPAAMAAwADAAMADwATAAMA/f8DAAcA"
    "/f/1/wcAEwAHAAMABwAAAPn/AAADAAMABwADAP3//f8AAAsACwALAA8AAwAAAA8ACwD5//3//f/1//3/AAD5/wMACwAAAP3/AAD9/wMADwAHAAAA/f/5//n/AAAHAAAA+f/9/wAA+f/5/wAAAAADAA8ADwAHAAMA"
    "AwAAAAAAAAD5//X/+f8AAAAAAAADAAAA9f/x//X/+f8AAA8ADwADAAMAAAD1//3/DwADAP3/AwD9//n/AwADAP3/AwADAPn/+f/5//n/+f/1/+3/9f/5//X//f8DAAMAAAD9//3/AAAAAAMABwADAAMA/f/5//3/"
    "AwAAAAAAAAD1//3/BwAHAAcAAADt//X/AwD5//n/CwAHAPX//f8HAP3/8f/5/wAAAAADAAMAAAAAAAAA/f/1//n//f8DAAsABwD9/wAAAwAAAAMABwAHAAMA/f/9//3//f/5//3/AwAAAP3/AwADAP3/AAADAAMA"
    "AwADAAAAAAAAAP3/AAADAAAAAAADAAAAAAAAAP3/AAALAAsAAAAAAAcAAwD5/wAABwAAAPn/AAAAAP3/AAAAAAMABwAAAPn/AAAAAP3/AAD9//3/AwAHAAAA+f/9/wMAAwADAAMAAwD9/wAAAwD9//3/BwALAAcA"
    "AwD9//3/AwAHAAAAAAADAAAA+f/9/wAA/f8AAAcAAAD5//3/AAAAAAMAAwD9/wAABwAAAAAABwAAAP3/AwAAAP3/AwAHAAMABwALAAMAAAADAAMAAAAAAAAAAwADAAAA/f/9/wAA/f/9/wAAAwAAAAMAAwD9//3/"
    "AAD9//3/AwAAAP3/AAAAAP3/AAAAAAAAAwADAAAAAAAAAAMAAwAAAP3//f/9//3//f/5//3/AAAAAP3/AAAAAP3//f/9//n/+f8AAAMAAAD9/wMABwD9//X/AAADAP3/AAAHAAMA/f/9//3//f8AAAAAAAD9//3/"
    "AAADAAMAAwAAAP3/AAAAAP3/AAAAAP3//f/9//3//f8AAAMAAwAAAAAAAAD9/wAAAAAAAAMAAwD9/wAAAwAAAAAAAwAAAP3/AAADAAAAAwADAP3//f/9//3/AAADAAMAAAAAAAMAAAD9/wMAAwADAAMAAwADAAMA"
    "AwADAAAA/f/9/wAAAAAAAAAAAAADAAMAAAD9//3/AAAAAAMAAwAAAP3/AAAAAAAAAAAAAP3/AAAAAAAAAwADAAAAAAADAAAAAAADAAAAAAAAAAAAAAADAAMAAwAAAAAAAAAAAAAAAAAAAAAAAAAAAP3/AAAAAAAA"
    "AAAAAAAAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/f/9/wAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP3/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD9/wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
/* Decodifica un sonido incrustado (base64 de PCM 16 bits mono 44100 Hz, little-endian) y lo instala en la ranura `id`. No depende de assets/sounds. */
static void sx_install(int id, short *buf, int n, const char *b64) {
    unsigned char *out = (unsigned char *)buf; int k = 0, acc = 0, bits = 0;
    for (const char *p = b64; *p && k < n * 2; p++) {
        int c = *p, v = (c >= 'A' && c <= 'Z') ? c - 'A' : (c >= 'a' && c <= 'z') ? c - 'a' + 26 : (c >= '0' && c <= '9') ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out[k++] = (unsigned char)((acc >> bits) & 255); acc &= 0xFFFF; }
    }
    if (k == n * 2) { samples[id].data = buf; samples[id].len = n; }
}
static void pv70_install(void) {
    unsigned char *out = (unsigned char *)pv70Buf; int n = 0, acc = 0, bits = 0;
    for (const char *p = PV70_B64; *p && n < (int)sizeof(pv70Buf); p++) {
        int c = *p, v = (c >= 'A' && c <= 'Z') ? c - 'A' : (c >= 'a' && c <= 'z') ? c - 'a' + 26 : (c >= '0' && c <= '9') ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out[n++] = (unsigned char)((acc >> bits) & 255); }
    }
    if (n == (int)sizeof(pv70Buf)) { samples[SND_PISTOL].data = pv70Buf; samples[SND_PISTOL].len = PV70_N; }     /* little-endian (PS2 y PC) */
}

int snd_gen(void) {
    int loaded = 0, len, rate;
    for (int i = 0; i < SND_COUNT; i++) {
        const SndAsset *a = find_asset(snd_names[i]); const short *w;
        if (a && (w = wav_parse(a->data, a->size, &len, &rate)) && rate == SND_RATE) { samples[i].data = (short *)w; samples[i].len = len; loaded++; }
    }
    if (loaded < SND_COUNT) {          /* respaldo: si falta algun archivo, se sintetiza ese sonido */
        Sample keep[SND_COUNT]; memcpy(keep, samples, sizeof(keep)); memset(samples, 0, sizeof(samples));
        snd_synth();
        for (int i = 0; i < SND_COUNT; i++) if (keep[i].data) samples[i] = keep[i];
    }
    { const SndAsset *a = find_asset("musica_menu.wav"); const short *w;
      if (a && (w = wav_parse(a->data, a->size, &len, &rate))) { musData = w; musLen = len; musRate = rate; } }
    pv70_install();
    sx_install(SND_PLATING, sx_platingBuf, SX_PLATING_N, SX_PLATING_B64);        /* sonidos de las placas: incrustados en el ejecutable */
    sx_install(SND_ARMORBREAK, sx_armorBuf, SX_ARMOR_N, SX_ARMOR_B64);
    return loaded;
}

/* ---------- Mezclador ---------- */
typedef struct { const short *s; int len; float pos, rate, vl, vr, lpk, lp; int on; } Voice;
#define NVOICE 20
static Voice voices[NVOICE];
static float master = 0.7f, lisX, lisZ, lisYaw;

void snd_volume(int level) { master = snd_clampf(level / 10.0f, 0, 1) * 0.9f; }
void snd_listener(float x, float z, float yaw) { lisX = x; lisZ = z; lisYaw = yaw; }

static void start(int id, float vl, float vr, float lpk, float rate) {
    if (id < 0 || id >= SND_COUNT || !samples[id].data) return;
    int slot = -1; float oldest = 1e30f;
    for (int i = 0; i < NVOICE; i++) {
        if (!voices[i].on) { slot = i; break; }
        float left = (float)voices[i].len - voices[i].pos;
        if (left < oldest) { oldest = left; slot = i; }
    }
    Voice *v = &voices[slot];
    v->s = samples[id].data; v->len = samples[id].len; v->pos = 0; v->rate = rate;
    v->vl = vl; v->vr = vr; v->lpk = lpk; v->lp = 0; v->on = 1;
}
static float jitter(void) { return 1.0f + nz() * 0.04f; }

void snd_play(int id, float vol) { start(id, vol * 0.7071f, vol * 0.7071f, 1.0f, jitter()); }

void snd_play3d(int id, float x, float z, float vol) {
    float dx = x - lisX, dz = z - lisZ, dist = sqrtf(dx * dx + dz * dz);
    float g = vol / (1.0f + dist * 0.09f);
    if (g < 0.01f) return;
    float side = dist > 0.3f ? (dx * cosf(lisYaw) - dz * sinf(lisYaw)) / dist : 0; /* +1 = derecha */
    float pan = snd_clampf(side, -1, 1) * 0.85f;
    float vl = g * sqrtf((1 - pan) * 0.5f), vr = g * sqrtf((1 + pan) * 0.5f);
    float lpk = 1.0f - snd_clampf(dist / 55.0f, 0, 1) * 0.88f; /* lo lejano suena apagado */
    start(id, vl, vr, lpk, jitter());
}

void snd_mix(short *out, int frames) {
    for (int f = 0; f < frames; f++) {
        float l = 0, r = 0;
        for (int i = 0; i < NVOICE; i++) {
            Voice *v = &voices[i]; if (!v->on) continue;
            int p = (int)v->pos; float fr = v->pos - p;
            if (p >= v->len - 1) { v->on = 0; continue; }
            float s = (v->s[p] + (v->s[p + 1] - v->s[p]) * fr) * (1.0f / 32768.0f);
            v->lp += v->lpk * (s - v->lp); s = v->lp;
            l += s * v->vl; r += s * v->vr; v->pos += v->rate;
        }
        if (musData) {
            float tgt = musOn ? 1.0f : 0.0f;
            musGain += (tgt - musGain) * (1.0f / 22050.0f) * (musOn ? 1.0f : 3.0f);
            if (musGain > 0.0005f) {
                int p = (int)musPos; if (p >= musLen - 1) { musPos -= (float)(musLen - 1); p = (int)musPos; }
                float fr = musPos - p, m = (musData[p] + (musData[p + 1] - musData[p]) * fr) * (1.0f / 32768.0f);
                m *= musGain * 0.55f; l += m; r += m; musPos += (float)musRate / (float)SND_RATE;
            }
        }
        l *= master; r *= master;
        l = snd_clampf(l, -1.5f, 1.5f); r = snd_clampf(r, -1.5f, 1.5f);
        l -= l * l * l / 6.75f; r -= r * r * r / 6.75f;
        out[f * 2] = (short)(l * 32000.0f); out[f * 2 + 1] = (short)(r * 32000.0f);
    }
}

/* macros propias de sound.c: se liberan para no afectar a los archivos que siguen */
#undef SR
#undef MAXLEN
#undef NVOICE

/* ################ game.c ################ */
/* Punto Cero (PS2): shooter tactico por rondas contra bots.
 * Render 3D por software (transformacion + recorte) y dibujo de triangulos con gsKit. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* ===== inicio platform.h ===== */
/* Capa de plataforma: el juego solo habla con estas funciones.
 * ps2_platform.c las implementa con gsKit + libpad (consola real / PCSX2).
 * host_platform.c las implementa con un rasterizador por software (pruebas en PC). */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

typedef uint32_t u32c;
#define RGBA(r, g, b, a) ((((u32c)(a)) << 24) | (((u32c)(r)) << 16) | (((u32c)(g)) << 8) | ((u32c)(b)))
#define CR(c) (((c) >> 16) & 255)
#define CG(c) (((c) >> 8) & 255)
#define CB(c) ((c) & 255)
#define CA(c) (((c) >> 24) & 255)

/* Botones (mascara propia, igual en PS2 y en PC) */
enum {
    B_UP = 1 << 0, B_DOWN = 1 << 1, B_LEFT = 1 << 2, B_RIGHT = 1 << 3,
    B_CROSS = 1 << 4, B_CIRCLE = 1 << 5, B_SQUARE = 1 << 6, B_TRIANGLE = 1 << 7,
    B_L1 = 1 << 8, B_R1 = 1 << 9, B_L2 = 1 << 10, B_R2 = 1 << 11,
    B_START = 1 << 12, B_SELECT = 1 << 13, B_L3 = 1 << 14, B_R3 = 1 << 15
};
typedef struct {
    float lx, ly, rx, ry;      /* sticks, -1..1 (arriba = -1 en ly/ry) */
    unsigned down, pressed;    /* botones mantenidos / recien presionados */
} Pad;

extern int SW, SH;             /* resolucion del framebuffer */

void gfx_init(void);
void gfx_begin(u32c clear);
void gfx_end(void);
/* Triangulo con color por vertice; z mayor = mas cerca (0..16777215) */
void gfx_tri(float x1, float y1, int z1, u32c c1,
             float x2, float y2, int z2, u32c c2,
             float x3, float y3, int z3, u32c c3);
/* Rectangulo 2D siempre al frente; el alfa del color se respeta */
void gfx_rect(float x1, float y1, float x2, float y2, u32c c);
void pad_init(void);
void pad_poll(Pad *p);
void audio_init(void);   /* PS2: carga audsrv; PC: nada */
const char *audio_status(void); /* texto corto para mostrar en pantalla: estado del sonido */
void audio_pump(void);   /* llamar una vez por cuadro: mantiene llena la cola de audio */

#ifdef PC_ONLINE
/* Solo version de PC: opciones de video, configuracion guardada, teclado de texto y salida. */
int  pc_cfg_load(char *buf, int max);                  /* devuelve bytes leidos (0 = no hay archivo) */
int  pc_cfg_save(const char *buf, int len);            /* 1 = guardado */
void pc_video_apply(int w, int h, int fullscreen);     /* cambia ventana / modo de pantalla y la resolucion de dibujo */
int  pc_video_poll(int *fullscreen);                   /* 1 si el jugador cambio a/desde pantalla completa con F11 */
void pc_set_brightness(int pct);                       /* 0..100, 50 = normal */
int  pc_text_input(char *out, int max);                /* caracteres escritos desde la ultima llamada (8 = borrar, 13 = enter, 27 = esc) */
void pc_quit(void);                                    /* cierra el juego */
/* Configuracion de controles (teclado y raton): la tabla vive en game.c y la lee pad_poll */
#define PC_NACT 14
extern int pc_bind[PC_NACT][2];                        /* codigo de tecla virtual (VK) por accion: principal y alternativa, 0 = libre */
int  pc_key_last(void);                                /* tecla o boton de raton pulsado desde la llamada anterior (0 = ninguna, 27 = Esc) */
#endif

/* Juego */
void game_init(void);
void game_frame(const Pad *p, float dt);

#endif

/* ===== fin platform.h ===== */


/* ===== inicio models.h ===== */
#ifndef MODELS_H
#define MODELS_H
typedef struct { int nv, nf; const float *v; const unsigned short *f; const unsigned char *c; const unsigned char *p; const unsigned char *vb; } Model;  /* p: 5 = cara del arma. vb: 3 bytes por vertice = hueso A, hueso B, peso de A (0..255) */
extern const Model model_soldado_tactico, model_superviviente, model_zombi_comun;
#endif

/* ===== fin models.h ===== */

#ifdef PC_ONLINE
/* ===== inicio net.h ===== */
/* Capa de red UDP minima (solo PC). Implementada por win_net.c (Winsock) y net_posix.c (pruebas en Linux). */
#ifndef NET_H
#define NET_H
int  net_open(int port);                                   /* 0 = ok. port 0 = cualquiera */
void net_close(void);
int  net_resolve(const char *text, unsigned *ip, int *port, int defport);   /* "a.b.c.d[:puerto]" o nombre; 0 = ok */
int  net_send(unsigned ip, int port, const void *d, int n);
int  net_recv(void *b, int max, unsigned *ip, int *port);  /* >0 bytes recibidos, 0 = nada */
void net_local_ip(char *out, int n);                       /* IP local de la red (para mostrar al anfitrion) */
const char *net_last_error(void);                          /* texto del ultimo fallo de red ("" si no hay) */
#endif

/* ===== fin net.h ===== */

#endif

#define PI 3.14159265f
#define HALF 30.0f
#define MAXBOX 140
#define MAXBOT 10

/* Aviso de autoria: tambien queda dentro del ejecutable */
static const char COPYRIGHT_NOTICE[] __attribute__((used)) = "PUNTO CERO - (C) 2026 LM STUDIOS - TODOS LOS DERECHOS RESERVADOS - PROHIBIDA SU COPIA Y REDISTRIBUCION CON OTRO NOMBRE";
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float rndf(float a, float b) { return a + (rand() / (float)RAND_MAX) * (b - a); }

/* ===== inicio rig.h ===== */
#ifndef RIG_H
#define RIG_H
/* ---------- Esqueleto (rig) de los soldados ----------
 * 12 huesos con piel por vertice (2 huesos por vertice). Animacion procedural mezclada: marcha/carrera con direccion
 * (adelante, atras, lateral), giro, agachado, apuntado, retroceso del arma, recarga y muerte. */
enum { BN_PELVIS, BN_CHEST, BN_HEAD, BN_THIGH_L, BN_SHIN_L, BN_FOOT_L, BN_THIGH_R, BN_SHIN_R, BN_FOOT_R, BN_ARMS, BN_FORE_L, BN_WEAP, BN_N };
typedef struct { float m[12]; } M34;                     /* matriz 3x4 por filas */
typedef struct { float r[BN_N][3], t[BN_N][3], root[4]; } SkPose;     /* rotaciones (x,y,z), traslaciones, raiz: rotX, rotZ, y, z */
typedef struct { float phase, legYaw, crouch, aim, run, spd, vfS, vsS, recoil, t, deadT, yawRate, prevYaw, air; int dead, deadKind, init, pist; } Rig;   /* pist: pistola = una sola mano */
typedef struct { float vx, vz, yaw, pitch, reloadU; int crouch, aim, fired, alive, air; } RigIn;

/* ---------- Esqueleto: matrices, pose y piel ---------- */
static const signed char BN_PARENT[BN_N] = { -1, 0, 1, 0, 3, 4, 0, 6, 7, 1, 9, 9 };
static const float BN_PIV[BN_N][3] = {
    { 0, 0.80f, 0 }, { 0, 0.85f, 0 }, { 0, 1.36f, 0 },
    { -0.19f, 0.78f, 0 }, { -0.19f, 0.47f, 0 }, { -0.19f, 0.22f, 0 },
    { 0.19f, 0.78f, 0 }, { 0.19f, 0.47f, 0 }, { 0.19f, 0.22f, 0 },
    { 0, 1.26f, -0.055f }, { -0.30f, 0.97f, -0.055f }, { 0.17f, 1.12f, -0.38f } };
static float angDiff(float a, float b) { return atan2f(sinf(a - b), cosf(a - b)); }
static float sstep(float a, float b, float x) { float t = clampf((x - a) / (b - a), 0, 1); return t * t * (3 - 2 * t); }
static void m34Mul(M34 *o, const M34 *a, const M34 *b) {
    M34 r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) r.m[i * 4 + j] = a->m[i * 4] * b->m[j] + a->m[i * 4 + 1] * b->m[4 + j] + a->m[i * 4 + 2] * b->m[8 + j];
        r.m[i * 4 + 3] = a->m[i * 4] * b->m[3] + a->m[i * 4 + 1] * b->m[7] + a->m[i * 4 + 2] * b->m[11] + a->m[i * 4 + 3];
    }
    *o = r;
}
static void m34Rot(M34 *o, float rx, float ry, float rz) {        /* R = Ry * Rx * Rz */
    float cx = cosf(rx), sx = sinf(rx), cy = cosf(ry), sy = sinf(ry), cz = cosf(rz), sz = sinf(rz);
    float a[3][3] = { { cz, -sz, 0 }, { sx * 0 + cx * sz, cx * cz, -sx }, { sx * sz, sx * cz, cx } };    /* Rx*Rz */
    float *m = o->m;
    for (int j = 0; j < 3; j++) { m[j] = cy * a[0][j] + sy * a[2][j]; m[4 + j] = a[1][j]; m[8 + j] = -sy * a[0][j] + cy * a[2][j]; }
    m[3] = m[7] = m[11] = 0;
}
static void m34Xf(const M34 *a, const float *p, float *o) {
    o[0] = a->m[0] * p[0] + a->m[1] * p[1] + a->m[2] * p[2] + a->m[3];
    o[1] = a->m[4] * p[0] + a->m[5] * p[1] + a->m[6] * p[2] + a->m[7];
    o[2] = a->m[8] * p[0] + a->m[9] * p[1] + a->m[10] * p[2] + a->m[11];
}
/* Resuelve la jerarquia: G[b] = G[padre] * (T(pivote) R T(-pivote) + t). yaw/wx.. = transformacion al mundo. */
static void rigSolve(const SkPose *po, float wx, float wy, float wz, float yaw, M34 *G) {
    M34 rt, rr, W, B, P;
    m34Rot(&rt, po->root[0], 0, po->root[1]); rt.m[3] = 0; rt.m[7] = po->root[2]; rt.m[11] = po->root[3];
    m34Rot(&rr, 0, yaw, 0); m34Mul(&W, &rr, &rt); W.m[3] += wx; W.m[7] += wy; W.m[11] += wz;
    for (int b = 0; b < BN_N; b++) {
        m34Rot(&B, po->r[b][0], po->r[b][1], po->r[b][2]);
        const float *p = BN_PIV[b];
        for (int i = 0; i < 3; i++) B.m[i * 4 + 3] = p[i] - (B.m[i * 4] * p[0] + B.m[i * 4 + 1] * p[1] + B.m[i * 4 + 2] * p[2]) + po->t[b][i];
        if (BN_PARENT[b] < 0) P = W; else P = G[(int)BN_PARENT[b]];
        m34Mul(&G[b], &P, &B);
    }
}

/* ---------- Animacion: estado y entradas ---------- */
static void rigUpdate(Rig *g, const RigIn *in, float dt) {
    if (!g->init) { g->prevYaw = in->yaw; g->init = 1; }
    g->t += dt;
    if (!in->alive) { if (!g->dead) { g->dead = 1; g->deadT = 0; } g->deadT += dt; return; }
    g->dead = 0; g->deadT = 0;
    float fx = -sinf(in->yaw), fz = -cosf(in->yaw), rx = cosf(in->yaw), rz = -sinf(in->yaw);
    float vf = in->vx * fx + in->vz * fz, vs = in->vx * rx + in->vz * rz, spd = sqrtf(in->vx * in->vx + in->vz * in->vz);
    float k = clampf(dt * 10, 0, 1);
    g->spd = lerpf(g->spd, spd, k); g->vfS = lerpf(g->vfS, vf, k); g->vsS = lerpf(g->vsS, vs, k);
    float yr = clampf(angDiff(in->yaw, g->prevYaw) / (dt > 1e-4f ? dt : 1e-4f), -12, 12); g->prevYaw = in->yaw;
    g->yawRate = lerpf(g->yawRate, yr, clampf(dt * 12, 0, 1));
    g->run = lerpf(g->run, clampf((g->spd - 2.9f) / 2.3f, 0, 1), clampf(dt * 8, 0, 1));
    g->crouch = lerpf(g->crouch, in->crouch ? 1.0f : 0.0f, clampf(dt * 9, 0, 1));
    g->aim = lerpf(g->aim, in->aim ? 1.0f : 0.0f, clampf(dt * 9, 0, 1));
    g->air = lerpf(g->air, in->air ? 1.0f : 0.0f, clampf(dt * 10, 0, 1));
    if (in->fired) g->recoil = 1;
    g->recoil = fmaxf(0, g->recoil - dt * 10);
    /* direccion de las piernas: las piernas giran hacia donde se camina, el torso sigue apuntando al frente */
    float target = 0; int dir = 1;
    if (g->spd > 0.35f) { float th = atan2f(g->vsS, g->vfS); if (fabsf(th) > PI * 0.5f) { th -= th > 0 ? PI : -PI; dir = -1; } target = th; }
    g->legYaw += (target - g->legYaw) * clampf(dt * 9, 0, 1);
    float stride = lerpf(4.3f, 3.3f, g->run) * lerpf(1.0f, 1.15f, g->crouch);
    g->phase += dir * g->spd * dt * stride;
    if (g->spd < 0.25f) {
        if (fabsf(g->yawRate) > 0.6f) g->phase += fabsf(g->yawRate) * dt * 1.1f;       /* pasitos al girar en el lugar */
        else { float n = floorf(g->phase / PI + 0.5f) * PI; g->phase += (n - g->phase) * clampf(dt * 6, 0, 1); }
    }
}
static float bump(float u, float a, float b, float c, float d) { return u < a || u > d ? 0 : (u < b ? sstep(a, b, u) : (u < c ? 1.0f : 1.0f - sstep(c, d, u))); }
/* Pose de muerte: kind 0 = cae de espaldas, 1/2 = cae hacia un costado. Primero las rodillas ceden. */
static void rigDeadPose(const Rig *g, SkPose *o) {
    float u = clampf(g->deadT / 0.95f, 0, 1), s1 = sstep(0.0f, 0.55f, u), s2 = sstep(0.30f, 1.0f, u), sg = g->deadKind == 2 ? -1.0f : 1.0f;
    for (int i = 0; i < 2; i++) {
        int th = i ? BN_THIGH_R : BN_THIGH_L, sh = i ? BN_SHIN_R : BN_SHIN_L, ft = i ? BN_FOOT_R : BN_FOOT_L;
        float a = 1.0f * s1 - 0.65f * s2 + (i ? 0.25f : -0.1f) * s2, b = 1.55f * s1 - 0.95f * s2 + (i ? 0.1f : 0.35f) * s2;
        o->r[th][0] = a; o->r[sh][0] = -b; o->r[ft][0] = -(a - b);
        o->r[th][2] = (i ? 0.18f : -0.12f) * s2;
    }
    o->t[BN_PELVIS][1] = -0.22f * s1 * (1 - s2); o->t[BN_PELVIS][2] = 0.15f * s1 * (1 - s2);
    o->r[BN_CHEST][0] = -0.25f * s1 * (1 - s2) + 0.12f * s2;
    o->r[BN_HEAD][0] = 0.15f * s1 + 0.35f * s2 * (g->deadKind == 0 ? 1 : 0.3f);
    o->r[BN_HEAD][2] = sg * 0.25f * s2;
    if (g->deadKind == 0) {      /* de espaldas: el frente del cuerpo mira al cielo, asi que los brazos caen hacia atras y el arma queda tendida junto al cuerpo */
        o->r[BN_ARMS][0] = -1.15f * s2 - 0.25f * s1 * (1 - s2); o->r[BN_ARMS][2] = 0.38f * s2;
        o->r[BN_FORE_L][0] = 0.35f * s2; o->r[BN_FORE_L][2] = -0.25f * s2;
        o->r[BN_WEAP][0] = 2.55f * s2; o->r[BN_WEAP][2] = 0.0f;
    } else {
        o->r[BN_ARMS][0] = 0.7f * s2 - 0.25f * s1; o->r[BN_ARMS][2] = sg * 0.3f * s2;      /* los brazos se sueltan */
        o->r[BN_FORE_L][0] = -0.6f * s2; o->r[BN_FORE_L][2] = -0.5f * s2;
        o->r[BN_WEAP][2] = sg * 0.5f * s2; o->r[BN_WEAP][0] = -0.35f * s2;
    }
    if (g->deadKind == 0) { o->root[0] = 1.50f * s2; o->root[2] = 0.30f * s2; o->root[3] = 0.10f * s2; }          /* de espaldas */
    else { o->root[1] = (g->deadKind == 2 ? -1 : 1) * 1.50f * s2; o->root[2] = 0.30f * s2; }                       /* de costado */
}
/* Pose viva: marcha, carrera, direccion, agachado, apuntado, retroceso y recarga. */
static void rigPose(const Rig *g, SkPose *o) {
    memset(o, 0, sizeof(*o));
    if (g->dead) { rigDeadPose(g, o); return; }
    float c = g->crouch, ph = g->phase, run = g->run, spd = g->spd;
    float gait = clampf(spd / 1.2f, 0, 1), turnAmp = clampf(fabsf(g->yawRate) / 3.0f, 0, 1) * (spd < 0.25f ? 1 : 0);
    float amt = fmaxf(gait, turnAmp * 0.5f);
    float A = lerpf(0.42f, 0.85f, run) * amt * lerpf(1.0f, 0.6f, c);
    float K = lerpf(0.60f, 1.35f, run) * amt * lerpf(1.0f, 0.7f, c);
    float sp = sinf(ph);
    for (int i = 0; i < 2; i++) {
        float s = sinf(ph + (i ? PI : 0)), cs = cosf(ph + (i ? PI : 0));
        int th = i ? BN_THIGH_R : BN_THIGH_L, sh = i ? BN_SHIN_R : BN_SHIN_L, ft = i ? BN_FOOT_R : BN_FOOT_L;
        float alpha = A * s + 1.35f * c + 0.40f * g->air * (i ? 1.0f : 0.5f);
        float beta = K * fmaxf(0, cs) + 0.05f * amt + 1.85f * c + 0.65f * g->air * (i ? 1.0f : 0.5f);
        float toe = 0.35f * fmaxf(0, cs) * fmaxf(0, -s) * amt;
        o->r[th][0] = alpha; o->r[sh][0] = -beta; o->r[ft][0] = -(alpha - beta) - toe;
    }
    float bob = fabsf(sp) * lerpf(0.030f, 0.060f, run) * amt;
    float tw = 0.10f * amt * sp;
    o->t[BN_PELVIS][0] = 0.02f * amt * sp; o->t[BN_PELVIS][1] = -bob - 0.28f * c - 0.05f * g->air; o->t[BN_PELVIS][2] = 0.19f * c;
    o->r[BN_PELVIS][1] = -g->legYaw + tw; o->r[BN_PELVIS][2] = 0.05f * amt * sp + 0.012f * sinf(g->t * 0.9f) * (1 - amt);
    o->r[BN_CHEST][1] = g->legYaw * 0.7f - tw * 0.8f; o->r[BN_CHEST][2] = -0.6f * o->r[BN_PELVIS][2];
    o->r[BN_HEAD][1] = g->legYaw * 0.3f - tw * 0.2f;
    o->r[BN_CHEST][0] = -(0.04f + 0.12f * run) * amt - 0.28f * c + 0.012f * sinf(g->t * 1.6f) * (1 - amt);
    /* brazos: arma baja al caminar, arriba al apuntar; el retroceso y la recarga se suman */
    float low = lerpf(-0.50f, -0.80f, run) * (1 - g->aim);
    o->r[BN_ARMS][0] = low + 0.02f * sinf(ph * 2) * amt + 0.07f * g->recoil;
    o->t[BN_WEAP][2] = 0.06f * g->recoil; o->r[BN_WEAP][0] = 0.05f * g->recoil;
    o->r[BN_ARMS][2] = 0.015f * sinf(ph) * amt;
    if (g->pist) {      /* pistola: la mano izquierda queda libre y cuelga con naturalidad, sin sujetar el arma */
        float sw = 0.10f * amt * sinf(ph + PI);
        o->r[BN_FORE_L][0] = -1.30f - sw - 0.35f * g->aim; o->r[BN_FORE_L][2] = -0.12f; o->r[BN_FORE_L][1] = 0.10f;
        o->r[BN_ARMS][0] += 0.18f * (1 - g->aim);      /* el brazo de la pistola va un poco mas alto y extendido */
    }
}
/* Apuntado vertical: reparte el pitch entre torso, cabeza y brazos (el arma queda alineada con la mira). */
static void rigAimPitch(const Rig *g, SkPose *o, float pitch) {
    if (g->dead) return;
    float p = clampf(pitch, -0.95f, 0.95f), cs = lerpf(0.15f, 0.35f, g->aim);
    o->r[BN_CHEST][0] += cs * p;
    o->r[BN_HEAD][0] += (1 - cs) * p * 0.9f;
    float armsFull = p - (o->r[BN_CHEST][0]);         /* para que el arma apunte exactamente al pitch */
    o->r[BN_ARMS][0] += g->aim * (armsFull) + (1 - g->aim) * 0.5f * p * 0;
}
static void rigReload(const Rig *g, SkPose *o, float u) {
    if (u < 0 || g->dead) return;
    float e1 = bump(u, 0.05f, 0.28f, 0.62f, 0.90f), s = sinf(clampf(u, 0, 1) * PI);
    if (g->pist) { o->r[BN_FORE_L][0] += 0.95f * e1; o->r[BN_FORE_L][2] += 0.10f * e1; }       /* pistola: la mano libre sube a buscar el cargador */
    else { o->r[BN_FORE_L][0] += -0.9f * e1; o->r[BN_FORE_L][2] += -0.55f * e1; o->r[BN_FORE_L][1] += 0.25f * e1; }
    o->r[BN_WEAP][2] += 0.30f * s; o->r[BN_WEAP][0] += 0.22f * s;
    o->r[BN_ARMS][0] += -0.12f * s;
}
#endif

/* ===== fin rig.h ===== */


/* ---------- Configuracion ---------- */
typedef struct { int diff, bots, rounds, sens, fov, invertY, cross, volume, skin, cam, map; } Settings;
#ifdef _EE
static Settings S = { 1, 4, 5, 5, 80, 0, 0, 7, 0, 0, 0 };   /* PS2: primera persona */
#else
static Settings S = { 1, 4, 5, 5, 80, 0, 0, 7, 0, 1, 0 };
#endif
static const char *DIFF_NAMES[] = { "FACIL", "NORMAL", "DIFICIL" };
static const float DIFF_HP[] = { 70, 100, 130 }, DIFF_REACT[] = { 0.85f, 0.5f, 0.28f };
static const float DIFF_AIM[] = { 0.16f, 0.09f, 0.045f }, DIFF_DMG[] = { 6, 9, 12 };
static const float DIFF_FIRE[] = { 0.55f, 0.38f, 0.26f }, DIFF_SPEED[] = { 3.0f, 3.6f, 4.2f };
static const int ROUND_OPTS[] = { 3, 5, 8, 12 };
static const char *SKIN_NAMES[] = { "SOLDADOS", "SUPERVIVIENTES", "ZOMBIS", "MIXTO" };
static const char *CAM_NAMES[] = { "PRIMERA PERSONA", "TERCERA PERSONA" };
static const char *CROSS_NAMES[] = { "BLANCO", "VERDE", "CIAN", "NARANJA", "ROJO" };
static const u32c CROSS_COL[] = { RGBA(255, 255, 255, 255), RGBA(111, 207, 151, 255), RGBA(91, 214, 224, 255), RGBA(224, 128, 58, 255), RGBA(229, 72, 77, 255) };

/* ---------- Mapa ---------- */
typedef struct { float x0, x1, z0, z1, top; u32c col; } Box;
static Box boxes[MAXBOX];
static int navBuildPending = 1;
static int nbox = 0;

static void addBox(float x, float z, float w, float d, float h, u32c c) {
    if (nbox >= MAXBOX) return;
    Box *b = &boxes[nbox++];
    b->x0 = x - w / 2; b->x1 = x + w / 2; b->z0 = z - d / 2; b->z1 = z + d / 2; b->top = h; b->col = c;
}

/* ===== inicio maps.h ===== */
/* PUNTO CERO - mapas (C) 2026 LM STUDIOS.  Todos los mapas ocupan la arena de 60 x 60 m (HALF = 30) y se construyen con cajas. */
#ifndef MAPS_H
#define MAPS_H

#define NMAPS 8
typedef struct { float x, z; } SpawnPt;
typedef struct { const char *name; u32c floorA, floorB, sky; int np, nb; SpawnPt ps[4], bs[10]; } MapDef;
static int curMap = 0;

/* ---- ayudas de construccion ---- */
#define B(x, z, w, d, h, c) addBox((x), (z), (w), (d), (h), (c))
static void wallH(float z, float xa, float xb, float h, u32c c) { addBox((xa + xb) * .5f, z, xb - xa, .8f, h, c); }     /* pared a lo largo de X */
static void wallV(float x, float za, float zb, float h, u32c c) { addBox(x, (za + zb) * .5f, .8f, zb - za, h, c); }     /* pared a lo largo de Z */
/* habitacion: doors = 1 norte(z-), 2 sur(z+), 4 oeste(x-), 8 este(x+); puerta de 3.4 m centrada, dx desplaza las puertas */
static void room(float cx, float cz, float w, float d, float h, u32c c, int doors, float off) {
    float x0 = cx - w / 2, x1 = cx + w / 2, z0 = cz - d / 2, z1 = cz + d / 2, g = 1.7f;
    if (doors & 1) { wallH(z0, x0, cx + off - g, h, c); wallH(z0, cx + off + g, x1, h, c); } else wallH(z0, x0, x1, h, c);
    if (doors & 2) { wallH(z1, x0, cx + off - g, h, c); wallH(z1, cx + off + g, x1, h, c); } else wallH(z1, x0, x1, h, c);
    if (doors & 4) { wallV(x0, z0 + .4f, cz + off - g, h, c); wallV(x0, cz + off + g, z1 - .4f, h, c); } else wallV(x0, z0 + .4f, z1 - .4f, h, c);
    if (doors & 8) { wallV(x1, z0 + .4f, cz + off - g, h, c); wallV(x1, cz + off + g, z1 - .4f, h, c); } else wallV(x1, z0 + .4f, z1 - .4f, h, c);
}
static void outer(u32c c) {
    addBox(0, -HALF - 0.5f, HALF * 2 + 2, 1, 6, c); addBox(0, HALF + 0.5f, HALF * 2 + 2, 1, 6, c);
    addBox(-HALF - 0.5f, 0, 1, HALF * 2, 6, c); addBox(HALF + 0.5f, 0, 1, HALF * 2, 6, c);
}

/* 0: BASE MILITAR (disposicion original conservada) */
static void mapBase(void) {
    const u32c WALL = RGBA(194, 171, 134, 255), WALL2 = RGBA(168, 150, 120, 255), CONC = RGBA(140, 146, 144, 255);
    const u32c CRATE = RGBA(142, 106, 67, 255), TEAL = RGBA(79, 122, 128, 255), RUST = RGBA(154, 90, 60, 255), OUT = RGBA(125, 116, 106, 255);
    outer(OUT);
    static const float bld[8][4] = { {-3.75f,-6,4.5f,.8f}, {3.75f,-6,4.5f,.8f}, {-3.75f,6,4.5f,.8f}, {3.75f,6,4.5f,.8f},
                                     {-6,-3.75f,.8f,4.5f}, {-6,3.75f,.8f,4.5f}, {6,-3.75f,.8f,4.5f}, {6,3.75f,.8f,4.5f} };
    for (int i = 0; i < 8; i++) addBox(bld[i][0], bld[i][1], bld[i][2], bld[i][3], 3.6f, CONC);
    addBox(0, 0, 2, 2, 1, CRATE); addBox(-2.6f, 2.6f, 1.4f, 1.4f, 2.2f, CRATE);
    for (int sx = -1; sx <= 1; sx += 2) for (int sz = -1; sz <= 1; sz += 2) {
        addBox(sx * 18, sz * 12, 9, .8f, 3.5f, WALL);
        addBox(sx * 13.6f, sz * 17.4f, .8f, 10, 3.5f, WALL2);
        addBox(sx * 8, sz * 14, 2, 2, 1, CRATE);
        addBox(sx * 9.4f, sz * 13.4f, 1.6f, 1.6f, 2.2f, CRATE);
        addBox(sx * 25.5f, sz * 6, 3, 3, 2.2f, CRATE);
        addBox(sx * 26, sz * 22, 2.2f, 2.2f, 2.2f, CRATE);
        addBox(sx * 3.8f, sz * 20, 4, 1, 1, CONC);
    }
    for (int sx = -1; sx <= 1; sx += 2) {
        addBox(sx * 22, 0, 2.6f, 9, 2.6f, sx < 0 ? TEAL : RUST);
        addBox(sx * 22, -11, 2.6f, 6, 2.6f, sx < 0 ? RUST : TEAL);
        addBox(sx * 22, 11, 2.6f, 6, 2.6f, sx < 0 ? RUST : TEAL);
    }
    addBox(0, -25, 7, 1, 1, CONC); addBox(0, 25, 7, 1, 1, CONC);
}

/* 1: CIUDAD URBANA - calles, manzanas de edificios, autos y una plaza central */
static void mapCity(void) {
    const u32c BA = RGBA(150, 120, 104, 255), BB = RGBA(128, 134, 140, 255), BC = RGBA(176, 160, 128, 255), CAR1 = RGBA(70, 98, 128, 255), CAR2 = RGBA(150, 62, 58, 255), CAR3 = RGBA(190, 186, 170, 255), CUR = RGBA(110, 110, 112, 255);
    outer(RGBA(104, 108, 114, 255));
    B(-20, -20, 13, 11, 6.5f, BA); B(0, -21, 12, 9, 5, BB); B(20, -20, 13, 11, 7, BC);
    B(-21, 0, 11, 12, 5.5f, BB); B(21, 0, 11, 12, 6, BA);
    B(-20, 20, 13, 11, 6, BC); B(0, 21, 12, 9, 5.5f, BA); B(20, 20, 13, 11, 6.5f, BB);
    B(0, 0, 4, 4, 1.0f, CUR); B(0, 0, 1.4f, 1.4f, 2.2f, CUR);                                      /* plaza: fuente */
    B(-8, -9, 2.2f, 4.6f, 1.4f, CAR1); B(9, 8, 2.2f, 4.6f, 1.4f, CAR2); B(-9.5f, 10, 4.6f, 2.2f, 1.4f, CAR3); B(10, -8.5f, 4.6f, 2.2f, 1.4f, CAR1);
    B(-27, -9, 2.2f, 4.6f, 1.4f, CAR2); B(27, 9.5f, 2.2f, 4.6f, 1.4f, CAR3); B(-4, 14, 4.6f, 2.2f, 1.4f, CAR2); B(5, -14, 4.6f, 2.2f, 1.4f, CAR3);
    B(-12.5f, 0, 1.2f, 5, 1.2f, CUR); B(12.5f, 0, 1.2f, 5, 1.2f, CUR);                              /* barricadas */
    B(0, -10.5f, 6, 1.0f, 1.1f, CUR); B(0, 10.5f, 6, 1.0f, 1.1f, CUR);
    B(-26, 27, 3, 3, 2.2f, CAR3); B(26, -27, 3, 3, 2.2f, CAR3);
}

/* 2: ZONA INDUSTRIAL - nave central con dos puertas por lado, contenedores, tanques */
static void mapIndustrial(void) {
    const u32c SH1 = RGBA(70, 110, 120, 255), SH2 = RGBA(160, 86, 58, 255), SH3 = RGBA(108, 120, 72, 255), SH4 = RGBA(190, 150, 70, 255), MET = RGBA(118, 122, 126, 255), TANK = RGBA(168, 168, 160, 255);
    outer(RGBA(96, 98, 100, 255));
    room(0, 0, 22, 16, 5.5f, MET, 15, 0);                      /* nave con puerta en cada lado */
    B(-5, 0, 1.4f, 1.4f, 5.5f, MET); B(5, 0, 1.4f, 1.4f, 5.5f, MET); B(0, -3.2f, 5, 1.6f, 1.3f, SH4); B(0, 3.2f, 5, 1.6f, 1.3f, SH4);
    for (int i = 0; i < 3; i++) { B(-23 + i * 2.8f, -22, 2.6f, 6.2f, 2.6f, i == 1 ? SH2 : SH1); B(-23 + i * 2.8f, -14, 2.6f, 6.2f, 2.6f, i == 1 ? SH1 : SH3); }
    for (int i = 0; i < 3; i++) { B(23 - i * 2.8f, 22, 2.6f, 6.2f, 2.6f, i == 1 ? SH1 : SH2); B(23 - i * 2.8f, 14, 2.6f, 6.2f, 2.6f, i == 1 ? SH3 : SH1); }
    B(-16, -25.5f, 5, 5, 4.2f, TANK); B(-9.5f, -25.5f, 5, 5, 4.2f, TANK); B(16, 25.5f, 5, 5, 4.2f, TANK); B(9.5f, 25.5f, 5, 5, 4.2f, TANK);
    B(22, -21, 6, 2.6f, 2.6f, SH2); B(22, -17, 6, 2.6f, 2.6f, SH3); B(-22, 18, 6, 2.6f, 2.6f, SH3); B(-22, 22, 6, 2.6f, 2.6f, SH2);
    B(-18, 0, 1, 8, 1.2f, MET); B(18, 0, 1, 8, 1.2f, MET); B(0, -15, 8, 1, 1.2f, MET); B(0, 15, 8, 1, 1.2f, MET);
    B(-26, 5, 2.4f, 2.4f, 2.2f, SH4); B(26, -5, 2.4f, 2.4f, 2.2f, SH4); B(-12, 8, 2.2f, 2.2f, 1.2f, SH4); B(12, -8, 2.2f, 2.2f, 1.2f, SH4);
}

/* 3: ALMACEN - estanterias en pasillos, zona de carga, oficina */
static void mapWarehouse(void) {
    const u32c SHELF = RGBA(150, 110, 70, 255), SHELF2 = RGBA(108, 120, 130, 255), CR = RGBA(166, 128, 82, 255), OFF = RGBA(180, 176, 160, 255);
    outer(RGBA(112, 104, 96, 255));
    for (int i = 0; i < 4; i++) { float x = -21 + i * 7;                                           /* 4 columnas de estanterias con pasos transversales */
        B(x, -17.5f, 1.4f, 8.5f, 3.0f, i & 1 ? SHELF2 : SHELF); B(x, -4.5f, 1.4f, 8.5f, 3.0f, i & 1 ? SHELF : SHELF2);
        B(x, 8, 1.4f, 5.5f, 3.0f, i & 1 ? SHELF2 : SHELF); }
    room(-20, 22, 14, 10, 3.4f, OFF, 1 | 8, 0);                                                    /* oficina */
    B(-20, 23, 3, 1.4f, 1.0f, CR);
    for (int i = 0; i < 4; i++) { B(8 + i * 5.2f, 24.5f, 2.2f, 2.2f, 2.2f, CR); B(10.5f + i * 5.2f, 20.5f, 1.8f, 1.8f, 1.8f, SHELF); }
    B(25.5f, -22, 3, 6, 2.2f, CR); B(25.5f, -12, 3, 3, 1.4f, CR); B(-26, 2, 2.4f, 2.4f, 2.2f, CR); B(26, 6, 2.4f, 2.4f, 2.2f, CR);
    B(0, 0, 6, 2.4f, 1.1f, CR); B(-9, 12, 2, 2, 1.1f, CR); B(14, 10, 2, 2, 1.1f, CR); B(0, 28.4f, 8, 1.2f, 2.6f, SHELF2); B(0, -28, 6, 1.4f, 2.6f, SHELF);
}

/* 4: INSTALACION MILITAR - bunker de habitaciones y pasillos */
static void mapBunker(void) {
    const u32c W1 = RGBA(120, 130, 122, 255), W2 = RGBA(96, 106, 100, 255), C = RGBA(142, 118, 80, 255), METAL = RGBA(76, 84, 90, 255);
    outer(RGBA(86, 92, 88, 255));
    room(0, 0, 20, 18, 3.6f, W1, 15, 0);                                                           /* sala de mando central */
    B(0, 0, 4.2f, 2.4f, 1.1f, METAL); B(-6.5f, -4.5f, 1.6f, 1.6f, 1.3f, C); B(6.5f, 4.5f, 1.6f, 1.6f, 1.3f, C);
    room(-21, -19, 14, 12, 3.4f, W2, 2 | 8, 0); room(21, -19, 14, 12, 3.4f, W2, 2 | 4, 0);        /* barracones norte */
    room(-21, 19, 14, 12, 3.4f, W2, 1 | 8, 0); room(21, 19, 14, 12, 3.4f, W2, 1 | 4, 0);          /* barracones sur */
    B(-21, -19, 3, 1.6f, 1.1f, C); B(21, 19, 3, 1.6f, 1.1f, C); B(-24, 19, 1.6f, 4, 1.1f, METAL); B(24, -19, 1.6f, 4, 1.1f, METAL);
    room(0, -25, 14, 8, 3.4f, W1, 2, 0); room(0, 25, 14, 8, 3.4f, W1, 1, 0);                      /* puestos de guardia */
    B(-10, -12, 1.6f, 1.6f, 1.3f, C); B(10, 12, 1.6f, 1.6f, 1.3f, C); B(-11, 11, 1.6f, 1.6f, 1.3f, METAL); B(11, -11, 1.6f, 1.6f, 1.3f, METAL);
    B(-26.5f, 0, 2, 7, 1.2f, C); B(26.5f, 0, 2, 7, 1.2f, C);
}

/* 5: ZONA DESERTICA - rocas, dunas bajas, ruinas y restos de vehiculos */
static void mapDesert(void) {
    const u32c ROCK = RGBA(150, 112, 78, 255), ROCK2 = RGBA(128, 94, 66, 255), RUIN = RGBA(194, 170, 128, 255), WRECK = RGBA(98, 100, 84, 255), DUNE = RGBA(206, 180, 128, 255);
    outer(RGBA(172, 140, 98, 255));
    B(-14, -14, 7, 6, 3.4f, ROCK); B(-11, -16.5f, 4, 3.5f, 2.2f, ROCK2); B(15, -9, 6, 7, 4.2f, ROCK2); B(18, -6, 3, 3, 2.4f, ROCK);
    B(-18, 8, 6, 5, 3.0f, ROCK2); B(-15, 11.5f, 3.5f, 3.5f, 2.0f, ROCK); B(8, 15, 8, 5, 3.4f, ROCK); B(24, 18, 5, 5, 2.8f, ROCK2); B(-26, -22, 4, 4, 3.0f, ROCK);
    B(0, -4, 12, 0.8f, 2.8f, RUIN); B(-5.6f, -1.5f, .8f, 5, 2.8f, RUIN); B(5.6f, -1.5f, .8f, 5, 2.8f, RUIN); B(0, 4.5f, 5, 0.8f, 2.8f, RUIN);   /* ruina de adobe con patio */
    B(-24, 0, 4.6f, 2.2f, 1.5f, WRECK); B(-21.5f, 2.6f, 2.2f, 3.6f, 1.2f, WRECK); B(24, -22, 5, 2.4f, 1.6f, WRECK);
    for (int i = 0; i < 4; i++) { B(-8 + i * 6, 24, 5, 3, 0.5f, DUNE); B(-20 + i * 9, -27, 6, 2.6f, 0.5f, DUNE); }                          /* dunas escalables */
    B(-5, 18, 3, 1.2f, 1.1f, RUIN); B(22, 3, 1.2f, 5, 1.1f, RUIN); B(-27, 15, 2.4f, 2.4f, 2.0f, ROCK); B(3, 8.5f, 1.6f, 1.6f, 1.2f, ROCK2);
}

/* 6: COMPLEJO DE EDIFICIOS - cuatro edificios con patio central */
static void mapComplex(void) {
    const u32c W1 = RGBA(186, 170, 146, 255), W2 = RGBA(150, 140, 126, 255), C = RGBA(140, 108, 70, 255), P = RGBA(120, 128, 120, 255);
    outer(RGBA(110, 106, 100, 255));
    room(-18, -17, 18, 14, 4.2f, W1, 2 | 8, 0); wallV(-18, -17.4f, -9.6f, 4.2f, W2);                 /* edificio A con tabique interior */
    room(18, -17, 18, 14, 4.2f, W2, 2 | 4, 0); wallH(-17, 14, 19, 4.2f, W1);                        /* edificio B */
    room(-18, 17, 18, 14, 4.2f, W2, 1 | 8, 0); wallH(17, -22, -15, 4.2f, W1);                       /* edificio C */
    room(18, 17, 18, 14, 4.2f, W1, 1 | 4, 0); wallV(18, 10.6f, 14.5f, 4.2f, W2);                    /* edificio D */
    B(0, 0, 5, 5, 1.2f, P); B(-6, 0, 1.4f, 1.4f, 2.2f, P); B(6, 0, 1.4f, 1.4f, 2.2f, P); B(0, -7, 1.4f, 1.4f, 2.2f, P); B(0, 7, 1.4f, 1.4f, 2.2f, P);
    B(-18, -17, 2.2f, 2.2f, 1.2f, C); B(18, -17, 2.2f, 2.2f, 1.2f, C); B(-18, 17, 2.2f, 2.2f, 1.2f, C); B(18, 17, 2.2f, 2.2f, 1.2f, C);
    B(-26.5f, 0, 2, 8, 1.2f, C); B(26.5f, 0, 2, 8, 1.2f, C); B(0, -27, 8, 1.4f, 1.2f, P); B(0, 27, 8, 1.4f, 1.2f, P);
}

/* 7: COMBATE EXTERIOR - campo abierto con trincheras, sacos de arena y casa en ruinas */
static void mapField(void) {
    const u32c SAND = RGBA(150, 134, 96, 255), LOG = RGBA(96, 80, 56, 255), RUIN = RGBA(170, 156, 134, 255), TANK = RGBA(86, 96, 76, 255), ROCK = RGBA(116, 112, 100, 255);
    outer(RGBA(80, 90, 70, 255));
    B(-14, -9, 12, 1.2f, 1.2f, SAND); B(14, -9, 12, 1.2f, 1.2f, SAND); B(-14, 9, 12, 1.2f, 1.2f, SAND); B(14, 9, 12, 1.2f, 1.2f, SAND);    /* lineas de trinchera */
    B(0, -17, 9, 1.0f, 1.1f, SAND); B(0, 17, 9, 1.0f, 1.1f, SAND); B(-25, -4, 1.2f, 8, 1.2f, SAND); B(25, 4, 1.2f, 8, 1.2f, SAND);
    room(0, 0, 10, 8, 3.4f, RUIN, 2 | 4 | 8, 0); /* casa en ruinas */
    B(-20, -22, 7, 3.2f, 2.0f, TANK); B(-17, -21.5f, 2.6f, 2.0f, 2.8f, TANK); B(21, 22, 7, 3.2f, 2.0f, TANK);
    B(-8, -25, 3, 3, 2.0f, ROCK); B(9, -23, 3.6f, 3, 2.4f, ROCK); B(-9, 24, 3.6f, 3, 2.4f, ROCK); B(7, 25, 3, 3, 2.0f, ROCK);
    B(-24, 18, 5, 0.9f, 0.5f, LOG); B(24, -18, 5, 0.9f, 0.5f, LOG); B(-5, 12.5f, 1.4f, 1.4f, 1.0f, LOG); B(5, -12.5f, 1.4f, 1.4f, 1.0f, LOG);
}

static void (*const MAP_BUILD[NMAPS])(void) = { mapBase, mapCity, mapIndustrial, mapWarehouse, mapBunker, mapDesert, mapComplex, mapField };

#define SP(x, z) { (x), (z) }
static const MapDef MAPS[NMAPS] = {
 { "BASE MILITAR", RGBA(166, 150, 120, 255), RGBA(179, 163, 132, 255), RGBA(201, 183, 156, 255), 3, 8,
   { SP(-0.5f,26.5f), SP(-24.0f,26.0f), SP(24.0f,26.0f) }, { SP(0.0f,-27.0f), SP(-25.0f,-27.0f), SP(25.0f,-27.0f), SP(-12.5f,-21.5f), SP(12.5f,-21.5f), SP(-9.0f,-11.0f), SP(9.0f,-11.0f), SP(0.0f,-15.0f) } },
 { "CIUDAD", RGBA(112, 112, 108, 255), RGBA(122, 122, 118, 255), RGBA(176, 190, 204, 255), 3, 8,
   { SP(0.0f,26.5f), SP(-8.0f,14.0f), SP(8.0f,14.0f) }, { SP(0.0f,-27.0f), SP(-9.0f,-27.0f), SP(9.0f,-27.0f), SP(-10.5f,-9.5f), SP(8.5f,-10.5f), SP(-27.5f,-12.5f), SP(26.5f,12.5f), SP(0.0f,-13.0f) } },
 { "ZONA INDUSTRIAL", RGBA(120, 118, 112, 255), RGBA(132, 130, 124, 255), RGBA(170, 178, 182, 255), 3, 8,
   { SP(0.0f,28.0f), SP(-27.0f,28.0f), SP(27.0f,-28.0f) }, { SP(0.0f,-28.0f), SP(-27.0f,-28.0f), SP(27.0f,-10.0f), SP(-27.0f,10.0f), SP(-20.0f,0.0f), SP(20.0f,0.0f), SP(0.0f,11.0f), SP(0.0f,-11.0f) } },
 { "ALMACEN", RGBA(122, 116, 106, 255), RGBA(110, 104, 96, 255), RGBA(150, 146, 140, 255), 3, 8,
   { SP(-0.5f,26.5f), SP(-8.0f,14.0f), SP(10.0f,26.0f) }, { SP(-28.0f,-27.0f), SP(-10.5f,-1.8f), SP(10.5f,-1.8f), SP(-0.5f,-26.5f), SP(28.0f,-27.0f), SP(-28.0f,8.0f), SP(28.0f,8.0f), SP(-0.5f,4.5f) } },
 { "INSTALACION MILITAR", RGBA(98, 104, 100, 255), RGBA(108, 114, 110, 255), RGBA(118, 126, 124, 255), 3, 8,
   { SP(-8.5f,26.5f), SP(8.5f,26.5f), SP(0.0f,19.0f) }, { SP(-8.5f,-26.5f), SP(8.5f,-26.5f), SP(-21.5f,-20.5f), SP(21.0f,-19.0f), SP(11.5f,-16.5f), SP(-21.0f,19.0f), SP(0.0f,6.0f), SP(0.0f,-6.0f) } },
 { "DESIERTO", RGBA(196, 168, 118, 255), RGBA(206, 180, 128, 255), RGBA(226, 204, 160, 255), 3, 8,
   { SP(0.0f,28.0f), SP(-22.0f,22.0f), SP(22.0f,26.0f) }, { SP(0.0f,-28.0f), SP(-27.0f,-10.0f), SP(27.0f,-14.0f), SP(-6.0f,-10.0f), SP(8.0f,-2.0f), SP(-24.0f,12.0f), SP(10.0f,20.0f), SP(0.0f,12.0f) } },
 { "COMPLEJO DE EDIFICIOS", RGBA(132, 130, 120, 255), RGBA(144, 142, 132, 255), RGBA(196, 190, 176, 255), 3, 8,
   { SP(-0.5f,25.5f), SP(-7.5f,11.5f), SP(7.5f,11.5f) }, { SP(-0.5f,-28.5f), SP(-18.5f,-19.5f), SP(17.5f,-19.5f), SP(-18.5f,14.5f), SP(15.5f,16.5f), SP(-3.0f,-10.0f), SP(3.0f,10.0f), SP(-28.5f,-2.5f) } },
 { "COMBATE EXTERIOR", RGBA(120, 124, 92, 255), RGBA(132, 136, 102, 255), RGBA(184, 196, 190, 255), 3, 8,
   { SP(0.0f,28.0f), SP(-14.0f,24.0f), SP(14.0f,24.0f) }, { SP(0.0f,-28.0f), SP(-14.0f,-24.0f), SP(14.0f,-26.0f), SP(-25.0f,-15.0f), SP(25.0f,-13.0f), SP(-10.0f,-14.0f), SP(10.0f,-14.0f), SP(0.0f,-10.0f) } },
};
#define SKY (MAPS[curMap].sky)
#endif

/* ===== fin maps.h ===== */

static void buildWorld(void) {
    nbox = 0;
    MAP_BUILD[curMap]();
    navBuildPending = 1;
}

#define STEP 0.55f
static float groundAt(float x, float z, float feet, float r) {
    float g = 0;
    for (int i = 0; i < nbox; i++) {
        Box *b = &boxes[i];
        if (b->top > g && b->top <= feet + STEP && x > b->x0 - r * .5f && x < b->x1 + r * .5f && z > b->z0 - r * .5f && z < b->z1 + r * .5f) g = b->top;
    }
    return g;
}
static int blockedAt(float x, float z, float feet, float r) {
    for (int i = 0; i < nbox; i++) {
        Box *b = &boxes[i];
        if (b->top > feet + STEP && x > b->x0 - r && x < b->x1 + r && z > b->z0 - r && z < b->z1 + r) return 1;
    }
    return 0;
}
static void moveBody(float *x, float *z, float y, float dx, float dz, float r, int *hit) {
    if (!blockedAt(*x + dx, *z, y, r)) *x += dx; else if (hit) *hit = 1;
    if (!blockedAt(*x, *z + dz, y, r)) *z += dz; else if (hit) *hit = 1;
}

static float rayBox(const float *o, const float *d, const float *mn, const float *mx) {
    float t0 = 0, t1 = 1e30f;
    for (int i = 0; i < 3; i++) {
        if (fabsf(d[i]) < 1e-9f) { if (o[i] < mn[i] || o[i] > mx[i]) return 1e30f; continue; }
        float a = (mn[i] - o[i]) / d[i], b = (mx[i] - o[i]) / d[i];
        if (a > b) { float t = a; a = b; b = t; }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
        if (t0 > t1) return 1e30f;
    }
    return t0;
}
static float raySphere(const float *o, const float *d, const float *c, float r) {
    float l[3] = { c[0] - o[0], c[1] - o[1], c[2] - o[2] };
    float tc = l[0] * d[0] + l[1] * d[1] + l[2] * d[2];
    if (tc < 0) return 1e30f;
    float d2 = l[0] * l[0] + l[1] * l[1] + l[2] * l[2] - tc * tc;
    if (d2 > r * r) return 1e30f;
    return tc - sqrtf(r * r - d2);
}
static float wallDist(const float *o, const float *d, float max) {
    float best = max;
    for (int i = 0; i < nbox; i++) {
        float mn[3] = { boxes[i].x0, 0, boxes[i].z0 }, mx[3] = { boxes[i].x1, boxes[i].top, boxes[i].z1 };
        float t = rayBox(o, d, mn, mx);
        if (t < best) best = t;
    }
    if (d[1] < 0) { float t = -o[1] / d[1]; if (t > 0 && t < best) best = t; }
    return best;
}
static int hasLOS(const float *a, const float *b) {
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    d[0] /= L; d[1] /= L; d[2] /= L;
    return wallDist(a, d, L) >= L - 0.05f;
}
/* ---------- Navegacion: cuadricula de 1 m, BFS de 8 vecinos, componente principal ---------- */
#define NG 60
static unsigned char navB[NG * NG];
static short navComp[NG * NG], navMain = 0;
static short navPar[NG * NG], navQ[NG * NG];
static int navCellOf(float x, float z) { int i = (int)(x + HALF), j = (int)(z + HALF); i = i < 0 ? 0 : (i >= NG ? NG - 1 : i); j = j < 0 ? 0 : (j >= NG ? NG - 1 : j); return j * NG + i; }
static void navCenter(int c, float *x, float *z) { *x = (c % NG) - HALF + 0.5f; *z = (c / NG) - HALF + 0.5f; }
static void navBuild(void) {
    for (int c = 0; c < NG * NG; c++) { float x, z; navCenter(c, &x, &z); navB[c] = blockedAt(x, z, 0, 0.5f) || x < -HALF + 0.6f || x > HALF - 0.6f || z < -HALF + 0.6f || z > HALF - 0.6f; navComp[c] = 0; }
    int nc = 0, best = 0, bestN = 0;
    for (int c = 0; c < NG * NG; c++) {
        if (navB[c] || navComp[c]) continue;
        nc++; int h = 0, t = 0, n = 0; navQ[t++] = (short)c; navComp[c] = (short)nc;
        while (h < t) { int u = navQ[h++]; n++; int ux = u % NG, uz = u / NG; static const int dx[4] = { 1, -1, 0, 0 }, dz[4] = { 0, 0, 1, -1 };
            for (int k = 0; k < 4; k++) { int vx = ux + dx[k], vz = uz + dz[k]; if (vx < 0 || vz < 0 || vx >= NG || vz >= NG) continue; int v = vz * NG + vx; if (navB[v] || navComp[v]) continue; navComp[v] = (short)nc; navQ[t++] = (short)v; } }
        if (n > bestN) { bestN = n; best = nc; }
    }
    navMain = (short)best; navBuildPending = 0;
}
static int navOk(float x, float z) { if (navBuildPending) navBuild(); int c = navCellOf(x, z); return !navB[c] && navComp[c] == navMain; }
static int navSnap(float x, float z) {              /* celda libre de la componente principal mas cercana */
    if (navBuildPending) navBuild();
    int c0 = navCellOf(x, z), cx = c0 % NG, cz = c0 / NG;
    for (int r = 0; r < NG; r++) for (int j = cz - r; j <= cz + r; j++) for (int i = cx - r; i <= cx + r; i++) {
        if (i < 0 || j < 0 || i >= NG || j >= NG) continue; if (r && j != cz - r && j != cz + r && i != cx - r && i != cx + r) continue;
        int c = j * NG + i; if (!navB[c] && navComp[c] == navMain) return c;
    }
    return c0;
}
/* camino de celdas desde (x0,z0) hasta (x1,z1); se submuestrea si es largo */
static int navPath(float x0, float z0, float x1, float z1, short *out, int max) {
    int s = navSnap(x0, z0), g = navSnap(x1, z1); if (s == g) { out[0] = (short)g; return 1; }
    for (int c = 0; c < NG * NG; c++) navPar[c] = -1;
    int h = 0, t = 0; navQ[t++] = (short)g; navPar[g] = (short)g;
    static const int dx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, dz[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    while (h < t && navPar[s] < 0) {
        int u = navQ[h++], ux = u % NG, uz = u / NG;
        for (int k = 0; k < 8; k++) {
            int vx = ux + dx[k], vz = uz + dz[k]; if (vx < 0 || vz < 0 || vx >= NG || vz >= NG) continue;
            int v = vz * NG + vx; if (navB[v] || navPar[v] >= 0 || navComp[v] != navMain) continue;
            if (k >= 4 && (navB[uz * NG + vx] || navB[vz * NG + ux])) continue;
            navPar[v] = (short)u; navQ[t++] = (short)v;
        }
    }
    if (navPar[s] < 0) return 0;
    int n = 0, c = s, tot = 0; for (int q = s; q != g; q = navPar[q]) tot++;
    int stride = tot > max * 2 ? 3 : (tot > max ? 2 : 1), k = 0;
    while (c != g && n < max - 1) { c = navPar[c]; if ((k++ % stride) == 0 || c == g) out[n++] = (short)c; }
    if (n == 0 || out[n - 1] != g) out[n++] = (short)g;
    return n;
}
static int walkClear(float x0, float z0, float x1, float z1) {      /* linea recta libre para un cuerpo de radio 0.4 */
    float dx = x1 - x0, dz = z1 - z0, L = sqrtf(dx * dx + dz * dz); int n = (int)(L / 0.4f) + 1;
    for (int i = 1; i <= n; i++) { float t = (float)i / n; if (blockedAt(x0 + dx * t, z0 + dz * t, 0, 0.42f)) return 0; }
    return 1;
}
static int freeSpot(float zmin, float zmax, float *ox, float *oz) {
    if (navBuildPending) navBuild();
    for (int i = 0; i < 120; i++) {
        float x = rndf(-HALF + 2, HALF - 2), z = rndf(zmin, zmax);
        if (!blockedAt(x, z, 0, 1.0f) && navOk(x, z)) { *ox = x; *oz = z; return 1; }
    }
    float cx, cz; navCenter(navSnap(0, (zmin + zmax) * .5f), &cx, &cz); *ox = cx; *oz = cz; return 0;
}

/* ---------- Render 3D ---------- */
typedef struct { float x, y, z, r, g, b; } V;
static float camX, camY, camZ, camYaw, camPitch, camF;

static float camRoll = 0, camFovAdd = 0;
static void toView(float wx, float wy, float wz, float *x, float *y, float *z) {
    float dx = wx - camX, dy = wy - camY, dz = wz - camZ;
    float cy = cosf(camYaw), sy = sinf(camYaw), cp = cosf(camPitch), sp = sinf(camPitch);
    float xr = dx * cy - dz * sy;
    float zh = -dx * sy - dz * cy;
    *x = xr; *z = zh * cp + dy * sp; *y = -zh * sp + dy * cp;
    if (camRoll != 0) { float cr = cosf(camRoll), sr = sinf(camRoll), tx = *x * cr - *y * sr; *y = *x * sr + *y * cr; *x = tx; }
}

#define NEAR 0.12f
/* Descarte por objeto: devuelve 0 si la esfera esta entera detras de la camara o fuera del campo de vision (no se transforma ni se dibuja). */
static int sphereVis(float wx, float wy, float wz, float r) {
    float x, y, z; toView(wx, wy, wz, &x, &y, &z);
    if (z + r < NEAR) return 0;
    float lx = (SW * 0.5f) / camF, ly = (SH * 0.5f) / camF;
    if (x - lx * z > r * sqrtf(1 + lx * lx) || -x - lx * z > r * sqrtf(1 + lx * lx)) return 0;
    if (y - ly * z > r * sqrtf(1 + ly * ly) || -y - ly * z > r * sqrtf(1 + ly * ly)) return 0;
    return 1;
}
static void emitPoly(V *in, int n, int overlay) {
    V out[12]; int m = 0;
    for (int i = 0; i < n; i++) {
        V *a = &in[i], *b = &in[(i + 1) % n];
        int ain = a->z >= NEAR, bin = b->z >= NEAR;
        if (ain) out[m++] = *a;
        if (ain != bin) {
            float t = (NEAR - a->z) / (b->z - a->z);
            V c = { lerpf(a->x, b->x, t), lerpf(a->y, b->y, t), NEAR, lerpf(a->r, b->r, t), lerpf(a->g, b->g, t), lerpf(a->b, b->b, t) };
            out[m++] = c;
        }
    }
    if (m < 3) return;
    /* Proyeccion. Cada vertice: x, y de pantalla, z de profundidad (lineal en pantalla), r, g, b. */
    typedef struct { float x, y, z, r, g, b; } SV;
    SV a[24], b2[24]; int na = m;
    for (int i = 0; i < m; i++) {
        float iz = 1.0f / out[i].z;
        a[i].x = SW * 0.5f + out[i].x * iz * camF;
        a[i].y = SH * 0.5f - out[i].y * iz * camF;
        a[i].z = overlay ? 16200000.0f + 50000.0f * iz : 16777000.0f * 0.1f * iz;   /* el arma y las manos siempre delante, ordenados por profundidad */
        a[i].r = out[i].r; a[i].g = out[i].g; a[i].b = out[i].b;
    }
    {   /* descarte: poligono entero fuera de la pantalla, o de area casi nula (de canto) */
        float mnx = a[0].x, mxx = mnx, mny = a[0].y, mxy = mny, ar = 0;
        for (int i = 1; i < m; i++) { if (a[i].x < mnx) mnx = a[i].x; if (a[i].x > mxx) mxx = a[i].x; if (a[i].y < mny) mny = a[i].y; if (a[i].y > mxy) mxy = a[i].y; }
        if (mxx < 0 || mnx > SW || mxy < 0 || mny > SH) return;
        for (int i = 0; i < m; i++) { int j = (i + 1) % m; ar += a[i].x * a[j].y - a[j].x * a[i].y; }
        if (fabsf(ar) < 0.02f) return;
    }
    /* Recorte 2D contra la pantalla con margen: la GS solo admite coordenadas acotadas. */
    const float M = 128.0f, lim[4] = { -M, SW + M, -M, SH + M };
    for (int e = 0; e < 4 && na >= 3; e++) {
        int nb = 0;
        for (int i = 0; i < na; i++) {
            SV *p = &a[i], *q = &a[(i + 1) % na];
            float pv = (e < 2) ? p->x : p->y, qv = (e < 2) ? q->x : q->y;
            int pin = (e & 1) ? pv <= lim[e] : pv >= lim[e];
            int qin = (e & 1) ? qv <= lim[e] : qv >= lim[e];
            if (pin) b2[nb++] = *p;
            if (pin != qin) {
                float t = (lim[e] - pv) / (qv - pv);
                SV c = { lerpf(p->x, q->x, t), lerpf(p->y, q->y, t), lerpf(p->z, q->z, t), lerpf(p->r, q->r, t), lerpf(p->g, q->g, t), lerpf(p->b, q->b, t) };
                if (e < 2) c.x = lim[e]; else c.y = lim[e];
                b2[nb++] = c;
            }
        }
        memcpy(a, b2, sizeof(SV) * nb); na = nb;
    }
    if (na < 3) return;
    int sz[24]; u32c sc[24];
    for (int i = 0; i < na; i++) {
        sz[i] = (int)clampf(a[i].z, 0, 16777000.0f);
        sc[i] = RGBA((int)clampf(a[i].r, 0, 255), (int)clampf(a[i].g, 0, 255), (int)clampf(a[i].b, 0, 255), 255);
    }
    for (int i = 1; i + 1 < na; i++)
        gfx_tri(a[0].x, a[0].y, sz[0], sc[0], a[i].x, a[i].y, sz[i], sc[i], a[i + 1].x, a[i + 1].y, sz[i + 1], sc[i + 1]);
}

static void fogColor(u32c base, float shade, float dist, float *r, float *g, float *b) {
    float f = clampf((dist - 28.0f) / 60.0f, 0, 1);
    *r = lerpf(CR(base) * shade, CR(SKY), f); *g = lerpf(CG(base) * shade, CG(SKY), f); *b = lerpf(CB(base) * shade, CB(SKY), f);
}

static const int FACE[6][4] = { {0,4,6,2}, {1,3,7,5}, {0,1,5,4}, {2,6,7,3}, {0,2,3,1}, {4,5,7,6} };
static const float FN[6][3] = { {-1,0,0}, {1,0,0}, {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1} };

/* Caja orientada en el mundo (giro en Y). */
static void obox(float cx, float cy, float cz, float hw, float hh, float hd, float yaw, u32c col) {
    if (!sphereVis(cx, cy, cz, sqrtf(hw * hw + hh * hh + hd * hd))) return;
    float c[8][3], ys = sinf(yaw), yc = cosf(yaw);
    for (int i = 0; i < 8; i++) {
        float lx = (i & 1) ? hw : -hw, ly = (i & 2) ? hh : -hh, lz = (i & 4) ? hd : -hd;
        c[i][0] = cx + lx * yc + lz * ys; c[i][1] = cy + ly; c[i][2] = cz - lx * ys + lz * yc;
    }
    for (int f = 0; f < 6; f++) {
        float nx = FN[f][0] * yc + FN[f][2] * ys, ny = FN[f][1], nz = -FN[f][0] * ys + FN[f][2] * yc;
        float fx = cx + nx * (fabsf(FN[f][0]) * hw + fabsf(FN[f][2]) * hd) , fy = cy + ny * hh, fz = cz + nz * (fabsf(FN[f][0]) * hw + fabsf(FN[f][2]) * hd);
        if (ny != 0) { fx = cx; fz = cz; }
        float vx = camX - fx, vy = camY - fy, vz = camZ - fz;
        if (nx * vx + ny * vy + nz * vz <= 0) continue;
        float dist = sqrtf(vx * vx + vy * vy + vz * vz);
        float lit = nx * -0.35f + ny * 0.8f + nz * 0.45f;
        float shade = clampf(0.62f + 0.38f * lit, 0.45f, 1.05f);
        V v[4];
        for (int k = 0; k < 4; k++) {
            float *p = c[FACE[f][k]];
            toView(p[0], p[1], p[2], &v[k].x, &v[k].y, &v[k].z);
            fogColor(col, shade, dist, &v[k].r, &v[k].g, &v[k].b);
#ifdef PS2_ONLINE
            {   /* iluminacion: oscurece cerca del suelo (oclusion) y vira calido al sol / frio en sombra */
                float ao = 0.76f + 0.24f * clampf(p[1] / 2.4f, 0, 1), wm = clampf(lit * 0.5f + 0.5f, 0, 1), fg = clampf((dist - 28.0f) / 60.0f, 0, 1);
                float k1 = lerpf(ao, 1, fg);
                v[k].r *= k1 * lerpf(0.92f + 0.14f * wm, 1, fg); v[k].g *= k1 * lerpf(0.97f + 0.05f * wm, 1, fg); v[k].b *= k1 * lerpf(1.05f - 0.13f * wm, 1, fg);
            }
#endif
        }
        emitPoly(v, 4, 0);
    }
}

/* Caja en espacio de camara, siempre por delante del mundo (arma en primera persona). */
static void vbox(float cx, float cy, float cz, float hw, float hh, float hd, u32c col) {
    for (int f = 0; f < 6; f++) {
        float nx = FN[f][0], ny = FN[f][1], nz = FN[f][2];
        float fx = cx + nx * hw, fy = cy + ny * hh, fz = cz + nz * hd;
        if (nx * -fx + ny * -fy + nz * -fz <= 0) continue;
        float shade = clampf(0.7f + 0.3f * (ny * 0.8f - nx * 0.35f), 0.45f, 1.0f);
        V v[4];
        for (int k = 0; k < 4; k++) {
            int i = FACE[f][k];
            v[k].x = cx + ((i & 1) ? hw : -hw); v[k].y = cy + ((i & 2) ? hh : -hh); v[k].z = cz + ((i & 4) ? hd : -hd);
            v[k].r = CR(col) * shade; v[k].g = CG(col) * shade; v[k].b = CB(col) * shade;
        }
        emitPoly(v, 4, 1);
    }
}

static void drawFloor(void) {
    for (int i = 0; i < 12; i++) for (int j = 0; j < 12; j++) {
        float x0 = -HALF + i * 5, z0 = -HALF + j * 5;
        if (!sphereVis(x0 + 2.5f, 0, z0 + 2.5f, 3.6f)) continue;
        float mx = x0 + 2.5f - camX, mz = z0 + 2.5f - camZ;
        float dist = sqrtf(mx * mx + mz * mz);
        u32c base = ((i + j) & 1) ? MAPS[curMap].floorB : MAPS[curMap].floorA;
        V v[4]; float px[4] = { x0, x0 + 5, x0 + 5, x0 }, pz[4] = { z0, z0, z0 + 5, z0 + 5 };
        for (int k = 0; k < 4; k++) {
            toView(px[k], 0, pz[k], &v[k].x, &v[k].y, &v[k].z);
            fogColor(base, 1.0f, dist, &v[k].r, &v[k].g, &v[k].b);
        }
        emitPoly(v, 4, 0);
    }
}

/* ---------- Texto (fuente 5x7) ---------- */
static const unsigned char FONT[59][5] = {
    {0,0,0,0,0},{0,0,0x5F,0,0},{0,7,0,7,0},{0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,8,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0,8,7,3,0},
    {0,0x1C,0x22,0x41,0},{0,0x41,0x22,0x1C,0},{0x2A,0x1C,0x7F,0x1C,0x2A},{8,8,0x3E,8,8},{0,0x80,0x70,0x30,0},{8,8,8,8,8},{0,0,0x60,0x60,0},{0x20,0x10,8,4,2},
    {0x3E,0x51,0x49,0x45,0x3E},{0,0x42,0x7F,0x40,0},{0x72,0x49,0x49,0x49,0x46},{0x21,0x41,0x49,0x4D,0x33},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x31},{0x41,0x21,0x11,9,7},
    {0x36,0x49,0x49,0x49,0x36},{0x46,0x49,0x49,0x29,0x1E},{0,0,0x14,0,0},{0,0x40,0x34,0,0},{0,8,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},{0,0x41,0x22,0x14,8},{2,1,0x59,9,6},
    {0x3E,0x41,0x5D,0x59,0x4E},{0x7C,0x12,0x11,0x12,0x7C},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x41,0x3E},{0x7F,0x49,0x49,0x49,0x41},{0x7F,9,9,9,1},{0x3E,0x41,0x41,0x51,0x73},
    {0x7F,8,8,8,0x7F},{0,0x41,0x7F,0x41,0},{0x20,0x40,0x41,0x3F,1},{0x7F,8,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},{0x7F,2,0x1C,2,0x7F},{0x7F,4,8,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,9,9,9,6},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,9,0x19,0x29,0x46},{0x26,0x49,0x49,0x49,0x32},{3,1,0x7F,1,3},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,8,0x14,0x63},{3,4,0x78,4,3},{0x61,0x59,0x49,0x4D,0x43}
};
static float textW(const char *s, float sc) { return (float)strlen(s) * 6 * sc; }
static void drawText(float x, float y, float sc, const char *s, u32c col) {
    for (; *s; s++, x += 6 * sc) {
        int ch = (unsigned char)*s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 90) continue;
        const unsigned char *g = FONT[ch - 32];
        for (int c = 0; c < 5; c++) {
            int r = 0;
            while (r < 7) {
                if (!((g[c] >> r) & 1)) { r++; continue; }
                int r0 = r; while (r < 7 && ((g[c] >> r) & 1)) r++;
                gfx_rect(x + c * sc, y + r0 * sc, x + (c + 1) * sc, y + r * sc, col);
            }
        }
    }
}
static void drawTextC(float cx, float y, float sc, const char *s, u32c col) { drawText(cx - textW(s, sc) * 0.5f + sc * 0.5f, y, sc, s, col); }

/* ---------- Armas ---------- */
typedef struct { const char *name; float dmg, rpm; int autof, mag, reserve; float spread, recoil; int pellets; float reload; } Weapon;
static const Weapon W[4] = {
    { "PISTOLA", 26, 380, 0, 12, 60, 0.004f, 0.016f, 1, 1.3f },
    { "RIFLE", 22, 640, 1, 30, 90, 0.008f, 0.013f, 1, 2.0f },
    { "ESCOPETA", 12, 80, 0, 6, 24, 0.05f, 0.05f, 8, 3.2f },
    { "AMETRALLADORA", 18, 780, 1, 100, 200, 0.014f, 0.011f, 1, 3.6f }
};

/* ---------- Estado de juego ---------- */
enum { M_MENU, M_SETTINGS, M_PLAYING, M_ROUNDEND, M_PAUSE, M_END, M_OPTIONS, M_MP_MENU, M_MP_LOBBY, M_KEYS, M_CODE, M_LAB };
typedef struct { float x, y, z, vx, vy, vz, yaw, pitch, hp; int alive, ground; float noise, bob, cr; int crWant; } Player;
static Rig pRig; static int pFired;      /* animacion del jugador (tercera persona) */
typedef struct { float x, z, yaw, hp; int alive, state, burst; float react, fireCD, pause, strafe, strafeT, wx, wz, lkx, lkz, lost, timer, dead, step, walk, mv; int remote, wep, shots; float tx, ty, tz, tyaw, py, pitch; Rig rg; float cr, aimW, hitT, coverX, coverZ, coverT, peekT, px, pz, rlT; int crWant, cov, fireEv, rl; short path[48]; int pn, pi; float pgx, pgz, repT, stuck; int dummy, unused; } Bot;   /* dummy: soldado quieto del modo secreto; unused: ranura libre */

static Player P;
static Bot bots[MAXBOT];
#ifdef PC_ONLINE
static int netMode, mpShots, mpRounds = 5;           /* netMode: 0 sin red, 1 anfitrion, 2 cliente */
static void mpSendHit(float dmg, int head);
#define ROUNDS() (netMode ? mpRounds : S.rounds)
#else
#define netMode 0
static int mpShots;
#define ROUNDS() (S.rounds)
#endif
static int nbots = 0;
static int mode = M_MENU, prevMode = M_PLAYING, sel = 0, settingsFrom = M_MENU;
static int sandbox, codeD[4], codePos; static float codeBad;      /* modo secreto (codigo 1122): campo de pruebas */
static int wsel, ammoMag[4], ammoRes[4];
static float wCD, wReload, wSwap, wKick, wFlash, freeze, afterT, orbit, hitFlash, hmT, hurtT, bannerT;
static int scoreMe, scoreBots, roundNo, kills, heads, deaths, firedLatch;
#ifdef PS2_ONLINE
/* ---- Estilo de juego "tactico moderno" (solo version PS2): apuntado, sprint, granadas, radar ---- */
static float bfAds, bfStam = 1, bfBloom, bfShake, bfSprA, bfNadeT, bfSpot[MAXBOT], bfDX[4], bfDZ[4], bfDT[4];
static int bfSprint, bfStamLock, bfNades = 2, bfNadePend;
static int bfNadeSt; static float bfNadeU, bfThrowT;      /* granada: 0 guardada, 1 sacando y quitando el seguro (mientras mantienes R2), 2 lanzando (al soltar R2) */
#define SWAP_T 0.22f
/* Placas de armadura (escudo) estilo Warzone: 3 placas de PLATE_HP cada una, L2 pone una, L3 corre */
#define PLATE_N 3
#define PLATE_HP 50.0f
#define PLATE_T 1.7f        /* segundos que tarda en ponerse una placa */
#define PLATE_STOCK 3       /* placas que llevas en la mochila al empezar cada ronda */
#define PLATE_ROLL -0.45f   /* inclinacion de la placa en la mano (si se ve girada al reves, cambia el signo) */
static float bfArmor, bfPlateU; static int bfPlateSt, bfPlates = PLATE_STOCK, bfSprintTog;
typedef struct { float x, y, z, vx, vy, vz, t; int on; } Nade;
typedef struct { float x, y, z, t; } Boom;
static Nade nades[4]; static Boom booms[4];
/* efectos de combate (particulas con alfa, trazadoras, vainas, marcas): reserva fija y pequena para cuidar la GS de la PS2 */
#define BF_NP 56
typedef struct { float x, y, z, vx, vy, vz, t, T, s0, s1, g, drag; int r, gc, b, a, kind; } Pt;      /* kind 0 humo/polvo, 1 chispa, 2 fuego, 3 destello, 4 escombro */
typedef struct { float x, y, z, tx, ty, tz, t; } Trc;
typedef struct { float x, y, z, vx, vy, vz, t, rot; } Cas;
typedef struct { float x, z, r, a; } Scorch;
static void bfFireFx(const float *o, const float *d, float tEnd, int hitBot);
static void bfImpact(const float *o, const float *d, float t);
static void bfBoomFx(float x, float y, float z);
static Pt pts[BF_NP]; static int ptI; static Trc trcs[10]; static Cas cass[6]; static Scorch scr[8]; static int scrI, bfShotN; static float bfFlashT;
#else
#define SWAP_T 0.35f
#endif
static float damageTaken;
static char bannerA[48], bannerB[48];
static char feed[4][40]; static float feedT[4];

enum { BS_PATROL, BS_SEARCH, BS_ALERT, BS_ENGAGE, BS_COVER, BS_HIDE, BS_PEEK };

#ifdef PS2_ONLINE
static void goreHit(Bot *b, float dmg, int head);
static void rdSpawn(int idx, float dmg, int head);
static void goreReset(void);
static float rdRay(const float *o, const float *d, float t);
#endif
static int aliveBots(void) { int n = 0; for (int i = 0; i < nbots; i++) n += bots[i].alive; return n; }
static void setBanner(const char *a, const char *b, float t) { strncpy(bannerA, a, 47); strncpy(bannerB, b, 47); bannerA[47] = bannerB[47] = 0; bannerT = t; }
static void pushFeed(const char *s) {
    for (int i = 3; i > 0; i--) { memcpy(feed[i], feed[i - 1], 40); feedT[i] = feedT[i - 1]; }
    strncpy(feed[0], s, 39); feed[0][39] = 0; feedT[0] = 4.0f;
}

static void spawnBots(void) {
    nbots = S.bots > MAXBOT ? MAXBOT : S.bots;
    for (int i = 0; i < nbots; i++) {
        Bot *b = &bots[i]; memset(b, 0, sizeof(*b));
        { const MapDef *md = &MAPS[curMap]; const SpawnPt *sp = &md->bs[i % md->nb]; int c = navSnap(sp->x, sp->z); navCenter(c, &b->x, &b->z); if (walkClear(sp->x, sp->z, sp->x, sp->z) && !blockedAt(sp->x, sp->z, 0, 0.45f)) { b->x = sp->x; b->z = sp->z; } }
        b->hp = DIFF_HP[S.diff]; b->alive = 1; b->state = BS_PATROL; b->yaw = rndf(0, 6.28f);
        b->strafe = (rand() & 1) ? 1 : -1; b->strafeT = 1; b->fireCD = 0.5f;
        freeSpot(-HALF + 2, HALF - 2, &b->wx, &b->wz); b->pn = 0;
    }
}
static void resetAmmo(void) {
    for (int i = 0; i < 4; i++) { ammoMag[i] = W[i].mag; ammoRes[i] = W[i].reserve; }
    wsel = 0; wCD = wReload = wSwap = wKick = wFlash = 0; firedLatch = 0;
#ifdef PS2_ONLINE
    bfAds = bfBloom = bfShake = bfSprA = bfNadeT = 0; bfStam = 1; bfSprint = bfStamLock = bfNadePend = 0; bfNadeSt = 0; bfNadeU = bfThrowT = 0; bfNades = netMode ? 0 : 2;
    bfArmor = 0; bfPlateU = 0; bfPlateSt = 0; bfPlates = PLATE_STOCK; bfSprintTog = 0;
    memset(bfSpot, 0, sizeof bfSpot); memset(bfDT, 0, sizeof bfDT); memset(nades, 0, sizeof nades); memset(booms, 0, sizeof booms); memset(pts, 0, sizeof pts); memset(trcs, 0, sizeof trcs); memset(cass, 0, sizeof cass); memset(scr, 0, sizeof scr); bfFlashT = 0; goreReset();
#endif
}
static float openYaw(float x, float z) {            /* mira hacia donde hay mas espacio libre */
    float best = -1, by = 0;
    for (int k = 0; k < 16; k++) { float y = k * (6.2832f / 16), d[3] = { -sinf(y), 0, -cosf(y) }, o[3] = { x, 1.4f, z }; float w = wallDist(o, d, 40); if (w > best) { best = w; by = y; } }
    return by;
}
static void spawnPlayer(void) {
    const MapDef *md = &MAPS[curMap]; const SpawnPt *sp = &md->ps[(roundNo - 1) % md->np];
    P.x = sp->x; P.z = sp->z; P.yaw = openYaw(sp->x, sp->z);
}
static void startRound(void) {
    sandbox = 0;
    spawnBots();
    memset(&P, 0, sizeof(P)); memset(&pRig, 0, sizeof(pRig)); spawnPlayer(); P.hp = 100; P.alive = 1; P.ground = 1;
    resetAmmo(); freeze = 2.4f; afterT = 0; mode = M_PLAYING; hurtT = 0;
    char b[40]; snprintf(b, 40, "RONDA %d", roundNo);
    setBanner(b, "ELIMINA A TODOS LOS BOTS", 2.2f);
}
static void startMatch(void) {
    if (curMap != S.map) { curMap = S.map; buildWorld(); }
    scoreMe = scoreBots = 0; roundNo = 1; kills = heads = deaths = 0; damageTaken = 0;
    memset(feed, 0, sizeof(feed)); memset(feedT, 0, sizeof(feedT));
    startRound();
}
static void endRound(int won) {
    mode = M_ROUNDEND; afterT = 3.0f;
    if (won) { scoreMe++; snd_play(SND_WIN, 0.8f); setBanner("RONDA GANADA", "ELIMINASTE A TODOS LOS BOTS", 2.8f); }
    else { scoreBots++; snd_play(SND_LOSE, 0.6f); setBanner("RONDA PERDIDA", "TE ELIMINARON", 2.8f); }
}

/* El cuerpo cae donde cabe: se prueba cada postura final contra paredes y cajas y se elige una que no las atraviese */
static int deadBad(const Bot *b, int k) {
    Rig g = b->rg; g.dead = 1; g.deadT = 1.0f; g.deadKind = k; SkPose po; M34 G[BN_N];
    rigPose(&g, &po); rigSolve(&po, b->x, 0, b->z, b->yaw, G);
    static const float pt[7][4] = { { BN_HEAD, 0, 1.55f, 0 }, { BN_FOOT_L, -0.19f, 0.1f, 0 }, { BN_FOOT_R, 0.19f, 0.1f, 0 }, { BN_CHEST, 0, 1.1f, 0 }, { BN_WEAP, 0.17f, 1.12f, -1.0f }, { BN_CHEST, -0.3f, 1.1f, 0 }, { BN_CHEST, 0.3f, 1.1f, 0 } };
    int bad = 0;
    for (int i = 0; i < 7; i++) { float p[3], q[3] = { pt[i][1], pt[i][2], pt[i][3] }; m34Xf(&G[(int)pt[i][0]], q, p);
        if (fabsf(p[0]) > HALF - 0.3f || fabsf(p[2]) > HALF - 0.3f || blockedAt(p[0], p[2], b->py, 0.2f)) bad++; }
    return bad;
}
static int deadPick(const Bot *b, int pref) {
    int best = pref, bb = 99;
    for (int i = 0; i < 3; i++) { int k = (pref + i) % 3, d = deadBad(b, k); if (d < bb) { bb = d; best = k; } if (d == 0) break; }
    return best;
}
#ifdef PS2_ONLINE
/* datos del ultimo impacto (los rellena quien llama a damageBot): punto, direccion y fuerza -> ragdoll y salpicaduras */
static int gHitOn; static float gHitP[3], gHitD[3], gHitF;
#endif
static void damageBot(Bot *b, float dmg, int head) {
    if (!b->alive) return;
#ifdef PC_ONLINE
    if (b->remote) { mpSendHit(dmg, head); return; }      /* el dano al otro jugador lo aplica el otro jugador */
#endif
    b->hp -= dmg;
    if (b->state == BS_PATROL || b->state == BS_SEARCH) { b->state = BS_ALERT; b->react = DIFF_REACT[S.diff] * 0.5f; b->lkx = P.x; b->lkz = P.z; }
    b->hitT = 2.0f;
#ifdef PS2_ONLINE
    goreHit(b, dmg, head);
#endif
    if (b->hp <= 0) {
        b->rg.deadKind = deadPick(b, head ? 0 : rand() % 3);
#ifdef PS2_ONLINE
        rdSpawn((int)(b - bots), dmg, head);      /* antes de marcarlo muerto: toma la pose viva como punto de partida */
#endif
        b->alive = 0; b->dead = 0; kills++;
        snd_play3d(SND_DEATH0 + (rand() & 1), b->x, b->z, 1.3f); if (head) heads++;
        pushFeed(head ? "BAJA - CABEZA" : "BAJA");
        if (aliveBots() == 0 && mode == M_PLAYING && !sandbox) endRound(1);
    }
}
static void hurtPlayer(float d) {
    if (!P.alive || sandbox) return;      /* en el campo de pruebas eres invulnerable */
    damageTaken += d;
#ifdef PS2_ONLINE
    if (bfArmor > 0) {      /* el escudo absorbe el dano antes que la vida */
        float had = bfArmor, ab = fminf(bfArmor, d); bfArmor -= ab; d -= ab;
        if (had > 0 && bfArmor <= 0.001f) { bfArmor = 0; snd_play(SND_ARMORBREAK, 1.0f); }   /* te quitaron todo el escudo */
        else snd_play(SND_HEAD, 0.5f);
        if (d <= 0.0f) { hurtT = 0.25f; return; }
    }
#endif
    P.hp -= d;
    if (hurtT <= 0 && P.hp > 0) snd_play(SND_HURT, 0.8f);
    hurtT = 0.25f;
    if (P.hp <= 0) { P.hp = 0; P.alive = 0; pRig.deadKind = rand() % 3; snd_play(SND_PDEATH, 1.0f); deaths++; pushFeed("TE ELIMINARON"); if (mode == M_PLAYING && !netMode) endRound(0); }
}

static void startReload(void) {
    if (wReload > 0 || ammoMag[wsel] >= W[wsel].mag || ammoRes[wsel] <= 0 || wSwap > 0) return;
    wReload = W[wsel].reload; snd_play(SND_RELOAD, 0.8f);
}
static void switchTo(int i) {
    i = (i + 4) % 4;
    if (i == wsel || !P.alive) return;
    wsel = i; wReload = 0; wSwap = SWAP_T;
}

/* ---------- Tercera persona: estado de animacion y camara ---------- */
static float anPhase, anAim, aimT, anFall, anSpd, anRun;
static void tpsCamera(float *cx, float *cy, float *cz) {
    float cp = cosf(P.pitch), sp = sinf(P.pitch);
    float f[3] = { -sinf(P.yaw) * cp, sp, -cosf(P.yaw) * cp }, r[3] = { cosf(P.yaw), 0, -sinf(P.yaw) };
    float e[3] = { P.x, P.y + 1.55f - 0.45f * P.cr, P.z };
    float dist = lerpf(4.0f, 3.5f, anAim), side = lerpf(1.2f, 1.3f, anAim), up = lerpf(0.1f, 0.05f, anAim);
    float o[3] = { -f[0] * dist + r[0] * side, -f[1] * dist + up, -f[2] * dist + r[2] * side };
    float L = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]), d[3] = { o[0] / L, o[1] / L, o[2] / L };
    float t = wallDist(e, d, L + 0.3f) - 0.3f;
    if (t > L) t = L; if (t < 0.35f) t = 0.35f;
    *cx = e[0] + d[0] * t; *cy = e[1] + d[1] * t; *cz = e[2] + d[2] * t;
    if (*cy < 0.25f) *cy = 0.25f;
}
static void fireWeapon(void) {
    const Weapon *w = &W[wsel];
    if (!P.alive || wCD > 0 || wReload > 0 || wSwap > 0) return;
    if (ammoMag[wsel] <= 0) { snd_play(SND_EMPTY, 0.8f); wCD = 0.25f; if (ammoRes[wsel] > 0) startReload(); return; }
    snd_play(SND_PISTOL + (wsel == 3 ? 1 : wsel), 1.0f);
    ammoMag[wsel]--; pFired = 1; if (netMode) mpShots++; wCD = 60.0f / w->rpm; wKick = 1; wFlash = 0.05f; P.noise = 1; firedLatch = 1;
    float spd = sqrtf(P.vx * P.vx + P.vz * P.vz);
    float spread = w->spread * (1 + clampf(spd / 5, 0, 1) * 2.2f + (P.ground ? 0 : 3));
#ifdef PS2_ONLINE
    spread *= (1 - 0.6f * bfAds) * (1 + bfBloom);          /* apuntando se cierra; disparar seguido la abre (se recupera al soltar) */
    bfBloom = fminf(bfBloom + (w->pellets > 1 ? 0.10f : (w->autof ? 0.14f : 0.30f)), 1.3f);
#endif
    float cp = cosf(P.pitch), sp = sinf(P.pitch);
    float f[3] = { -sinf(P.yaw) * cp, sp, -cosf(P.yaw) * cp };
    float r[3] = { cosf(P.yaw), 0, -sinf(P.yaw) };
    float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
    float o[3] = { P.x, P.y + 1.62f - 0.5f * P.cr, P.z };
    if (S.cam) {   /* tercera persona: el rayo sale de la camara, a la altura del personaje, y pasa por la mira */
        float c[3]; tpsCamera(&c[0], &c[1], &c[2]);
        float k = (o[0] - c[0]) * f[0] + (o[1] - c[1]) * f[1] + (o[2] - c[2]) * f[2]; if (k < 0) k = 0;
        for (int i = 0; i < 3; i++) o[i] = c[i] + f[i] * k;
    }
    aimT = 1.2f;
    int anyHit = 0, anyHead = 0;
    for (int p = 0; p < w->pellets; p++) {
        float sx = rndf(-1, 1) * spread, sy = rndf(-1, 1) * spread, d[3];
        for (int k = 0; k < 3; k++) d[k] = f[k] + r[k] * sx + u[k] * sy;
        float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); d[0] /= L; d[1] /= L; d[2] /= L;
        float tBest = wallDist(o, d, 100); Bot *hit = NULL; int head = 0;
#ifdef PS2_ONLINE
        tBest = rdRay(o, d, tBest);          /* los cadaveres tambien reciben balazos */
#endif
        for (int i = 0; i < nbots; i++) {
            Bot *b = &bots[i]; if (!b->alive) continue;
            float cq = b->rg.crouch; float mn[3] = { b->x - .32f, b->py, b->z - .32f }, mx[3] = { b->x + .32f, b->py + 1.42f - 0.45f * cq, b->z + .32f }, c[3] = { b->x, b->py + 1.6f - 0.5f * cq, b->z };
            float tb = rayBox(o, d, mn, mx), th = raySphere(o, d, c, .22f), t = tb < th ? tb : th;
            if (t < tBest) { tBest = t; hit = b; head = th <= tb; }
        }
        if (hit) {
            float dmg = w->dmg * (head ? 2.5f : 1.0f);
            if (w->pellets > 1) dmg *= clampf(1.15f - tBest / 35, 0.25f, 1);
#ifdef PS2_ONLINE
            gHitOn = 1; for (int k = 0; k < 3; k++) { gHitP[k] = o[k] + d[k] * tBest; gHitD[k] = d[k]; }
            gHitF = clampf(2.2f + dmg * 0.05f, 2, 8) * (w->pellets > 1 ? 1.7f : 1.0f);
#endif
            damageBot(hit, dmg, head); anyHit = 1; anyHead |= head;
#ifdef PS2_ONLINE
            gHitOn = 0;
#endif
        }
#ifdef PS2_ONLINE
        if (p == 0) bfFireFx(o, d, tBest, hit != NULL); else if (!hit && (p & 1) == 0 && tBest < 99) bfImpact(o, d, tBest);
#endif
    }
    if (anyHit) { snd_play(anyHead ? SND_HEAD : SND_HIT, 0.9f); hitFlash = 0.12f; hmT = anyHead ? 1 : 0.5f; }
#ifdef PS2_ONLINE
    { float rk = 1 - 0.35f * bfAds; P.pitch += w->recoil * rndf(0.7f, 1.3f) * rk; P.yaw += rndf(-0.5f, 0.5f) * w->recoil * rk; }
#else
    P.pitch += w->recoil * rndf(0.7f, 1.3f); P.yaw += rndf(-0.5f, 0.5f) * w->recoil;
#endif
    if (ammoMag[wsel] == 0 && ammoRes[wsel] > 0) startReload();
}


#ifdef PS2_ONLINE
/* ---------- Granadas, indicadores de dano y radar tactico (solo PS2) ---------- */

/* ---------- Efectos de combate estilo "cine belico" (solo PS2) ---------- */
static void bfPt(int kind, float x, float y, float z, float vx, float vy, float vz, float T, float s0, float s1, int r, int g, int b, int a, float grav, float drag) {
    Pt *q = &pts[ptI++ % BF_NP];
    q->kind = kind; q->x = x; q->y = y; q->z = z; q->vx = vx; q->vy = vy; q->vz = vz; q->t = q->T = T; q->s0 = s0; q->s1 = s1; q->r = r; q->gc = g; q->b = b; q->a = a; q->g = grav; q->drag = drag;
}
static void bfDust(float x, float y, float z, float n, float spd) {         /* nube de polvo + chispas en un impacto */
    for (int i = 0; i < n; i++) bfPt(0, x, y, z, rndf(-1, 1) * spd, rndf(0.2f, 1) * spd, rndf(-1, 1) * spd, rndf(0.5f, 0.9f), 0.06f, 0.28f, 176, 160, 130, 150, -0.5f, 2.5f);
    for (int i = 0; i < 2; i++) bfPt(1, x, y, z, rndf(-3, 3), rndf(1, 4), rndf(-3, 3), rndf(0.25f, 0.45f), 0.03f, 0.03f, 255, 214, 120, 255, 14, 0.5f);
}
static void bfImpact(const float *o, const float *d, float t) {
    float x = o[0] + d[0] * (t - 0.05f), y = o[1] + d[1] * (t - 0.05f), z = o[2] + d[2] * (t - 0.05f);
    if (y < 0.03f) y = 0.03f;
    bfDust(x, y, z, 2, 1.2f);
}
static void bfTracer(float x, float y, float z, float tx, float ty, float tz) {
    static int k; Trc *q = &trcs[k++ % 10]; q->x = x; q->y = y; q->z = z; q->tx = tx; q->ty = ty; q->tz = tz; q->t = 0.07f;
}
static void bfCasing(void) {
    float cp = cosf(P.pitch), sp = sinf(P.pitch), f[3] = { -sinf(P.yaw) * cp, sp, -cosf(P.yaw) * cp }, r[3] = { cosf(P.yaw), 0, -sinf(P.yaw) };
    static int k; Cas *c = &cass[k++ % 6];
    c->x = P.x + f[0] * 0.4f + r[0] * 0.18f; c->y = P.y + 1.45f - 0.5f * P.cr - 0.12f; c->z = P.z + f[2] * 0.4f + r[2] * 0.18f;
    c->vx = r[0] * rndf(1.5f, 2.6f) + P.vx * 0.5f; c->vy = rndf(1.6f, 2.6f); c->vz = r[2] * rndf(1.5f, 2.6f) + P.vz * 0.5f; c->t = 1.4f; c->rot = rndf(0, 6);
}
static void bfFireFx(const float *o, const float *d, float tEnd, int hitBot) {   /* trazadora, destello y vaina del disparo del jugador */
    float cp = cosf(P.pitch), sp = sinf(P.pitch), f[3] = { -sinf(P.yaw) * cp, sp, -cosf(P.yaw) * cp }, r[3] = { cosf(P.yaw), 0, -sinf(P.yaw) };
    float m[3] = { P.x + f[0] * 0.9f + r[0] * 0.14f, P.y + 1.55f - 0.5f * P.cr - 0.13f + f[1] * 0.9f, P.z + f[2] * 0.9f + r[2] * 0.14f };
    if (W[wsel].autof ? (++bfShotN % 2 == 0) : 1) { float e = tEnd > 60 ? 60 : tEnd; bfTracer(m[0], m[1], m[2], o[0] + d[0] * e, o[1] + d[1] * e, o[2] + d[2] * e); }
    bfPt(3, m[0], m[1], m[2], 0, 0, 0, 0.05f, 0.10f, 0.16f, 255, 225, 150, 230, 0, 0);
    bfPt(0, m[0], m[1], m[2], f[0] * 1.0f, 0.4f, f[2] * 1.0f, 0.7f, 0.04f, 0.22f, 190, 190, 190, 90, -0.3f, 1.5f);
    if (W[wsel].pellets == 1) bfCasing();
    if (tEnd < 99) { if (hitBot) bfPt(0, o[0] + d[0] * tEnd, o[1] + d[1] * tEnd, o[2] + d[2] * tEnd, 0, 0.4f, 0, 0.4f, 0.05f, 0.2f, 150, 40, 40, 170, 1, 3); else bfImpact(o, d, tEnd); }
}
static void bfBoomFx(float x, float y, float z) {                    /* explosion: destello, bola de fuego, onda de polvo, columna de humo, escombros, marca de quemadura */
    bfPt(3, x, y + 0.5f, z, 0, 0, 0, 0.18f, 1.2f, 4.5f, 255, 240, 190, 255, 0, 0);
    for (int i = 0; i < 7; i++) bfPt(2, x + rndf(-.4f, .4f), y + rndf(0.1f, 0.9f), z + rndf(-.4f, .4f), rndf(-2, 2), rndf(1, 3.2f), rndf(-2, 2), rndf(0.45f, 0.8f), 0.7f, 2.3f, 255, 190, 80, 235, -1, 1.5f);
    for (int i = 0; i < 9; i++) { float a = i * 0.7f + rndf(0, .5f); bfPt(0, x, 0.15f, z, cosf(a) * 7, 0.3f, sinf(a) * 7, rndf(0.7f, 1.1f), 0.4f, 1.5f, 170, 150, 120, 160, 0, 4); }
    for (int i = 0; i < 9; i++) bfPt(0, x + rndf(-.5f, .5f), y + 0.3f, z + rndf(-.5f, .5f), rndf(-.8f, .8f), rndf(1.5f, 3.4f), rndf(-.8f, .8f), rndf(1.6f, 2.8f), 0.8f, 2.6f, 70, 66, 62, 175, -0.6f, 0.8f);
    for (int i = 0; i < 10; i++) bfPt(i < 6 ? 1 : 4, x, y + 0.2f, z, rndf(-8, 8), rndf(3, 9), rndf(-8, 8), rndf(0.6f, 1.2f), 0.04f, 0.04f, i < 6 ? 255 : 60, i < 6 ? 200 : 54, i < 6 ? 90 : 48, 255, 18, 0.3f);
    Scorch *c = &scr[scrI++ % 8]; c->x = x; c->z = z; c->r = 1.7f; c->a = 150;
}
static void bfUpdateFx(float dt) {
    for (int i = 0; i < BF_NP; i++) { Pt *q = &pts[i]; if (q->t <= 0) continue; q->t -= dt;
        q->vy -= q->g * dt; float k = fmaxf(0, 1 - q->drag * dt); q->vx *= k; q->vz *= k; if (q->kind != 1 && q->kind != 4) q->vy *= k;
        q->x += q->vx * dt; q->y += q->vy * dt; q->z += q->vz * dt;
        if (q->y < 0.03f && (q->kind == 1 || q->kind == 4)) { q->y = 0.03f; q->vy = -q->vy * 0.35f; q->vx *= 0.6f; q->vz *= 0.6f; } }
    for (int i = 0; i < 10; i++) if (trcs[i].t > 0) trcs[i].t -= dt;
    for (int i = 0; i < 6; i++) { Cas *c = &cass[i]; if (c->t <= 0) continue; c->t -= dt; c->vy -= 9 * dt; c->x += c->vx * dt; c->y += c->vy * dt; c->z += c->vz * dt; c->rot += 14 * dt;
        if (c->y < 0.02f) { c->y = 0.02f; c->vy = -c->vy * 0.3f; c->vx *= 0.5f; c->vz *= 0.5f; if (fabsf(c->vy) < 0.4f) c->vy = 0; } }
    if (bfFlashT > 0) bfFlashT -= dt;
}
static int bfProj(float wx, float wy, float wz, float *sx, float *sy, int *zd, float *pxs) {
    float x, y, z; toView(wx, wy, wz, &x, &y, &z); if (z < NEAR) return 0;
    float iz = 1.0f / z; *sx = SW * 0.5f + x * iz * camF; *sy = SH * 0.5f - y * iz * camF;
    *zd = (int)clampf(16777000.0f * 0.1f * iz * 1.03f, 0, 16777000.0f); *pxs = camF * iz; return 1;
}
static void bfFan(float cx, float cy, int zd, float rad, int n, u32c cc, u32c ce, float sy) {   /* disco suave: centro opaco, borde transparente */
    float px[8], py[8]; if (n > 8) n = 8;
    for (int i = 0; i < n; i++) { float a = i * 6.2832f / n + 0.4f; px[i] = cx + cosf(a) * rad; py[i] = cy + sinf(a) * rad * sy; }
    for (int i = 0; i < n; i++) { int j = (i + 1) % n; gfx_tri(cx, cy, zd, cc, px[i], py[i], zd, ce, px[j], py[j], zd, ce); }
}
/* ---------- Ragdoll y sangre (solo PS2) ----------
 * Al morir, cada bot pasa a una simulacion Verlet de 17 particulas (columna, cadera, hombros, cabeza, piernas con pies, mano y arma)
 * unidas por restricciones de distancia. De las particulas se reconstruyen las 12 matrices de hueso, asi que el mismo modelo con piel
 * se dibuja sin cambios. El impacto (direccion y fuerza del arma, explosion, cabezazo) decide como sale despedido el cuerpo.
 * La sangre son gotas con gravedad que, al chocar con suelo, cajas o paredes, dejan manchas (pila circular fija: GORE_ND). */
#define RD_NP 17
#define RD_H (1.0f / 60.0f)
#define GORE_ND 72
#define GORE_NB 48
enum { RP_HIP, RP_WAIST, RP_NECK, RP_HEAD, RP_HIPL, RP_HIPR, RP_KNL, RP_KNR, RP_ANL, RP_ANR, RP_TOL, RP_TOR, RP_SHL, RP_SHR, RP_HAND, RP_GRIP, RP_MUZ };
static const struct { signed char bone; float p[3]; float r; } RPD[RD_NP] = {
    { BN_PELVIS, { 0, 0.80f, 0 }, 0.14f }, { BN_PELVIS, { 0, 1.06f, 0 }, 0.14f }, { BN_CHEST, { 0, 1.36f, 0 }, 0.10f }, { BN_HEAD, { 0, 1.50f, 0 }, 0.14f },
    { BN_THIGH_L, { -0.19f, 0.78f, 0 }, 0.10f }, { BN_THIGH_R, { 0.19f, 0.78f, 0 }, 0.10f },
    { BN_SHIN_L, { -0.19f, 0.47f, 0 }, 0.09f }, { BN_SHIN_R, { 0.19f, 0.47f, 0 }, 0.09f },
    { BN_FOOT_L, { -0.19f, 0.22f, 0 }, 0.08f }, { BN_FOOT_R, { 0.19f, 0.22f, 0 }, 0.08f },
    { BN_FOOT_L, { -0.19f, 0.05f, -0.20f }, 0.06f }, { BN_FOOT_R, { 0.19f, 0.05f, -0.20f }, 0.06f },
    { BN_CHEST, { -0.22f, 1.24f, 0 }, 0.10f }, { BN_CHEST, { 0.22f, 1.24f, 0 }, 0.10f },
    { BN_ARMS, { 0.17f, 1.12f, -0.38f }, 0.07f }, { BN_WEAP, { 0.17f, 1.12f, -0.38f }, 0.05f }, { BN_WEAP, { 0.17f, 1.12f, -1.02f }, 0.05f } };
/* restricciones: t 0 = distancia fija, 1 = distancia minima, 2 = distancia maxima; f = factor sobre la distancia de reposo; k = rigidez */
static const struct { signed char a, b, t; float f, k; } RCN[] = {
    { RP_HIP, RP_WAIST, 0, 1, 1 }, { RP_WAIST, RP_NECK, 0, 1, 1 },
    { RP_HIP, RP_HIPL, 0, 1, 1 }, { RP_HIP, RP_HIPR, 0, 1, 1 }, { RP_HIPL, RP_HIPR, 0, 1, 1 }, { RP_WAIST, RP_HIPL, 0, 1, 1 }, { RP_WAIST, RP_HIPR, 0, 1, 1 },
    { RP_WAIST, RP_SHL, 0, 1, 1 }, { RP_WAIST, RP_SHR, 0, 1, 1 }, { RP_NECK, RP_SHL, 0, 1, 1 }, { RP_NECK, RP_SHR, 0, 1, 1 }, { RP_SHL, RP_SHR, 0, 1, 1 },
    { RP_SHL, RP_HIPL, 0, 1, 0.15f }, { RP_SHR, RP_HIPR, 0, 1, 0.15f },          /* limitan la torsion del torso */
    { RP_HIP, RP_NECK, 1, 0.88f, 1 },                                              /* limite de flexion de la cintura */
    { RP_NECK, RP_HEAD, 0, 1, 1 }, { RP_WAIST, RP_HEAD, 1, 0.65f, 1 },
    { RP_HIPL, RP_KNL, 0, 1, 1 }, { RP_KNL, RP_ANL, 0, 1, 1 }, { RP_ANL, RP_TOL, 0, 1, 1 }, { RP_KNL, RP_TOL, 0, 1, 1 }, { RP_HIPL, RP_ANL, 1, 0.55f, 1 },
    { RP_HIPR, RP_KNR, 0, 1, 1 }, { RP_KNR, RP_ANR, 0, 1, 1 }, { RP_ANR, RP_TOR, 0, 1, 1 }, { RP_KNR, RP_TOR, 0, 1, 1 }, { RP_HIPR, RP_ANR, 1, 0.55f, 1 },
    { RP_KNL, RP_KNR, 1, 0.42f, 1 }, { RP_ANL, RP_ANR, 1, 0.40f, 1 }, { RP_TOL, RP_TOR, 1, 0.35f, 1 },
    { RP_GRIP, RP_MUZ, 0, 1, 1 }, { RP_HAND, RP_HIP, 1, 0.5f, 1 } };
#define RD_NC ((int)(sizeof RCN / sizeof RCN[0]))
static float rcnRest[RD_NC]; static int rcnReady;
/* colision entre partes del cuerpo: pares de particulas (pies, manos, cabeza, pecho...) que no deben atravesarse.
 * La distancia minima es la suma de radios, pero nunca mas del 85% de la distancia de reposo (asi no pelean con las articulaciones). */
#define RD_NPAIR (RD_NP * (RD_NP - 1) / 2)
static unsigned char rpA[RD_NPAIR], rpB[RD_NPAIR]; static float rpMin[RD_NPAIR]; static int rpN;
typedef struct {
    float p[RD_NP][3], q[RD_NP][3];      /* posicion actual y anterior (Verlet) */
    M34 G[BN_N];                          /* matrices de hueso listas para dibujar */
    float rz[BN_N][9];                    /* giro residual de cada hueso sobre su eje (se calibra con la pose viva al morir) */
    int on, asleep, held;
    float t, sleepT, relT, acc, bleed, bleedAcc, splatCD, wound, pr, prT, px, pz, py;
} Ragdoll;
typedef struct { float x, y, z, vx, vy, vz, t, sz; int on; } BDrop;
typedef struct { float x, y, z, r, age; signed char ax; unsigned char seed; } BDec;     /* ax: 0 suelo/techo de caja, 1 +x, 2 -x, 3 +z, 4 -z (normal de la superficie) */
static Ragdoll rds[MAXBOT]; static BDrop bdrops[GORE_NB]; static BDec bdecs[GORE_ND]; static int bdecI, bdropI;

static void rvSub(const float *a, const float *b, float *o) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static float rvDot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void rvCross(const float *a, const float *b, float *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static void rvNorm(float *v) { float l = sqrtf(rvDot(v, v)); if (l < 1e-6f) { v[0] = 0; v[1] = 1; v[2] = 0; return; } v[0] /= l; v[1] /= l; v[2] /= l; }
/* base ortonormal: arriba = u, derecha = r (ortogonalizada contra u), atras = r x u. R guarda las columnas r, u, atras. */
static void rvFrame(float *R, const float *u0, const float *r0) {
    float u[3] = { u0[0], u0[1], u0[2] }, r[3], b[3]; rvNorm(u);
    float k = rvDot(r0, u); r[0] = r0[0] - u[0] * k; r[1] = r0[1] - u[1] * k; r[2] = r0[2] - u[2] * k;
    if (rvDot(r, r) < 1e-8f) { float t[3] = { 1, 0, 0 }; if (fabsf(u[0]) > 0.9f) { t[0] = 0; t[2] = 1; } k = rvDot(t, u); r[0] = t[0] - u[0] * k; r[1] = t[1] - u[1] * k; r[2] = t[2] - u[2] * k; }
    rvNorm(r); rvCross(r, u, b);
    for (int i = 0; i < 3; i++) { R[i * 3] = r[i]; R[i * 3 + 1] = u[i]; R[i * 3 + 2] = b[i]; }
}
/* rotacion mas corta que lleva el vector unitario a hacia c (3x3 por filas) */
static void rvArc(float *R, const float *a, const float *c) {
    float v[3], cs = rvDot(a, c); rvCross(a, c, v);
    if (cs > -0.9999f) {
        float k = 1.0f / (1.0f + cs), vv = rvDot(v, v);
        R[0] = 1 + k * (v[0] * v[0] - vv); R[1] = -v[2] + k * v[0] * v[1];  R[2] = v[1] + k * v[0] * v[2];
        R[3] = v[2] + k * v[1] * v[0];     R[4] = 1 + k * (v[1] * v[1] - vv); R[5] = -v[0] + k * v[1] * v[2];
        R[6] = -v[1] + k * v[2] * v[0];    R[7] = v[0] + k * v[2] * v[1];  R[8] = 1 + k * (v[2] * v[2] - vv);
    } else {
        float t[3] = { 1, 0, 0 }, k[3]; if (fabsf(a[0]) > 0.9f) { t[0] = 0; t[1] = 1; } rvCross(a, t, k); rvNorm(k);
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i * 3 + j] = 2 * k[i] * k[j] - (i == j ? 1 : 0);
    }
}
/* hueso hijo: rota desde el marco del padre Rp para que su eje de reposo `rest` apunte a `cur` (mundo) */
static void rvBone(float *Rout, const float *Rp, const float *rest, const float *cur) {
    float a[3] = { rest[0], rest[1], rest[2] }, c[3] = { cur[0], cur[1], cur[2] }, cl[3], A[9]; rvNorm(a); rvNorm(c);
    for (int i = 0; i < 3; i++) cl[i] = Rp[i] * c[0] + Rp[3 + i] * c[1] + Rp[6 + i] * c[2];     /* Rp^T * c */
    rvArc(A, a, cl);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) Rout[i * 3 + j] = Rp[i * 3] * A[j] + Rp[i * 3 + 1] * A[3 + j] + Rp[i * 3 + 2] * A[6 + j];
}
static void rvSetG(M34 *g, const float *R, const float *P, const float *piv) {
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) g->m[i * 4 + j] = R[i * 3 + j]; g->m[i * 4 + 3] = P[i] - (R[i * 3] * piv[0] + R[i * 3 + 1] * piv[1] + R[i * 3 + 2] * piv[2]); }
}
/* torso: marcos de pelvis (Rp) y pecho (Rc) y pivote del pecho (Pc) a partir de las particulas */
static void rdTorso(float p[][3], float *Rp, float *Rc, float *Pc) {
    float up[3], r[3];
    rvSub(p[RP_WAIST], p[RP_HIP], up); rvSub(p[RP_HIPR], p[RP_HIPL], r); rvFrame(Rp, up, r);
    float u2[3] = { Rp[1], Rp[4], Rp[7] }, d = BN_PIV[BN_CHEST][1] - BN_PIV[BN_PELVIS][1];
    for (int i = 0; i < 3; i++) Pc[i] = p[RP_HIP][i] + u2[i] * d;
    rvSub(p[RP_NECK], Pc, up); rvSub(p[RP_SHR], p[RP_SHL], r); rvFrame(Rc, up, r);
}
static void rdRest(int i, const float *from, float *o) { for (int k = 0; k < 3; k++) o[k] = RPD[i].p[k] - from[k]; }
/* rotacion de un hueso con un solo eje: arco mas corto desde el marco del padre + giro residual rz (si calib != NULL, se calcula con la pose viva) */
static void rdBoneR(float *Rout, const float *Rpar, const float *rest, const float *cur, int bone, float (*rz)[9], const M34 *calib) {
    float A[9]; rvBone(A, Rpar, rest, cur);
    if (calib) for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) rz[bone][i * 3 + j] = A[i] * calib[bone].m[j] + A[3 + i] * calib[bone].m[4 + j] + A[6 + i] * calib[bone].m[8 + j];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) Rout[i * 3 + j] = A[i * 3] * rz[bone][j] + A[i * 3 + 1] * rz[bone][3 + j] + A[i * 3 + 2] * rz[bone][6 + j];
}
/* particulas -> matrices de hueso. fo: 0..1 relajacion del antebrazo izquierdo */
static void rdBuild(float p[][3], M34 *G, float fo, float (*rz)[9], const M34 *calib) {
    float Rp[9], Rc[9], Rt[9], Pc[3], dir[3], rs[3], Pa[3], v[3];
    rdTorso(p, Rp, Rc, Pc);
    rvSetG(&G[BN_PELVIS], Rp, p[RP_HIP], BN_PIV[BN_PELVIS]);
    rvSetG(&G[BN_CHEST], Rc, Pc, BN_PIV[BN_CHEST]);
    rdRest(RP_HEAD, BN_PIV[BN_HEAD], rs); rvSub(p[RP_HEAD], p[RP_NECK], dir); rdBoneR(Rt, Rc, rs, dir, BN_HEAD, rz, calib); rvSetG(&G[BN_HEAD], Rt, p[RP_NECK], BN_PIV[BN_HEAD]);
    for (int s = 0; s < 2; s++) {
        int th = s ? BN_THIGH_R : BN_THIGH_L, sh = s ? BN_SHIN_R : BN_SHIN_L, ft = s ? BN_FOOT_R : BN_FOOT_L;
        int ph = s ? RP_HIPR : RP_HIPL, pk = s ? RP_KNR : RP_KNL, pa = s ? RP_ANR : RP_ANL, pt = s ? RP_TOR : RP_TOL;
        float R1[9], R2[9], R3[9];
        rdRest(pk, BN_PIV[th], rs); rvSub(p[pk], p[ph], dir); rdBoneR(R1, Rp, rs, dir, th, rz, calib); rvSetG(&G[th], R1, p[ph], BN_PIV[th]);
        rdRest(pa, BN_PIV[sh], rs); rvSub(p[pa], p[pk], dir); rdBoneR(R2, R1, rs, dir, sh, rz, calib); rvSetG(&G[sh], R2, p[pk], BN_PIV[sh]);
        rdRest(pt, BN_PIV[ft], rs); rvSub(p[pt], p[pa], dir); rdBoneR(R3, R2, rs, dir, ft, rz, calib); rvSetG(&G[ft], R3, p[pa], BN_PIV[ft]);
    }
    /* brazos: el hombro va fijo al pecho, el eje apunta a la mano */
    for (int i = 0; i < 3; i++) v[i] = BN_PIV[BN_ARMS][i] - BN_PIV[BN_CHEST][i];
    for (int i = 0; i < 3; i++) Pa[i] = Pc[i] + Rc[i * 3] * v[0] + Rc[i * 3 + 1] * v[1] + Rc[i * 3 + 2] * v[2];
    rdRest(RP_HAND, BN_PIV[BN_ARMS], rs); rvSub(p[RP_HAND], Pa, dir); rdBoneR(Rt, Rc, rs, dir, BN_ARMS, rz, calib); rvSetG(&G[BN_ARMS], Rt, Pa, BN_PIV[BN_ARMS]);
    { M34 B; m34Rot(&B, -0.6f * fo, 0, -0.5f * fo); const float *pv = BN_PIV[BN_FORE_L];
      for (int i = 0; i < 3; i++) B.m[i * 4 + 3] = pv[i] - (B.m[i * 4] * pv[0] + B.m[i * 4 + 1] * pv[1] + B.m[i * 4 + 2] * pv[2]);
      m34Mul(&G[BN_FORE_L], &G[BN_ARMS], &B); }
    { float Rw[9]; rvSub(RPD[RP_MUZ].p, RPD[RP_GRIP].p, rs); rvSub(p[RP_MUZ], p[RP_GRIP], dir); rdBoneR(Rw, Rt, rs, dir, BN_WEAP, rz, calib); rvSetG(&G[BN_WEAP], Rw, p[RP_GRIP], BN_PIV[BN_WEAP]); }
}

/* ---- sangre ---- */
static void decalAdd(float x, float y, float z, int ax, float r) {
    BDec *d = &bdecs[bdecI++ % GORE_ND]; d->x = x; d->y = y; d->z = z; d->ax = (signed char)ax; d->r = r; d->age = 0; d->seed = (unsigned char)(rand() & 255);
}
static void bloodBurst(const float *pos, const float *dir, int n, float spd, float spread, float sz) {
    for (int i = 0; i < n; i++) {
        BDrop *d = &bdrops[bdropI++ % GORE_NB]; float k = spd * rndf(0.35f, 1.15f);
        d->on = 1; d->x = pos[0]; d->y = pos[1]; d->z = pos[2]; d->t = 2.5f; d->sz = sz * rndf(0.6f, 1.4f);
        d->vx = dir[0] * k + rndf(-1, 1) * spread * spd; d->vy = dir[1] * k + rndf(-0.3f, 1) * spread * spd; d->vz = dir[2] * k + rndf(-1, 1) * spread * spd;
    }
}
static void bloodUpdate(float dt) {
    for (int i = 0; i < GORE_NB; i++) {
        BDrop *d = &bdrops[i]; if (!d->on) continue;
        d->t -= dt; if (d->t <= 0) { d->on = 0; continue; }
        d->vy -= 9.8f * dt; float k = fmaxf(0, 1 - 0.6f * dt); d->vx *= k; d->vz *= k;
        float mv = sqrtf(d->vx * d->vx + d->vy * d->vy + d->vz * d->vz) * dt; int ns = (int)(mv / 0.12f) + 1; if (ns > 6) ns = 6;
        float sdt = dt / ns;
        for (int s = 0; s < ns && d->on; s++) {
            float ox = d->x, oy = d->y, oz = d->z;
            d->x += d->vx * sdt; d->y += d->vy * sdt; d->z += d->vz * sdt;
            if (d->y <= 0) { decalAdd(d->x, 0, d->z, 0, d->sz); d->on = 0; break; }
            for (int j = 0; j < nbox; j++) {
                const Box *b = &boxes[j];
                if (d->x <= b->x0 || d->x >= b->x1 || d->z <= b->z0 || d->z >= b->z1 || d->y >= b->top) continue;
                if (oy >= b->top) decalAdd(d->x, b->top, d->z, 0, d->sz);
                else if (ox <= b->x0) decalAdd(b->x0, d->y, d->z, 2, d->sz);
                else if (ox >= b->x1) decalAdd(b->x1, d->y, d->z, 1, d->sz);
                else if (oz <= b->z0) decalAdd(d->x, d->y, b->z0, 4, d->sz);
                else if (oz >= b->z1) decalAdd(d->x, d->y, b->z1, 3, d->sz);
                else decalAdd(d->x, b->top, d->z, 0, d->sz);
                d->on = 0; break;
            }
            if (!d->on) break;
            if (d->x < -HALF) { decalAdd(-HALF, d->y, d->z, 1, d->sz); d->on = 0; }
            else if (d->x > HALF) { decalAdd(HALF, d->y, d->z, 2, d->sz); d->on = 0; }
            else if (d->z < -HALF) { decalAdd(d->x, d->y, -HALF, 3, d->sz); d->on = 0; }
            else if (d->z > HALF) { decalAdd(d->x, d->y, HALF, 4, d->sz); d->on = 0; }
        }
    }
    for (int i = 0; i < GORE_ND; i++) if (bdecs[i].r > 0 && bdecs[i].age < 60) bdecs[i].age += dt;
}
/* salpicadura de cada impacto: sale por detras del objetivo (hacia donde iba la bala) y un poco de niebla hacia el tirador */
static void goreHit(Bot *b, float dmg, int head) {
    float hp[3], D[3];
    if (gHitOn) { for (int i = 0; i < 3; i++) { hp[i] = gHitP[i]; D[i] = gHitD[i]; } }
    else { hp[0] = b->x; hp[1] = b->py + 1.2f; hp[2] = b->z; D[0] = b->x - P.x; D[1] = 0.1f; D[2] = b->z - P.z; rvNorm(D); }
    int n = 3 + (int)(dmg / 10); if (n > 14) n = 14; if (head) n += 4;
    bloodBurst(hp, D, n, 4.0f + clampf(dmg / 25, 0, 4), 0.30f, 0.07f + clampf(dmg / 600, 0, 0.06f));
    float nd[3] = { -D[0], -D[1], -D[2] }; bloodBurst(hp, nd, 2, 1.8f, 0.5f, 0.05f);
}
static void goreReset(void) { memset(rds, 0, sizeof rds); memset(bdrops, 0, sizeof bdrops); memset(bdecs, 0, sizeof bdecs); bdecI = bdropI = 0; }

/* ---- ragdoll ---- */
static void rdPush(Ragdoll *rd, const float *P, const float *D, float F) {
    for (int i = 0; i < RD_NP; i++) {
        float d[3]; rvSub(rd->p[i], P, d); float w = expf(-rvDot(d, d) / 0.30f), a = F * (0.30f + 1.1f * w) * RD_H;
        rd->q[i][0] -= D[0] * a; rd->q[i][1] -= D[1] * a + F * 0.10f * RD_H; rd->q[i][2] -= D[2] * a;
    }
    rd->asleep = 0; rd->sleepT = 0;
}
static void rdSpawn(int idx, float dmg, int head) {
    Bot *b = &bots[idx]; Ragdoll *rd = &rds[idx]; Rig g = b->rg; SkPose po; M34 G[BN_N];
    memset(rd, 0, sizeof *rd);
    g.pist = 0; rigPose(&g, &po); rigAimPitch(&g, &po, b->pitch); rigReload(&g, &po, b->rl ? clampf(b->rlT / 1.7f, 0, 1) : -1.0f);
    rigSolve(&po, b->x, b->py, b->z, b->yaw, G);
    float fx = -sinf(b->yaw), fz = -cosf(b->yaw), rx = cosf(b->yaw), rz = -sinf(b->yaw);
    float v0[3] = { (g.vfS * fx + g.vsS * rx) * 0.9f, 0, (g.vfS * fz + g.vsS * rz) * 0.9f };
    float j[3] = { rndf(-0.3f, 0.3f), 0, rndf(-0.3f, 0.3f) };
    for (int i = 0; i < RD_NP; i++) {
        m34Xf(&G[(int)RPD[i].bone], RPD[i].p, rd->p[i]);
        for (int k = 0; k < 3; k++) rd->q[i][k] = rd->p[i][k] - (v0[k] + j[k]) * RD_H;
    }
    rd->on = 1; rd->held = 1; rd->relT = rndf(0.15f, 0.45f); rd->wound = clampf(0.5f + dmg / 120.0f + (head ? 0.3f : 0), 0.5f, 1);
    rd->bleed = 1.0f + rd->wound; rd->prT = clampf(0.30f + dmg / 160.0f * 0.5f + (head ? 0.15f : 0), 0.30f, 1.10f);
    float hp[3] = { b->x, b->py + 1.2f, b->z }, D[3] = { -fx, 0.1f, -fz }, F = 3.0f;
    if (gHitOn) { for (int i = 0; i < 3; i++) { hp[i] = gHitP[i]; D[i] = gHitD[i]; } F = gHitF; }
    else { D[0] = b->x - P.x; D[2] = b->z - P.z; rvNorm(D); }
    F = clampf(F * (1 + clampf(-b->hp / 60.0f, 0, 1.2f)), 0, 12);        /* sobredano = mas fuerza */
    rdPush(rd, hp, D, F);
    if (head) { float hd[3] = { D[0], D[1] + 0.3f, D[2] }; rvNorm(hd); float a = F * 0.8f * RD_H;
        for (int k = 0; k < 3; k++) { rd->q[RP_HEAD][k] -= hd[k] * a; rd->q[RP_NECK][k] -= hd[k] * a * 0.5f; } }
    { float up[3] = { D[0] * 0.5f, 0.5f, D[2] * 0.5f }; rvNorm(up);
      bloodBurst(hp, D, 6 + (int)clampf(dmg / 10, 0, 10) + (head ? 6 : 0), 3.5f + F * 0.3f, 0.45f, 0.09f);
      if (head) bloodBurst(hp, up, 6, 3.0f, 0.5f, 0.07f); }
    rd->px = rd->p[RP_HIP][0]; rd->pz = rd->p[RP_HIP][2];
    rdBuild(rd->p, rd->G, 0, rd->rz, G);
}
static void rdCollide(Ragdoll *rd, int i, const Box **nb, int nn) {
    float *p = rd->p[i], *q = rd->q[i], r = RPD[i].r;
    float vy = p[1] - q[1], vh = sqrtf((p[0] - q[0]) * (p[0] - q[0]) + (p[2] - q[2]) * (p[2] - q[2])) / RD_H;
    for (int j = 0; j < nn; j++) {
        const Box *b = nb[j];
        if (p[0] < b->x0 - r || p[0] > b->x1 + r || p[2] < b->z0 - r || p[2] > b->z1 + r || p[1] - r >= b->top) continue;
        float up = b->top + r - p[1], pl = p[0] - (b->x0 - r), pr = (b->x1 + r) - p[0], zl = p[2] - (b->z0 - r), zr = (b->z1 + r) - p[2];
        float mh = pl; int ax = 2; if (pr < mh) { mh = pr; ax = 1; } if (zl < mh) { mh = zl; ax = 4; } if (zr < mh) { mh = zr; ax = 3; }
        if (up <= mh || up < 0.12f) {
            p[1] = b->top + r; if (vy < -0.02f) q[1] = p[1] + 0.25f * vy; else q[1] = p[1];
            q[0] += (p[0] - q[0]) * 0.35f; q[2] += (p[2] - q[2]) * 0.35f;
            if (vy / RD_H < -4.0f && rd->splatCD <= 0 && i < RP_HAND) { decalAdd(p[0], b->top, p[2], 0, 0.10f + rd->wound * 0.12f); rd->splatCD = 0.15f; }
        } else {
            if (ax == 2) { p[0] = b->x0 - r; q[0] = p[0]; } else if (ax == 1) { p[0] = b->x1 + r; q[0] = p[0]; }
            else if (ax == 4) { p[2] = b->z0 - r; q[2] = p[2]; } else { p[2] = b->z1 + r; q[2] = p[2]; }
            if (vh > 3.5f && rd->splatCD <= 0 && i < RP_HAND) {      /* cuerpo estampado contra la pared: mancha segun la velocidad */
                float sx = ax == 2 ? b->x0 : (ax == 1 ? b->x1 : p[0]), sz = ax == 4 ? b->z0 : (ax == 3 ? b->z1 : p[2]);
                decalAdd(sx, p[1], sz, ax, clampf(0.05f * vh, 0.12f, 0.45f) * rd->wound); rd->splatCD = 0.15f;
            }
        }
    }
    if (p[1] < r) {
        p[1] = r; if (vy < -0.02f) q[1] = p[1] + 0.25f * vy; else q[1] = p[1];
        q[0] += (p[0] - q[0]) * 0.35f; q[2] += (p[2] - q[2]) * 0.35f;
        if (vy / RD_H < -3.5f && rd->splatCD <= 0 && i < RP_HAND) { decalAdd(p[0], 0, p[2], 0, clampf(-vy / RD_H * 0.035f, 0.10f, 0.35f) * rd->wound); rd->splatCD = 0.15f; }
    }
    float lim = HALF - r;
    if (p[0] < -lim) { p[0] = -lim; q[0] = p[0]; } if (p[0] > lim) { p[0] = lim; q[0] = p[0]; }
    if (p[2] < -lim) { p[2] = -lim; q[2] = p[2]; } if (p[2] > lim) { p[2] = lim; q[2] = p[2]; }
}
static void rdStep(Ragdoll *rd) {
    const Box *nb[28]; int nn = 0;
    if (!rcnReady) {
        for (int c = 0; c < RD_NC; c++) { float d[3]; rvSub(RPD[(int)RCN[c].a].p, RPD[(int)RCN[c].b].p, d); rcnRest[c] = sqrtf(rvDot(d, d)); }
        for (int a = 0; a < RD_NP; a++) for (int b = a + 1; b < RD_NP; b++) {
            float d[3]; rvSub(RPD[a].p, RPD[b].p, d); float rest = sqrtf(rvDot(d, d)), m = RPD[a].r + RPD[b].r;
            if (rest * 0.85f < m) m = rest * 0.85f;
            if (m > 0.03f) { rpA[rpN] = (unsigned char)a; rpB[rpN] = (unsigned char)b; rpMin[rpN] = m; rpN++; }
        }
        rcnReady = 1;
    }
    rd->t += RD_H; if (rd->splatCD > 0) rd->splatCD -= RD_H;
    if (rd->held && rd->t > rd->relT) rd->held = 0;
    for (int i = 0; i < RD_NP; i++) for (int k = 0; k < 3; k++) { float v = (rd->p[i][k] - rd->q[i][k]) * 0.997f; rd->q[i][k] = rd->p[i][k]; rd->p[i][k] += v; }
    for (int i = 0; i < RD_NP; i++) rd->p[i][1] -= 9.8f * RD_H * RD_H;
    for (int j = 0; j < nbox && nn < 28; j++) {       /* solo las cajas cercanas al cuerpo */
        const Box *b = &boxes[j]; float cx = rd->p[RP_HIP][0], cz = rd->p[RP_HIP][2];
        if (cx > b->x0 - 2.6f && cx < b->x1 + 2.6f && cz > b->z0 - 2.6f && cz < b->z1 + 2.6f) nb[nn++] = b;
    }
    float Rp[9], Rc[9], Pc[3], sh[3], L = 0;
    { float rs[3]; rdRest(RP_HAND, BN_PIV[BN_ARMS], rs); L = sqrtf(rvDot(rs, rs)); }
    rdTorso(rd->p, Rp, Rc, Pc);
    { float v[3]; for (int i = 0; i < 3; i++) v[i] = BN_PIV[BN_ARMS][i] - BN_PIV[BN_CHEST][i];
      for (int i = 0; i < 3; i++) sh[i] = Pc[i] + Rc[i * 3] * v[0] + Rc[i * 3 + 1] * v[1] + Rc[i * 3 + 2] * v[2]; }
    for (int it = 0; it < 4; it++) {
        for (int c = 0; c < RD_NC; c++) {
            float *a = rd->p[(int)RCN[c].a], *b = rd->p[(int)RCN[c].b], d[3]; rvSub(b, a, d);
            float len = sqrtf(rvDot(d, d)), tg = rcnRest[c] * RCN[c].f; if (len < 1e-6f) continue;
            if ((RCN[c].t == 1 && len >= tg) || (RCN[c].t == 2 && len <= tg)) continue;
            float k = (len - tg) / len * 0.5f * RCN[c].k;
            for (int m = 0; m < 3; m++) { a[m] += d[m] * k; b[m] -= d[m] * k; }
        }
        { float *h = rd->p[RP_HAND], d[3]; rvSub(h, sh, d); float len = sqrtf(rvDot(d, d)); if (len > 1e-6f) for (int m = 0; m < 3; m++) h[m] = sh[m] + d[m] * (L / len); }
        if (rd->held) for (int m = 0; m < 3; m++) rd->p[RP_GRIP][m] = rd->p[RP_HAND][m];
        { float ramp = clampf(rd->t / 0.12f, 0, 1) * 0.5f;      /* las partes del cuerpo no se atraviesan entre si (entra suave al morir para evitar tirones) */
          for (int c = 0; c < rpN; c++) {
              int a = rpA[c], b = rpB[c]; if (rd->held && (a == RP_GRIP || b == RP_GRIP)) continue;
              float *pa = rd->p[a], *pb = rd->p[b], d[3]; rvSub(pa, pb, d); float l2 = rvDot(d, d), m = rpMin[c];
              if (l2 >= m * m) continue;
              float l = sqrtf(l2), k;
              if (l < 1e-5f) { d[0] = 0; d[1] = 1; d[2] = 0; l = 0; } else { d[0] /= l; d[1] /= l; d[2] /= l; }
              k = (m - l) * ramp;
              for (int e = 0; e < 3; e++) { pa[e] += d[e] * k; pb[e] -= d[e] * k; }
          } }
        for (int i = 0; i < RD_NP; i++) { if (rd->held && i == RP_GRIP) continue; rdCollide(rd, i, nb, nn); }
    }
    float mv = 0; for (int i = 0; i < RD_NP; i++) { float d[3]; rvSub(rd->p[i], rd->q[i], d); float l = rvDot(d, d); if (l > mv) mv = l; }
    if (mv < 0.003f * 0.003f) rd->sleepT += RD_H; else rd->sleepT = 0;      /* ~0,18 m/s */
    if (rd->sleepT > 0.4f || rd->t > 7.0f) rd->asleep = 1;
}
/* cadaver contra cadaver: las partes de un cuerpo no atraviesan las de otro (el que esta dormido hace de obstaculo fijo) */
static void rdBodies(void) {
    for (int i = 0; i < nbots; i++) {
        Ragdoll *A = &rds[i]; if (!A->on) continue;
        for (int j = i + 1; j < nbots; j++) {
            Ragdoll *B = &rds[j]; if (!B->on || (A->asleep && B->asleep)) continue;
            float hd[3]; rvSub(A->p[RP_HIP], B->p[RP_HIP], hd); if (rvDot(hd, hd) > 2.6f * 2.6f) continue;
            float sa = A->asleep ? 0.0f : 1.0f, sb = B->asleep ? 0.0f : 1.0f, wa = sa / (sa + sb), wb = sb / (sa + sb);
            for (int a = 0; a < RP_HAND; a++) for (int b = 0; b < RP_HAND; b++) {
                float *pa = A->p[a], *pb = B->p[b], d[3]; rvSub(pa, pb, d);
                float m = RPD[a].r + RPD[b].r, l2 = rvDot(d, d); if (l2 >= m * m) continue;
                float l = sqrtf(l2); if (l < 1e-5f) { d[0] = 0; d[1] = 1; d[2] = 0; l = 0; } else { d[0] /= l; d[1] /= l; d[2] /= l; }
                float pen = (m - l) * 0.6f;
                for (int e = 0; e < 3; e++) {
                    pa[e] += d[e] * pen * wa; A->q[a][e] += d[e] * pen * wa * 0.7f;
                    pb[e] -= d[e] * pen * wb; B->q[b][e] -= d[e] * pen * wb * 0.7f;
                }
            }
        }
    }
}
static void rdUpdate(float dt) {
    unsigned char stepped[MAXBOT]; memset(stepped, 0, sizeof stepped);
    for (int i = 0; i < nbots; i++) {
        Ragdoll *rd = &rds[i]; if (!rd->on || rd->asleep) continue;
        rd->acc += dt; if (rd->acc > 0.1f) rd->acc = 0.1f;
        while (rd->acc >= RD_H) { rd->acc -= RD_H; rdStep(rd); if (rd->asleep) break; }
        stepped[i] = 1;
    }
    rdBodies();
    for (int i = 0; i < nbots; i++) {
        Ragdoll *rd = &rds[i]; if (!rd->on) continue;
        if (stepped[i]) {
            rdBuild(rd->p, rd->G, sstep(0.0f, 0.4f, rd->t), rd->rz, NULL);
            if (!rd->asleep && rd->t < 3.0f) { rd->px = rd->p[RP_HIP][0]; rd->pz = rd->p[RP_HIP][2]; }
        }
        if (rd->bleed > 0) {                    /* sigue sangrando: rastro en el suelo mientras el cuerpo se arrastra */
            rd->bleed -= dt; rd->bleedAcc += dt;
            if (rd->bleedAcc > 0.12f) { rd->bleedAcc = 0; float d[3] = { 0, -0.2f, 0 }; bloodBurst(rd->p[(rd->t * 8 > 1 && ((int)(rd->t * 20) & 1)) ? RP_WAIST : RP_HIP], d, 1, 0.8f, 0.8f, 0.06f); }
        }
        if (rd->t > 0.4f) rd->pr += (rd->prT - rd->pr) * clampf(dt * 0.6f, 0, 1);      /* charco que crece bajo el cuerpo */
        rd->py = groundAt(rd->px, rd->pz, rd->p[RP_HIP][1] + 0.3f, 0.1f) + 0.012f;
    }
}
static void goreUpdate(float dt) { rdUpdate(dt); bloodUpdate(dt); }
/* bala contra cadaveres: el cuerpo se mueve y suelta sangre; devuelve la distancia del choque o t si no hay */
static float rdRay(const float *o, const float *d, float t) {
    int hi = -1;
    for (int i = 0; i < nbots; i++) {
        if (!rds[i].on) continue;
        for (int k = 0; k < RP_HAND; k++) { float s = raySphere(o, d, rds[i].p[k], RPD[k].r + 0.05f); if (s < t) { t = s; hi = i; } }
    }
    if (hi >= 0) {
        float hp[3] = { o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t }; Ragdoll *rd = &rds[hi];
        rdPush(rd, hp, d, 1.5f + W[wsel].dmg * 0.04f * (W[wsel].pellets > 1 ? 1.5f : 1.0f));
        if (rd->wound > 0.3f) bloodBurst(hp, d, 3, 3.0f, 0.4f, 0.06f);
    }
    return t;
}
/* manchas: se proyectan como abanicos irregulares pegados a la superficie (suelo, caja o pared) */
static void goreFan(const float *c, const float *ta, const float *tb, float r, int n, u32c cc, u32c ce, unsigned seed, float sa, float sb) {
    float X[9], Y[9]; int Z[9], ok = 1; float ps;
    if (n > 8) n = 8;
    for (int i = 0; i < n && ok; i++) {
        seed = seed * 1664525u + 1013904223u; float rr = r * (0.6f + 0.6f * ((seed >> 8) & 255) / 255.0f), a = i * 6.2832f / n + 0.5f;
        float cs = cosf(a) * rr * sa, sn = sinf(a) * rr * sb;
        ok = bfProj(c[0] + ta[0] * cs + tb[0] * sn, c[1] + ta[1] * cs + tb[1] * sn, c[2] + ta[2] * cs + tb[2] * sn, &X[i], &Y[i], &Z[i], &ps);
    }
    float cx, cy; int cz; if (!ok || !bfProj(c[0], c[1], c[2], &cx, &cy, &cz, &ps)) return;
    for (int i = 0; i < n; i++) { int j = (i + 1) % n; gfx_tri(cx, cy, cz, cc, X[i], Y[i], Z[i], ce, X[j], Y[j], Z[j], ce); }
}
static void goreDraw(void) {
    static const float N6[5][3] = { { 0, 1, 0 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    static const float TA[5][3] = { { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0, 1 }, { 1, 0, 0 }, { 1, 0, 0 } }, TB[5][3] = { { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 } };
    float sx, sy, ps; int zd;
    for (int i = 0; i < nbots; i++) {            /* charcos */
        const Ragdoll *rd = &rds[i]; if (!rd->on || rd->pr < 0.05f) continue;
        float c[3] = { rd->px, rd->py, rd->pz }; if (!bfProj(c[0], c[1], c[2], &sx, &sy, &zd, &ps) || rd->pr * ps < 1.0f) continue;
        goreFan(c, TA[0], TB[0], rd->pr, 8, RGBA(104, 8, 10, 215), RGBA(84, 6, 8, 200), (unsigned)(i * 7919 + 13), 1, 1);
    }
    for (int i = 0; i < GORE_ND; i++) {
        const BDec *d = &bdecs[i]; if (d->r <= 0) continue;
        int ax = d->ax; float c[3] = { d->x + N6[ax][0] * 0.012f, d->y + N6[ax][1] * 0.012f, d->z + N6[ax][2] * 0.012f };
        float ex = camX - c[0], ey = camY - c[1], ez = camZ - c[2];
        if (ex * N6[ax][0] + ey * N6[ax][1] + ez * N6[ax][2] <= 0) continue;                   /* se ve desde el lado de la superficie */
        if (ex * ex + ey * ey + ez * ez > 38.0f * 38.0f) continue;
        if (!bfProj(c[0], c[1], c[2], &sx, &sy, &zd, &ps) || d->r * ps < 0.8f) continue;
        float u = clampf(d->age / 25.0f, 0, 1); int cr = (int)lerpf(150, 82, u), cg = (int)lerpf(10, 6, u), cb = (int)lerpf(12, 8, u);
        int n = d->r * ps > 6 ? 6 : 4;
        goreFan(c, TA[ax], TB[ax], d->r, n, RGBA(cr, cg, cb, 225), RGBA(cr * 0.8f, cg, cb, 205), d->seed, 1, ax ? 1.25f : 1);
        if (ax && d->r > 0.09f) {                                                                 /* chorreo hacia abajo en las paredes */
            float c2[3] = { c[0], c[1] - d->r * 1.6f, c[2] };
            goreFan(c2, TA[ax], TB[ax], d->r * 0.35f, 4, RGBA(cr, cg, cb, 205), RGBA(cr, cg, cb, 190), d->seed + 5u, 0.7f, 2.2f);
        }
    }
}
static void bfDrawFx(void) {
    float sx, sy, ps; int zd;
    goreDraw();
    for (int i = 0; i < 10; i++) if (trcs[i].t > 0) {       /* trazadoras */
        float x2, y2, ps2; int z2; if (!bfProj(trcs[i].x, trcs[i].y, trcs[i].z, &sx, &sy, &zd, &ps) || !bfProj(trcs[i].tx, trcs[i].ty, trcs[i].tz, &x2, &y2, &z2, &ps2)) continue;
        float dx = x2 - sx, dy = y2 - sy, L = sqrtf(dx * dx + dy * dy); if (L < 1) continue; float nx = -dy / L * 0.9f * SH / 224.0f, ny = dx / L * 0.9f * SH / 224.0f;
        int a = (int)(clampf(trcs[i].t / 0.07f, 0, 1) * 230); u32c c = RGBA(255, 232, 160, a), c0 = RGBA(255, 232, 160, 0);
        float hx = sx + dx * 0.35f, hy = sy + dy * 0.35f;       /* la cola se desvanece */
        gfx_tri(sx + nx, sy + ny, zd, c0, sx - nx, sy - ny, zd, c0, hx + nx, hy + ny, zd, c);
        gfx_tri(sx - nx, sy - ny, zd, c0, hx - nx, hy - ny, zd, c, hx + nx, hy + ny, zd, c);
        gfx_tri(hx + nx, hy + ny, zd, c, hx - nx, hy - ny, zd, c, x2, y2, z2, c0);
    }
    for (int i = 0; i < 8; i++) if (scr[i].a > 0) {         /* marcas de quemadura en el suelo */
        if (!bfProj(scr[i].x, 0.02f, scr[i].z, &sx, &sy, &zd, &ps)) continue;
        float rad = scr[i].r * ps, e; { float ex, ey, eq; int ez; if (bfProj(scr[i].x, 0.02f, scr[i].z - scr[i].r, &ex, &ey, &ez, &eq)) e = fabsf(ey - sy) / rad; else e = 0.3f; if (e < 0.05f) continue; if (e > 1) e = 1; }
        bfFan(sx, sy, zd, rad, 8, RGBA(20, 16, 14, (int)scr[i].a), RGBA(20, 16, 14, 0), e);
    }
    {   /* sombras blandas bajo los soldados */
        for (int i = 0; i < nbots; i++) if (bots[i].alive && bfProj(bots[i].x, bots[i].py + 0.02f, bots[i].z, &sx, &sy, &zd, &ps)) {
            float ex, ey, eq; int ez; float e = 0.3f; if (bfProj(bots[i].x, bots[i].py + 0.02f, bots[i].z - 0.5f, &ex, &ey, &ez, &eq)) e = clampf(fabsf(ey - sy) / (0.5f * ps), 0.08f, 1);
            bfFan(sx, sy, zd, 0.55f * ps, 6, RGBA(10, 10, 10, 110), RGBA(10, 10, 10, 0), e);
        }
        if (S.cam && P.alive && bfProj(P.x, P.y + 0.02f, P.z, &sx, &sy, &zd, &ps)) bfFan(sx, sy, zd, 0.55f * ps, 6, RGBA(10, 10, 10, 110), RGBA(10, 10, 10, 0), 0.4f);
    }
    for (int i = 0; i < 6; i++) if (cass[i].t > 0) obox(cass[i].x, cass[i].y + 0.01f, cass[i].z, 0.008f, 0.008f, 0.02f, cass[i].rot, RGBA(198, 152, 58, 255));
    for (int i = 0; i < BF_NP; i++) { Pt *q = &pts[i]; if (q->t <= 0) continue;
        if (!bfProj(q->x, q->y, q->z, &sx, &sy, &zd, &ps)) continue;
        float u = 1 - q->t / q->T, sz = lerpf(q->s0, q->s1, u);
        if (q->kind == 1) {      /* chispa: trazo corto en la direccion del movimiento */
            float x2, y2, p2; int z2; if (!bfProj(q->x - q->vx * 0.05f, q->y - q->vy * 0.05f, q->z - q->vz * 0.05f, &x2, &y2, &z2, &p2)) continue;
            int a = (int)(clampf(q->t / q->T * 2, 0, 1) * q->a); u32c c = RGBA(q->r, q->gc, q->b, a);
            float w = 0.8f * SH / 224.0f; gfx_tri(sx - w, sy, zd, c, sx + w, sy, zd, c, x2, y2, zd, RGBA(q->r, q->gc, q->b, 0)); continue; }
        if (q->kind == 4) { float h = fmaxf(1.0f, sz * ps); gfx_rect(sx - h, sy - h, sx + h, sy + h, RGBA(q->r, q->gc, q->b, 255)); continue; }
        float rad = clampf(sz * ps, 1, 260);
        float fade = q->kind == 3 ? (1 - u) * (1 - u) : (u < 0.15f ? u / 0.15f : 1 - (u - 0.15f) / 0.85f);
        int r = q->r, g = q->gc, b = q->b;
        if (q->kind == 2) { r = (int)lerpf(255, 95, u); g = (int)lerpf(200, 45, u); b = (int)lerpf(90, 28, u); }
        int a = (int)clampf(fade * q->a, 0, 255);
        bfFan(sx, sy, zd, rad, q->kind == 3 ? 6 : 6, RGBA(r, g, b, a), RGBA(r, g, b, 0), 1.0f);
    }
}
static void bfHurtFrom(float x, float z) {            /* marca de donde vino el disparo: el indicador gira con la camara */
    int k = 0; for (int i = 1; i < 4; i++) if (bfDT[i] < bfDT[k]) k = i;
    float dx = x - P.x, dz = z - P.z, l = sqrtf(dx * dx + dz * dz); if (l < 0.01f) return;
    bfDX[k] = dx / l; bfDZ[k] = dz / l; bfDT[k] = 1.4f;
}
static void bfThrow(void) {
    float cp = cosf(P.pitch), sp = sinf(P.pitch);
    float f[3] = { -sinf(P.yaw) * cp, sp, -cosf(P.yaw) * cp };
    for (int i = 0; i < 4; i++) if (!nades[i].on) {
        Nade *n = &nades[i];
        n->x = P.x + f[0] * 0.6f; n->y = P.y + 1.45f - 0.5f * P.cr + f[1] * 0.6f; n->z = P.z + f[2] * 0.6f;
        float sp2 = 15.0f; n->vx = f[0] * sp2 + P.vx * 0.5f; n->vy = f[1] * sp2 + 3.2f; n->vz = f[2] * sp2 + P.vz * 0.5f;
        n->t = 2.4f; n->on = 1; bfNades--; return;
    }
}
static void bfExplode(Nade *n) {
    const float R = 8.0f;
    snd_play3d(SND_BOOM, n->x, n->z, 1.0f);
    bfBoomFx(n->x, n->y, n->z);
    float o[3] = { n->x, n->y + 0.15f, n->z };
    for (int i = 0; i < nbots; i++) {
        Bot *b = &bots[i]; if (!b->alive) continue;
        float c[3] = { b->x, b->py + 0.95f - 0.4f * b->rg.crouch, b->z }, d[3] = { c[0] - o[0], c[1] - o[1], c[2] - o[2] };
        float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); if (L > R) continue;
        if (L > 0.01f) { d[0] /= L; d[1] /= L; d[2] /= L; if (wallDist(o, d, L) < L - 0.4f) continue; }   /* una pared tapa la onda */
#ifdef PS2_ONLINE
        { float dd[3] = { d[0], d[1] + 0.6f, d[2] }; if (L <= 0.01f) { dd[0] = dd[2] = 0; } float dl = sqrtf(dd[0] * dd[0] + dd[1] * dd[1] + dd[2] * dd[2]); if (dl > 1e-4f) { dd[0] /= dl; dd[1] /= dl; dd[2] /= dl; } else dd[1] = 1;
          gHitOn = 1; for (int k = 0; k < 3; k++) { gHitP[k] = c[k]; gHitD[k] = dd[k]; } gHitF = 4.0f + 7.0f * (1 - L / R); }
#endif
        damageBot(b, 170.0f * (1 - L / R), 0);
#ifdef PS2_ONLINE
        gHitOn = 0;
#endif
    }
    { float c[3] = { P.x, P.y + 1.0f - 0.4f * P.cr, P.z }, d[3] = { c[0] - o[0], c[1] - o[1], c[2] - o[2] };
      float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      bfShake = fmaxf(bfShake, clampf(1.3f * (1 - L / 22), 0, 1.3f)); bfFlashT = fmaxf(bfFlashT, clampf(0.9f * (1 - L / 16), 0, 0.9f));
      if (P.alive && L < R) {
          int ok = 1; if (L > 0.01f) { d[0] /= L; d[1] /= L; d[2] /= L; ok = wallDist(o, d, L) >= L - 0.4f; }
          if (ok) { hurtPlayer(110.0f * (1 - L / R)); bfHurtFrom(n->x, n->z); }
      } }
    n->on = 0;
}
static void bfUpdateNades(float dt) {
    for (int i = 0; i < 4; i++) {
        Nade *n = &nades[i]; if (!n->on) continue;
        n->t -= dt; n->vy -= 18 * dt;
        float ox = n->x, oz = n->z, dx = n->vx * dt, dz = n->vz * dt;
        moveBody(&n->x, &n->z, n->y - 0.12f, dx, dz, 0.10f, NULL);
        if (fabsf(n->x - ox - dx) > 1e-4f) { n->vx *= -0.45f; n->vz *= 0.8f; snd_play3d(SND_BOUNCE, n->x, n->z, 0.7f); }
        if (fabsf(n->z - oz - dz) > 1e-4f) { n->vz *= -0.45f; n->vx *= 0.8f; snd_play3d(SND_BOUNCE, n->x, n->z, 0.7f); }
        n->y += n->vy * dt;
        float g = groundAt(n->x, n->z, n->y, 0.1f) + 0.06f;
        if (n->y < g) { n->y = g; if (n->vy < -1.6f) snd_play3d(SND_BOUNCE, n->x, n->z, 0.6f); n->vy = -n->vy * 0.35f; n->vx *= 0.65f; n->vz *= 0.65f; if (fabsf(n->vy) < 0.6f) n->vy = 0; }
        n->x = clampf(n->x, -HALF + .2f, HALF - .2f); n->z = clampf(n->z, -HALF + .2f, HALF - .2f);
        if (n->t <= 0) bfExplode(n);
    }
    for (int i = 0; i < 4; i++) if (booms[i].t > 0) booms[i].t -= dt;
}
static void bfDrawNades(void) {
    for (int i = 0; i < 4; i++) if (nades[i].on) {
        Nade *n = &nades[i];
        obox(n->x, n->y, n->z, 0.045f, 0.06f, 0.045f, n->t * 9, RGBA(58, 70, 44, 255));
        obox(n->x, n->y + 0.07f, n->z, 0.02f, 0.015f, 0.02f, n->t * 9, RGBA(120, 124, 120, 255));
    }
    bfDrawFx();
}
#endif

/* ---------- MODO SECRETO (codigo 1122): campo de pruebas ----------
 * Soldados que se quedan quietos para dispararles, ver su skin o ver como mueren (balas, granadas). Eres invulnerable y la municion es infinita. */
static void labClear(void) {
    for (int i = 0; i < MAXBOT; i++) { memset(&bots[i], 0, sizeof bots[i]); bots[i].unused = 1; }
#ifdef PS2_ONLINE
    memset(rds, 0, sizeof rds); memset(bdrops, 0, sizeof bdrops); memset(bdecs, 0, sizeof bdecs); bdecI = bdropI = 0;
#endif
}
static void labSpawn(void) {
    int idx = -1; float old = -1;
    for (int i = 0; i < MAXBOT; i++) if (bots[i].unused) { idx = i; break; }
    if (idx < 0) for (int i = 0; i < MAXBOT; i++) if (!bots[i].alive && bots[i].dead > old) { old = bots[i].dead; idx = i; }      /* recicla el cadaver mas viejo */
    if (idx < 0) { setBanner("LIMITE DE SOLDADOS", "ABAJO: BORRAR TODOS", 1.5f); return; }
    float fx = -sinf(P.yaw), fz = -cosf(P.yaw), rx = cosf(P.yaw), rz = -sinf(P.yaw), px = 0, pz = 0; int ok = 0;
    static const float LAT[5] = { 0, 1.1f, -1.1f, 2.2f, -2.2f };
    for (int di = 0; di < 4 && !ok; di++) for (int li = 0; li < 5 && !ok; li++) {
        float d = 5.0f - di, lt = LAT[li]; px = P.x + fx * d + rx * lt; pz = P.z + fz * d + rz * lt;
        if (fabsf(px) > HALF - 1 || fabsf(pz) > HALF - 1 || blockedAt(px, pz, 0, 0.45f) || !walkClear(P.x, P.z, px, pz)) continue;
        int free = 1; for (int k = 0; k < MAXBOT; k++) if (k != idx && bots[k].alive && (bots[k].x - px) * (bots[k].x - px) + (bots[k].z - pz) * (bots[k].z - pz) < 0.8f * 0.8f) free = 0;
        ok = free;
    }
    if (!ok) { setBanner("SIN ESPACIO AQUI", "MUEVETE A OTRO LUGAR", 1.5f); return; }
    Bot *b = &bots[idx]; memset(b, 0, sizeof *b);
    b->x = px; b->z = pz; b->yaw = atan2f(-(P.x - px), -(P.z - pz)); b->hp = DIFF_HP[S.diff]; b->alive = 1; b->dummy = 1; b->state = BS_PATROL;
#ifdef PS2_ONLINE
    memset(&rds[idx], 0, sizeof rds[idx]); bfSpot[idx] = 0;
#endif
    snd_play(SND_TICK, 0.8f);
}
static void labStart(void) {
    if (curMap != S.map) { curMap = S.map; buildWorld(); }
    sandbox = 1; nbots = MAXBOT; roundNo = 1; scoreMe = scoreBots = 0; kills = heads = deaths = 0; damageTaken = 0;
    memset(feed, 0, sizeof(feed)); memset(feedT, 0, sizeof(feedT));
    memset(&P, 0, sizeof(P)); memset(&pRig, 0, sizeof(pRig)); spawnPlayer(); P.hp = 100; P.alive = 1; P.ground = 1;
    resetAmmo(); labClear(); freeze = 0.3f; afterT = 0; mode = M_PLAYING; hurtT = 0;
    setBanner("CAMPO DE PRUEBAS", "ARRIBA: SOLDADO   ABAJO: BORRAR", 3.0f);
    labSpawn();
}
static void labSet(int i, int dir) {
    if (i == 1) { S.map = (S.map + dir + NMAPS) % NMAPS; curMap = S.map; buildWorld(); }
    else if (i == 2) S.skin = (S.skin + dir + 4) % 4;
    snd_play(SND_TICK, 0.6f);
}

static void botShoot(Bot *b, float dist) {
    snd_play3d(SND_BOTSHOT, b->x, b->z, 1.0f);
#ifdef PS2_ONLINE
    bfSpot[b - bots] = 2.5f;      /* el que dispara aparece en el radar */
#endif
    float o[3] = { b->x - sinf(b->yaw) * 0.6f, 1.35f, b->z - cosf(b->yaw) * 0.6f };
    float k = DIFF_AIM[S.diff];
    float t[3] = { P.x + rndf(-1, 1) * k * (1 + dist * .35f), P.y + 1.15f - 0.42f * P.cr + rndf(-1, 1) * k * (1 + dist * .3f), P.z + rndf(-1, 1) * k * (1 + dist * .35f) };
    float d[3] = { t[0] - o[0], t[1] - o[1], t[2] - o[2] };
    float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); d[0] /= L; d[1] /= L; d[2] /= L;
    float wd = wallDist(o, d, 80);
#ifdef PS2_ONLINE
    { float e = wd; bfTracer(o[0], o[1], o[2], o[0] + d[0] * e, o[1] + d[1] * e, o[2] + d[2] * e);
      bfPt(3, o[0], o[1], o[2], 0, 0, 0, 0.05f, 0.09f, 0.15f, 255, 225, 150, 220, 0, 0);
      if (wd < 79) bfImpact(o, d, wd); }
#endif
    float mn[3] = { P.x - .3f, P.y, P.z - .3f }, mx[3] = { P.x + .3f, P.y + 1.75f - 0.6f * P.cr, P.z + .3f };
    if (rayBox(o, d, mn, mx) < wd) {
#ifdef PS2_ONLINE
        bfHurtFrom(b->x, b->z);
#endif
        hurtPlayer(DIFF_DMG[S.diff] * rndf(0.8f, 1.2f));
    }
}

#ifdef PC_ONLINE
/* ---------- Multijugador online: UDP, un anfitrion (el que crea la partida) y un cliente ----------
 * Cada jugador simula su propio personaje y envia su estado 30 veces por segundo. El que dispara calcula el
 * impacto y avisa al otro (evento fiable con confirmacion); el anfitrion decide quien gana cada ronda. */
#define MP_PORT 7777
#ifdef PS2_ONLINE
#define PS2IP_DEFAULT "192.168.1."
#else
#define PS2IP_DEFAULT "127.0.0.1"
#endif
#define MP_VER 3
enum { PK_HELLO = 1, PK_WELCOME, PK_BUSY, PK_PING, PK_STATE, PK_EV, PK_ACK, PK_BYE };
enum { EV_START = 1, EV_ROUNDEND, EV_MATCHEND, EV_HIT };
enum { MPP_IDLE, MPP_HOST_WAIT, MPP_JOINING, MPP_CLIENT_LOBBY, MPP_MATCH };
static int mpPhase = MPP_IDLE, peerValid, peerPort, hostScore, cliScore, mpTag, curTag, havePeerSeq;
static unsigned peerIp;
static char myName[16] = "JUGADOR", peerName[16], joinText[40] = PS2IP_DEFAULT, mpNotice[48], localIp[24];
static float mpNoticeT, mpRx, mpTx, mpHello, mpPing, mpJoinT, evResend;
static unsigned short mySeq, peerSeq;
typedef struct { unsigned short id; unsigned char type, len; unsigned char d[32]; } Ev;
static Ev evq[16]; static int evN; static unsigned short evNext = 1, evExpect = 1;
typedef struct { unsigned char b[120]; int n, r; } Pk;
static void pkBegin(Pk *p, int type) { p->b[0] = 'P'; p->b[1] = 'C'; p->b[2] = MP_VER; p->b[3] = (unsigned char)type; p->n = 4; p->r = 4; }
static void pk8(Pk *p, int v) { if (p->n < 120) p->b[p->n++] = (unsigned char)v; }
static void pk16(Pk *p, int v) { pk8(p, v & 255); pk8(p, (v >> 8) & 255); }
static void pk32(Pk *p, unsigned v) { pk16(p, (int)(v & 65535)); pk16(p, (int)(v >> 16)); }
static void pkf(Pk *p, float f) { if (p->n + 4 <= 120) { memcpy(p->b + p->n, &f, 4); p->n += 4; } }
static void pkName(Pk *p, const char *s) { for (int i = 0; i < 15; i++) pk8(p, s[i] ? (unsigned char)s[i] : 0); }
static int rd8(Pk *p) { return p->r < p->n ? p->b[p->r++] : 0; }
static int rd16(Pk *p) { int a = rd8(p); return a | (rd8(p) << 8); }
static unsigned rd32(Pk *p) { unsigned a = (unsigned)rd16(p); return a | ((unsigned)rd16(p) << 16); }
static float rdf(Pk *p) { float f = 0; if (p->r + 4 <= p->n) memcpy(&f, p->b + p->r, 4); p->r += 4; return f; }
static void rdName(Pk *p, char *o) { for (int i = 0; i < 15; i++) { int c = rd8(p); o[i] = (char)((c >= 32 && c < 127) ? c : 0); } o[15] = 0; }
static void mpSend(const Pk *p) { if (peerPort) net_send(peerIp, peerPort, p->b, p->n); }
static unsigned worldHash(void) { return 0x50430300u + NMAPS; }      /* version del contenido: todos los mapas existen en ambos lados */
static void mpEv(int type, const void *d, int len) {
    if (evN >= 16) { memmove(evq, evq + 1, 15 * sizeof(Ev)); evN = 15; }
    Ev *e = &evq[evN++]; e->id = evNext++; if (evNext == 0) evNext = 1; e->type = (unsigned char)type; e->len = (unsigned char)len; memcpy(e->d, d, len); evResend = 0;
}
static void mpResetLink(void) { evN = 0; evNext = 1; evExpect = 1; havePeerSeq = 0; mySeq = 0; mpRx = 0; mpTx = 0; mpPing = 0; }
static void mpNote(const char *s) { strncpy(mpNotice, s, 47); mpNotice[47] = 0; mpNoticeT = 5.0f; }
static void mpLeave(const char *note, int toMode) {
    if (netMode && peerValid) { Pk p; pkBegin(&p, PK_BYE); for (int i = 0; i < 3; i++) mpSend(&p); }
    net_close(); netMode = 0; mpPhase = MPP_IDLE; peerValid = 0; peerPort = 0; evN = 0;
    for (int i = 0; i < MAXBOT; i++) bots[i].remote = 0;
    nbots = 0; mode = toMode; sel = 0; if (note && note[0]) mpNote(note);
}
static int mpHost(void) {
    if (net_open(MP_PORT) != 0) { const char *e = net_last_error(); mpNote(e[0] ? e : "NO SE PUDO ABRIR EL PUERTO UDP 7777"); return 0; }
    netMode = 1; mpPhase = MPP_HOST_WAIT; peerValid = 0; peerPort = 0; mpResetLink(); mpRounds = S.rounds; net_local_ip(localIp, 24);
    mode = M_MP_LOBBY; sel = 0; return 1;
}
static int mpJoin(void) {
    unsigned ip; int port;
    if (net_resolve(joinText, &ip, &port, MP_PORT) != 0) { mpNote("DIRECCION IP NO VALIDA"); return 0; }
    if (net_open(0) != 0) { const char *e = net_last_error(); mpNote(e[0] ? e : "NO SE PUDO ABRIR LA RED"); return 0; }
    netMode = 2; mpPhase = MPP_JOINING; peerValid = 0; peerIp = ip; peerPort = port; mpResetLink(); mpHello = 0; mpJoinT = 0;
    mode = M_MP_LOBBY; sel = 0; return 1;
}
static void mpBeginRound(int round, int newMatch, int tag, float mx, float mz, float myaw, float ox, float oz, float oyaw, int myScore, int oppScore) {
    memset(bots, 0, sizeof(bots)); nbots = 1;
    Bot *b = &bots[0]; b->remote = 1; b->alive = 1; b->hp = 100; b->x = b->tx = ox; b->z = b->tz = oz; b->yaw = b->tyaw = oyaw; b->py = b->ty = 0;
    memset(&P, 0, sizeof(P)); memset(&pRig, 0, sizeof(pRig)); P.x = mx; P.z = mz; P.yaw = myaw; P.hp = 100; P.alive = 1; P.ground = 1;
    resetAmmo(); freeze = 2.4f; afterT = 0; mode = M_PLAYING; hurtT = 0; roundNo = round; curTag = tag; mpPhase = MPP_MATCH; mpShots = 0;
    scoreMe = myScore; scoreBots = oppScore;
    if (newMatch) { kills = heads = deaths = 0; damageTaken = 0; memset(feed, 0, sizeof(feed)); memset(feedT, 0, sizeof(feedT)); }
    char a[40], c[48]; snprintf(a, 40, "RONDA %d", round); snprintf(c, 48, "DUELO CONTRA %s", peerName);
    setBanner(a, c, 2.2f);
}
static void mpHostBeginRound(int newMatch) {
    if (newMatch) { hostScore = cliScore = 0; roundNo = 1; if (curMap != S.map) { curMap = S.map; buildWorld(); } }
    const MapDef *md = &MAPS[curMap]; float hx = md->ps[(roundNo - 1) % md->np].x, hz = md->ps[(roundNo - 1) % md->np].z, hyaw = openYaw(hx, hz);
    float cx = md->bs[(roundNo - 1) % md->nb].x, cz = md->bs[(roundNo - 1) % md->nb].z, cyaw = openYaw(cx, cz);
    mpTag = (mpTag + 1) & 255;
    unsigned char d[32]; Pk q; pkBegin(&q, 0);
    q.n = 0; pk8(&q, roundNo); pk8(&q, newMatch); pk8(&q, mpTag); pk8(&q, hostScore); pk8(&q, cliScore);
    pkf(&q, hx); pkf(&q, hz); pkf(&q, hyaw); pkf(&q, cx); pkf(&q, cz); pkf(&q, cyaw); pk8(&q, curMap);
    memcpy(d, q.b, q.n); mpEv(EV_START, d, q.n);
    mpBeginRound(roundNo, newMatch, mpTag, hx, hz, hyaw, cx, cz, cyaw, hostScore, cliScore);
}
static void mpShowRoundEnd(int winner, int hs, int cs) {       /* 0 anfitrion, 1 cliente, 2 empate */
    hostScore = hs; cliScore = cs;
    scoreMe = netMode == 1 ? hs : cs; scoreBots = netMode == 1 ? cs : hs;
    mode = M_ROUNDEND; afterT = 3.0f;
    int me = netMode == 1 ? 0 : 1;
    if (winner == 2) setBanner("EMPATE", "AMBOS CAYERON", 2.8f);
    else if (winner == me) { snd_play(SND_WIN, 0.8f); setBanner("RONDA GANADA", "ELIMINASTE AL RIVAL", 2.8f); }
    else { snd_play(SND_LOSE, 0.6f); setBanner("RONDA PERDIDA", "TE ELIMINARON", 2.8f); }
}
static void mpFinishRound(int winner) {                         /* solo el anfitrion decide */
    int hs = hostScore + (winner == 0), cs = cliScore + (winner == 1);
    unsigned char d[3] = { (unsigned char)winner, (unsigned char)hs, (unsigned char)cs }; mpEv(EV_ROUNDEND, d, 3);
    mpShowRoundEnd(winner, hs, cs);
}
static void mpSendHit(float dmg, int head) {
    unsigned char d[4]; int v = (int)(dmg * 10); if (v > 65535) v = 65535;
    d[0] = (unsigned char)(v & 255); d[1] = (unsigned char)(v >> 8); d[2] = (unsigned char)head; d[3] = (unsigned char)curTag;
    mpEv(EV_HIT, d, 4);
}
static void mpProcessEv(int type, const unsigned char *d, int len) {
    Pk q; q.n = len; q.r = 0; memcpy(q.b, d, len);
    if (type == EV_START && netMode == 2 && len >= 30) {
        int round = rd8(&q), nm = rd8(&q), tag = rd8(&q), hs = rd8(&q), cs = rd8(&q);
        float hx = rdf(&q), hz = rdf(&q), hy = rdf(&q), cx = rdf(&q), cz = rdf(&q), cy = rdf(&q);
        int mp = rd8(&q); if (mp >= 0 && mp < NMAPS && mp != curMap) { curMap = mp; buildWorld(); }
        hostScore = hs; cliScore = cs;
        mpBeginRound(round, nm, tag, cx, cz, cy, hx, hz, hy, cs, hs);
    } else if (type == EV_ROUNDEND && netMode == 2) {
        int w = rd8(&q), hs = rd8(&q), cs = rd8(&q); mpShowRoundEnd(w, hs, cs);
    } else if (type == EV_MATCHEND && netMode == 2) {
        hostScore = rd8(&q); cliScore = rd8(&q); scoreMe = cliScore; scoreBots = hostScore; mode = M_END; sel = 0;
    } else if (type == EV_HIT && len >= 4) {
        float dmg = (float)(rd8(&q) | (rd8(&q) << 8)) / 10.0f; int head = rd8(&q), tag = rd8(&q); (void)head;
        if (tag == curTag && mode == M_PLAYING && P.alive) hurtPlayer(dmg);
    }
}
static void mpHandle(Pk *p, unsigned ip, int port) {
    if (p->n < 4 || p->b[0] != 'P' || p->b[1] != 'C' || p->b[2] != MP_VER) return;
    int type = p->b[3]; int fromPeer = peerPort && ip == peerIp && port == peerPort;
    if (type == PK_HELLO && netMode == 1) {
        char nm[16]; rdName(p, nm); unsigned h = rd32(p);
        Pk r;
        if (h != worldHash()) { pkBegin(&r, PK_BUSY); pk8(&r, 2); net_send(ip, port, r.b, r.n); return; }
        if (peerValid && !fromPeer) { pkBegin(&r, PK_BUSY); pk8(&r, 1); net_send(ip, port, r.b, r.n); return; }
        if (!peerValid) { peerValid = 1; peerIp = ip; peerPort = port; mpResetLink(); }
        strncpy(peerName, nm, 15); peerName[15] = 0; if (!peerName[0]) strncpy(peerName, "JUGADOR", 15);
        mpRx = 0;
        pkBegin(&r, PK_WELCOME); pkName(&r, myName); pk32(&r, worldHash()); pk8(&r, mpRounds); mpSend(&r);
        return;
    }
    if (!fromPeer) return;
    mpRx = 0;
    switch (type) {
    case PK_WELCOME:
        if (netMode == 2 && mpPhase == MPP_JOINING) {
            rdName(p, peerName); unsigned h = rd32(p); int r = rd8(p);
            if (h != worldHash()) { mpLeave("VERSION DEL JUEGO INCOMPATIBLE", M_MP_MENU); return; }
            mpRounds = r < 1 ? 5 : r; peerValid = 1; mpPhase = MPP_CLIENT_LOBBY;
        }
        break;
    case PK_BUSY:
        if (netMode == 2 && mpPhase == MPP_JOINING) mpLeave(rd8(p) == 2 ? "VERSION DEL JUEGO INCOMPATIBLE" : "LA PARTIDA ESTA LLENA", M_MP_MENU);
        break;
    case PK_BYE:
        if (netMode == 1 && mpPhase == MPP_MATCH) mpLeave("EL RIVAL SE FUE DE LA PARTIDA", M_MENU);
        else if (netMode == 1) { peerValid = 0; peerPort = 0; peerName[0] = 0; }
        else if (netMode == 2) mpLeave("EL ANFITRION CERRO LA PARTIDA", M_MP_MENU);
        break;
    case PK_ACK: { int id = rd16(p); if (evN > 0 && evq[0].id == id) { memmove(evq, evq + 1, (evN - 1) * sizeof(Ev)); evN--; evResend = 0; } break; }
    case PK_EV: {
        int id = rd16(p), et = rd8(p), len = rd8(p); if (len > 32) break;
        Pk a; pkBegin(&a, PK_ACK); pk16(&a, id); mpSend(&a);
        if (id == evExpect) { evExpect++; if (evExpect == 0) evExpect = 1; mpProcessEv(et, p->b + p->r, len); }
        break; }
    case PK_STATE: {
        if (mpPhase != MPP_MATCH || !bots[0].remote) break;
        int seq = rd16(p), tag = rd8(p);
        if (tag != curTag) break;
        if (havePeerSeq && (short)(seq - peerSeq) <= 0) break;
        havePeerSeq = 1; peerSeq = (unsigned short)seq;
        Bot *b = &bots[0];
        b->tx = rdf(p); b->ty = rdf(p); b->tz = rdf(p); b->tyaw = rdf(p); b->pitch = rdf(p);
        int alive = rd8(p); b->hp = (float)rd8(p); b->wep = rd8(p); int shots = rd8(p);
        int fl = rd8(p); b->crWant = fl & 1; b->aimW = (fl & 2) ? 1.0f : 0.0f; if ((fl & 4) && !b->rl) b->rlT = 0; b->rl = (fl & 4) != 0;
        if (shots != (b->shots & 255)) { b->fireEv = 1; snd_play3d(SND_PISTOL + (b->wep == 3 ? 1 : (b->wep & 3)), b->x, b->z, 1.0f); b->shots = shots; }
        if (!alive && b->alive && mode == M_PLAYING) { b->rg.deadKind = deadPick(b, rand() % 3);
#ifdef PS2_ONLINE
            rdSpawn((int)(b - bots), 60.0f, 0);
#endif
            b->alive = 0; b->dead = 0; kills++; snd_play3d(SND_DEATH0 + (rand() & 1), b->x, b->z, 1.3f); pushFeed("BAJA"); }
        break; }
    default: break;
    }
}
static void mpPump(float dt) {
    if (!netMode) { if (mpNoticeT > 0) mpNoticeT -= dt; return; }
    unsigned char buf[160]; unsigned ip; int port, n;
    while ((n = net_recv(buf, sizeof buf, &ip, &port)) > 0) { Pk p; p.n = n > 120 ? 120 : n; p.r = 4; memcpy(p.b, buf, p.n); mpHandle(&p, ip, port); if (!netMode) return; }
    mpRx += dt; mpTx += dt; mpPing += dt;
    if (mpNoticeT > 0) mpNoticeT -= dt;
    if (netMode == 2 && mpPhase == MPP_JOINING) {
        mpJoinT += dt; mpHello -= dt;
        if (mpHello <= 0) { Pk h; pkBegin(&h, PK_HELLO); pkName(&h, myName); pk32(&h, worldHash()); mpSend(&h); mpHello = 0.5f; }
        if (mpJoinT > 8.0f) mpLeave("SIN RESPUESTA - REVISA IP Y PUERTO", M_MP_MENU);
        return;
    }
    if (!peerValid) return;
    if (mpRx > 6.0f) {
        if (netMode == 1 && mpPhase == MPP_HOST_WAIT) { peerValid = 0; peerPort = 0; peerName[0] = 0; }
        else mpLeave("SE PERDIO LA CONEXION", netMode == 1 ? M_MENU : M_MP_MENU);
        return;
    }
    if (evN > 0) { evResend -= dt; if (evResend <= 0) { Ev *e = &evq[0]; Pk q; pkBegin(&q, PK_EV); pk16(&q, e->id); pk8(&q, e->type); pk8(&q, e->len); memcpy(q.b + q.n, e->d, e->len); q.n += e->len; mpSend(&q); evResend = 0.1f; } }
    if (mpPhase == MPP_MATCH) {
        if (mpTx >= 1.0f / 30.0f) {
            mpTx = 0; Pk q; pkBegin(&q, PK_STATE); pk16(&q, ++mySeq); pk8(&q, curTag);
            pkf(&q, P.x); pkf(&q, P.y); pkf(&q, P.z); pkf(&q, P.yaw); pkf(&q, P.pitch);
            pk8(&q, P.alive); pk8(&q, (int)P.hp); pk8(&q, wsel); pk8(&q, mpShots & 255); pk8(&q, (P.crWant ? 1 : 0) | (anAim > 0.5f ? 2 : 0) | (wReload > 0 ? 4 : 0)); mpSend(&q);
        }
    } else if (mpPing >= 0.4f) { mpPing = 0; Pk q; pkBegin(&q, PK_PING); mpSend(&q); }
}
static void mpUpdateRemote(float dt) {       /* suaviza al jugador remoto y calcula su animacion de caminar */
    if (!netMode || mpPhase != MPP_MATCH || nbots < 1 || !bots[0].remote) return;
    Bot *b = &bots[0]; float k = clampf(dt * 20.0f, 0, 1), ox = b->x, oz = b->z;
    b->x += (b->tx - b->x) * k; b->z += (b->tz - b->z) * k; b->py += (b->ty - b->py) * k;
    float dy = atan2f(sinf(b->tyaw - b->yaw), cosf(b->tyaw - b->yaw)); b->yaw += dy * k;
    float sp = sqrtf((b->x - ox) * (b->x - ox) + (b->z - oz) * (b->z - oz)) / (dt > 0.001f ? dt : 0.001f);
    b->mv = lerpf(b->mv, clampf(sp / 5.4f, 0, 1), clampf(dt * 10, 0, 1)); b->walk += sp * dt * 2.4f;
    if (netMode == 1 && mode == M_PLAYING && freeze <= 0 && (!P.alive || !b->alive)) mpFinishRound(P.alive ? 0 : (b->alive ? 1 : 2));
}
#endif


/* Busca un punto detras de una caja (respecto del jugador) donde el jugador no tenga linea de vision. */
/* El bot camina hacia (tx,tz) rodeando paredes: ruta por la cuadricula, atajo directo si el camino esta libre */
static void navSteer(Bot *b, float tx, float tz, float dt, float *mx, float *mz) {
    float dx = tx - b->x, dz = tz - b->z;
    b->repT -= dt;
    if (walkClear(b->x, b->z, tx, tz)) { float L = sqrtf(dx * dx + dz * dz) + 1e-4f; *mx = dx / L; *mz = dz / L; b->pn = 0; return; }
    float gd = (tx - b->pgx) * (tx - b->pgx) + (tz - b->pgz) * (tz - b->pgz);
    if (b->pn <= 0 || b->pi >= b->pn || gd > 4.0f || b->repT <= 0) {
        b->pn = navPath(b->x, b->z, tx, tz, b->path, 48); b->pi = 0; b->pgx = tx; b->pgz = tz; b->repT = 2.5f;
        if (b->pn <= 0) { float L = sqrtf(dx * dx + dz * dz) + 1e-4f; *mx = dx / L; *mz = dz / L; return; }
    }
    for (;;) {   /* salta al waypoint mas lejano con linea libre */
        float wx, wz; int adv = 0;
        for (int k = b->pn - 1; k > b->pi; k--) { navCenter(b->path[k], &wx, &wz); if (walkClear(b->x, b->z, wx, wz)) { b->pi = k; adv = 1; break; } }
        navCenter(b->path[b->pi], &wx, &wz);
        if ((wx - b->x) * (wx - b->x) + (wz - b->z) * (wz - b->z) < 0.5f && b->pi < b->pn - 1) { b->pi++; continue; }
        (void)adv; float ddx = wx - b->x, ddz = wz - b->z, L = sqrtf(ddx * ddx + ddz * ddz) + 1e-4f; *mx = ddx / L; *mz = ddz / L; break;
    }
}
static int botFindCover(const Bot *b, float *ox, float *oz) {
    float eye[3] = { P.x, P.y + 1.4f - 0.4f * P.cr, P.z }, best = 1e9f; int found = 0;
    for (int i = 0; i < nbox; i++) {
        const Box *bx = &boxes[i];
        if (bx->top < 0.9f || bx->top > 3.6f) continue;
        float cx = (bx->x0 + bx->x1) * 0.5f, cz = (bx->z0 + bx->z1) * 0.5f;
        float bd = sqrtf((cx - b->x) * (cx - b->x) + (cz - b->z) * (cz - b->z)); if (bd > 20) continue;
        float dx = cx - P.x, dz = cz - P.z, L = sqrtf(dx * dx + dz * dz) + 1e-4f; dx /= L; dz /= L;
        float ext = fabsf(dx) * (bx->x1 - bx->x0) * 0.5f + fabsf(dz) * (bx->z1 - bx->z0) * 0.5f;
        float px = clampf(cx + dx * (ext + 0.9f), -HALF + 1, HALF - 1), pz = clampf(cz + dz * (ext + 0.9f), -HALF + 1, HALF - 1);
        if (blockedAt(px, pz, 0, 0.45f) || !navOk(px, pz)) continue;
        float c[3] = { px, 0.9f, pz };
        if (hasLOS(eye, c)) continue;
        float pd = sqrtf((px - P.x) * (px - P.x) + (pz - P.z) * (pz - P.z));
        float score = sqrtf((px - b->x) * (px - b->x) + (pz - b->z) * (pz - b->z)) + (pd < 6 ? 25.0f : 0) + rndf(0, 2);
        if (score < best) { best = score; *ox = px; *oz = pz; found = 1; }
    }
    return found;
}
static void updateBots(float dt, int active) {
    int di = S.diff; float ex = P.x, ey = P.y + 1.37f - 0.45f * P.cr, ez = P.z;
    for (int i = 0; i < nbots; i++) {
        Bot *b = &bots[i];
        if (!b->alive) { b->dead += dt; continue; }
        if (b->remote) continue;                                  /* el jugador remoto lo mueve la red */
        if (b->dummy) { b->state = BS_PATROL; b->crWant = 0; if (b->hitT > 0) b->hitT -= dt; continue; }   /* modo secreto: se quedan quietos */
        if (b->hitT > 0) b->hitT -= dt;
        if (b->coverT > 0) b->coverT -= dt;
        float dx = P.x - b->x, dz = P.z - b->z, dist = sqrtf(dx * dx + dz * dz);
        float toYaw = atan2f(-dx, -dz), dyaw = toYaw - b->yaw;
        dyaw = atan2f(sinf(dyaw), cosf(dyaw));
        float from[3] = { b->x, 1.55f, b->z }, to[3] = { ex, ey, ez };
        int sees = active && P.alive && dist < 48 && fabsf(dyaw) < 1.7f && hasLOS(from, to);
        if (!sees && active && P.alive && dist < 20 * P.noise) { b->lkx = P.x; b->lkz = P.z; if (b->state == BS_PATROL) b->state = BS_SEARCH; }
        if (sees) {
            b->lkx = P.x; b->lkz = P.z; b->lost = 0;
            if (b->state == BS_PATROL || b->state == BS_SEARCH) { b->state = BS_ALERT; b->react = DIFF_REACT[di] * rndf(0.8f, 1.3f); }
        } else if (b->state == BS_ENGAGE || b->state == BS_ALERT) { b->lost += dt; if (b->lost > 2) { b->state = BS_SEARCH; b->lost = 0; } }
        if (!active || !P.alive) { b->state = BS_PATROL; b->crWant = 0; }
        float mx = 0, mz = 0, speed = 0, face = 1e9f;
        if (b->state == BS_PATROL) {
            float wx = b->wx - b->x, wz = b->wz - b->z, L = sqrtf(wx * wx + wz * wz);
            b->timer += dt;
            if (L < 1.2f || b->timer > 9) { if (rand() % 5 < 2) { float cx, cz; navCenter(navSnap(P.x + rndf(-8, 8), P.z + rndf(-8, 8)), &cx, &cz); b->wx = cx; b->wz = cz; } else freeSpot(-HALF + 2, HALF - 2, &b->wx, &b->wz); b->timer = 0; }
            if (L < 1e-3f) L = 1; navSteer(b, b->wx, b->wz, dt, &mx, &mz); speed = DIFF_SPEED[di] * 0.55f; face = atan2f(-mx, -mz);
        } else if (b->state == BS_SEARCH) {
            float wx = b->lkx - b->x, wz = b->lkz - b->z, L = sqrtf(wx * wx + wz * wz);
            if (L < 1.5f) { b->timer += dt; if (b->timer > 1.2f) { b->state = BS_PATROL; b->timer = 0; freeSpot(-HALF + 2, HALF - 2, &b->wx, &b->wz); } }
            else { navSteer(b, b->lkx, b->lkz, dt, &mx, &mz); speed = DIFF_SPEED[di] * 0.8f; face = atan2f(-mx, -mz); }
        } else if (b->state == BS_ALERT) {
            face = toYaw; b->react -= dt;
            if (b->react <= 0 && sees) { b->state = BS_ENGAGE; b->fireCD = rndf(0.1f, 0.3f); b->burst = 3 + rand() % 3; }
        } else if (b->state == BS_COVER) {          /* corre hacia la cobertura */
            float wx = b->coverX - b->x, wz = b->coverZ - b->z, L = sqrtf(wx * wx + wz * wz);
            b->timer += dt; b->crWant = 0;
            if (L < 0.6f || b->timer > 7) { b->state = BS_HIDE; b->timer = rndf(1.2f, 2.6f); }
            else { navSteer(b, b->coverX, b->coverZ, dt, &mx, &mz); speed = DIFF_SPEED[di] * 1.25f; face = atan2f(-mx, -mz); }
        } else if (b->state == BS_HIDE) {           /* agachado detras de la cobertura */
            face = toYaw; b->crWant = 1; b->timer -= dt;
            if (b->timer <= 0 || (b->hitT > 0 && b->timer < 1.0f)) { b->state = BS_PEEK; b->peekT = rndf(1.6f, 3.0f); b->burst = 2 + rand() % 3; b->cov = rand() % 2; b->fireCD = 0.2f; }
        } else if (b->state == BS_PEEK) {           /* asoma: apunta y dispara agachado o de pie */
            face = toYaw; b->crWant = b->cov; b->peekT -= dt; b->fireCD -= dt;
            if (sees && b->fireCD <= 0 && fabsf(dyaw) < 0.5f) { b->fireCD = DIFF_FIRE[di] * rndf(0.9f, 1.3f); botShoot(b, dist); b->fireEv = 1; if (--b->burst <= 0) b->peekT = 0; }
            if (b->peekT <= 0) { if (b->hp < DIFF_HP[di] * 0.5f && rand() % 2) { b->state = BS_HIDE; b->timer = rndf(1.5f, 3.0f); } else { b->state = BS_ENGAGE; b->crWant = 0; } }
        } else {
            face = toYaw; b->strafeT -= dt;
            if (b->strafeT <= 0) { b->strafe = -b->strafe; b->strafeT = rndf(0.7f, 1.6f); b->crWant = (rand() % 100) < 25; }
            if (b->coverT <= 0 && ((b->hitT > 0 && rand() % 100 < 4) || b->hp < DIFF_HP[di] * 0.45f || rand() % 1000 < 4)) {
                float cx, cz; b->coverT = rndf(5, 9);
                if (botFindCover(b, &cx, &cz)) { b->coverX = cx; b->coverZ = cz; b->state = BS_COVER; b->timer = 0; b->crWant = 0; }
            }
            float rx = cosf(b->yaw), rz = -sinf(b->yaw), nx = dx / (dist + 1e-4f), nz = dz / (dist + 1e-4f);
            float adv = dist > 16 ? 1 : (dist < 6 ? -0.8f : 0);
            mx = rx * b->strafe * 0.8f + nx * adv; mz = rz * b->strafe * 0.8f + nz * adv; speed = DIFF_SPEED[di] * (b->crWant ? 0.35f : 0.7f);
            b->fireCD -= dt; if (b->pause > 0) b->pause -= dt;
            if (sees && b->fireCD <= 0 && b->pause <= 0 && fabsf(dyaw) < 0.5f) {
                b->fireCD = DIFF_FIRE[di] * rndf(0.8f, 1.2f);
                if (--b->burst <= 0) { b->pause = rndf(0.5f, 1.2f); b->burst = 3 + rand() % 3; }
                botShoot(b, dist); b->fireEv = 1;
            }
        }
        if (face < 1e8f) { float d2 = atan2f(sinf(face - b->yaw), cosf(face - b->yaw)); b->yaw += clampf(d2, -dt * 6, dt * 6); }
        float ml = sqrtf(mx * mx + mz * mz);
        if (ml > 0.01f && speed > 0) {
            int hit = 0; moveBody(&b->x, &b->z, 0, mx / ml * speed * dt, mz / ml * speed * dt, 0.38f, &hit);
            b->step += speed * dt;
            if (b->step > 2.0f) { b->step = 0; snd_play3d(SND_STEP, b->x, b->z, 0.8f); }
            if (hit) { b->strafe = -b->strafe; b->repT = 0; b->stuck += dt; if (b->state == BS_PATROL && b->stuck > 1.5f) { freeSpot(-HALF + 2, HALF - 2, &b->wx, &b->wz); b->stuck = 0; b->pn = 0; } } else b->stuck = 0;
        }
        b->x = clampf(b->x, -HALF + .5f, HALF - .5f); b->z = clampf(b->z, -HALF + .5f, HALF - .5f);
        { int mvg = (ml > 0.01f && speed > 0); if (mvg) b->walk += speed * dt * 2.6f; b->mv = mvg ? fminf(1.0f, b->mv + dt * 8) : fmaxf(0.0f, b->mv - dt * 8); }
    }
}

static void updatePlayer(const Pad *pd, float dt, int canAct) {
    float ix = canAct ? pd->lx : 0, iz = canAct ? -pd->ly : 0;
    float il = sqrtf(ix * ix + iz * iz); if (il > 1) { ix /= il; iz /= il; }
#ifdef PS2_ONLINE
    /* Controles tipo shooter moderno: L3 = correr, R3 = caminar en silencio, O = agacharse/levantarse, L2 = poner placa, L1 = apuntar */
    int walk = (pd->down & B_R3) != 0;
    if (canAct && P.alive && (pd->pressed & (B_CIRCLE | B_SELECT))) P.crWant = !P.crWant;
#else
    int walk = (pd->down & B_CIRCLE) != 0;
    if (canAct && P.alive && (pd->pressed & (B_L3 | B_SELECT))) P.crWant = !P.crWant;      /* agacharse / levantarse */
#endif
    if (!P.alive) P.crWant = 0;
    P.cr = lerpf(P.cr, P.crWant ? 1.0f : 0.0f, clampf(dt * 9, 0, 1));
    float sp = (walk ? 2.6f : 5.4f) * lerpf(1.0f, 0.5f, P.cr);
#ifdef PS2_ONLINE
    {
        /* L3 (clic): empieza a correr y sigue corriendo mientras empujes la palanca hacia delante; apuntar, disparar, agacharse o soltar la palanca lo corta */
        if (canAct && P.alive && (pd->pressed & B_L3)) { bfPlateSt = 0; bfPlateU = 0; bfSprintTog = 1; }      /* tambien cancela la placa que estabas poniendo */
        if (!canAct || !P.alive || iz <= 0.35f || (pd->down & (B_L1 | B_R1)) || P.crWant || walk) bfSprintTog = 0;
        int want = bfSprintTog && !bfPlateSt && P.cr < 0.4f && P.ground;
        if (bfStam <= 0.0f) bfStamLock = 1; else if (bfStam > 0.3f) bfStamLock = 0;
        bfSprint = want && !bfStamLock;
        if (!bfSprint) bfSprintTog = 0;
        bfStam = clampf(bfStam + (bfSprint ? -dt / 4.5f : dt / 6.0f), 0, 1);
        int aimWant = canAct && P.alive && (pd->down & B_L1) && !bfSprint && wReload <= 0 && wSwap <= 0 && !bfNadePend && !bfPlateSt;
        bfAds = lerpf(bfAds, aimWant ? 1.0f : 0.0f, clampf(dt * 11, 0, 1));
        bfSprA = lerpf(bfSprA, bfSprint ? 1.0f : 0.0f, clampf(dt * 9, 0, 1));
        bfBloom = fmaxf(0, bfBloom - dt * 2.4f);
        if (bfSprint) sp = 7.6f; sp *= lerpf(1.0f, 0.55f, bfAds);
        if (bfPlateSt) sp *= 0.6f;              /* mientras te pones la placa te mueves mas despacio */
        bfShake = fmaxf(0, bfShake - dt * 2.0f);
        if (bfNadeT > 0) bfNadeT -= dt;
    }
#endif
    float fx = -sinf(P.yaw), fz = -cosf(P.yaw), rx = cosf(P.yaw), rz = -sinf(P.yaw);
    float tvx = (fx * iz + rx * ix) * sp, tvz = (fz * iz + rz * ix) * sp;
    float k = (P.ground ? 14.0f : 2.5f) * dt; if (k > 1) k = 1;
    P.vx = lerpf(P.vx, tvx, k); P.vz = lerpf(P.vz, tvz, k);
    if (canAct && P.alive && (pd->pressed & B_CROSS) && P.ground) { if (P.crWant) P.crWant = 0; else { P.vy = 6.6f; P.ground = 0; } }
    moveBody(&P.x, &P.z, P.y, P.vx * dt, P.vz * dt, 0.38f, NULL);
    P.vy -= 20 * dt; P.y += P.vy * dt;
    float g = groundAt(P.x, P.z, P.y, 0.38f);
    if (P.y <= g) { P.y = g; P.vy = 0; P.ground = 1; } else if (P.y > g + 0.03f) P.ground = 0;
    P.x = clampf(P.x, -HALF + .4f, HALF - .4f); P.z = clampf(P.z, -HALF + .4f, HALF - .4f);
    float spd = sqrtf(P.vx * P.vx + P.vz * P.vz);
    static float pstep = 0;
    if (P.ground && spd > 1.2f) {
        P.bob += spd * dt * 1.7f;
        if (!walk && P.cr < 0.5f) { P.noise = fmaxf(P.noise, 0.6f); if (canAct && P.alive) { pstep += spd * dt; if (pstep > 2.4f) { pstep = 0; snd_play(SND_STEP, 0.45f); } } }
    }
    P.noise = fmaxf(0, P.noise - dt * 1.5f);
    if (P.alive && canAct) {
        float sens = 0.4f + S.sens * 0.28f;
#ifdef PS2_ONLINE
        sens *= 1 - 0.45f * bfAds;              /* al apuntar la mira se mueve mas lento y preciso */
#endif
        float lx = pd->rx * fabsf(pd->rx) * 0.5f + pd->rx * 0.5f, ly = pd->ry * fabsf(pd->ry) * 0.5f + pd->ry * 0.5f;
        P.yaw -= lx * sens * 2.6f * dt;
        P.pitch -= ly * sens * 1.8f * dt * (S.invertY ? -1 : 1);
    }
    P.pitch = clampf(P.pitch, -1.45f, 1.45f);
}

static void updateWeapon(const Pad *pd, float dt, int canAct) {
    if (wCD > 0) wCD -= dt;
    if (wSwap > 0) wSwap -= dt;
    if (wReload > 0 && wsel != 2) {   /* clics de la recarga del fusil: cargador fuera, cargador dentro, cerrojo */
        float u0 = 1 - wReload / W[wsel].reload, u1 = 1 - (wReload - dt) / W[wsel].reload;
        if (u0 < 0.30f && u1 >= 0.30f) snd_play(SND_TICK, 0.7f);
        if (u0 < 0.72f && u1 >= 0.72f) snd_play(SND_TICK, 0.8f);
        if (u0 < 0.84f && u1 >= 0.84f) snd_play(SND_EMPTY, 0.5f);
    }
    if (wReload > 0) { wReload -= dt; if (wReload <= 0) { wReload = 0; int n = W[wsel].mag - ammoMag[wsel]; if (n > ammoRes[wsel]) n = ammoRes[wsel]; ammoMag[wsel] += n; ammoRes[wsel] -= n; } }
    wKick = fmaxf(0, wKick - dt * 9); wFlash = fmaxf(0, wFlash - dt);
#ifdef PS2_ONLINE
    if ((!canAct || !P.alive) && bfNadeSt) { bfNadeSt = 0; bfNadePend = 0; bfNadeU = 0; }
    if (!canAct || !P.alive) { bfPlateSt = 0; bfPlateU = 0; }
#endif
    if (!canAct || !P.alive) return;
#ifdef PS2_ONLINE
    /* R1 dispara, R2 granada, L1 apunta, L2 pone placa, L3 corre, Cuadrado recarga, Triangulo/cruceta cambian de arma */
    /* Placa de armadura: L2 pone una placa (si sigues apretando L2 pone la siguiente). Disparar, apuntar, recargar, granada o cambiar de arma la cancela. */
    if (bfPlateSt && (pd->pressed & (B_R1 | B_L1 | B_SQUARE | B_R2 | B_TRIANGLE | B_LEFT | B_RIGHT))) { bfPlateSt = 0; bfPlateU = 0; }
    if (!bfPlateSt) {
        if ((pd->pressed & B_L2) && bfPlates > 0 && bfArmor < PLATE_HP * PLATE_N - 0.01f && !bfNadePend && !bfSprint && wReload <= 0 && wSwap <= 0) { bfPlateSt = 1; bfPlateU = 0; snd_play(SND_PLATING, 0.9f); }
    } else {
        bfPlateU = fminf(1.0f, bfPlateU + dt / PLATE_T);
        if (bfPlateU >= 1.0f) {
            bfArmor = fminf(PLATE_HP * PLATE_N, bfArmor + PLATE_HP); bfPlates--;
            if ((pd->down & B_L2) && bfPlates > 0 && bfArmor < PLATE_HP * PLATE_N - 0.01f) { bfPlateU = 0; snd_play(SND_PLATING, 0.9f); }   /* L2 mantenido: siguiente placa */
            else { bfPlateSt = 0; bfPlateU = 0; }
        }
    }
    int trig = (pd->down & B_R1) != 0 && !bfSprint && !bfPlateSt;
    if (bfSprint || bfNadePend || bfPlateSt) wCD = fmaxf(wCD, 0.12f);
    if (!(pd->down & B_R1)) firedLatch = 0;
    if (trig && (W[wsel].autof || !firedLatch)) fireWeapon();
    if (!bfNadePend) {
        if (pd->pressed & B_SQUARE) startReload();
        if (pd->pressed & (B_TRIANGLE | B_RIGHT)) switchTo(wsel + 1);
        if (pd->pressed & B_LEFT) switchTo(wsel - 1);
    }
    /* Granada: R2 la saca y le quita el seguro; mientras sigas apretando R2 la mantienes lista; al soltar R2 la lanzas.
     * Si sueltas antes de quitar el seguro, la guarda. */
    if (bfNadeSt == 0) {
        if ((pd->pressed & B_R2) && bfNades > 0 && wSwap <= 0 && wReload <= 0 && !netMode && !bfSprint) { bfNadeSt = 1; bfNadeU = 0; bfNadePend = 1; }
    } else if (bfNadeSt == 1) {
        float u0 = bfNadeU;
        if (pd->down & B_R2) bfNadeU = fminf(1.0f, bfNadeU + dt / 0.95f);
        else if (bfNadeU >= 0.62f) { bfNadeSt = 2; bfThrowT = 0; }                       /* ya sin seguro: lanzar */
        else { bfNadeU -= dt / 0.45f; if (bfNadeU <= 0) { bfNadeU = 0; bfNadeSt = 0; bfNadePend = 0; } }   /* aun con seguro: guardarla */
        if (u0 < 0.62f && bfNadeU >= 0.62f) snd_play(SND_PIN, 0.9f);                      /* clic del seguro al sacarlo */
    } else {
        float t0 = bfThrowT; bfThrowT += dt;
        if (t0 < 0.14f && bfThrowT >= 0.14f) bfThrow();                                    /* la granada sale a mitad del brazo */
        if (bfThrowT >= 0.42f) { bfNadeSt = 0; bfNadePend = 0; bfNadeU = 0; wCD = fmaxf(wCD, 0.12f); }
    }
#else
    int trig = (pd->down & (B_R2 | B_R1)) != 0;
    if (!trig) firedLatch = 0;
    if (trig && (W[wsel].autof || !firedLatch)) fireWeapon();
    if (pd->pressed & B_SQUARE) startReload();
    if (pd->pressed & B_TRIANGLE) switchTo(wsel + 1);
    if (pd->pressed & (B_L1 | B_L2)) switchTo(wsel - 1);
#endif
}

/* ---------- Dibujo de escena y HUD ---------- */
static void setCamera(float x, float y, float z, float yaw, float pitch) {
    camX = x; camY = y; camZ = z; camYaw = yaw; camPitch = pitch;
    camF = (SW * 0.5f) / tanf((S.fov + camFovAdd) * 0.5f * PI / 180.0f);
}
static void botPart(const Bot *b, float lx, float ly, float lz, float hw, float hh, float hd, u32c c) {
    float s = sinf(b->yaw), co = cosf(b->yaw);
    obox(b->x + lx * co + lz * s, ly, b->z - lx * s + lz * co, hw, hh, hd, b->yaw, c);
}
/* Dibuja un soldado con piel por vertice. weap: 0 pistola (oculta el fusil y dibuja una pistola), 2 escopeta (madera), otros fusil. */
/* ---------- Personajes voxel (reemplazan a los modelos generados: model_soldado_tactico, etc.) ----------
 * Cada personaje es un conjunto de cajas rigidas, una por hueso del esqueleto (piel de un solo hueso, sin mezcla).
 * 25 cajas -> 200 vertices y 131 caras (las bases ocultas no se generan), con culling por cara. Los tres skins comparten
 * la geometria y solo cambian la paleta. Se arma una vez en game_init() y el esqueleto/ragdoll no cambian. */
enum { VC_HELM, VC_FACE, VC_EYE, VC_UNI, VC_SLV, VC_VEST, VC_PACK, VC_PADS, VC_BOOT, VC_GLOVE, VC_GUN, VC_GUN2, VC_N };
typedef struct { signed char bone; float x0, x1, y0, y1, z0, z1; unsigned char col, part, bottom; } VoxBox;   /* part 5 = arma; bottom: se dibuja la cara de abajo */
static const VoxBox VOXB[] = {
    { BN_PELVIS, -.21f, .21f, .70f, .86f, -.12f, .12f, VC_UNI, 0, 1 },                 /* cadera */
    { BN_CHEST, -.22f, .22f, .86f, 1.30f, -.13f, .13f, VC_UNI, 0, 0 },                 /* torso */
    { BN_CHEST, -.19f, .19f, .98f, 1.27f, -.17f, -.13f, VC_VEST, 0, 0 },               /* chaleco */
    { BN_CHEST, -.15f, .15f, .95f, 1.26f, .13f, .29f, VC_PACK, 0, 0 },                 /* mochila */
    { BN_HEAD, -.135f, .135f, 1.32f, 1.58f, -.14f, .13f, VC_FACE, 0, 1 },              /* cara */
    { BN_HEAD, -.10f, .10f, 1.47f, 1.51f, -.15f, -.14f, VC_EYE, 0, 0 },                /* sombra de los ojos */
    { BN_HEAD, -.18f, .18f, 1.52f, 1.76f, -.17f, .18f, VC_HELM, 0, 0 },                /* casco */
    { BN_ARMS, .21f, .35f, 1.14f, 1.30f, -.12f, .02f, VC_SLV, 0, 0 },                  /* hombro derecho */
    { BN_ARMS, .21f, .33f, 1.05f, 1.19f, -.42f, -.04f, VC_SLV, 0, 0 },                 /* brazo derecho */
    { BN_ARMS, .12f, .23f, 1.03f, 1.13f, -.46f, -.34f, VC_GLOVE, 0, 0 },               /* mano derecha (empunadura) */
    { BN_ARMS, -.36f, -.22f, .96f, 1.30f, -.12f, .02f, VC_SLV, 0, 0 },                 /* brazo izquierdo */
    { BN_FORE_L, -.34f, -.20f, .95f, 1.09f, -.50f, -.04f, VC_SLV, 0, 0 },              /* antebrazo izquierdo (1) */
    { BN_FORE_L, -.20f, .00f, .96f, 1.08f, -.64f, -.48f, VC_SLV, 0, 0 },               /* antebrazo izquierdo (2) */
    { BN_FORE_L, .00f, .15f, .97f, 1.09f, -.72f, -.52f, VC_GLOVE, 0, 0 },              /* mano izquierda (guardamano) */
    { BN_THIGH_L, -.29f, -.09f, .47f, .78f, -.10f, .10f, VC_UNI, 0, 1 },
    { BN_THIGH_R, .09f, .29f, .47f, .78f, -.10f, .10f, VC_UNI, 0, 1 },
    { BN_SHIN_L, -.28f, -.10f, .22f, .47f, -.09f, .09f, VC_UNI, 0, 0 },
    { BN_SHIN_R, .10f, .28f, .22f, .47f, -.09f, .09f, VC_UNI, 0, 0 },
    { BN_SHIN_L, -.26f, -.12f, .38f, .52f, -.12f, -.09f, VC_PADS, 0, 0 },              /* rodilleras */
    { BN_SHIN_R, .12f, .26f, .38f, .52f, -.12f, -.09f, VC_PADS, 0, 0 },
    { BN_FOOT_L, -.28f, -.10f, .00f, .22f, -.21f, .10f, VC_BOOT, 0, 1 },
    { BN_FOOT_R, .10f, .28f, .00f, .22f, -.21f, .10f, VC_BOOT, 0, 1 },
    { BN_WEAP, .145f, .195f, 1.10f, 1.17f, -.70f, -.30f, VC_GUN, 5, 0 },               /* fusil: cuerpo */
    { BN_WEAP, .152f, .188f, 1.115f, 1.155f, -1.08f, -.70f, VC_GUN2, 5, 0 },           /* fusil: guardamano y canon */
    { BN_WEAP, .155f, .185f, 1.02f, 1.10f, -.60f, -.52f, VC_GUN2, 5, 0 }               /* fusil: cargador */
};
#define VOX_NB ((int)(sizeof VOXB / sizeof VOXB[0]))
static const unsigned char VOX_PAL[3][VC_N][3] = {
    /* 0 soldado:       casco          cara           ojos          uniforme       mangas         chaleco        mochila        rodilleras    botas           guantes        fusil          fusil2 */
    { { 112, 124, 62 }, { 222, 178, 134 }, { 46, 34, 30 }, { 88, 100, 58 }, { 118, 128, 66 }, { 178, 150, 86 }, { 96, 80, 60 }, { 50, 58, 36 }, { 128, 82, 44 }, { 222, 178, 134 }, { 84, 88, 96 }, { 52, 54, 60 } },
    /* 1 superviviente */
    { { 88, 62, 42 }, { 222, 178, 134 }, { 46, 34, 30 }, { 66, 88, 124 }, { 160, 64, 52 }, { 130, 118, 104 }, { 176, 126, 52 }, { 48, 66, 96 }, { 62, 52, 46 }, { 222, 178, 134 }, { 84, 88, 96 }, { 52, 54, 60 } },
    /* 2 zombi */
    { { 38, 42, 34 }, { 116, 156, 102 }, { 210, 44, 34 }, { 86, 82, 72 }, { 100, 92, 80 }, { 120, 64, 58 }, { 64, 60, 54 }, { 70, 66, 58 }, { 52, 48, 42 }, { 116, 156, 102 }, { 84, 88, 96 }, { 52, 54, 60 } }
};
static Model voxM[3];
static float vxV[3][VOX_NB * 24];
static unsigned short vxF[3][VOX_NB * 24];
static unsigned char vxC[3][VOX_NB * 18], vxP[3][VOX_NB * 6], vxB[3][VOX_NB * 24];
static void voxBuild(void) {
    for (int k = 0; k < 3; k++) {
        int nv = 0, nf = 0;
        for (int b = 0; b < VOX_NB; b++) {
            const VoxBox *x = &VOXB[b]; int base = nv;
            for (int i = 0; i < 8; i++, nv++) {
                vxV[k][nv * 3] = (i & 1) ? x->x1 : x->x0; vxV[k][nv * 3 + 1] = (i & 2) ? x->y1 : x->y0; vxV[k][nv * 3 + 2] = (i & 4) ? x->z1 : x->z0;
                vxB[k][nv * 3] = vxB[k][nv * 3 + 1] = (unsigned char)x->bone; vxB[k][nv * 3 + 2] = 255;
            }
            for (int f = 0; f < 6; f++) {
                if (f == 2 && !x->bottom) continue;                         /* cara -y oculta: no se genera */
                /* drawRig espera el orden inverso al de FACE para que la normal salga hacia afuera */
                vxF[k][nf * 4] = (unsigned short)(base + FACE[f][0]); vxF[k][nf * 4 + 1] = (unsigned short)(base + FACE[f][3]);
                vxF[k][nf * 4 + 2] = (unsigned short)(base + FACE[f][2]); vxF[k][nf * 4 + 3] = (unsigned short)(base + FACE[f][1]);
                for (int j = 0; j < 3; j++) vxC[k][nf * 3 + j] = VOX_PAL[k][x->col][j];
                vxP[k][nf] = x->part; nf++;
            }
        }
        voxM[k].nv = nv; voxM[k].nf = nf; voxM[k].v = vxV[k]; voxM[k].f = vxF[k]; voxM[k].c = vxC[k]; voxM[k].p = vxP[k]; voxM[k].vb = vxB[k];
    }
}
/* ---------- Pistola HK VP70 de baja poligonizacion ----------
 * Generada a partir de low-poly_hk_vp70m.glb (5555 triangulos, 13 piezas, con culata): se quito la culata y se reconstruyo
 * el marco extruyendo su silueta lateral (guardamonte incluido), la corredera como prisma con la nariz achaflanada,
 * mas gatillo, miras, estrias y ventana como cuadrilateros. Todo en colores planos, sin texturas.
 *   VPM: 218 triangulos (primera persona; la placa del cargador va al final y se oculta al recargar)
 *   VPL:  72 triangulos (soldados a menos de 18 m; mas lejos se usan 3 cajas)
 * Espacio: x derecha, y arriba, z adelante (boca en +z), el mismo que usa wbox() en primera persona.
 * Colores de VPM_C: 0 corredera, 1 marco, 2 gatillo/placa, 3 estrias, 4 miras. VPL_C: 0 corredera, 1 marco. */
#define VPM_NV 160
#define VPM_NT 218
#define VPM_NTB 206    /* triangulos del cuerpo; el resto es la placa del cargador */
static const float VPM_V[VPM_NV * 3] = {   /* x derecha, y arriba, z adelante (boca en +z), mismo espacio que wbox */
    -0.0187f, -0.0051f, -0.1024f, -0.0187f, 0.0000f, -0.0982f, -0.0187f, -0.0059f, -0.0939f,
    -0.0187f, -0.0148f, -0.0502f, -0.0187f, -0.0514f, -0.0536f, -0.0187f, -0.0930f, -0.0757f,
    -0.0187f, -0.1615f, -0.0973f, -0.0187f, -0.1598f, -0.0115f, -0.0187f, -0.0612f, 0.0191f,
    -0.0187f, -0.0586f, 0.1237f, -0.0187f, -0.0471f, 0.1360f, -0.0187f, -0.0123f, 0.1454f,
    -0.0187f, 0.0034f, 0.1891f, -0.0187f, 0.0166f, 0.1896f, -0.0187f, 0.0166f, -0.1029f,
    -0.0187f, -0.0174f, 0.0527f, -0.0187f, -0.0093f, 0.0625f, -0.0187f, -0.0093f, 0.1152f,
    -0.0187f, -0.0191f, 0.1292f, -0.0187f, -0.0344f, 0.1335f, -0.0187f, -0.0463f, 0.1301f,
    -0.0187f, -0.0561f, 0.1160f, -0.0187f, -0.0518f, 0.0956f, -0.0187f, -0.0561f, 0.0922f,
    -0.0187f, -0.0561f, 0.0667f, -0.0187f, -0.0454f, 0.0527f, -0.0187f, -0.0293f, 0.0493f,
    0.0187f, -0.0051f, -0.1024f, 0.0187f, 0.0000f, -0.0982f, 0.0187f, -0.0059f, -0.0939f,
    0.0187f, -0.0148f, -0.0502f, 0.0187f, -0.0514f, -0.0536f, 0.0187f, -0.0930f, -0.0757f,
    0.0187f, -0.1615f, -0.0973f, 0.0187f, -0.1598f, -0.0115f, 0.0187f, -0.0612f, 0.0191f,
    0.0187f, -0.0586f, 0.1237f, 0.0187f, -0.0471f, 0.1360f, 0.0187f, -0.0123f, 0.1454f,
    0.0187f, 0.0034f, 0.1891f, 0.0187f, 0.0166f, 0.1896f, 0.0187f, 0.0166f, -0.1029f,
    0.0187f, -0.0174f, 0.0527f, 0.0187f, -0.0093f, 0.0625f, 0.0187f, -0.0093f, 0.1152f,
    0.0187f, -0.0191f, 0.1292f, 0.0187f, -0.0344f, 0.1335f, 0.0187f, -0.0463f, 0.1301f,
    0.0187f, -0.0561f, 0.1160f, 0.0187f, -0.0518f, 0.0956f, 0.0187f, -0.0561f, 0.0922f,
    0.0187f, -0.0561f, 0.0667f, 0.0187f, -0.0454f, 0.0527f, 0.0187f, -0.0293f, 0.0493f,
    -0.0208f, 0.0060f, -0.1020f, 0.0208f, 0.0060f, -0.1020f, 0.0208f, 0.0502f, -0.1020f,
    0.0140f, 0.0570f, -0.1020f, -0.0140f, 0.0570f, -0.1020f, -0.0208f, 0.0502f, -0.1020f,
    -0.0208f, 0.0060f, 0.1955f, 0.0208f, 0.0060f, 0.1955f, 0.0208f, 0.0502f, 0.1955f,
    0.0140f, 0.0570f, 0.1955f, -0.0140f, 0.0570f, 0.1955f, -0.0208f, 0.0502f, 0.1955f,
    -0.0170f, 0.0272f, 0.2253f, 0.0170f, 0.0272f, 0.2253f, 0.0170f, 0.0519f, 0.2253f,
    0.0119f, 0.0570f, 0.2253f, -0.0119f, 0.0570f, 0.2253f, -0.0170f, 0.0519f, 0.2253f,
    -0.0060f, -0.0569f, 0.0429f, -0.0060f, -0.0539f, 0.1029f, -0.0060f, -0.0395f, 0.0697f,
    -0.0060f, -0.0225f, 0.0697f, -0.0060f, -0.0085f, 0.0897f, -0.0060f, -0.0089f, 0.0425f,
    0.0060f, -0.0569f, 0.0429f, 0.0060f, -0.0539f, 0.1029f, 0.0060f, -0.0395f, 0.0697f,
    0.0060f, -0.0225f, 0.0697f, 0.0060f, -0.0085f, 0.0897f, 0.0060f, -0.0089f, 0.0425f,
    -0.0102f, 0.0561f, -0.0744f, -0.0102f, 0.0561f, -0.0531f, -0.0102f, 0.0646f, -0.0744f,
    -0.0102f, 0.0646f, -0.0531f, 0.0102f, 0.0561f, -0.0744f, 0.0102f, 0.0561f, -0.0531f,
    0.0102f, 0.0646f, -0.0744f, 0.0102f, 0.0646f, -0.0531f, -0.0030f, 0.0561f, 0.1934f,
    -0.0030f, 0.0561f, 0.2146f, -0.0030f, 0.0646f, 0.1934f, -0.0030f, 0.0646f, 0.2146f,
    0.0030f, 0.0561f, 0.1934f, 0.0030f, 0.0561f, 0.2146f, 0.0030f, 0.0646f, 0.1934f,
    0.0030f, 0.0646f, 0.2146f, -0.0162f, -0.1683f, -0.0893f, -0.0162f, -0.1683f, 0.0043f,
    -0.0162f, -0.1606f, -0.0893f, -0.0162f, -0.1606f, 0.0043f, 0.0162f, -0.1683f, -0.0893f,
    0.0162f, -0.1683f, 0.0043f, 0.0162f, -0.1606f, -0.0893f, 0.0162f, -0.1606f, 0.0043f,
    -0.0215f, 0.0187f, -0.0871f, -0.0215f, 0.0187f, -0.0799f, -0.0215f, 0.0493f, -0.0799f,
    -0.0215f, 0.0493f, -0.0871f, -0.0215f, 0.0187f, -0.0718f, -0.0215f, 0.0187f, -0.0646f,
    -0.0215f, 0.0493f, -0.0646f, -0.0215f, 0.0493f, -0.0718f, -0.0215f, 0.0187f, -0.0565f,
    -0.0215f, 0.0187f, -0.0493f, -0.0215f, 0.0493f, -0.0493f, -0.0215f, 0.0493f, -0.0565f,
    -0.0215f, 0.0187f, -0.0412f, -0.0215f, 0.0187f, -0.0340f, -0.0215f, 0.0493f, -0.0340f,
    -0.0215f, 0.0493f, -0.0412f, -0.0215f, 0.0187f, -0.0259f, -0.0215f, 0.0187f, -0.0187f,
    -0.0215f, 0.0493f, -0.0187f, -0.0215f, 0.0493f, -0.0259f, -0.0193f, -0.0501f, 0.0404f,
    -0.0193f, -0.0501f, 0.0510f, -0.0193f, -0.0395f, 0.0510f, -0.0193f, -0.0395f, 0.0404f,
    0.0215f, 0.0187f, -0.0871f, 0.0215f, 0.0187f, -0.0799f, 0.0215f, 0.0493f, -0.0799f,
    0.0215f, 0.0493f, -0.0871f, 0.0215f, 0.0187f, -0.0718f, 0.0215f, 0.0187f, -0.0646f,
    0.0215f, 0.0493f, -0.0646f, 0.0215f, 0.0493f, -0.0718f, 0.0215f, 0.0187f, -0.0565f,
    0.0215f, 0.0187f, -0.0493f, 0.0215f, 0.0493f, -0.0493f, 0.0215f, 0.0493f, -0.0565f,
    0.0215f, 0.0187f, -0.0412f, 0.0215f, 0.0187f, -0.0340f, 0.0215f, 0.0493f, -0.0340f,
    0.0215f, 0.0493f, -0.0412f, 0.0215f, 0.0187f, -0.0259f, 0.0215f, 0.0187f, -0.0187f,
    0.0215f, 0.0493f, -0.0187f, 0.0215f, 0.0493f, -0.0259f, 0.0193f, -0.0501f, 0.0404f,
    0.0193f, -0.0501f, 0.0510f, 0.0193f, -0.0395f, 0.0510f, 0.0193f, -0.0395f, 0.0404f,
    0.0215f, 0.0209f, 0.0298f, 0.0215f, 0.0209f, 0.1148f, 0.0215f, 0.0442f, 0.1148f,
    0.0215f, 0.0442f, 0.0298f,
};
static const unsigned char VPM_T[VPM_NT * 3] = {
    41, 28, 27, 14, 0, 1, 41, 29, 28, 14, 1, 2, 41, 30, 29, 14, 2, 3, 31, 33, 32, 4, 5, 6,
    31, 34, 33, 4, 6, 7, 30, 34, 31, 3, 4, 7, 30, 35, 34, 3, 7, 8, 37, 47, 46, 10, 19, 20,
    36, 47, 37, 9, 10, 20, 36, 48, 47, 9, 20, 21, 35, 48, 36, 8, 9, 21, 48, 50, 49, 21, 22, 23,
    35, 50, 48, 8, 21, 23, 35, 51, 50, 8, 23, 24, 30, 51, 35, 3, 8, 24, 41, 51, 30, 14, 3, 24,
    41, 52, 51, 14, 24, 25, 41, 53, 52, 14, 25, 26, 41, 42, 53, 14, 26, 15, 41, 43, 42, 14, 15, 16,
    41, 44, 43, 14, 16, 17, 46, 38, 37, 19, 10, 11, 45, 38, 46, 18, 19, 11, 44, 38, 45, 17, 18, 11,
    41, 38, 44, 14, 17, 11, 41, 39, 38, 14, 11, 12, 39, 41, 40, 12, 13, 14, 0, 28, 1, 0, 27, 28,
    1, 29, 2, 1, 28, 29, 2, 30, 3, 2, 29, 30, 3, 31, 4, 3, 30, 31, 4, 32, 5, 4, 31, 32,
    5, 33, 6, 5, 32, 33, 6, 34, 7, 6, 33, 34, 7, 35, 8, 7, 34, 35, 8, 36, 9, 8, 35, 36,
    9, 37, 10, 9, 36, 37, 10, 38, 11, 10, 37, 38, 11, 39, 12, 11, 38, 39, 12, 40, 13, 12, 39, 40,
    13, 41, 14, 13, 40, 41, 14, 27, 0, 14, 41, 27, 15, 43, 16, 15, 42, 43, 16, 44, 17, 16, 43, 44,
    17, 45, 18, 17, 44, 45, 18, 46, 19, 18, 45, 46, 19, 47, 20, 19, 46, 47, 20, 48, 21, 20, 47, 48,
    21, 49, 22, 21, 48, 49, 22, 50, 23, 22, 49, 50, 23, 51, 24, 23, 50, 51, 24, 52, 25, 24, 51, 52,
    25, 53, 26, 25, 52, 53, 26, 42, 15, 26, 53, 42, 54, 55, 61, 54, 61, 60, 55, 56, 62, 55, 62, 61,
    56, 57, 63, 56, 63, 62, 57, 58, 64, 57, 64, 63, 58, 59, 65, 58, 65, 64, 59, 54, 60, 59, 60, 65,
    60, 61, 67, 60, 67, 66, 61, 62, 68, 61, 68, 67, 62, 63, 69, 62, 69, 68, 63, 64, 70, 63, 70, 69,
    64, 65, 71, 64, 71, 70, 65, 60, 66, 65, 66, 71, 54, 56, 55, 54, 57, 56, 54, 58, 57, 54, 59, 58,
    66, 67, 68, 66, 68, 69, 66, 69, 70, 66, 70, 71, 78, 80, 79, 72, 73, 74, 83, 80, 78, 77, 72, 74,
    83, 81, 80, 77, 74, 75, 81, 83, 82, 75, 76, 77, 72, 79, 73, 72, 78, 79, 73, 80, 74, 73, 79, 80,
    74, 81, 75, 74, 80, 81, 75, 82, 76, 75, 81, 82, 76, 83, 77, 76, 82, 83, 77, 78, 72, 77, 83, 78,
    84, 87, 86, 84, 85, 87, 88, 91, 89, 88, 90, 91, 86, 91, 90, 86, 87, 91, 84, 90, 88, 84, 86, 90,
    85, 91, 87, 85, 89, 91, 92, 95, 94, 92, 93, 95, 96, 99, 97, 96, 98, 99, 94, 99, 98, 94, 95, 99,
    92, 98, 96, 92, 94, 98, 93, 99, 95, 93, 97, 99, 108, 109, 110, 108, 110, 111, 112, 113, 114, 112, 114, 115,
    116, 117, 118, 116, 118, 119, 120, 121, 122, 120, 122, 123, 124, 125, 126, 124, 126, 127, 128, 129, 130, 128, 130, 131,
    132, 134, 133, 132, 135, 134, 136, 138, 137, 136, 139, 138, 140, 142, 141, 140, 143, 142, 144, 146, 145, 144, 147, 146,
    148, 150, 149, 148, 151, 150, 152, 154, 153, 152, 155, 154, 156, 158, 157, 156, 159, 158, 100, 103, 102, 100, 101, 103,
    104, 107, 105, 104, 106, 107, 100, 105, 101, 100, 104, 105, 102, 107, 106, 102, 103, 107, 100, 106, 104, 100, 102, 106,
    101, 107, 103, 101, 105, 107,
};
static const unsigned char VPM_C[VPM_NT] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
};


#define VPL_NV 38
#define VPL_NT 72
#define VPL_NTB 72    /* triangulos del cuerpo; el resto es la placa del cargador */
static const float VPL_V[VPL_NV * 3] = {   /* mismo espacio; version corta para soldados */
    -0.0187f, -0.0051f, -0.1024f, -0.0187f, -0.0148f, -0.0502f, -0.0187f, -0.1615f, -0.0973f,
    -0.0187f, -0.1598f, -0.0115f, -0.0187f, -0.0612f, 0.0191f, -0.0187f, -0.0586f, 0.1237f,
    -0.0187f, -0.0123f, 0.1454f, -0.0187f, 0.0166f, 0.1896f, -0.0187f, 0.0166f, -0.1029f,
    -0.0187f, -0.0093f, 0.0625f, -0.0187f, -0.0093f, 0.1152f, -0.0187f, -0.0344f, 0.1335f,
    -0.0187f, -0.0561f, 0.1160f, -0.0187f, -0.0561f, 0.0667f, -0.0187f, -0.0293f, 0.0493f,
    0.0187f, -0.0051f, -0.1024f, 0.0187f, -0.0148f, -0.0502f, 0.0187f, -0.1615f, -0.0973f,
    0.0187f, -0.1598f, -0.0115f, 0.0187f, -0.0612f, 0.0191f, 0.0187f, -0.0586f, 0.1237f,
    0.0187f, -0.0123f, 0.1454f, 0.0187f, 0.0166f, 0.1896f, 0.0187f, 0.0166f, -0.1029f,
    0.0187f, -0.0093f, 0.0625f, 0.0187f, -0.0093f, 0.1152f, 0.0187f, -0.0344f, 0.1335f,
    0.0187f, -0.0561f, 0.1160f, 0.0187f, -0.0561f, 0.0667f, 0.0187f, -0.0293f, 0.0493f,
    -0.0208f, 0.0060f, -0.1020f, 0.0208f, 0.0060f, -0.1020f, 0.0208f, 0.0570f, -0.1020f,
    -0.0208f, 0.0570f, -0.1020f, -0.0191f, 0.0166f, 0.2253f, 0.0191f, 0.0166f, 0.2253f,
    0.0191f, 0.0570f, 0.2253f, -0.0191f, 0.0570f, 0.2253f,
};
static const unsigned char VPL_T[VPL_NT * 3] = {
    23, 16, 15, 8, 0, 1, 16, 18, 17, 1, 2, 3, 16, 19, 18, 1, 3, 4, 20, 26, 21, 5, 6, 11,
    20, 27, 26, 5, 11, 12, 19, 27, 20, 4, 5, 12, 19, 28, 27, 4, 12, 13, 16, 28, 19, 1, 4, 13,
    23, 28, 16, 8, 1, 13, 23, 29, 28, 8, 13, 14, 23, 24, 29, 8, 14, 9, 23, 25, 24, 8, 9, 10,
    25, 21, 26, 10, 11, 6, 23, 21, 25, 8, 10, 6, 21, 23, 22, 6, 7, 8, 0, 16, 1, 0, 15, 16,
    1, 17, 2, 1, 16, 17, 2, 18, 3, 2, 17, 18, 3, 19, 4, 3, 18, 19, 4, 20, 5, 4, 19, 20,
    5, 21, 6, 5, 20, 21, 6, 22, 7, 6, 21, 22, 7, 23, 8, 7, 22, 23, 8, 15, 0, 8, 23, 15,
    9, 25, 10, 9, 24, 25, 10, 26, 11, 10, 25, 26, 11, 27, 12, 11, 26, 27, 12, 28, 13, 12, 27, 28,
    13, 29, 14, 13, 28, 29, 14, 24, 9, 14, 29, 24, 30, 31, 35, 30, 35, 34, 31, 32, 36, 31, 36, 35,
    32, 33, 37, 32, 37, 36, 33, 30, 34, 33, 34, 37, 30, 32, 31, 30, 33, 32, 34, 35, 36, 34, 36, 37,
};
static const unsigned char VPL_C[VPL_NT] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
/* soldados: el espacio del arma pasa al hueso BN_WEAP girado 180 grados en Y (rotacion propia: el sentido de los triangulos se conserva) */
static void vpDrawRig(const M34 *wm) {
    float tv[VPL_NV][3];
    for (int i = 0; i < VPL_NV; i++) {
        float lp[3] = { 0.17f - VPL_V[i * 3], 1.111f + VPL_V[i * 3 + 1], -0.395f - VPL_V[i * 3 + 2] };
        m34Xf(wm, lp, tv[i]);
    }
    const float *m = wm->m;
    float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]), sg = det < 0 ? -1.0f : 1.0f;
    static const u32c pal[2] = { RGBA(48, 50, 55, 255), RGBA(30, 31, 34, 255) };
    for (int t = 0; t < VPL_NT; t++) {
        const float *a = tv[VPL_T[t * 3]], *b = tv[VPL_T[t * 3 + 1]], *c = tv[VPL_T[t * 3 + 2]];
        float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx, nl = sqrtf(nx * nx + ny * ny + nz * nz);
        if (nl < 1e-9f) continue;
        nl = sg / nl; nx *= nl; ny *= nl; nz *= nl;                                   /* normal hacia afuera */
        float fx = (a[0] + b[0] + c[0]) * (1.0f / 3), fy = (a[1] + b[1] + c[1]) * (1.0f / 3), fz = (a[2] + b[2] + c[2]) * (1.0f / 3);
        float ex = camX - fx, ey = camY - fy, ez = camZ - fz;
        if (nx * ex + ny * ey + nz * ez <= 0) continue;
        float dist = sqrtf(ex * ex + ey * ey + ez * ez), shade = clampf(0.62f + 0.38f * (nx * -0.35f + ny * 0.8f + nz * 0.45f), 0.45f, 1.05f);
        V v[3]; const float *pp[3] = { a, b, c };
        for (int k = 0; k < 3; k++) { toView(pp[k][0], pp[k][1], pp[k][2], &v[k].x, &v[k].y, &v[k].z); fogColor(pal[VPL_C[t]], shade, dist, &v[k].r, &v[k].g, &v[k].b); }
        emitPoly(v, 3, 0);
    }
}
static const M34 *rigOverG = NULL;      /* si no es NULL, drawRig usa estas matrices de hueso (ragdoll) en vez de la animacion */
static void drawRig(const Model *m, const Rig *g, float pitch, float reloadU, int weap, float wx, float wy, float wz, float yaw, float tr, float tg, float tb, float flash) {
    if (!sphereVis(wx, wy + 0.9f, wz, 1.8f)) return;     /* fuera de la vista: no se transforma */
    static float tv[600][3];
    SkPose po; M34 G[BN_N];
    if (rigOverG) memcpy(G, rigOverG, sizeof G);
    else {
        Rig gl = *g; gl.pist = (weap == 0); rigPose(&gl, &po);
        rigAimPitch(&gl, &po, pitch); rigReload(&gl, &po, reloadU);
        rigSolve(&po, wx, wy, wz, yaw, G);
    }
    for (int i = 0; i < m->nv && i < 600; i++) {
        const float *p = &m->v[i * 3]; const unsigned char *s = &m->vb[i * 3];
        float a[3], b[3], w = s[2] / 255.0f;
        m34Xf(&G[s[0]], p, a);
        if (s[2] == 255) { tv[i][0] = a[0]; tv[i][1] = a[1]; tv[i][2] = a[2]; continue; }
        m34Xf(&G[s[1]], p, b);
        tv[i][0] = a[0] * w + b[0] * (1 - w); tv[i][1] = a[1] * w + b[1] * (1 - w); tv[i][2] = a[2] * w + b[2] * (1 - w);
    }
    if (g->dead && !rigOverG) {      /* cuerpo caido: se apoya exactamente sobre el suelo (ni flotando ni hundido) */
        float lo = 1e9f; for (int i = 0; i < m->nv && i < 600; i++) if (tv[i][1] < lo) lo = tv[i][1];
        float gnd = groundAt(wx, wz, wy + 0.3f, 0.2f), dy = gnd - lo;
        if (g->deadT > 0.15f) for (int i = 0; i < m->nv && i < 600; i++) tv[i][1] += dy;
    }
    for (int f = 0; f < m->nf; f++) {
        int part = m->p[f];
        if (part == 5 && weap == 0) continue;
        const unsigned short *qd = &m->f[f * 4];
        const float *a = tv[qd[0]], *b = tv[qd[1]], *c2 = tv[qd[2]];
        float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c2[0] - a[0], vy = c2[1] - a[1], vz = c2[2] - a[2];
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        float nl = sqrtf(nx * nx + ny * ny + nz * nz); if (nl < 1e-8f) continue;
        nx = -nx / nl; ny = -ny / nl; nz = -nz / nl;
        float ex = camX - (a[0] + c2[0]) * 0.5f, ey = camY - (a[1] + c2[1]) * 0.5f, ez = camZ - (a[2] + c2[2]) * 0.5f;
        if (nx * ex + ny * ey + nz * ez <= 0) continue;
        float dist = sqrtf(ex * ex + ey * ey + ez * ez);
        float shade = clampf(0.62f + 0.38f * (nx * -0.35f + ny * 0.8f + nz * 0.45f), 0.45f, 1.05f) * 1.3f;
        float cr = m->c[f * 3], cg = m->c[f * 3 + 1], cb = m->c[f * 3 + 2];
        if (part == 5 && weap == 2) { cr = 120; cg = 84; cb = 52; }
        u32c col = RGBA((int)clampf(cr * tr, 0, 255), (int)clampf(cg * tg, 0, 255), (int)clampf(cb * tb, 0, 255), 255);
        V v[4];
        for (int k = 0; k < 4; k++) {
            const float *pt = tv[qd[k]];
            toView(pt[0], pt[1], pt[2], &v[k].x, &v[k].y, &v[k].z);
            fogColor(col, shade, dist, &v[k].r, &v[k].g, &v[k].b);
        }
        emitPoly(v, 4, 0);
    }
    if (weap == 0 && !g->dead) {       /* pistola: corredera y empunadura, sujetas por el hueso del arma */
        float lx0 = camX - wx, lz0 = camZ - wz;
        if (lx0 * lx0 + lz0 * lz0 <= 18.0f * 18.0f) vpDrawRig(&G[BN_WEAP]);       /* cerca: malla VP70 (72 triangulos) */
        else {                                                                      /* lejos: 3 cajas (x0, x1, y0, y1, z0, z1, tipo: 0 metal, 1 polimero) */
        static const float bxs[3][7] = {
            { 0.149f, 0.191f, 1.117f, 1.168f, -0.62f, -0.293f, 0 },     /* corredera */
            { 0.152f, 0.188f, 1.080f, 1.117f, -0.585f, -0.293f, 1 },    /* marco */
            { 0.152f, 0.188f, 0.950f, 1.080f, -0.39f, -0.30f, 1 } };    /* empunadura */
        for (int bi = 0; bi < 3; bi++) {
            const float *bb = bxs[bi]; float c[8][3];
            for (int i = 0; i < 8; i++) { float lp[3] = { (i & 1) ? bb[1] : bb[0], (i & 2) ? bb[3] : bb[2], (i & 4) ? bb[5] : bb[4] }; m34Xf(&G[BN_WEAP], lp, c[i]); }
            float cxm = 0, cym = 0, czm = 0; for (int i = 0; i < 8; i++) { cxm += c[i][0] / 8; cym += c[i][1] / 8; czm += c[i][2] / 8; }
            for (int fcs = 0; fcs < 6; fcs++) {
                const float *p0 = c[FACE[fcs][0]], *p1 = c[FACE[fcs][1]], *p2 = c[FACE[fcs][2]];
                float ax = p1[0] - p0[0], ay = p1[1] - p0[1], az = p1[2] - p0[2], bx2 = p2[0] - p0[0], by2 = p2[1] - p0[1], bz2 = p2[2] - p0[2];
                float nx = ay * bz2 - az * by2, ny = az * bx2 - ax * bz2, nz = ax * by2 - ay * bx2, nl = sqrtf(nx * nx + ny * ny + nz * nz); if (nl < 1e-8f) continue;
                nx /= nl; ny /= nl; nz /= nl;
                float fx0 = (p0[0] + c[FACE[fcs][2]][0]) * 0.5f, fy0 = (p0[1] + c[FACE[fcs][2]][1]) * 0.5f, fz0 = (p0[2] + c[FACE[fcs][2]][2]) * 0.5f;
                if (nx * (cxm - fx0) + ny * (cym - fy0) + nz * (czm - fz0) > 0) { nx = -nx; ny = -ny; nz = -nz; }     /* normal hacia afuera */
                float ex = camX - fx0, ey = camY - fy0, ez = camZ - fz0;
                if (nx * ex + ny * ey + nz * ez <= 0) continue;
                float dist = sqrtf(ex * ex + ey * ey + ez * ez), shade = clampf(0.62f + 0.38f * (nx * -0.35f + ny * 0.8f + nz * 0.45f), 0.45f, 1.05f);
                V v[4];
                for (int k = 0; k < 4; k++) { const float *pt = c[FACE[fcs][k]]; toView(pt[0], pt[1], pt[2], &v[k].x, &v[k].y, &v[k].z); fogColor(bb[6] > 0.5f ? RGBA(30, 31, 34, 255) : RGBA(48, 50, 55, 255), shade, dist, &v[k].r, &v[k].g, &v[k].b); }
                emitPoly(v, 4, 0);
            }
        }
        }
    }
    if (flash > 0 && !g->dead) {
        float lp[3] = { 0.17f, 1.14f, (weap == 0 ? -0.62f : -1.08f) - 0.07f }, wp[3]; m34Xf(&G[BN_WEAP], lp, wp);
        obox(wp[0], wp[1], wp[2], .07f, .07f, .09f, yaw, RGBA(255, 214, 110, 255));
    }
}
static const Model *botModel(int i) {
    if (bots[i].remote) return &voxM[0];
    int k = S.skin < 3 ? S.skin : i % 3;
    return &voxM[k];
}

/* Alimenta el esqueleto de cada soldado con su movimiento real (velocidad medida) y sus estados. */
static void updateRigs(float dt) {
    for (int i = 0; i < nbots; i++) {
        Bot *b = &bots[i];
        float vx = (b->x - b->px) / (dt > 1e-4f ? dt : 1e-4f), vz = (b->z - b->pz) / (dt > 1e-4f ? dt : 1e-4f);
        if (!b->rg.init) { vx = vz = 0; }
        b->px = b->x; b->pz = b->z;
        float sp = sqrtf(vx * vx + vz * vz); if (sp > 9) { vx *= 9 / sp; vz *= 9 / sp; }
        float pitch = 0; int aim = 0;
        if (b->remote) { pitch = b->pitch; aim = b->aimW > 0.5f; }
        else {
            aim = b->state == BS_ALERT || b->state == BS_ENGAGE || b->state == BS_PEEK;
            if (aim && P.alive) { float dx = P.x - b->x, dz = P.z - b->z; pitch = atan2f(P.y + 1.2f - (1.35f - 0.5f * b->rg.crouch), sqrtf(dx * dx + dz * dz) + 0.01f); }
        }
        if (b->rl) { b->rlT += dt; if (b->rlT > 1.7f) b->rl = 0; }
        RigIn in = { vx, vz, b->yaw, pitch, b->rl ? clampf(b->rlT / 1.7f, 0, 1) : -1.0f, b->crWant, aim, b->fireEv, b->alive, b->py > 0.12f };
        b->fireEv = 0;
        rigUpdate(&b->rg, &in, dt);
        b->cr = b->rg.crouch; b->pitch = pitch;
        if (!b->alive) b->dead += 0;
    }
}
static void drawBots(void) {
    for (int i = 0; i < nbots; i++) {
        const Bot *b = &bots[i];
        if (b->unused) continue;
#ifdef PS2_ONLINE
        if (rds[i].on) {      /* cadaver con ragdoll: ligeramente enrojecido segun la herida */
            const Ragdoll *rd = &rds[i]; float tn = 1.0f - 0.28f * rd->wound;
            rigOverG = rd->G; drawRig(botModel(i), &b->rg, 0.0f, -1.0f, 1, rd->p[RP_HIP][0], rd->p[RP_HIP][1], rd->p[RP_HIP][2], b->yaw, 1.0f, tn, tn, 0); rigOverG = NULL;
            continue;
        }
#endif
        drawRig(botModel(i), &b->rg, b->rg.dead ? 0.0f : b->pitch, b->rl ? clampf(b->rlT / 1.7f, 0, 1) : -1.0f, 1, b->x, b->py, b->z, b->yaw, 1, 1, 1, 0);
    }
}

static float dbgYaw = 0;   /* solo para pruebas */
static float charAnim(const Pad *pd, float dt) {
    float f0 = -sinf(P.yaw), f1 = -cosf(P.yaw);
    float spd = sqrtf(P.vx * P.vx + P.vz * P.vz), fwd = P.vx * f0 + P.vz * f1;
    float want = P.ground ? clampf(spd / 5.4f, 0, 1) : anSpd * 0.9f;
    anSpd = lerpf(anSpd, want, clampf(dt * 12, 0, 1));
    anRun = lerpf(anRun, clampf((spd - 2.8f) / 2.2f, 0, 1), clampf(dt * 8, 0, 1));
    anPhase += (fwd >= -0.4f ? 1.0f : -1.0f) * spd * dt * 2.1f;
    if (aimT > 0) aimT -= dt;
#ifdef PS2_ONLINE
    int trig = pd && (pd->down & B_R1) && P.alive && !bfSprint;
#else
    int trig = pd && (pd->down & (B_R2 | B_R1)) && P.alive;
#endif
    anAim = lerpf(anAim, (trig || aimT > 0) ? 1.0f : 0.0f, clampf(dt * 9, 0, 1));
#ifdef PS2_ONLINE
    if (bfAds > 0.5f) anAim = fmaxf(anAim, bfAds);
#endif
    anFall = P.alive ? 0 : fminf(1.0f, anFall + dt * 2.2f);
    float ru = wReload > 0 ? clampf(1 - wReload / W[wsel].reload, 0, 1) : -1.0f;
    RigIn in = { P.vx, P.vz, P.yaw, P.pitch, ru, P.crWant != 0, anAim > 0.4f, pFired, P.alive, !P.ground };
    pFired = 0;
    rigUpdate(&pRig, &in, dt);
    return spd;
}
static void drawPlayerChar(void) {
    float ru = wReload > 0 ? clampf(1 - wReload / W[wsel].reload, 0, 1) : -1.0f;
    drawRig(&voxM[0], &pRig, P.pitch, ru, wsel, P.x, P.y, P.z, P.yaw + dbgYaw, 0.85f, 1.0f, 1.3f, wFlash);
}
/* Piernas y botas del jugador: aparecen al mirar hacia abajo. */
static void drawPlayerLegs(void) {
    if (!P.alive || camPitch > -0.3f) return;
    float spd = clampf(sqrtf(P.vx * P.vx + P.vz * P.vz) / 5.4f, 0, 1), sw = sinf(P.bob * 2) * spd * 0.28f;
    u32c PANT = RGBA(66, 74, 52, 255), PANT2 = RGBA(50, 57, 40, 255), BOOT = RGBA(26, 24, 22, 255), SOLE = RGBA(14, 14, 14, 255);
    float s = sinf(P.yaw), co = cosf(P.yaw);
    for (int k = 0; k < 2; k++) {
        float lx = k ? .12f : -.12f, lz = -.14f + (k ? -sw : sw), lift = (k ? -sw : sw) > 0 ? (k ? -sw : sw) * .12f : 0;
        float wx = P.x + lx * co + lz * s, wz = P.z - lx * s + lz * co;
        obox(wx, P.y + .46f + lift, wz, .1f, .29f, .11f, P.yaw, PANT);
        obox(wx, P.y + .5f + lift, wz, .105f, .08f, .115f, P.yaw, PANT2);
        float bx = P.x + lx * co + (lz - .02f) * s, bz = P.z - lx * s + (lz - .02f) * co;
        obox(bx, P.y + .13f + lift, bz, .105f, .13f, .125f, P.yaw, BOOT);
        float tx = P.x + lx * co + (lz - .08f) * s, tz = P.z - lx * s + (lz - .08f) * co;
        obox(tx, P.y + .05f + lift, tz, .108f, .05f, .17f, P.yaw, BOOT);
        obox(tx, P.y + .012f + lift, tz, .11f, .014f, .18f, P.yaw, SOLE);
    }
}
/* Borde del casco: sombra sutil en la parte alta de la vista. */
static void drawHelmetRim(void) {
    float h = SH * 0.05f;
    for (int i = 0; i < 6; i++) gfx_rect(0, h * i / 6.0f, (float)SW, h * (i + 1) / 6.0f, RGBA(8, 10, 8, (int)(170 - i * 26)));
}

/* ---------- Fusil detallado en primera persona (cajas con orientacion libre, espacio de camara) ---------- */
static float WR[3][3], WT[3];     /* espacio del arma -> vista: columnas = ejes x,y,z del arma */
static void gbox(const float *c, const float *ax, const float *ay, const float *az, float hw, float hh, float hd, u32c col) {
    float k[8][3];
    for (int i = 0; i < 8; i++) {
        float sx = (i & 1) ? hw : -hw, sy = (i & 2) ? hh : -hh, sz = (i & 4) ? hd : -hd;
        for (int j = 0; j < 3; j++) k[i][j] = c[j] + ax[j] * sx + ay[j] * sy + az[j] * sz;
    }
    for (int f = 0; f < 6; f++) {
        float n[3], ext = fabsf(FN[f][0]) * hw + fabsf(FN[f][1]) * hh + fabsf(FN[f][2]) * hd;
        for (int j = 0; j < 3; j++) n[j] = ax[j] * FN[f][0] + ay[j] * FN[f][1] + az[j] * FN[f][2];
        float fc[3] = { c[0] + n[0] * ext, c[1] + n[1] * ext, c[2] + n[2] * ext };
        if (-(n[0] * fc[0] + n[1] * fc[1] + n[2] * fc[2]) <= 0) continue;
        float shade = clampf(0.7f + 0.3f * (n[1] * 0.8f - n[0] * 0.35f), 0.45f, 1.0f);
        V v[4];
        for (int q = 0; q < 4; q++) {
            const float *pk = k[FACE[f][q]];
            v[q].x = pk[0]; v[q].y = pk[1]; v[q].z = pk[2];
            v[q].r = CR(col) * shade; v[q].g = CG(col) * shade; v[q].b = CB(col) * shade;
        }
        emitPoly(v, 4, 1);
    }
}
static void wpt(float x, float y, float z, float *o) {   /* punto del arma -> vista */
    for (int j = 0; j < 3; j++) o[j] = WR[j][0] * x + WR[j][1] * y + WR[j][2] * z + WT[j];
}
static void wbox(float x, float y, float z, float hw, float hh, float hd, u32c col) {
    float c[3], ax[3] = { WR[0][0], WR[1][0], WR[2][0] }, ay[3] = { WR[0][1], WR[1][1], WR[2][1] }, az[3] = { WR[0][2], WR[1][2], WR[2][2] };
    wpt(x, y, z, c); gbox(c, ax, ay, az, hw, hh, hd, col);
}
/* caja entre dos puntos de vista (antebrazos) */
static void limb(const float *a, const float *b, float th, u32c col, u32c patch) {
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (L < 1e-4f) return;
    for (int j = 0; j < 3; j++) d[j] /= L;
    float up[3] = { 0, 1, 0 }, x[3] = { up[1] * d[2] - up[2] * d[1], up[2] * d[0] - up[0] * d[2], up[0] * d[1] - up[1] * d[0] };
    float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]); if (xl < 1e-4f) { x[0] = 1; x[1] = 0; x[2] = 0; xl = 1; }
    for (int j = 0; j < 3; j++) x[j] /= xl;
    float y[3] = { d[1] * x[2] - d[2] * x[1], d[2] * x[0] - d[0] * x[2], d[0] * x[1] - d[1] * x[0] };
    float c[3] = { (a[0] + b[0]) * .5f, (a[1] + b[1]) * .5f, (a[2] + b[2]) * .5f };
    gbox(c, x, y, d, th, th * .9f, L * .5f, col);
    float c1[3], c2[3], c3[3];                       /* manchas de camuflaje */
    for (int j = 0; j < 3; j++) { c1[j] = c[j] + y[j] * th * .75f + d[j] * L * .15f + x[j] * th * .2f; c2[j] = c[j] + x[j] * th * .8f - d[j] * L * .2f; c3[j] = c[j] - x[j] * th * .75f + d[j] * L * .05f + y[j] * th * .1f; }
    gbox(c1, x, y, d, th * .55f, th * .3f, L * .16f, patch);
    gbox(c2, x, y, d, th * .3f, th * .55f, L * .18f, patch);
    gbox(c3, x, y, d, th * .3f, th * .5f, L * .14f, patch);
    float c4[3]; for (int j = 0; j < 3; j++) c4[j] = b[j] - d[j] * L * .06f;     /* puño */
    gbox(c4, x, y, d, th * 1.06f, th * .96f, L * .07f, RGBA(24, 26, 24, 255));
}
static float smooth(float t) { t = clampf(t, 0, 1); return t * t * (3 - 2 * t); }
static void rotMat(float yaw, float pitch, float roll, float R[3][3]) {
    float cr = cosf(roll), sr = sinf(roll), cp = cosf(pitch), sp = sinf(pitch), cy = cosf(yaw), sy = sinf(yaw);
    float A[3][3] = { { cr, -sr, 0 }, { sr, cr, 0 }, { 0, 0, 1 } };           /* roll (z) */
    float B[3][3] = { { 1, 0, 0 }, { 0, cp, sp }, { 0, -sp, cp } };            /* pitch: z hacia arriba con p>0 */
    float C[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };            /* yaw: z hacia +x con y>0 */
    float T[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { T[i][j] = 0; for (int k = 0; k < 3; k++) T[i][j] += B[i][k] * A[k][j]; }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { R[i][j] = 0; for (int k = 0; k < 3; k++) R[i][j] += C[i][k] * T[k][j]; }
}
/* ---------- Modelos en primera persona: fusil, pistola, escopeta, ametralladora ---------- */
typedef struct { float grip[3], base[3], tilt[3], key[10][4]; float mz; } VMDef;   /* tilt = cabeceo, giro, alabeo durante la recarga */
static const VMDef VMD[4] = {
    /* 0 pistola */   { { 0, -0.055f, -0.035f }, { 0.10f, -0.17f, 0.40f }, { 0.38f, -0.15f, -0.35f },
        { { 0.00f, -0.035f, -0.06f, -0.03f }, { 0.16f, -0.02f, -0.15f, -0.04f }, { 0.30f, -0.02f, -0.15f, -0.04f }, { 0.40f, -0.02f, -0.30f, -0.04f }, { 0.50f, -0.22f, -0.42f, 0.00f },
          { 0.60f, -0.14f, -0.30f, -0.02f }, { 0.70f, -0.02f, -0.18f, -0.04f }, { 0.76f, -0.02f, -0.15f, -0.04f }, { 0.86f, -0.05f, 0.05f, 0.00f }, { 1.00f, -0.035f, -0.06f, -0.03f } }, 0.24f },
    /* 1 fusil */     { { 0, -0.085f, -0.075f }, { 0.21f, -0.24f, 0.47f }, { 0.42f, -0.30f, -0.62f },
        { { 0.00f, -0.01f, -0.045f, 0.33f }, { 0.16f, -0.01f, -0.10f, 0.05f }, { 0.30f, -0.01f, -0.10f, 0.05f }, { 0.40f, -0.02f, -0.26f, 0.04f }, { 0.50f, -0.20f, -0.40f, 0.00f },
          { 0.60f, -0.14f, -0.30f, 0.03f }, { 0.70f, -0.02f, -0.15f, 0.05f }, { 0.76f, -0.01f, -0.10f, 0.05f }, { 0.86f, -0.05f, 0.00f, -0.01f }, { 1.00f, -0.01f, -0.045f, 0.33f } }, 0.70f },
    /* 2 escopeta */  { { 0, -0.08f, -0.10f }, { 0.20f, -0.24f, 0.46f }, { 0.35f, -0.20f, 0.55f },
        { { 0.00f, 0.0f, -0.03f, 0.33f }, { 0.12f, -0.18f, -0.35f, 0.05f }, { 0.24f, 0.0f, -0.07f, 0.02f }, { 0.34f, -0.18f, -0.35f, 0.05f }, { 0.46f, 0.0f, -0.07f, 0.02f },
          { 0.56f, -0.18f, -0.35f, 0.05f }, { 0.68f, 0.0f, -0.07f, 0.02f }, { 0.78f, 0.0f, -0.03f, 0.20f }, { 0.88f, 0.0f, -0.03f, 0.44f }, { 1.00f, 0.0f, -0.03f, 0.33f } }, 0.90f },
    /* 3 ametralladora */ { { 0, -0.09f, -0.10f }, { 0.22f, -0.27f, 0.50f }, { 0.30f, -0.25f, -0.50f },
        { { 0.00f, -0.01f, -0.04f, 0.32f }, { 0.16f, -0.01f, -0.10f, 0.03f }, { 0.30f, -0.01f, -0.10f, 0.03f }, { 0.40f, -0.01f, -0.30f, 0.03f }, { 0.50f, -0.22f, -0.45f, 0.00f },
          { 0.60f, -0.14f, -0.32f, 0.03f }, { 0.70f, -0.02f, -0.14f, 0.03f }, { 0.76f, -0.01f, -0.10f, 0.03f }, { 0.86f, -0.05f, 0.06f, -0.02f }, { 1.00f, -0.01f, -0.04f, 0.32f } }, 0.84f }
};
static void lhandKey(const VMDef *d, float u, float *o) {
    for (int i = 0; i + 1 < 10; i++)
        if (u <= d->key[i + 1][0]) {
            float t = smooth((u - d->key[i][0]) / (d->key[i + 1][0] - d->key[i][0]));
            for (int j = 0; j < 3; j++) o[j] = lerpf(d->key[i][1 + j], d->key[i + 1][1 + j], t);
            return;
        }
    for (int j = 0; j < 3; j++) o[j] = d->key[9][1 + j];
}
/* colores del modelo en primera persona */
#define C_GM RGBA(84, 88, 94, 255)
#define C_GD RGBA(54, 57, 62, 255)
#define C_GL RGBA(116, 120, 128, 255)
#define C_BR RGBA(190, 150, 74, 255)
#define C_WD RGBA(120, 82, 48, 255)
#define C_WD2 RGBA(94, 62, 36, 255)
/* pistola VP70 en primera persona: transforma los vertices una vez y dibuja cada triangulo de frente (colores planos) */
static void vpDrawVM(int withMag) {
    float pv[VPM_NV][3];
    for (int i = 0; i < VPM_NV; i++) wpt(VPM_V[i * 3], VPM_V[i * 3 + 1], VPM_V[i * 3 + 2], pv[i]);
    static const u32c pal[5] = { C_GM, C_GD, C_GL, RGBA(16, 16, 19, 255), RGBA(40, 42, 46, 255) };
    int nt = withMag ? VPM_NT : VPM_NTB;
    for (int t = 0; t < nt; t++) {
        const float *a = pv[VPM_T[t * 3]], *b = pv[VPM_T[t * 3 + 1]], *c = pv[VPM_T[t * 3 + 2]];
        float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx, nl = sqrtf(nx * nx + ny * ny + nz * nz);
        if (nl < 1e-12f) continue;
        nx /= nl; ny /= nl; nz /= nl;
        if (nx * a[0] + ny * a[1] + nz * a[2] >= 0) continue;                    /* de espaldas a la camara (esta en el origen) */
        float shade = clampf(0.7f + 0.3f * (ny * 0.8f - nx * 0.35f), 0.45f, 1.0f);
        u32c col = pal[VPM_C[t]]; V v[3]; const float *pp[3] = { a, b, c };
        for (int k = 0; k < 3; k++) { v[k].x = pp[k][0]; v[k].y = pp[k][1]; v[k].z = pp[k][2]; v[k].r = CR(col) * shade; v[k].g = CG(col) * shade; v[k].b = CB(col) * shade; }
        emitPoly(v, 3, 1);
    }
}
static void handR(float gx, float gy, float gz, const float *anchor, int glove) {   /* mano derecha en la empunadura (glove=0: solo antebrazo, sin cajas de guante) */
    u32c GLV = RGBA(52, 54, 58, 255), KNU = RGBA(92, 94, 100, 255), SL = RGBA(176, 152, 110, 255), SP = RGBA(142, 120, 84, 255);
    if (!glove) { float a0[3]; wpt(gx + 0.045f, gy - 0.012f, gz - 0.030f, a0); limb(anchor, a0, 0.036f, SL, SP); return; }
    wbox(gx + 0.034f, gy + 0.0f, gz + 0.005f, 0.014f, 0.040f, 0.030f, GLV);
    wbox(gx + 0.040f, gy + 0.025f, gz + 0.005f, 0.008f, 0.013f, 0.022f, KNU);
    wbox(gx + 0.010f, gy + 0.016f, gz + 0.035f, 0.027f, 0.010f, 0.008f, GLV);
    wbox(gx + 0.010f, gy - 0.008f, gz + 0.035f, 0.027f, 0.010f, 0.008f, GLV);
    wbox(gx + 0.010f, gy - 0.031f, gz + 0.031f, 0.025f, 0.010f, 0.008f, GLV);
    wbox(gx + 0.000f, gy + 0.045f, gz + 0.085f, 0.008f, 0.008f, 0.034f, GLV);        /* indice sobre el guardamonte */
    wbox(gx - 0.020f, gy + 0.040f, gz - 0.020f, 0.010f, 0.020f, 0.032f, GLV);        /* pulgar */
    float a[3]; wpt(gx + 0.045f, gy - 0.012f, gz - 0.030f, a); limb(anchor, a, 0.036f, SL, SP);
}
static void handL(float lx, float ly, float lz, const float *anchor, int glove) {   /* mano izquierda de apoyo (glove=0: solo antebrazo) */
    u32c GLV = RGBA(52, 54, 58, 255), KNU = RGBA(92, 94, 100, 255), SL = RGBA(176, 152, 110, 255), SP = RGBA(142, 120, 84, 255);
    if (!glove) { float a0[3]; wpt(lx - 0.035f, ly - 0.01f, lz - 0.05f, a0); limb(anchor, a0, 0.036f, SP, SL); return; }
    wbox(lx - 0.030f, ly, lz, 0.012f, 0.036f, 0.038f, GLV);
    wbox(lx - 0.032f, ly + 0.024f, lz, 0.007f, 0.011f, 0.028f, KNU);
    for (int i = 0; i < 4; i++) wbox(lx - 0.004f, ly - 0.032f, lz - 0.026f + i * 0.0173f, 0.033f, 0.009f, 0.0076f, GLV);
    wbox(lx + 0.034f, ly + 0.012f, lz - 0.010f, 0.009f, 0.015f, 0.027f, GLV);
    float a[3]; wpt(lx - 0.035f, ly - 0.01f, lz - 0.05f, a); limb(anchor, a, 0.036f, SP, SL);
}
static void magItem(int w, float x, float y, float z) {   /* cargador (o caja) segun el arma */
    if (w == 0) { wbox(x, y, z, 0.016f, 0.036f, 0.019f, C_GD); wbox(x, y - 0.038f, z, 0.018f, 0.006f, 0.021f, C_GM); }
    else if (w == 1) { wbox(x, y, z, 0.019f, 0.05f, 0.023f, C_GD); wbox(x, y - 0.05f, z + 0.01f, 0.0195f, 0.025f, 0.023f, C_GD); wbox(x, y - 0.09f, z + 0.02f, 0.021f, 0.016f, 0.024f, C_GM); }
    else { wbox(x, y, z, 0.05f, 0.05f, 0.07f, C_GD); wbox(x, y + 0.052f, z, 0.046f, 0.004f, 0.066f, C_GM); wbox(x - 0.05f, y + 0.02f, z, 0.004f, 0.01f, 0.05f, C_BR); }
}
static void drawVM(int w, float kick, float swap, float u, int reloading, float spd) {
    const VMDef *d = &VMD[w];
    u32c GM = C_GM, GD = C_GD, GL = C_GL, BR = C_BR;
    float k = reloading ? smooth(fminf(u / 0.16f, 1.0f)) * (1 - smooth((u - 0.86f) / 0.14f)) : 0;
    float bobx = sinf(P.bob) * 0.008f * spd, boby = fabsf(cosf(P.bob)) * 0.010f * spd + sinf(orbit * 0 + P.bob * 0.5f) * 0.002f;
    float pitch = d->tilt[0] * k + kick * 0.7f, yaw = d->tilt[1] * k + 0.05f - swap * 0.4f, roll = d->tilt[2] * k - swap * 0.5f;
#ifdef PS2_ONLINE
    /* apuntando: el arma pasa al centro y la mira queda a la altura del ojo; corriendo: arma baja y ladeada */
    static const float ADSP[4][3] = { { 0, -0.134f, 0.46f }, { 0, -0.200f, 0.50f }, { 0, -0.178f, 0.50f }, { 0, -0.212f, 0.52f } };   /* y mas bajo: el arma ya no tapa el centro de la pantalla */
    float ad = bfAds, sa = bfSprA; bobx *= 1 - 0.85f * ad; boby *= 1 - 0.85f * ad;
    yaw = yaw * (1 - ad) + 0.0f * ad + 0.32f * sa; pitch = pitch * (1 - 0.6f * ad) + (-0.30f) * sa; roll = roll * (1 - ad) + 0.25f * sa;
#endif
    rotMat(yaw, pitch, roll, WR);
    float hx = d->base[0] - 0.09f * k + bobx, hy = d->base[1] - swap * 0.3f + 0.05f * k - kick * 0.02f - boby, hz = d->base[2] - kick * 0.9f - 0.06f * k;
#ifdef PS2_ONLINE
    hx = lerpf(hx, ADSP[w][0] + bobx, ad * (1 - k)); hy = lerpf(hy, ADSP[w][1] - swap * 0.3f - kick * 0.01f, ad * (1 - k)); hz = lerpf(hz, ADSP[w][2] - kick * 0.5f, ad * (1 - k));
    hx += 0.05f * sa; hy -= 0.07f * sa; hz -= 0.05f * sa;
#endif
    float hp[3] = { hx, hy, hz };
    for (int j = 0; j < 3; j++) WT[j] = hp[j] - (WR[j][0] * d->grip[0] + WR[j][1] * d->grip[1] + WR[j][2] * d->grip[2]);
    float lh[3]; if (reloading) lhandKey(d, u, lh); else { lh[0] = d->key[0][1]; lh[1] = d->key[0][2]; lh[2] = d->key[0][3]; }
    const float AR[3] = { w == 0 ? 0.26f : 0.36f, -0.5f, 0.32f }, AL[3] = { w == 0 ? -0.20f : -0.34f, -0.5f, 0.32f };
    int magSeated = !reloading || u < 0.16f || u >= 0.76f, magOnHand = reloading && ((u >= 0.16f && u < 0.42f) || (u >= 0.5f && u < 0.76f));
    float pumpZ = 0.33f;
    if (w == 0) {          /* ---- pistola ---- */
        vpDrawVM(magSeated);                                                    /* VP70: 218 triangulos, ver VPM_* */
    } else if (w == 1) {   /* ---- fusil ---- */
        wbox(0, -0.012f, 0.00f, 0.027f, 0.036f, 0.115f, GM);
        wbox(0, 0.030f, 0.02f, 0.029f, 0.027f, 0.14f, GD);
        wbox(0.0295f, 0.026f, 0.025f, 0.002f, 0.016f, 0.045f, RGBA(12, 12, 14, 255));
        wbox(0, 0.060f, 0.045f, 0.013f, 0.004f, 0.22f, GL);
        for (int i = 0; i < 9; i++) wbox(0, 0.0635f, -0.06f + i * 0.057f, 0.0135f, 0.0025f, 0.012f, GD);
        wbox(0, 0.058f, -0.075f, 0.009f, 0.009f, 0.014f, GL);
        wbox(-0.032f, 0.01f, -0.03f, 0.004f, 0.007f, 0.012f, GL);
        wbox(-0.029f, -0.02f, 0.01f, 0.004f, 0.01f, 0.012f, GL);
        wbox(0, 0.018f, 0.33f, 0.034f, 0.034f, 0.18f, GM);
        wbox(0, 0.056f, 0.33f, 0.013f, 0.004f, 0.18f, GL);
        wbox(-0.036f, 0.018f, 0.33f, 0.004f, 0.013f, 0.15f, GL); wbox(0.036f, 0.018f, 0.33f, 0.004f, 0.013f, 0.15f, GL);
        wbox(0, -0.018f, 0.33f, 0.013f, 0.004f, 0.15f, GL);
        for (int i = 0; i < 5; i++) { wbox(-0.0345f, 0.018f, 0.20f + i * 0.065f, 0.0035f, 0.03f, 0.012f, GD); wbox(0.0345f, 0.018f, 0.20f + i * 0.065f, 0.0035f, 0.03f, 0.012f, GD); }
        wbox(0, 0.02f, 0.58f, 0.011f, 0.011f, 0.06f, GD);
        wbox(0, 0.045f, 0.50f, 0.012f, 0.014f, 0.022f, GL); wbox(0, 0.07f, 0.50f, 0.004f, 0.012f, 0.004f, GD);      /* mira frontal */
        wbox(0, 0.02f, 0.665f, 0.017f, 0.017f, 0.028f, RGBA(18, 19, 22, 255));
        for (int i = 0; i < 3; i++) wbox(0, 0.02f, 0.65f + i * 0.02f, 0.0185f, 0.0185f, 0.004f, GD);
        wbox(0, 0.070f, 0.10f, 0.027f, 0.011f, 0.04f, GD);
        /* mira de punto rojo hueca: solo el marco, sin cristal ni caja solida, para ver a traves */
        wbox(0, 0.136f, 0.10f, 0.031f, 0.004f, 0.035f, GM); wbox(0, 0.080f, 0.10f, 0.031f, 0.004f, 0.035f, GM);
        wbox(-0.027f, 0.108f, 0.10f, 0.004f, 0.028f, 0.035f, GM); wbox(0.027f, 0.108f, 0.10f, 0.004f, 0.028f, 0.035f, GM);
        wbox(0, 0.115f, 0.0615f, 0.0035f, 0.0035f, 0.001f, RGBA(255, 70, 50, 255));
        wbox(0, 0.142f, 0.10f, 0.02f, 0.006f, 0.03f, GD);
        wbox(0, -0.06f, -0.07f, 0.021f, 0.03f, 0.021f, GD); wbox(0, -0.095f, -0.082f, 0.022f, 0.03f, 0.023f, GD); wbox(0, -0.125f, -0.09f, 0.024f, 0.006f, 0.025f, GM);
        wbox(0, -0.052f, 0.045f, 0.004f, 0.006f, 0.05f, GM); wbox(0, -0.04f, -0.005f, 0.0035f, 0.014f, 0.004f, GL);
        wbox(0, 0.004f, -0.17f, 0.017f, 0.017f, 0.075f, GM);
        if (magSeated) {
            wbox(0, -0.085f, 0.035f, 0.019f, 0.05f, 0.023f, GD); wbox(0, -0.15f, 0.045f, 0.0195f, 0.025f, 0.023f, GD); wbox(0, -0.19f, 0.055f, 0.021f, 0.016f, 0.024f, GM);
            wbox(0, -0.115f, 0.0125f, 0.016f, 0.012f, 0.002f, GL); wbox(-0.0195f, -0.12f, 0.04f, 0.0015f, 0.03f, 0.014f, GM);
        }
    } else if (w == 2) {   /* ---- escopeta ---- */
        if (reloading && u >= 0.72f) pumpZ = lh[2];
        wbox(0, 0.01f, 0.0f, 0.032f, 0.05f, 0.14f, GD);                         /* caja de mecanismos */
        wbox(0.0335f, 0.02f, 0.02f, 0.002f, 0.015f, 0.045f, RGBA(12, 12, 14, 255));
        wbox(0, -0.047f, 0.03f, 0.02f, 0.004f, 0.04f, RGBA(10, 10, 12, 255));   /* ventana de carga */
        wbox(0, 0.04f, 0.50f, 0.015f, 0.015f, 0.38f, GM);                       /* canon */
        wbox(0, 0.058f, 0.50f, 0.006f, 0.003f, 0.36f, GL);                      /* cinta ventilada */
        wbox(0, 0.066f, 0.86f, 0.004f, 0.006f, 0.004f, RGBA(224, 128, 58, 255));/* mira */
        wbox(0, 0.04f, 0.885f, 0.018f, 0.018f, 0.012f, GD);
        wbox(0, -0.005f, 0.46f, 0.012f, 0.012f, 0.33f, GM);                     /* tubo del cargador */
        wbox(0, -0.005f, 0.795f, 0.016f, 0.016f, 0.01f, GD);
        wbox(0, -0.012f, pumpZ, 0.034f, 0.03f, 0.10f, C_WD);                    /* bomba */
        for (int i = 0; i < 4; i++) wbox(0, -0.012f, pumpZ - 0.07f + i * 0.045f, 0.0355f, 0.031f, 0.004f, C_WD2);
        wbox(0, -0.08f, -0.10f, 0.02f, 0.05f, 0.026f, C_WD); wbox(0, -0.02f, -0.13f, 0.02f, 0.04f, 0.04f, C_WD2);   /* empunadura y culata */
        wbox(0, -0.04f, -0.01f, 0.004f, 0.004f, 0.05f, GM);
    } else {               /* ---- ametralladora ---- */
        wbox(0, 0.0f, 0.0f, 0.042f, 0.05f, 0.17f, GM);                          /* receptor */
        wbox(0, 0.055f, 0.02f, 0.036f, 0.012f, 0.14f, GD);                      /* tapa */
        wbox(0, 0.07f, 0.02f, 0.012f, 0.004f, 0.14f, GL);                       /* riel */
        wbox(0, 0.08f, -0.05f, 0.016f, 0.012f, 0.01f, GD); wbox(0, 0.09f, 0.1f, 0.004f, 0.02f, 0.004f, GD);   /* miras */
        wbox(-0.044f, 0.01f, 0.03f, 0.004f, 0.025f, 0.06f, GD);                 /* guia de cinta */
        for (int i = 0; i < 4; i++) wbox(-0.06f, -0.01f - i * 0.016f, 0.03f, 0.014f, 0.006f, 0.012f, BR);  /* eslabones de cinta */
        wbox(0, 0.025f, 0.48f, 0.017f, 0.017f, 0.32f, GD);                      /* canon */
        wbox(0, 0.025f, 0.40f, 0.027f, 0.027f, 0.20f, GM);                      /* cubrecanon */
        for (int i = 0; i < 4; i++) { wbox(0, 0.054f, 0.34f + i * 0.04f, 0.015f, 0.003f, 0.012f, RGBA(12, 12, 14, 255)); wbox(0.0285f, 0.025f, 0.34f + i * 0.04f, 0.002f, 0.012f, 0.012f, RGBA(12, 12, 14, 255)); }
        wbox(0, 0.025f, 0.805f, 0.021f, 0.021f, 0.03f, GD);                     /* apagallamas */
        wbox(0, -0.03f, 0.30f, 0.036f, 0.03f, 0.10f, GM);                       /* guardamanos */
        wbox(-0.02f, -0.045f, 0.52f, 0.004f, 0.004f, 0.12f, GD); wbox(0.02f, -0.045f, 0.52f, 0.004f, 0.004f, 0.12f, GD);   /* bipode plegado */
        wbox(0, -0.04f, 0.43f, 0.025f, 0.01f, 0.012f, GL);
        wbox(0, -0.06f, -0.09f, 0.021f, 0.035f, 0.022f, GD); wbox(0, -0.095f, -0.1f, 0.022f, 0.03f, 0.024f, GD);   /* empunadura */
        wbox(0, -0.05f, 0.04f, 0.004f, 0.006f, 0.05f, GM);
        wbox(0, 0.005f, -0.21f, 0.02f, 0.02f, 0.06f, GM);
        if (magSeated) { wbox(0, -0.1f, 0.03f, 0.05f, 0.05f, 0.07f, GD); wbox(0, -0.052f, 0.03f, 0.046f, 0.004f, 0.066f, GM); wbox(-0.052f, -0.07f, 0.03f, 0.004f, 0.02f, 0.05f, GL); }
    }
    handR(d->grip[0], d->grip[1] + 0.012f, d->grip[2] + 0.004f, AR, w != 0);      /* pistola: sin cajas de guante alrededor del VP70 */
    float lx = lh[0], ly = lh[1], lz = lh[2];
    handL(lx, ly, lz, AL, w != 0);
    if (w != 2) {
        if (magOnHand) magItem(w, lx, ly + (u < 0.42f ? 0.0f : -0.01f), lz);
        if (reloading && u >= 0.34f && u < 0.52f) { float t = (u - 0.34f) / 0.18f; magItem(w, lx, ly - t * t * 0.55f, lz + t * 0.05f); }
    } else if (reloading) {   /* cartuchos: la mano los lleva del cinturon al puerto de carga */
        int carry = (u >= 0.12f && u < 0.24f) || (u >= 0.34f && u < 0.46f) || (u >= 0.56f && u < 0.68f);
        if (carry) { wbox(lx, ly + 0.02f, lz, 0.009f, 0.009f, 0.028f, RGBA(180, 40, 36, 255)); wbox(lx, ly + 0.02f, lz - 0.03f, 0.0095f, 0.0095f, 0.008f, BR); }
    }
    if (wFlash > 0) {
        float m = d->mz;
        wbox(0, d->grip[1] * 0 + (w == 0 ? 0.03f : 0.02f), m + 0.02f, 0.045f, 0.045f, 0.03f, RGBA(255, 214, 110, 255));
        wbox(0, (w == 0 ? 0.03f : 0.02f), m - 0.01f, 0.022f, 0.022f, 0.03f, RGBA(255, 250, 220, 255));
        wbox(0, (w == 0 ? 0.03f : 0.02f), m + 0.04f, 0.012f, 0.07f, 0.01f, RGBA(255, 170, 60, 255)); wbox(0, (w == 0 ? 0.03f : 0.02f), m + 0.04f, 0.07f, 0.012f, 0.01f, RGBA(255, 170, 60, 255));
    }
}
#ifdef PS2_ONLINE
/* ---- animacion de la granada en primera persona ---- */
static void lerp3(const float *a, const float *b, float t, float *o) { for (int i = 0; i < 3; i++) o[i] = a[i] + (b[i] - a[i]) * t; }
static void nadeSpace(const float *p, float yaw, float pitch, float roll) { rotMat(yaw, pitch, roll, WR); for (int j = 0; j < 3; j++) WT[j] = p[j]; }
static void nadeBody(int pinOn) {          /* cuerpo, cuello, palanca (lado +x) y pasador con anilla (lado -x) en el espacio actual */
    u32c G1 = RGBA(62, 76, 48, 255), G2 = RGBA(44, 55, 35, 255), M1 = RGBA(158, 162, 158, 255), M2 = RGBA(112, 116, 112, 255);
    wbox(0, 0, 0, 0.030f, 0.036f, 0.030f, G1);
    wbox(0, 0.018f, 0, 0.0315f, 0.0035f, 0.0315f, G2); wbox(0, 0, 0, 0.0315f, 0.0035f, 0.0315f, G2); wbox(0, -0.018f, 0, 0.0315f, 0.0035f, 0.0315f, G2);
    wbox(0, 0.046f, 0, 0.015f, 0.010f, 0.015f, M2); wbox(0, 0.058f, 0, 0.011f, 0.003f, 0.011f, M1);
    wbox(0.034f, 0.006f, 0, 0.004f, 0.050f, 0.013f, M1); wbox(0.020f, 0.058f, 0, 0.018f, 0.004f, 0.013f, M1);      /* palanca */
    if (pinOn) {
        wbox(-0.012f, 0.050f, 0, 0.018f, 0.0025f, 0.0025f, M1);
        wbox(-0.040f, 0.060f, 0, 0.010f, 0.0025f, 0.0025f, M1); wbox(-0.040f, 0.040f, 0, 0.010f, 0.0025f, 0.0025f, M1);
        wbox(-0.050f, 0.050f, 0, 0.0025f, 0.010f, 0.0025f, M1); wbox(-0.030f, 0.050f, 0, 0.0025f, 0.010f, 0.0025f, M1);
    }
}
static void drawNadeVM(void) {
    u32c GLV = RGBA(52, 54, 58, 255), KNU = RGBA(92, 94, 100, 255), SL = RGBA(176, 152, 110, 255), SP = RGBA(142, 120, 84, 255), M1 = RGBA(158, 162, 158, 255);
    float u = bfNadeSt == 2 ? 1.0f : bfNadeU, th = bfThrowT, R[3], Lh[3], yaw = 0.30f, pitch = -0.20f, roll = -0.15f;
    const float R0[3] = { 0.12f, -0.46f, 0.30f }, R1[3] = { 0.075f, -0.115f, 0.38f };
    lerp3(R0, R1, smooth(clampf(u / 0.28f, 0, 1)), R);
    int inHand = 1;
    if (bfNadeSt == 2) {                                  /* lanzamiento: el brazo se echa atras, avanza y vuelve */
        const float W1[3] = { 0.15f, -0.10f, 0.18f }, F1[3] = { 0.03f, -0.02f, 0.66f }, D1[3] = { 0.14f, -0.46f, 0.30f };
        float w = smooth(clampf(th / 0.10f, 0, 1)), s2 = smooth(clampf((th - 0.10f) / 0.10f, 0, 1)), r = smooth(clampf((th - 0.22f) / 0.20f, 0, 1));
        lerp3(R1, W1, w, R); if (th > 0.10f) lerp3(W1, F1, s2, R); if (th > 0.22f) lerp3(F1, D1, r, R);
        pitch = -0.20f - 0.55f * s2; inHand = th < 0.14f;
    }
    /* mano derecha: antebrazo, palma debajo, dedos sobre la palanca y pulgar */
    { float a0[3]; nadeSpace(R, yaw, pitch, roll); wpt(0.0f, -0.058f, -0.055f, a0);
      const float anc[3] = { 0.30f, -0.50f, 0.20f }; limb(anc, a0, 0.040f, SL, SP);
      if (inHand) nadeBody(u < 0.5f && bfNadeSt == 1);
      wbox(0, -0.048f, -0.010f, 0.034f, 0.012f, 0.034f, GLV);
      wbox(0.041f, 0.000f, -0.010f, 0.010f, 0.040f, 0.032f, GLV); wbox(0.046f, 0.030f, -0.010f, 0.006f, 0.010f, 0.026f, KNU);
      wbox(-0.030f, 0.004f, -0.036f, 0.012f, 0.022f, 0.010f, GLV); }
    /* mano izquierda: llega al seguro, tira y se retira con el pasador */
    if (bfNadeSt == 1) {
        float PP[3] = { R[0] - 0.045f, R[1] + 0.052f, R[2] - 0.010f }, L0[3] = { -0.26f, -0.46f, 0.28f }, P1[3] = { PP[0] - 0.12f, PP[1] - 0.010f, PP[2] - 0.030f }, P2[3] = { -0.32f, -0.50f, 0.25f };
        if (u < 0.28f) { Lh[0] = L0[0]; Lh[1] = L0[1]; Lh[2] = L0[2]; }
        else if (u < 0.50f) lerp3(L0, PP, smooth(clampf((u - 0.28f) / 0.22f, 0, 1)), Lh);
        else if (u < 0.64f) lerp3(PP, P1, smooth(clampf((u - 0.50f) / 0.14f, 0, 1)), Lh);
        else lerp3(P1, P2, smooth(clampf((u - 0.70f) / 0.30f, 0, 1)), Lh);
        float a0[3]; nadeSpace(Lh, 0.20f, -0.10f, 0.10f); wpt(0.0f, -0.030f, -0.045f, a0);
        const float anc[3] = { -0.30f, -0.50f, 0.20f }; limb(anc, a0, 0.040f, SP, SL);
        wbox(0.0f, 0.0f, -0.020f, 0.022f, 0.030f, 0.012f, GLV); wbox(0.022f, 0.010f, -0.004f, 0.010f, 0.007f, 0.018f, GLV); wbox(0.022f, -0.010f, -0.004f, 0.010f, 0.007f, 0.018f, GLV);
        if (u >= 0.5f && u < 0.97f) {                    /* pasador con anilla en la mano */
            wbox(0.034f, 0.0f, 0, 0.0025f, 0.0025f, 0.020f, M1);
            wbox(0.046f, 0.010f, 0, 0.010f, 0.0025f, 0.0025f, M1); wbox(0.046f, -0.010f, 0, 0.010f, 0.0025f, 0.0025f, M1);
            wbox(0.056f, 0.0f, 0, 0.0025f, 0.010f, 0.0025f, M1); wbox(0.036f, 0.0f, 0.0f, 0.0025f, 0.010f, 0.0025f, M1);
        }
    }
}
/* ---- animacion de poner una placa de armadura en primera persona (copiada del video de referencia) ----
 * Solo se ve la mano derecha: sube desde abajo a la derecha con la placa grande y pegada a la camara, la inclina hacia ti
 * (se ve la etiqueta), la baja hacia el pecho y la mete en el chaleco saliendo por abajo a la derecha. La camara hace una
 * pequena reverencia hacia abajo mientras dura. */
static void plateKey(const float (*K)[7], int n, float u, float *o) {     /* K[i] = { instante 0..1, x, y, z, giro, cabeceo, alabeo }; interpola suave entre claves */
    if (u <= K[0][0]) { for (int j = 0; j < 6; j++) o[j] = K[0][j + 1]; return; }
    for (int i = 1; i < n; i++) if (u <= K[i][0]) {
        float s = smooth((u - K[i - 1][0]) / (K[i][0] - K[i - 1][0]));
        for (int j = 0; j < 6; j++) o[j] = K[i - 1][j + 1] + (K[i][j + 1] - K[i - 1][j + 1]) * s;
        return;
    }
    for (int j = 0; j < 6; j++) o[j] = K[n - 1][j + 1];
}
#define PLATE_IN 0.46f      /* la placa aparece en la mano */
#define PLATE_OUT 0.95f     /* la placa ya esta dentro del chaleco: desaparece */
static float bfPlateDip(void) {            /* cuanto baja la camara (radianes) durante la animacion */
    if (!bfPlateSt) return 0;
    return 0.08f * smooth((bfPlateU - 0.45f) / 0.15f) * (1 - smooth((bfPlateU - 0.92f) / 0.08f));
}
static void plateBody(void) {              /* placa grande (ceramica azul oscuro con fibras verticales y etiqueta plateada); la mano derecha la agarra por el borde derecho */
    u32c FACE = RGBA(30, 36, 46, 255), EDGE = RGBA(70, 78, 88, 255), RIB = RGBA(46, 54, 66, 255), TAG = RGBA(196, 200, 204, 255), TAG2 = RGBA(110, 114, 120, 255);
    const float cx = -0.040f, cy = -0.222f, cz = 0.020f, hw = 0.150f, hh = 0.175f, hd = 0.010f, fz = cz - hd - 0.001f;
    wbox(cx, cy, cz, hw, hh, hd, FACE);
    wbox(cx, cy + hh + 0.020f, cz, hw * 0.74f, 0.020f, hd, FACE);                                 /* hombros recortados */
    wbox(cx - hw, cy, cz, 0.004f, hh, hd + 0.002f, EDGE); wbox(cx + hw, cy, cz, 0.004f, hh, hd + 0.002f, EDGE);
    wbox(cx, cy - hh, cz, hw, 0.004f, hd + 0.002f, EDGE); wbox(cx, cy + hh + 0.040f, cz, hw * 0.74f, 0.004f, hd + 0.002f, EDGE);
    for (int i = -4; i <= 4; i++) wbox(cx + i * 0.030f, cy + 0.005f, fz, 0.0022f, hh * 0.97f, 0.0008f, RIB);   /* fibras verticales */
    wbox(cx - 0.040f, cy - 0.040f, fz - 0.001f, 0.050f, 0.032f, 0.0012f, TAG);                    /* etiqueta plateada */
    wbox(cx - 0.040f, cy - 0.030f, fz - 0.002f, 0.032f, 0.003f, 0.0008f, TAG2); wbox(cx - 0.040f, cy - 0.047f, fz - 0.002f, 0.032f, 0.003f, 0.0008f, TAG2);
}
static void plateHand(const float *p, int withPlate) {      /* mano derecha: agarra la placa POR ARRIBA (palma y pulgar sobre la cara, dedos por encima del borde) */
    u32c GLV = RGBA(34, 36, 40, 255), KNU = RGBA(70, 72, 78, 255), SK = RGBA(30, 32, 36, 255), SK2 = RGBA(24, 26, 30, 255), SKIN = RGBA(176, 128, 96, 255);
    float a0[3];
    nadeSpace(p, p[3], p[4], p[5]); wpt(0.0f, -0.030f, -0.040f, a0);
    { const float anc[3] = { 0.75f, -0.25f, 0.12f }; limb(anc, a0, 0.040f, SK, SK2); }            /* manga oscura que viene de la derecha, como en el video */
    if (withPlate) plateBody();
    wbox(0.000f, -0.028f, -0.024f, 0.042f, 0.032f, 0.012f, GLV);                                   /* palma sobre la cara de la placa, junto al borde de arriba */
    wbox(0.000f, 0.010f, 0.012f, 0.040f, 0.012f, 0.024f, GLV);                                     /* dedos por encima del borde de arriba, hacia atras */
    wbox(0.010f, 0.024f, 0.012f, 0.030f, 0.004f, 0.020f, KNU);                                     /* nudillos */
    wbox(-0.034f, -0.070f, -0.030f, 0.011f, 0.020f, 0.008f, SKIN);                                 /* pulgar con yema de piel sobre la cara */
}
static void plateHandL(const float *p, float open) {      /* mano izquierda: abre la solapa del chaleco para que entre la placa; al principio solo asoma la punta por la esquina inferior izquierda */
    u32c GLV = RGBA(34, 36, 40, 255), SK = RGBA(30, 32, 36, 255), SK2 = RGBA(24, 26, 30, 255), SKIN = RGBA(176, 128, 96, 255), FLAP = RGBA(46, 52, 62, 255), INS = RGBA(10, 12, 14, 255);
    float a0[3];
    nadeSpace(p, p[3], p[4], p[5]); wpt(0.0f, -0.030f, -0.045f, a0);
    { const float anc[3] = { -0.90f, -1.20f, 0.12f }; limb(anc, a0, 0.040f, SK, SK2); }
    wbox(0.0f, 0.0f, -0.020f, 0.022f, 0.030f, 0.012f, GLV); wbox(0.022f, 0.010f, -0.004f, 0.010f, 0.007f, 0.018f, GLV); wbox(0.022f, -0.010f, -0.004f, 0.010f, 0.007f, 0.018f, GLV);
    wbox(0.034f, 0.010f, -0.004f, 0.005f, 0.006f, 0.015f, SKIN);                                   /* yema del dedo (piel) */
    if (open > 0.01f) {                                                                            /* solapa abierta: interior oscuro detras de la placa, labio y borde por delante */
        wbox(0.105f, -0.020f, 0.090f, 0.090f * open, 0.075f, 0.003f, INS);
        wbox(0.105f, -0.085f, -0.012f, 0.100f * open, 0.026f, 0.004f, FLAP);
        wbox(-0.015f, -0.020f, -0.012f, 0.012f * open, 0.070f, 0.004f, FLAP);
    }
}
static void drawPlateVM(void) {
    /* Linea de tiempo (u = (t_video - 0.40) / 1.65): 0-0.27 la derecha entra abajo a abrir el chaleco y se hunde a por la placa; la izquierda asoma
     * y aguanta la solapa; 0.46-0.60 sube la placa; 0.60-0.85 la muestra; 0.78-0.95 la izquierda abre el chaleco y la derecha empuja la placa hacia abajo, dentro. */
    static const float KR[][7] = {      /* mano derecha: { instante 0..1, x, y, z, giro, cabeceo, alabeo } */
        { 0.00f, 0.600f, -0.700f, 0.300f, 0.30f, -0.50f, 0.40f }, { 0.06f, 0.420f, -0.300f, 0.300f, 0.30f, -0.50f, 0.40f },
        { 0.12f, 0.300f, -0.180f, 0.300f, 0.30f, -0.50f, 0.30f }, { 0.20f, 0.300f, -0.400f, 0.300f, 0.30f, -0.50f, 0.30f },
        { 0.27f, 0.450f, -0.650f, 0.310f, 0.30f, -0.55f, 0.50f }, { 0.42f, 0.550f, -0.750f, 0.300f, 0.30f, -0.60f, 0.55f },
        { 0.50f, 0.308f, -0.434f, 0.362f, 0.30f, -0.60f, 0.55f }, { 0.55f, 0.159f, -0.108f, 0.349f, 0.25f, -0.60f, 0.50f },
        { 0.61f, 0.082f, -0.020f, 0.355f, 0.25f, -0.65f, 0.15f }, { 0.67f, 0.122f, -0.042f, 0.357f, 0.25f, -0.65f, -0.20f },
        { 0.72f, 0.132f, -0.062f, 0.357f, 0.25f, -0.65f, -0.20f }, { 0.79f, 0.074f, -0.090f, 0.353f, 0.20f, -0.62f, -0.18f },
        { 0.85f, 0.050f, -0.115f, 0.360f, 0.20f, -0.60f, -0.15f }, { 0.90f, 0.060f, -0.170f, 0.380f, 0.20f, -0.75f, -0.10f },
        { 0.94f, 0.080f, -0.215f, 0.390f, 0.20f, -0.85f, -0.05f }, { 0.98f, 0.200f, -0.450f, 0.340f, 0.30f, -0.80f, 0.20f },
        { 1.00f, 0.600f, -0.800f, 0.300f, 0.30f, -0.50f, 0.40f } };
    static const float KL[][7] = {      /* mano izquierda */
        { 0.00f, -0.450f, -0.600f, 0.280f, 0.20f, -0.20f, 0.10f }, { 0.10f, -0.340f, -0.140f, 0.280f, 0.20f, -0.20f, 0.10f },
        { 0.25f, -0.270f, -0.100f, 0.280f, 0.20f, -0.20f, 0.10f }, { 0.33f, -0.300f, -0.140f, 0.280f, 0.20f, -0.20f, 0.15f },
        { 0.45f, -0.270f, -0.100f, 0.280f, 0.20f, -0.20f, 0.10f }, { 0.76f, -0.270f, -0.110f, 0.280f, 0.20f, -0.20f, 0.10f },
        { 0.84f, -0.140f, -0.100f, 0.220f, 0.20f, -0.30f, 0.05f }, { 0.95f, -0.140f, -0.100f, 0.220f, 0.20f, -0.30f, 0.05f },
        { 1.00f, -0.450f, -0.600f, 0.280f, 0.20f, -0.20f, 0.10f } };
    float u = bfPlateU, R[6], L[6];
    plateKey(KR, 17, u, R); plateHand(R, u >= PLATE_IN && u < PLATE_OUT);
    if (u >= 0.06f && u < 0.98f) {
        float open = smooth((u - 0.78f) / 0.08f) * (1 - smooth((u - 0.95f) / 0.04f));
        plateKey(KL, 9, u, L); plateHandL(L, open);
    }
}
#endif
static void drawWeaponModel(void) {
    float kick = wKick * 0.06f, swap = wSwap > 0 ? fminf(wSwap / SWAP_T, 1.4f) : 0;
    float spd = clampf(sqrtf(P.vx * P.vx + P.vz * P.vz) / 5.4f, 0, 1) * (P.ground ? 1.0f : 0.0f);
#ifdef PS2_ONLINE
    if (bfNadeSt) {          /* al sacar la granada el arma baja; al lanzar vuelve a subir */
        float nl = bfNadeSt == 1 ? smooth(clampf(bfNadeU / 0.28f, 0, 1)) : 1 - smooth(clampf((bfThrowT - 0.20f) / 0.22f, 0, 1));
        if (nl > 0) swap = fmaxf(swap, 1.7f * nl);
    }
    if (bfPlateSt) {         /* al ponerte la placa el arma baja y luego vuelve a subir */
        float pl = smooth(clampf(bfPlateU / 0.14f, 0, 1)) * (1 - smooth(clampf((bfPlateU - 0.86f) / 0.14f, 0, 1)));
        swap = fmaxf(swap, 1.7f * pl);
    }
#endif
    drawVM(wsel, kick, swap, wReload > 0 ? clampf(1 - wReload / W[wsel].reload, 0, 1) : 0, wReload > 0, spd);
#ifdef PS2_ONLINE
    if (bfNadeSt) drawNadeVM();
    if (bfPlateSt) drawPlateVM();
#endif
}
static void drawCross(void) {
    float cx = SW * 0.5f, cy = SH * 0.5f, t = SH / 448.0f, len = 7 * t, gap = 5 * t, th = 1.2f * t;
    u32c c = CROSS_COL[S.cross];
#ifdef PS2_ONLINE
    {   /* la mira se abre con el movimiento y los disparos seguidos (refleja la dispersion real); apuntando queda solo un punto */
        float spd = sqrtf(P.vx * P.vx + P.vz * P.vz);
        float sprd = W[wsel].spread * (1 + clampf(spd / 5, 0, 1) * 2.2f + (P.ground ? 0 : 3)) * (1 - 0.6f * bfAds) * (1 + bfBloom);
        gap = fminf(3 * t + sprd * camF, 34 * t); len = 5 * t;
        if (bfAds > 0.6f) { gfx_rect(cx - 2.6f * t, cy - 2.6f * t, cx + 2.6f * t, cy + 2.6f * t, RGBA(0, 0, 0, 150)); gfx_rect(cx - 1.5f * t, cy - 1.5f * t, cx + 1.5f * t, cy + 1.5f * t, c); goto bf_hit; }
    }
#endif
    gfx_rect(cx - gap - len, cy - th, cx - gap, cy + th, c); gfx_rect(cx + gap, cy - th, cx + gap + len, cy + th, c);
    gfx_rect(cx - th, cy - gap - len, cx + th, cy - gap, c); gfx_rect(cx - th, cy + gap, cx + th, cy + gap + len, c);
#ifdef PS2_ONLINE
bf_hit:
#endif
    if (hitFlash > 0) {
        u32c h = hmT > 0.9f ? RGBA(229, 72, 77, 255) : RGBA(255, 255, 255, 255); float a = 9 * t, b = 16 * t;
        gfx_rect(cx - b, cy - b, cx - a, cy - b + 2 * t, h); gfx_rect(cx + a, cy - b, cx + b, cy - b + 2 * t, h);
        gfx_rect(cx - b, cy + b - 2 * t, cx - a, cy + b, h); gfx_rect(cx + a, cy + b - 2 * t, cx + b, cy + b, h);
    }
}
static void drawHud(void) {
    float t = SH / 224.0f, mx = SW * 0.07f, my = SH * 0.08f; char buf[64];
    drawCross();
    if (hurtT > 0) { int a = (int)(hurtT / 0.25f * 120); float e = SH * 0.08f; u32c c = RGBA(229, 72, 77, a);
        gfx_rect(0, 0, SW, e, c); gfx_rect(0, SH - e, SW, SH, c); gfx_rect(0, e, e, SH - e, c); gfx_rect(SW - e, e, SW, SH - e, c); }
#ifdef PS2_ONLINE
    {   /* indicadores de direccion de dano: flechas rojas alrededor de la mira que giran con la camara */
        float fx = -sinf(P.yaw), fz = -cosf(P.yaw), rx = cosf(P.yaw), rz = -sinf(P.yaw), cx = SW * 0.5f, cy = SH * 0.5f, R = SH * 0.25f;
        for (int i = 0; i < 4; i++) if (bfDT[i] > 0) {
            float fw = bfDX[i] * fx + bfDZ[i] * fz, rt = bfDX[i] * rx + bfDZ[i] * rz, sx = rt, sy = -fw, qx = -sy, qy = sx;
            int a = (int)clampf(bfDT[i] / 0.5f, 0, 1) * 0 + (int)(clampf(bfDT[i] / 0.5f, 0, 1) * 210);
            u32c col = RGBA(229, 72, 77, a), col2 = RGBA(229, 72, 77, a / 3);
            gfx_tri(cx + sx * (R + 11 * t), cy + sy * (R + 11 * t), 16777215, col, cx + sx * R + qx * 6 * t, cy + sy * R + qy * 6 * t, 16777215, col2, cx + sx * R - qx * 6 * t, cy + sy * R - qy * 6 * t, 16777215, col2);
        }
        if (bfStam < 0.995f) {   /* aguante para correr */
            float w = SW * 0.14f, x0 = SW * 0.5f - w * 0.5f, y0 = SH - my * 0.55f;
            gfx_rect(x0, y0, x0 + w, y0 + 2 * t, RGBA(0, 0, 0, 150));
            gfx_rect(x0, y0, x0 + w * bfStam, y0 + 2 * t, bfStamLock ? RGBA(229, 72, 77, 255) : RGBA(232, 236, 233, 230));
        }
    }
#endif
#ifdef PS2_ONLINE
    if (bfFlashT > 0) gfx_rect(0, 0, SW, SH, RGBA(255, 244, 220, (int)(clampf(bfFlashT, 0, 1) * 230)));    /* fogonazo de la explosion */
    if (P.alive && P.hp < 35) { int a = (int)((35 - P.hp) / 35 * 110); float e = SH * 0.14f;             /* poca vida: bordes oscuros */
        gfx_rect(0, 0, SW, e, RGBA(60, 0, 0, a)); gfx_rect(0, SH - e, SW, SH, RGBA(60, 0, 0, a)); gfx_rect(0, e, e, SH - e, RGBA(60, 0, 0, a)); gfx_rect(SW - e, e, SW, SH - e, RGBA(60, 0, 0, a)); }
#endif
    /* vida */
    drawText(mx, SH - my - 14 * t - 6 * t, t * 0.8f, "VIDA", RGBA(143, 160, 166, 255));
    snprintf(buf, 64, "%d", (int)ceilf(P.hp));
    drawText(mx, SH - my - 12 * t, t * 1.6f, buf, RGBA(232, 236, 233, 255));
    float bw = SW * 0.2f; u32c hc = P.hp > 60 ? RGBA(111, 207, 151, 255) : (P.hp > 30 ? RGBA(224, 128, 58, 255) : RGBA(229, 72, 77, 255));
    gfx_rect(mx, SH - my + 2 * t, mx + bw, SH - my + 4 * t, RGBA(0, 0, 0, 160)); gfx_rect(mx, SH - my + 2 * t, mx + bw * P.hp / 100, SH - my + 4 * t, hc);
#ifdef PS2_ONLINE
    {   /* escudo: tres placas encima de la vida (azul = llena, naranja = casi rota; la que se esta poniendo se rellena en blanco) */
        float pg = 2 * t, pw = (bw - pg * (PLATE_N - 1)) / PLATE_N, py1 = SH - my - 29 * t, py2 = SH - my - 24 * t;
        int cur = (int)(bfArmor / PLATE_HP + 0.001f);
        for (int i = 0; i < PLATE_N; i++) {
            float x0 = mx + i * (pw + pg), x1 = x0 + pw, f = clampf((bfArmor - i * PLATE_HP) / PLATE_HP, 0, 1);
            gfx_rect(x0, py1, x1, py2, RGBA(0, 0, 0, 160));
            if (f > 0) gfx_rect(x0, py1, x0 + pw * f, py2, f < 0.35f ? RGBA(224, 128, 58, 255) : RGBA(150, 196, 232, 255));
            if (bfPlateSt && i == cur) gfx_rect(x0, py1, x0 + pw * bfPlateU, py2, RGBA(232, 244, 255, 210));
        }
        snprintf(buf, 64, "x%d", bfPlates); drawText(mx + bw + 3 * t, py1 - t, t * 0.8f, buf, bfPlates > 0 ? RGBA(143, 160, 166, 255) : RGBA(229, 72, 77, 255));
    }
#endif
    /* municion */
    snprintf(buf, 64, "%d / %d", ammoMag[wsel], ammoRes[wsel]);
    drawText(SW - mx - textW(buf, t * 1.6f), SH - my - 12 * t, t * 1.6f, buf, wReload > 0 ? RGBA(224, 128, 58, 255) : RGBA(232, 236, 233, 255));
    drawText(SW - mx - textW(W[wsel].name, t * 0.8f), SH - my - 20 * t, t * 0.8f, wReload > 0 ? "RECARGANDO" : W[wsel].name, RGBA(143, 160, 166, 255));
#ifdef PS2_ONLINE
    if (!netMode) { snprintf(buf, 64, "GRANADAS %d", bfNades); drawText(SW - mx - textW(buf, t * 0.8f), SH - my - 28 * t, t * 0.8f, buf, bfNades > 0 ? RGBA(143, 160, 166, 255) : RGBA(229, 72, 77, 255)); }
#endif
    /* marcador */
    if (sandbox) {
        drawTextC(SW * 0.5f, my * 0.7f, t * 1.0f, "CAMPO DE PRUEBAS", RGBA(224, 128, 58, 255));
        snprintf(buf, 64, "SOLDADOS %d   BAJAS %d", aliveBots(), kills); drawTextC(SW * 0.5f, my * 0.7f + 10 * t, t * 0.8f, buf, RGBA(143, 160, 166, 255));
        drawTextC(SW * 0.5f, my * 0.7f + 19 * t, t * 0.65f, "ARRIBA: SOLDADO   ABAJO: BORRAR", RGBA(143, 160, 166, 255));
    } else {
    snprintf(buf, 64, "RONDA %d", roundNo); drawTextC(SW * 0.5f, my * 0.7f, t * 1.0f, buf, RGBA(232, 236, 233, 255));
    snprintf(buf, 64, "TU %d - %d BOTS", scoreMe, scoreBots); drawTextC(SW * 0.5f, my * 0.7f + 10 * t, t * 0.9f, buf, RGBA(91, 180, 214, 255));
    snprintf(buf, 64, "VIVOS %d / %d", aliveBots(), nbots); drawTextC(SW * 0.5f, my * 0.7f + 19 * t, t * 0.7f, buf, RGBA(143, 160, 166, 255));
    }
    /* radar */
    float rs = SH * 0.17f, rx0 = mx, ry0 = my, rc = rs * 0.5f, R = 24.0f;
    gfx_rect(rx0, ry0, rx0 + rs, ry0 + rs, RGBA(8, 11, 13, 150));
    gfx_rect(rx0 + rc - 0.5f, ry0, rx0 + rc + 0.5f, ry0 + rs, RGBA(255, 255, 255, 50)); gfx_rect(rx0, ry0 + rc - 0.5f, rx0 + rs, ry0 + rc + 0.5f, RGBA(255, 255, 255, 50));
    float ca = cosf(P.yaw), sa = sinf(P.yaw);
    for (int i = 0; i < nbots; i++) {
#ifdef PS2_ONLINE
        if (!bots[i].alive || bfSpot[i] <= 0) continue;
#else
        if (!bots[i].alive || netMode) continue;
#endif
        float dx = bots[i].x - P.x, dz = bots[i].z - P.z, px = (dx * ca - dz * sa) / R * rc, py = (dx * sa + dz * ca) / R * rc;
        if (fabsf(px) > rc - 3 || fabsf(py) > rc - 3) continue;
#ifdef PS2_ONLINE
        { int al = (int)(clampf(bfSpot[i] / 0.5f, 0, 1) * 255); gfx_rect(rx0 + rc + px - 2, ry0 + rc + py - 2, rx0 + rc + px + 2, ry0 + rc + py + 2, RGBA(229, 72, 77, al)); }
#else
        gfx_rect(rx0 + rc + px - 2, ry0 + rc + py - 2, rx0 + rc + px + 2, ry0 + rc + py + 2, RGBA(229, 72, 77, 255));
#endif
    }
#ifdef PS2_ONLINE
    { float h = rc * 0.9f; gfx_tri(rx0 + rc, ry0 + rc, 16777215, RGBA(91, 180, 214, 70), rx0 + rc - h * 0.7f, ry0 + rc - h, 16777215, RGBA(91, 180, 214, 0), rx0 + rc + h * 0.7f, ry0 + rc - h, 16777215, RGBA(91, 180, 214, 0)); }   /* cono de vision */
#endif
    gfx_rect(rx0 + rc - 2, ry0 + rc - 2, rx0 + rc + 2, ry0 + rc + 2, RGBA(91, 180, 214, 255));
    /* avisos de baja */
    for (int i = 0; i < 4; i++) if (feedT[i] > 0 && feed[i][0]) drawText(SW - mx - textW(feed[i], t * 0.8f), my + i * 9 * t, t * 0.8f, feed[i], RGBA(232, 236, 233, 255));
    if (bannerT > 0) { drawTextC(SW * 0.5f, SH * 0.3f, t * 2.0f, bannerA, RGBA(232, 236, 233, 255)); drawTextC(SW * 0.5f, SH * 0.3f + 18 * t, t * 0.9f, bannerB, RGBA(143, 160, 166, 255)); }
}

/* ---------- Menus ---------- */
typedef struct { const char *label; } Item;
static void drawMenuBg(void) { gfx_rect(0, 0, SW, SH, RGBA(8, 11, 13, 150)); }
static void drawList(const char **items, const char **vals, int n, float y0) {
    float t = SH / 224.0f, w = SW * 0.62f, x0 = (SW - w) * 0.5f, rh = 12 * t;
    for (int i = 0; i < n; i++) {
        float y = y0 + i * (rh + 2 * t); int on = i == sel;
        gfx_rect(x0, y, x0 + w, y + rh, on ? RGBA(224, 128, 58, 255) : RGBA(19, 26, 31, 230));
        u32c tc = on ? RGBA(23, 16, 10, 255) : RGBA(232, 236, 233, 255);
        drawText(x0 + 5 * t, y + 2.5f * t, t * 1.0f, items[i], tc);
        if (vals && vals[i]) {
            char b[40]; snprintf(b, 40, on ? "< %s >" : "%s", vals[i]);
            drawText(x0 + w - 5 * t - textW(b, t), y + 2.5f * t, t, b, tc);
        }
    }
}
static void drawTitle(void) {
    float t = SH / 224.0f;
    drawTextC(SW * 0.5f, SH * 0.1f, t * 4.5f, "PUNTO CERO", RGBA(232, 236, 233, 255));
    gfx_rect(SW * 0.5f - 60 * t, SH * 0.1f + 36 * t, SW * 0.5f + 60 * t, SH * 0.1f + 38 * t, RGBA(224, 128, 58, 255));
    drawTextC(SW * 0.5f, SH * 0.1f + 42 * t, t * 0.9f, "TACTICO POR RONDAS - 1 CONTRA BOTS", RGBA(143, 160, 166, 255));
    drawTextC(SW * 0.5f, SH - 9 * t, t * 0.62f, "(C) 2026 LM STUDIOS - TODOS LOS DERECHOS RESERVADOS", RGBA(110, 124, 130, 255));
}
static char vbuf[8][24];
static void settingsLabels(const char **vals) {
    vals[0] = DIFF_NAMES[S.diff];
    snprintf(vbuf[1], 24, "%d", S.bots); vals[1] = vbuf[1];
    snprintf(vbuf[2], 24, "%d", S.rounds); vals[2] = vbuf[2];
    snprintf(vbuf[3], 24, "%d", S.sens); vals[3] = vbuf[3];
    snprintf(vbuf[4], 24, "%d", S.fov); vals[4] = vbuf[4];
    vals[5] = S.invertY ? "SI" : "NO"; vals[6] = CROSS_NAMES[S.cross];
    snprintf(vbuf[7], 24, "%d", S.volume); vals[7] = vbuf[7]; vals[8] = SKIN_NAMES[S.skin]; vals[9] = CAM_NAMES[S.cam]; vals[10] = MAPS[S.map].name; vals[11] = NULL;
}
static void changeSetting(int i, int dir) {
    switch (i) {
    case 0: S.diff = (S.diff + dir + 3) % 3; break;
    case 1: S.bots = (int)clampf((float)(S.bots + dir), 1, 8); break;
    case 2: { int k = 0; for (int j = 0; j < 4; j++) if (ROUND_OPTS[j] == S.rounds) k = j; S.rounds = ROUND_OPTS[(k + dir + 4) % 4]; break; }
    case 3: S.sens = (int)clampf((float)(S.sens + dir), 1, 10); break;
    case 4: S.fov = (int)clampf((float)(S.fov + dir * 5), 60, 100); break;
    case 5: S.invertY = !S.invertY; break;
    case 6: S.cross = (S.cross + dir + 5) % 5; break;
    case 9: S.cam = !S.cam; break;
    case 10: S.map = (S.map + dir + NMAPS) % NMAPS; if (settingsFrom == M_MENU) { curMap = S.map; buildWorld(); } break;
    case 8: S.skin = (S.skin + dir + 4) % 4; break;
    case 7: S.volume = (int)clampf((float)(S.volume + dir), 0, 10); snd_volume(S.volume); break;
    }
    snd_play(SND_TICK, 0.6f);
}
static void menuNav(const Pad *pd, int n) {
    if (pd->pressed & B_UP) { sel = (sel + n - 1) % n; snd_play(SND_TICK, 0.6f); }
    if (pd->pressed & B_DOWN) { sel = (sel + 1) % n; snd_play(SND_TICK, 0.6f); }
}


#ifdef PC_ONLINE
/* ---------- Opciones de video, guardado de configuracion ---------- */
static const int RES_W[5] = { 800, 1024, 1280, 1600, 1920 }, RES_H[5] = { 600, 768, 720, 900, 1080 };
static const char *RES_NAMES[5] = { "800X600", "1024X768", "1280X720", "1600X900", "1920X1080" };
/* ---- Controles de PC: acciones configurables ---- */
static const char *ACT_NAMES[PC_NACT] = { "AVANZAR", "RETROCEDER", "IZQUIERDA", "DERECHA", "DISPARAR", "APUNTAR", "RECARGAR", "AGACHARSE", "CAMINAR SILENCIOSO", "SALTAR / ACEPTAR", "ARMA SIGUIENTE", "ARMA ANTERIOR", "PAUSA", "GRANADA" };
static const int KB_DEF[PC_NACT][2] = { { 'W', 0 }, { 'S', 0 }, { 'A', 0 }, { 'D', 0 }, { 0x01, 0x11 }, { 0x02, 0 }, { 'R', 0 }, { 'C', 0 }, { 0x10, 0 }, { 0x20, 0 }, { 'E', 0 }, { 'Q', 0 }, { 'P', 0 }, { 'G', 0 } };
int pc_bind[PC_NACT][2] = { { 'W', 0 }, { 'S', 0 }, { 'A', 0 }, { 'D', 0 }, { 0x01, 0x11 }, { 0x02, 0 }, { 'R', 0 }, { 'C', 0 }, { 0x10, 0 }, { 0x20, 0 }, { 'E', 0 }, { 'Q', 0 }, { 'P', 0 }, { 'G', 0 } };
static const char *keyName(int vk, char *buf) {
    if (vk == 0) return "---";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { buf[0] = (char)vk; buf[1] = 0; return buf; }
    if (vk >= 0x70 && vk <= 0x87) { snprintf(buf, 8, "F%d", vk - 0x6F); return buf; }
    if (vk >= 0x60 && vk <= 0x69) { snprintf(buf, 8, "NUM %d", vk - 0x60); return buf; }
    switch (vk) {
    case 0x01: return "CLIC IZQ"; case 0x02: return "CLIC DER"; case 0x04: return "CLIC MEDIO"; case 0x05: return "RATON 4"; case 0x06: return "RATON 5";
    case 0x08: return "RETROCESO"; case 0x09: return "TAB"; case 0x0D: return "ENTER"; case 0x10: return "SHIFT"; case 0x11: return "CTRL"; case 0x14: return "BLOQ MAYUS";
    case 0x1B: return "ESC"; case 0x20: return "ESPACIO"; case 0x21: return "RE PAG"; case 0x22: return "AV PAG"; case 0x23: return "FIN"; case 0x24: return "INICIO";
    case 0x25: return "FLECHA IZQ"; case 0x26: return "FLECHA ARR"; case 0x27: return "FLECHA DER"; case 0x28: return "FLECHA ABA"; case 0x2D: return "INSERT"; case 0x2E: return "SUPR";
    case 0xBA: return ";"; case 0xBB: return "+"; case 0xBC: return ","; case 0xBD: return "-"; case 0xBE: return "."; case 0xBF: return "/"; case 0xC0: return "N"; case 0xDB: return "["; case 0xDC: return "\\"; case 0xDD: return "]"; case 0xDE: return "'";
    }
    snprintf(buf, 8, "VK %d", vk); return buf;
}
static void keysDefault(void) { for (int a = 0; a < PC_NACT; a++) { pc_bind[a][0] = KB_DEF[a][0]; pc_bind[a][1] = KB_DEF[a][1]; } }
static int keysEdit = 0, keysSlot = 0, keysWait = 0;      /* keysWait: esperando que el jugador pulse una tecla */
static void keysAssign(int a, int slot, int vk) {
    int old = pc_bind[a][slot];
    for (int a2 = 0; a2 < PC_NACT; a2++) for (int s2 = 0; s2 < 2; s2++) if (pc_bind[a2][s2] == vk && !(a2 == a && s2 == slot)) pc_bind[a2][s2] = old;    /* la tecla ya usada pasa a ocupar el lugar anterior */
    pc_bind[a][slot] = vk;
    for (int s = 0; s < 2; s++) if (pc_bind[a][s] && pc_bind[a][s] == pc_bind[a][1 - s] && s == 1) pc_bind[a][1] = 0;
}

typedef struct { int res, bright, full; } Vid;
static Vid vidCur = { 2, 50, 0 }, vidNew = { 2, 50, 0 };
static const Vid VID_DEF = { 2, 50, 0 };
static int optsFrom = M_MENU, editing, ovl, edCur;
#ifdef PS2_ONLINE
static const int MAIN_ACT[] = { 0, 1, 2 }, PAUSE_ACT[] = { 0, 1, 3 };      /* en PS2 no hay OPCIONES de video ni SALIR */
#define MAIN_N 3
#define PAUSE_N 3
#else
static const int MAIN_ACT[] = { 0, 1, 2, 3, 4 }, PAUSE_ACT[] = { 0, 1, 2, 3 };
#define MAIN_N 5
#define PAUSE_N 4
#endif
static int cfgI(const char *v) { int sg = 1, r = 0; if (*v == '-') { sg = -1; v++; } while (*v >= '0' && *v <= '9') { r = r * 10 + (*v - '0'); v++; } return r * sg; }
static void cfgText(char *dst, const char *v, int max) {
    int n = 0;
    while (*v && *v != '\r' && *v != '\n' && n < max - 1) { char c = *v++; if (c >= 'a' && c <= 'z') c -= 32; if (c >= 32 && c <= 90) dst[n++] = c; }
    dst[n] = 0;
}
static int ceq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void cfgSave(void) {
    char b[600];
    snprintf(b, 600, "diff=%d\nbots=%d\nrounds=%d\nsens=%d\nfov=%d\ninvertY=%d\ncross=%d\nvolume=%d\nskin=%d\ncam=%d\nmap=%d\nres=%d\nbright=%d\nfull=%d\nname=%s\nip=%s\n",
             S.diff, S.bots, S.rounds, S.sens, S.fov, S.invertY, S.cross, S.volume, S.skin, S.cam, S.map, vidCur.res, vidCur.bright, vidCur.full, myName, joinText);
    pc_cfg_save(b, (int)strlen(b));
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static void cfgLoad(void) {
    char b[1024]; int n = pc_cfg_load(b, 1024); if (n <= 0) return;
    b[n < 1023 ? n : 1023] = 0;
    char *p = b;
    while (*p) {
        char *key = p; while (*p && *p != '\n') p++; if (*p) *p++ = 0;
        char *v = key; while (*v && *v != '=') v++; if (!*v) continue; *v++ = 0;
        int x = cfgI(v);
        if (ceq(key, "diff")) S.diff = clampi(x, 0, 2);
        else if (ceq(key, "bots")) S.bots = clampi(x, 1, 8);
        else if (ceq(key, "rounds")) { for (int j = 0; j < 4; j++) if (ROUND_OPTS[j] == x) S.rounds = x; }
        else if (ceq(key, "sens")) S.sens = clampi(x, 1, 10);
        else if (ceq(key, "fov")) S.fov = clampi(x, 60, 100);
        else if (ceq(key, "invertY")) S.invertY = x != 0;
        else if (ceq(key, "cross")) S.cross = clampi(x, 0, 4);
        else if (ceq(key, "volume")) S.volume = clampi(x, 0, 10);
        else if (ceq(key, "skin")) S.skin = clampi(x, 0, 3);
        else if (ceq(key, "cam")) S.cam = x != 0;
        else if (ceq(key, "map")) S.map = clampi(x, 0, NMAPS - 1);
        else if (ceq(key, "res")) vidCur.res = clampi(x, 0, 4);
        else if (ceq(key, "bright")) vidCur.bright = clampi(x, 0, 100);
        else if (ceq(key, "full")) vidCur.full = x != 0;
        else if (ceq(key, "name")) { cfgText(myName, v, 16); if (!myName[0]) strncpy(myName, "JUGADOR", 15); }
        else if (ceq(key, "ip")) { cfgText(joinText, v, 40); if (!joinText[0]) strncpy(joinText, "127.0.0.1", 39); }
    }
}
static void vidApply(void) {
    vidCur = vidNew;
    pc_video_apply(RES_W[vidCur.res], RES_H[vidCur.res], vidCur.full);
    pc_set_brightness(vidCur.bright);
    cfgSave();
}
#endif
void game_init(void) {
    srand(12345);
    voxBuild();
    buildWorld();
#ifdef PC_ONLINE
    cfgLoad(); curMap = S.map; buildWorld(); vidNew = vidCur; pc_video_apply(RES_W[vidCur.res], RES_H[vidCur.res], vidCur.full); pc_set_brightness(vidCur.bright);
#endif
    snd_gen(); snd_volume(S.volume);
    mode = M_MENU; sel = 0; orbit = 0;
    snd_music(1);
}

/* ---------- Contador de FPS (R3 lo muestra/oculta) ---------- */
static float fpsRawDt = 1.0f / 60.0f;       /* tiempo real del ultimo cuadro, sin recortar: lo escribe main() */
static float fpsShow = 60, fpsMs = 16.7f, fpsAcc; static int fpsFrames, fpsOn = 1;
static void fpsTick(void) {
    fpsAcc += fpsRawDt; fpsFrames++;
    if (fpsAcc >= 0.5f) { fpsShow = fpsFrames / fpsAcc; fpsMs = 1000.0f * fpsAcc / fpsFrames; fpsAcc = 0; fpsFrames = 0; }
}
static void fpsDraw(void) {
    if (!fpsOn) return;
    float t = SH / 224.0f, sc = t * 0.9f; char a[16], b[16];
    snprintf(a, 16, "%d FPS", (int)(fpsShow + 0.5f)); snprintf(b, 16, "%d.%d MS", (int)fpsMs, (int)(fpsMs * 10) % 10);
    float w = textW(a, sc) > textW(b, sc * 0.75f) ? textW(a, sc) : textW(b, sc * 0.75f), x1 = SW - 4 * t, x0 = x1 - w - 6 * t, y0 = 3 * t;
    u32c col = fpsShow >= 50 ? RGBA(111, 207, 151, 255) : (fpsShow >= 30 ? RGBA(224, 168, 58, 255) : RGBA(229, 72, 77, 255));
    gfx_rect(x0, y0, x1, y0 + 8 * sc + 8 * sc * 0.75f + 5 * t, RGBA(0, 0, 0, 140));
    drawText(x1 - 3 * t - textW(a, sc) + sc * 0.5f * 0, y0 + 2 * t, sc, a, col);
    drawText(x1 - 3 * t - textW(b, sc * 0.75f), y0 + 3 * t + 8 * sc, sc * 0.75f, b, RGBA(143, 160, 166, 255));
}
void game_frame(const Pad *pd, float dt) {
    float t = SH / 224.0f;
    if (pd->pressed & B_R3) fpsOn = !fpsOn;
    fpsTick();
    #ifdef PC_ONLINE
    { int f; if (pc_video_poll(&f)) { vidCur.full = vidNew.full = f; cfgSave(); } }
    mpPump(dt);
    if (!editing) { char tmp[16]; pc_text_input(tmp, 16); }
    snd_music(mode == M_MENU || mode == M_CODE || mode == M_LAB || mode == M_END || mode == M_MP_MENU || mode == M_MP_LOBBY || (mode == M_SETTINGS && settingsFrom == M_MENU) || (mode == M_OPTIONS && optsFrom == M_MENU));
#else
    snd_music(mode == M_MENU || mode == M_CODE || mode == M_LAB || mode == M_END || (mode == M_SETTINGS && settingsFrom == M_MENU));
#endif
    /* --- logica --- */
    if (hurtT > 0) hurtT -= dt;
    if (hitFlash > 0) hitFlash -= dt;
    if (bannerT > 0) bannerT -= dt;
    for (int i = 0; i < 4; i++) if (feedT[i] > 0) feedT[i] -= dt;
    int inGame = mode == M_PLAYING || mode == M_ROUNDEND || mode == M_PAUSE;
    if (inGame) charAnim(mode == M_PLAYING ? pd : NULL, dt);
    if (mode == M_PLAYING || mode == M_ROUNDEND) {
        int canAct = mode == M_PLAYING && freeze <= 0;
        if (mode == M_PLAYING && freeze > 0) freeze -= dt;
        const Pad *gp = pd;
#ifdef PC_ONLINE
        static const Pad nul;
        if (netMode && ovl) gp = &nul;
#endif
        updatePlayer(gp, dt, canAct);
        snd_listener(P.x, P.z, P.yaw);
        updateWeapon(gp, dt, canAct);
        if (sandbox) {
            if (canAct && (pd->pressed & B_UP)) labSpawn();
            if (canAct && (pd->pressed & B_DOWN)) { labClear(); snd_play(SND_TICK, 0.8f); }
            for (int i = 0; i < 4; i++) ammoRes[i] = W[i].reserve;
#ifdef PS2_ONLINE
            bfNades = 9; bfPlates = 9;
#endif
            P.hp = 100;
        }
        updateBots(dt, canAct);
#ifdef PS2_ONLINE
        bfUpdateNades(dt); bfUpdateFx(dt); goreUpdate(dt);
        for (int i = 0; i < 4; i++) if (bfDT[i] > 0) bfDT[i] -= dt;
        for (int i = 0; i < nbots; i++) {      /* radar tactico: solo se ven los enemigos que tienes a la vista o que acaban de disparar */
            if (bfSpot[i] > 0) bfSpot[i] -= dt;
            if (!bots[i].alive || !P.alive) continue;
            float o[3] = { P.x, P.y + 1.5f - 0.5f * P.cr, P.z }, d[3] = { bots[i].x - o[0], bots[i].py + 1.2f - o[1], bots[i].z - o[2] };
            float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (L < 34 && L > 0.1f) { d[0] /= L; d[1] /= L; d[2] /= L; if (wallDist(o, d, L) >= L - 0.5f) bfSpot[i] = fmaxf(bfSpot[i], 1.0f); }
        }
#endif
#ifdef PC_ONLINE
        mpUpdateRemote(dt);
#endif
        updateRigs(dt);
        if (mode == M_ROUNDEND) {
            afterT -= dt;
            if (afterT <= 0) {
                #ifdef PC_ONLINE
                if (netMode == 1) {
                    if (hostScore >= mpRounds || cliScore >= mpRounds) { unsigned char d[2] = { (unsigned char)hostScore, (unsigned char)cliScore }; mpEv(EV_MATCHEND, d, 2); mode = M_END; sel = 0; }
                    else { roundNo++; mpHostBeginRound(0); }
                } else if (netMode == 2) { afterT = 0.5f; }      /* el cliente espera la orden del anfitrion */
                else
#endif
                if (scoreMe >= ROUNDS() || scoreBots >= ROUNDS()) { mode = M_END; sel = 0; }
                else { roundNo++; startRound(); }
            }
        }
#ifdef PC_ONLINE
        if (netMode) {
            if (pd->pressed & B_START) { ovl = !ovl; sel = 0; }
            else if (ovl) {
                menuNav(pd, 2);
                if (pd->pressed & B_CIRCLE) ovl = 0;
                else if (pd->pressed & B_CROSS) { ovl = 0; if (sel == 1) mpLeave(NULL, M_MENU); }
            }
        } else
#endif
        if (mode == M_PLAYING && (pd->pressed & B_START)) { mode = M_PAUSE; sel = 0; prevMode = M_PLAYING; }
    } else if (mode == M_PAUSE && sandbox) {
        menuNav(pd, 5);
        if ((pd->pressed & B_START) || (pd->pressed & B_CIRCLE)) mode = prevMode;
        else {
            int d = ((pd->pressed & B_RIGHT) ? 1 : 0) - ((pd->pressed & B_LEFT) ? 1 : 0);
            if (sel == 3 && d) { S.skin = (S.skin + d + 4) % 4; snd_play(SND_TICK, 0.6f); }
            else if (pd->pressed & B_CROSS) {
                if (sel == 0) mode = prevMode;
                else if (sel == 1) { labSpawn(); mode = prevMode; }
                else if (sel == 2) { labClear(); mode = prevMode; }
                else if (sel == 3) { S.skin = (S.skin + 1) % 4; snd_play(SND_TICK, 0.6f); }
                else { mode = M_MENU; sel = 0; nbots = 0; sandbox = 0; }
            }
        }
    } else if (mode == M_PAUSE) {
#ifdef PC_ONLINE
        menuNav(pd, PAUSE_N);
        if ((pd->pressed & B_START) || (pd->pressed & B_CIRCLE)) mode = prevMode;
        else if (pd->pressed & B_CROSS) {
            int act = PAUSE_ACT[sel];
            if (act == 0) mode = prevMode;
            else if (act == 1) { settingsFrom = M_PAUSE; mode = M_SETTINGS; sel = 0; }
            else if (act == 2) { optsFrom = M_PAUSE; vidNew = vidCur; mode = M_OPTIONS; sel = 0; }
            else { mode = M_MENU; sel = 0; nbots = 0; }
        }
#else
        menuNav(pd, 3);
        if ((pd->pressed & B_START) || (pd->pressed & B_CIRCLE)) mode = prevMode;
        else if (pd->pressed & B_CROSS) {
            if (sel == 0) mode = prevMode;
            else if (sel == 1) { settingsFrom = M_PAUSE; mode = M_SETTINGS; sel = 0; }
            else { mode = M_MENU; sel = 0; nbots = 0; }
        }
#endif
    } else if (mode == M_MENU) {
        if (pd->pressed & B_SELECT) { mode = M_CODE; codePos = 0; memset(codeD, 0, sizeof codeD); codeBad = 0; sel = 0; snd_play(SND_TICK, 0.8f); }      /* SELECT: pide el codigo secreto */
#ifdef PC_ONLINE
        orbit += dt * 0.08f; menuNav(pd, MAIN_N);
        if (pd->pressed & B_CROSS) {
            switch (MAIN_ACT[sel]) {
            case 0: startMatch(); break;
            case 1: mode = M_MP_MENU; sel = 2; nbots = 0; break;
            case 2: settingsFrom = M_MENU; mode = M_SETTINGS; sel = 0; break;
            case 3: optsFrom = M_MENU; vidNew = vidCur; mode = M_OPTIONS; sel = 0; break;
            default: cfgSave(); pc_quit(); break;
            }
        }
#else
        orbit += dt * 0.08f; menuNav(pd, 2);
        if (pd->pressed & B_CROSS) { if (sel == 0) startMatch(); else { settingsFrom = M_MENU; mode = M_SETTINGS; sel = 0; } }
#endif
    } else if (mode == M_CODE) {
        orbit += dt * 0.08f; if (codeBad > 0) codeBad -= dt;
        if (pd->pressed & B_LEFT) codePos = (codePos + 3) % 4;
        if (pd->pressed & B_RIGHT) codePos = (codePos + 1) % 4;
        if (pd->pressed & B_UP) codeD[codePos] = (codeD[codePos] + 1) % 10;
        if (pd->pressed & B_DOWN) codeD[codePos] = (codeD[codePos] + 9) % 10;
        if (pd->pressed & (B_UP | B_DOWN | B_LEFT | B_RIGHT)) snd_play(SND_TICK, 0.6f);
        if (pd->pressed & B_CROSS) {
            int v = ((codeD[0] * 10 + codeD[1]) * 10 + codeD[2]) * 10 + codeD[3];
            if (v == 1122) { mode = M_LAB; sel = 0; snd_play(SND_WIN, 0.7f); }
            else { codeBad = 1.0f; snd_play(SND_EMPTY, 0.8f); }
        }
        if (pd->pressed & B_CIRCLE) { mode = M_MENU; sel = 0; }
    } else if (mode == M_LAB) {
        orbit += dt * 0.08f; menuNav(pd, 4);
        if (pd->pressed & B_LEFT) labSet(sel, -1);
        if (pd->pressed & B_RIGHT) labSet(sel, 1);
        if (pd->pressed & B_CROSS) { if (sel == 0) labStart(); else if (sel == 3) { mode = M_MENU; sel = 0; } else labSet(sel, 1); }
        if (pd->pressed & B_CIRCLE) { mode = M_MENU; sel = 0; }
    } else if (mode == M_SETTINGS) {
        orbit += dt * 0.08f; menuNav(pd, 12);
        if (pd->pressed & B_LEFT) changeSetting(sel, -1);
        if (pd->pressed & (B_RIGHT | (sel < 11 ? B_CROSS : 0))) changeSetting(sel, 1);
        if (((pd->pressed & B_CROSS) && sel == 11) || (pd->pressed & B_CIRCLE)) {
            mode = settingsFrom;
#ifdef PC_ONLINE
            sel = settingsFrom == M_PAUSE ? 1 : 2; cfgSave();
#else
            sel = 1;
#endif
        }
    } else if (mode == M_END) {
        orbit += dt * 0.08f;
#ifdef PC_ONLINE
        if (netMode) {
            int hostSide = netMode == 1; menuNav(pd, hostSide ? 2 : 1);
            if (pd->pressed & B_CROSS) { if (hostSide && sel == 0) mpHostBeginRound(1); else mpLeave(NULL, M_MENU); }
        } else
#endif
        {
        menuNav(pd, 2);
        if (pd->pressed & B_CROSS) { if (sel == 0) startMatch(); else { mode = M_MENU; sel = 0; nbots = 0; } }
        }
#ifdef PC_ONLINE
    } else if (mode == M_OPTIONS) {
        orbit += dt * 0.08f; menuNav(pd, 6);
        int d = ((pd->pressed & B_RIGHT) ? 1 : 0) - ((pd->pressed & B_LEFT) ? 1 : 0), ok = (pd->pressed & B_CROSS) != 0;
        if (sel == 0 && (d || ok)) { vidNew.res = (vidNew.res + (d ? d : 1) + 5) % 5; snd_play(SND_TICK, 0.6f); }
        else if (sel == 1 && d) { vidNew.bright = clampi(vidNew.bright + d * 5, 0, 100); pc_set_brightness(vidNew.bright); snd_play(SND_TICK, 0.6f); }
        else if (sel == 2 && (d || ok)) { vidNew.full = !vidNew.full; snd_play(SND_TICK, 0.6f); }
        else if (sel == 3 && ok) { vidApply(); mpNote("CAMBIOS APLICADOS Y GUARDADOS"); snd_play(SND_TICK, 0.8f); }
        else if (sel == 4 && ok) { vidNew = VID_DEF; vidApply(); mpNote("VALORES ORIGINALES RESTAURADOS"); snd_play(SND_TICK, 0.8f); }
        if ((ok && sel == 5) || (pd->pressed & B_CIRCLE)) {
            vidNew = vidCur; pc_set_brightness(vidCur.bright); mode = optsFrom; sel = optsFrom == M_PAUSE ? 2 : 3;
        }
    } else if (mode == M_MP_MENU) {
        orbit += dt * 0.08f;
        if (editing) {
#ifdef PS2_ONLINE
            /* PS2: sin teclado. Izq/Der mueven el cursor, Arriba/Abajo cambian el caracter (al final agregan uno), Cuadrado borra, X termina */
            char *tg = sel == 0 ? myName : joinText; int mx = sel == 0 ? 15 : 21, len = (int)strlen(tg);
            const char *cs = sel == 0 ? " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789" : "0123456789.:"; int ncs = (int)strlen(cs);
            if (edCur > len) edCur = len;
            if (pd->pressed & B_LEFT) { if (edCur > 0) edCur--; }
            if (pd->pressed & B_RIGHT) { if (edCur < len) edCur++; else if (len < mx && len > 0) { tg[len] = tg[len - 1]; tg[len + 1] = 0; edCur = len; } }
            if (pd->pressed & (B_UP | B_DOWN)) {
                int d = (pd->pressed & B_UP) ? 1 : -1;
                if (edCur == len) { if (len < mx) { tg[len] = cs[sel == 0 ? 1 : 1]; tg[len + 1] = 0; } }
                else { int k = 0; for (int j = 0; j < ncs; j++) if (cs[j] == tg[edCur]) k = j; tg[edCur] = cs[(k + d + ncs) % ncs]; }
                snd_play(SND_TICK, 0.5f);
            }
            if (pd->pressed & B_SQUARE) { if (len > 0) { int at = edCur < len ? edCur : len - 1; memmove(tg + at, tg + at + 1, len - at); if (edCur > at) edCur = at; if (edCur > (int)strlen(tg)) edCur = (int)strlen(tg); } }
            if (pd->pressed & (B_CROSS | B_START | B_CIRCLE)) { editing = 0; if (!myName[0]) strncpy(myName, "JUGADOR", 15); if (!joinText[0]) strncpy(joinText, "192.168.1.100", 39); cfgSave(); }
            if (0)
#endif
            {
            char c[16]; int n = pc_text_input(c, 16); char *tg = sel == 0 ? myName : joinText; int mx = sel == 0 ? 15 : 39;
            for (int i = 0; i < n; i++) {
                int ch = (unsigned char)c[i], len = (int)strlen(tg);
                if (ch == 13 || ch == 27) { editing = 0; if (!myName[0]) strncpy(myName, "JUGADOR", 15); if (!joinText[0]) strncpy(joinText, "127.0.0.1", 39); cfgSave(); break; }
                else if (ch == 8) { if (len) tg[len - 1] = 0; }
                else if (ch >= 32 && ch < 127) { if (ch >= 'a' && ch <= 'z') ch -= 32; if (len < mx) { tg[len] = (char)ch; tg[len + 1] = 0; } }
            }
            }
        } else {
            menuNav(pd, 5);
            if (pd->pressed & B_CROSS) {
                if (sel < 2) { editing = 1; edCur = (int)strlen(sel == 0 ? myName : joinText); char tmp[16]; pc_text_input(tmp, 16); }
                else if (sel == 2) { cfgSave(); mpHost(); }
                else if (sel == 3) { cfgSave(); mpJoin(); }
                else { mode = M_MENU; sel = 1; }
            } else if (pd->pressed & B_CIRCLE) { mode = M_MENU; sel = 1; }
        }
    } else if (mode == M_MP_LOBBY) {
        orbit += dt * 0.08f;
        int hostSide = netMode == 1, n = (hostSide && peerValid) ? 2 : 1;
        if (sel >= n) sel = n - 1;
        menuNav(pd, n);
        if (pd->pressed & B_CROSS) { if (hostSide && peerValid && sel == 0) mpHostBeginRound(1); else mpLeave(NULL, M_MP_MENU); }
        else if (pd->pressed & B_CIRCLE) mpLeave(NULL, M_MP_MENU);
#endif
    }
    t = SH / 224.0f;
    /* --- dibujo --- */
    gfx_begin(SKY);
    if (inGame) {
        float bob = P.ground ? sinf(P.bob * 2) * 0.035f * clampf(sqrtf(P.vx * P.vx + P.vz * P.vz) / 5, 0, 1) : 0;
        { /* sensacion de movimiento: inclinacion lateral al desplazarse y apertura de campo al correr */
            float rs = P.vx * cosf(P.yaw) - P.vz * sinf(P.yaw);
            camRoll = lerpf(camRoll, clampf(-rs * 0.0055f, -0.03f, 0.03f) * (P.alive ? 1 : 0), clampf(dt * 8, 0, 1));
#ifdef PS2_ONLINE
            camFovAdd = lerpf(camFovAdd, (5.0f * anRun * (1 - anAim) + 3.0f * bfSprA) * (P.alive ? 1 : 0) - 16.0f * bfAds, clampf(dt * 9, 0, 1));
#else
            camFovAdd = lerpf(camFovAdd, 5.0f * anRun * (1 - anAim) * (P.alive ? 1 : 0), clampf(dt * 5, 0, 1));
#endif
        }
        if (S.cam) { float cx, cy, cz; tpsCamera(&cx, &cy, &cz); setCamera(cx, cy, cz, P.yaw, P.pitch); }
        else setCamera(P.x, P.y + 1.62f - 0.5f * P.cr + bob - (P.alive ? 0 : 1.1f), P.z, P.yaw, P.pitch);
#ifdef PS2_ONLINE
        camPitch -= bfPlateDip(); camRoll += bfPlateDip() * 0.25f;     /* reverencia de la camara al ponerte la placa */
        if (bfShake > 0) { camPitch += sinf(orbit * 0 + P.bob * 0 + bfShake * 61.0f) * 0.035f * bfShake; camYaw += rndf(-1, 1) * 0.03f * bfShake; camRoll += rndf(-1, 1) * 0.02f * bfShake; }
#endif
    } else { camRoll = 0; camFovAdd = 0; setCamera(cosf(orbit) * 22, 9 + sinf(orbit * 1.7f) * 2, sinf(orbit) * 22, 0, 0); }
    if (!inGame) { /* camara orbital mirando al centro */
        float dx = -camX, dz = -camZ, dy = 1.5f - camY;
        camYaw = atan2f(dx, -dz) * -1.0f; camYaw = atan2f(-dx, -dz); camPitch = atan2f(dy, sqrtf(dx * dx + dz * dz));
    }
#ifdef PS2_ONLINE
    if (inGame) {   /* cielo con degradado: mas oscuro arriba, igual al color de la niebla en el horizonte */
        float hy = clampf(SH * 0.5f + camF * tanf(camPitch), 0, SH);
        if (hy > 2) { u32c top = RGBA((int)(CR(SKY) * 0.66f), (int)(CG(SKY) * 0.72f), (int)(CB(SKY) * 0.86f), 255);
            gfx_tri(0, 0, 1, top, SW, 0, 1, top, 0, hy, 1, SKY); gfx_tri(SW, 0, 1, top, SW, hy, 1, SKY, 0, hy, 1, SKY); }
    }
#endif
    drawFloor();
    for (int i = 0; i < nbox; i++) {
        Box *b = &boxes[i];
        obox((b->x0 + b->x1) * .5f, b->top * .5f, (b->z0 + b->z1) * .5f, (b->x1 - b->x0) * .5f, b->top * .5f, (b->z1 - b->z0) * .5f, 0, b->col);
    }
    drawBots();
#ifdef PS2_ONLINE
    bfDrawNades();
#endif
    if (inGame && S.cam) drawPlayerChar();
    if (inGame && !S.cam) drawPlayerLegs();
    if (inGame && P.alive && !S.cam) { drawWeaponModel(); drawHelmetRim(); }
    if (inGame && mode != M_PAUSE) drawHud();
    if (mode == M_MENU) {
        drawMenuBg(); drawTitle();
#ifdef PC_ONLINE
        const char *all[] = { "PARTIDA RAPIDA", "MULTIJUGADOR ONLINE", "CONFIGURACION", "OPCIONES", "SALIR DEL JUEGO" }, *it[5];
        for (int i = 0; i < MAIN_N; i++) it[i] = all[MAIN_ACT[i]];
        drawList(it, NULL, MAIN_N, SH * 0.46f);
#else
        const char *it[] = { "PARTIDA RAPIDA", "CONFIGURACION" };
        drawList(it, NULL, 2, SH * 0.52f);
#endif
        drawTextC(SW * 0.5f, SH * 0.9f - 6 * t, t * 0.7f, "ARRIBA/ABAJO MOVER   X ACEPTAR", RGBA(143, 160, 166, 255));
    } else if (mode == M_CODE) {
        drawMenuBg(); drawTitle();
        drawTextC(SW * 0.5f, SH * 0.40f, t * 1.6f, "CODIGO SECRETO", RGBA(232, 236, 233, 255));
        { float bw = 20 * t, gap = 8 * t, x0 = SW * 0.5f - (4 * bw + 3 * gap) * 0.5f, y0 = SH * 0.50f;
          for (int i = 0; i < 4; i++) {
              float x = x0 + i * (bw + gap); int on = i == codePos; char dg[2] = { (char)('0' + codeD[i]), 0 };
              u32c bg = codeBad > 0 ? RGBA(229, 72, 77, 255) : (on ? RGBA(224, 128, 58, 255) : RGBA(19, 26, 31, 230));
              gfx_rect(x, y0, x + bw, y0 + 26 * t, bg);
              drawText(x + bw * 0.5f - 3 * t * 2.2f + 1.1f * t, y0 + 4 * t, t * 2.2f, dg, on ? RGBA(23, 16, 10, 255) : RGBA(232, 236, 233, 255));
          } }
        if (codeBad > 0) drawTextC(SW * 0.5f, SH * 0.50f + 32 * t, t * 0.9f, "CODIGO INCORRECTO", RGBA(229, 72, 77, 255));
        drawTextC(SW * 0.5f, SH * 0.9f - 6 * t, t * 0.7f, "IZQ/DER DIGITO   ARRIBA/ABAJO CAMBIAR   X ACEPTAR   O VOLVER", RGBA(143, 160, 166, 255));
    } else if (mode == M_LAB) {
        drawMenuBg(); drawTextC(SW * 0.5f, SH * 0.08f, t * 3.0f, "MODO SECRETO", RGBA(224, 128, 58, 255));
        const char *it[] = { "CAMPO DE PRUEBAS", "MAPA", "ENEMIGOS (SKIN)", "VOLVER" }, *vals[4] = { NULL, MAPS[S.map].name, SKIN_NAMES[S.skin], NULL };
        drawList(it, vals, 4, SH * 0.30f);
        drawTextC(SW * 0.5f, SH * 0.62f, t * 0.7f, "SOLDADOS QUIETOS: DISPARALES, MIRA SU SKIN, USA GRANADAS", RGBA(143, 160, 166, 255));
        drawTextC(SW * 0.5f, SH * 0.62f + 9 * t, t * 0.7f, "EN JUEGO: ARRIBA = NUEVO SOLDADO   ABAJO = BORRAR TODOS", RGBA(143, 160, 166, 255));
        drawTextC(SW * 0.5f, SH * 0.92f - 6 * t, t * 0.7f, "IZQ/DER CAMBIAR   X ACEPTAR   O VOLVER", RGBA(143, 160, 166, 255));
    } else if (mode == M_SETTINGS) {
        drawMenuBg(); drawTextC(SW * 0.5f, SH * 0.08f, t * 3.0f, "CONFIGURACION", RGBA(232, 236, 233, 255));
        const char *it[] = { "DIFICULTAD", "BOTS POR RONDA", "RONDAS PARA GANAR", "SENSIBILIDAD", "CAMPO DE VISION", "INVERTIR EJE Y", "COLOR DE MIRA", "VOLUMEN", "ENEMIGOS", "CAMARA", "MAPA", "VOLVER" };
        const char *vals[12]; settingsLabels(vals);
        drawList(it, vals, 12, SH * 0.15f);
        drawTextC(SW * 0.5f, SH * 0.92f - 6 * t, t * 0.7f, "IZQ/DER CAMBIAR   O VOLVER", RGBA(143, 160, 166, 255));
    } else if (mode == M_PAUSE && sandbox) {
        drawMenuBg(); drawTextC(SW * 0.5f, SH * 0.16f, t * 4.0f, "PAUSA", RGBA(232, 236, 233, 255)); drawTextC(SW * 0.5f, SH * 0.16f + 34 * t, t * 0.9f, "CAMPO DE PRUEBAS", RGBA(224, 128, 58, 255));
        const char *it[] = { "CONTINUAR", "SOLDADO NUEVO", "BORRAR SOLDADOS", "ENEMIGOS (SKIN)", "MENU PRINCIPAL" }, *vals[5] = { NULL, NULL, NULL, SKIN_NAMES[S.skin], NULL };
        drawList(it, vals, 5, SH * 0.40f);
    } else if (mode == M_PAUSE) {
        drawMenuBg(); drawTextC(SW * 0.5f, SH * 0.2f, t * 4.0f, "PAUSA", RGBA(232, 236, 233, 255));
#ifdef PC_ONLINE
        const char *all[] = { "CONTINUAR", "CONFIGURACION", "OPCIONES", "MENU PRINCIPAL" }, *it[4];
        for (int i = 0; i < PAUSE_N; i++) it[i] = all[PAUSE_ACT[i]];
        drawList(it, NULL, PAUSE_N, SH * 0.42f);
#else
        const char *it[] = { "CONTINUAR", "CONFIGURACION", "MENU PRINCIPAL" };
        drawList(it, NULL, 3, SH * 0.45f);
#endif
#ifdef PS2_ONLINE
        { const char *h[] = { "R1 DISPARAR  L1 APUNTAR  R2 MANTEN/SOLTAR GRANADA  L2 PONER PLACA", "CUADRADO RECARGAR  X SALTAR  O AGACHARSE  L3 CORRER  R3 CAMINAR SILENCIO", "TRIANGULO O CRUCETA IZQ/DER: CAMBIAR ARMA" };
          for (int i = 0; i < 3; i++) drawTextC(SW * 0.5f, SH * 0.80f + i * 8 * t, t * 0.65f, h[i], RGBA(143, 160, 166, 255)); }
#endif
    } else if (mode == M_END) {
        int won = scoreMe >= ROUNDS(); char b[48];
        drawMenuBg(); drawTextC(SW * 0.5f, SH * 0.12f, t * 4.5f, won ? "VICTORIA" : "DERROTA", won ? RGBA(111, 207, 151, 255) : RGBA(229, 72, 77, 255));
        snprintf(b, 48, "MARCADOR %d - %d", scoreMe, scoreBots); drawTextC(SW * 0.5f, SH * 0.12f + 40 * t, t * 1.2f, b, RGBA(232, 236, 233, 255));
        snprintf(b, 48, "BAJAS %d   CABEZAS %d   MUERTES %d", kills, heads, deaths); drawTextC(SW * 0.5f, SH * 0.12f + 56 * t, t * 0.9f, b, RGBA(143, 160, 166, 255));
        snprintf(b, 48, "DANO RECIBIDO %d", (int)damageTaken); drawTextC(SW * 0.5f, SH * 0.12f + 68 * t, t * 0.9f, b, RGBA(143, 160, 166, 255));
#ifdef PC_ONLINE
        if (netMode) {
            const char *it1[] = { "REVANCHA", "SALIR DE LA PARTIDA" }, *it2[] = { "SALIR DE LA PARTIDA" };
            if (netMode == 1) drawList(it1, NULL, 2, SH * 0.6f);
            else { drawList(it2, NULL, 1, SH * 0.6f); drawTextC(SW * 0.5f, SH * 0.6f + 22 * t, t * 0.8f, "EL ANFITRION DECIDE LA REVANCHA", RGBA(143, 160, 166, 255)); }
        } else
#endif
        {
        const char *it[] = { "REVANCHA", "MENU PRINCIPAL" };
        drawList(it, NULL, 2, SH * 0.6f);
        }
    }
#ifdef PC_ONLINE
    else if (mode == M_OPTIONS) {
        drawMenuBg(); drawTextC(SW * 0.5f, SH * 0.08f, t * 3.0f, "OPCIONES", RGBA(232, 236, 233, 255));
        const char *it[] = { "RESOLUCION", "BRILLO", "PANTALLA", "APLICAR", "RESTAURAR", "VOLVER" };
        const char *vals[6] = { RES_NAMES[vidNew.res], NULL, vidNew.full ? "PANTALLA COMPLETA" : "VENTANA", NULL, NULL, NULL };
        drawList(it, vals, 6, SH * 0.2f);
        {   /* barra de brillo */
            float w = SW * 0.62f, x0 = (SW - w) * 0.5f, rh = 12 * t, y = SH * 0.2f + 1 * (rh + 2 * t), bw = 52 * t, bx = x0 + w - 5 * t - bw;
            char b[16]; snprintf(b, 16, "%d%%", vidNew.bright);
            u32c tc = sel == 1 ? RGBA(23, 16, 10, 255) : RGBA(232, 236, 233, 255);
            gfx_rect(bx, y + 3.5f * t, bx + bw, y + rh - 3.5f * t, RGBA(8, 11, 13, 255));
            gfx_rect(bx, y + 3.5f * t, bx + bw * vidNew.bright / 100.0f, y + rh - 3.5f * t, sel == 1 ? RGBA(255, 244, 220, 255) : RGBA(224, 128, 58, 255));
            drawText(bx - 6 * t - textW(b, t), y + 2.5f * t, t, b, tc);
        }
        if (vidNew.res != vidCur.res || vidNew.full != vidCur.full) drawTextC(SW * 0.5f, SH * 0.2f + 6 * (12 * t + 2 * t) + 4 * t, t * 0.8f, "HAY CAMBIOS SIN APLICAR - ELEGI APLICAR", RGBA(224, 128, 58, 255));
        drawTextC(SW * 0.5f, SH * 0.92f - 6 * t, t * 0.7f, "IZQ/DER CAMBIAR   X ACEPTAR   O VOLVER   F11 PANTALLA COMPLETA", RGBA(143, 160, 166, 255));
    } else if (mode == M_MP_MENU) {
        drawMenuBg(); drawTextC(SW * 0.5f, SH * 0.08f, t * 3.0f, "MULTIJUGADOR ONLINE", RGBA(232, 236, 233, 255));
        char nb[24], ib[48]; snprintf(nb, 24, "%s%s", myName, (editing && sel == 0) ? "_" : ""); snprintf(ib, 48, "%s%s", joinText, (editing && sel == 1) ? "_" : "");
        const char *it[] = { "NOMBRE DE JUGADOR", "DIRECCION IP", "CREAR PARTIDA", "UNIRSE A UNA PARTIDA", "VOLVER" };
        const char *vals[5] = { nb, ib, NULL, NULL, NULL };
        drawList(it, vals, 5, SH * 0.2f);
        drawTextC(SW * 0.5f, SH * 0.62f, t * 0.75f, "CREAR: VOS SOS EL ANFITRION (UDP 7777). EL OTRO ENTRA CON TU IP.", RGBA(143, 160, 166, 255));
        drawTextC(SW * 0.5f, SH * 0.62f + 10 * t, t * 0.75f, "UNIRSE: ESCRIBI LA IP DEL ANFITRION (IP:PUERTO SI USA OTRO).", RGBA(143, 160, 166, 255));
        #ifdef PS2_ONLINE
        if (editing) {   /* cursor bajo el caracter que se esta editando */
            float w = SW * 0.62f, x0 = (SW - w) * 0.5f, rh = 12 * t, y = SH * 0.2f + sel * (rh + 2 * t); char *tg = sel == 0 ? myName : joinText, pre[48]; int k = edCur; if (k > 46) k = 46;
            memcpy(pre, tg, k); pre[k] = 0;
            char full[48]; snprintf(full, 48, "< %s >", tg); float xs = x0 + w - 5 * t - textW(full, t) + textW("< ", t) + textW(pre, t);
            gfx_rect(xs, y + rh - 3.5f * t, xs + 5 * t, y + rh - 2.5f * t, RGBA(23, 16, 10, 255));
        }
        drawTextC(SW * 0.5f, SH * 0.92f - 6 * t, t * 0.7f, editing ? "ARRIBA/ABAJO LETRA  IZQ/DER MOVER  CUADRADO BORRAR  X LISTO" : "X EDITAR / ACEPTAR   O VOLVER", RGBA(143, 160, 166, 255));
#else
        drawTextC(SW * 0.5f, SH * 0.92f - 6 * t, t * 0.7f, editing ? "ESCRIBI CON EL TECLADO   ENTER TERMINAR" : "X EDITAR / ACEPTAR   O VOLVER", RGBA(143, 160, 166, 255));
#endif
    } else if (mode == M_MP_LOBBY) {
        drawMenuBg(); drawTextC(SW * 0.5f, SH * 0.1f, t * 3.0f, netMode == 1 ? "PARTIDA CREADA" : "UNIRSE A PARTIDA", RGBA(232, 236, 233, 255));
        char b[64]; u32c dim = RGBA(143, 160, 166, 255), lt = RGBA(232, 236, 233, 255); float y = SH * 0.26f;
        if (netMode == 1) {
            snprintf(b, 64, "PUERTO UDP %d   TU IP LOCAL %s", MP_PORT, localIp); drawTextC(SW * 0.5f, y, t * 0.9f, b, lt);
            drawTextC(SW * 0.5f, y + 12 * t, t * 0.8f, "POR INTERNET PASALE TU IP PUBLICA (Y ABRI EL PUERTO UDP 7777)", dim);
            if (peerValid) { snprintf(b, 64, "JUGADOR CONECTADO: %s", peerName); drawTextC(SW * 0.5f, y + 30 * t, t * 1.1f, b, RGBA(111, 207, 151, 255)); }
            else drawTextC(SW * 0.5f, y + 30 * t, t * 1.1f, "ESPERANDO JUGADOR...", RGBA(224, 128, 58, 255));
            snprintf(b, 64, "RONDAS PARA GANAR: %d (CONFIGURACION)", mpRounds); drawTextC(SW * 0.5f, y + 44 * t, t * 0.8f, b, dim);
            const char *it1[] = { "INICIAR PARTIDA", "CANCELAR" }, *it2[] = { "CANCELAR" };
            if (peerValid) drawList(it1, NULL, 2, SH * 0.64f); else drawList(it2, NULL, 1, SH * 0.64f);
        } else {
            if (mpPhase == MPP_JOINING) { snprintf(b, 64, "CONECTANDO A %s...", joinText); drawTextC(SW * 0.5f, y + 20 * t, t * 1.1f, b, RGBA(224, 128, 58, 255)); }
            else { snprintf(b, 64, "CONECTADO CON %s", peerName); drawTextC(SW * 0.5f, y + 14 * t, t * 1.1f, b, RGBA(111, 207, 151, 255)); drawTextC(SW * 0.5f, y + 30 * t, t * 0.9f, "ESPERANDO QUE EL ANFITRION INICIE LA PARTIDA", lt); }
            const char *it2[] = { "SALIR" }; drawList(it2, NULL, 1, SH * 0.64f);
        }
    }
    if (netMode && mode == M_PLAYING) {
        char b[40]; snprintf(b, 40, "VS %s", peerName); drawTextC(SW * 0.5f, 3 * t, t * 0.8f, b, RGBA(232, 236, 233, 200));
        if (mpRx > 2.0f) drawTextC(SW * 0.5f, SH * 0.45f, t * 1.2f, "CONEXION INESTABLE...", RGBA(229, 72, 77, 255));
        if (ovl) { drawMenuBg(); drawTextC(SW * 0.5f, SH * 0.2f, t * 3.0f, "PAUSA", RGBA(232, 236, 233, 255)); const char *it[] = { "CONTINUAR", "SALIR DE LA PARTIDA" }; drawList(it, NULL, 2, SH * 0.42f); drawTextC(SW * 0.5f, SH * 0.62f, t * 0.75f, "EL JUEGO SIGUE EN LINEA", RGBA(143, 160, 166, 255)); }
    }
    if (mpNoticeT > 0 && mode != M_PLAYING) drawTextC(SW * 0.5f, SH * 0.86f, t * 0.9f, mpNotice, RGBA(224, 128, 58, 255));
#endif
    fpsDraw();
    gfx_end();
}

/* macros propias de game.c: se liberan para no afectar a los archivos que siguen */
#undef HALF
#undef MAXBOX
#undef MAXBOT
#undef STEP
#undef NG
#undef NEAR
#undef ROUNDS
#undef netMode
#undef SWAP_T
#undef BF_NP
#undef MP_PORT
#undef PS2IP_DEFAULT
#undef MP_VER
#undef C_GM
#undef C_GD
#undef C_GL
#undef C_BR
#undef C_WD
#undef C_WD2
#undef MAIN_N
#undef PAUSE_N

/* ################ ps2_platform.c ################ */
/* Plataforma PS2: gsKit (video) + libpad (mando DualShock) */
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <libpad.h>
#include <iopcontrol.h>
#include <iopheap.h>
#include <sbv_patches.h>
#include <audsrv.h>
#include <debug.h>



extern unsigned char sio2man_irx[], padman_irx[], libsd_irx[];
extern unsigned int size_libsd_irx;
extern unsigned int size_sio2man_irx, size_padman_irx;
extern unsigned char audsrv_irx[];
extern unsigned int size_audsrv_irx;

int SW = 640, SH = 448;
static GSGLOBAL *gs;
static char padBuf[256] __attribute__((aligned(64)));
static int padReady = 0;
static unsigned prevDown = 0;

static u64 col(u32c c, int forceOpaque) {
    int a = forceOpaque ? 0x80 : (CA(c) * 128 / 255);
    return GS_SETREG_RGBAQ(CR(c), CG(c), CB(c), a, 0x00);
}

void gfx_init(void) {
    gs = gsKit_init_global();
    gs->PSM = GS_PSM_CT24;
    gs->PSMZ = GS_PSMZ_24;
    gs->ZBuffering = GS_SETTING_ON;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->PrimAlphaEnable = GS_SETTING_OFF;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
    gsKit_set_test(gs, GS_ZTEST_ON);
    gsKit_mode_switch(gs, GS_ONESHOT);
    SW = gs->Width;
    SH = gs->Height;
}

void gfx_begin(u32c clear) {
    gs->PrimAlphaEnable = GS_SETTING_OFF;
    gsKit_clear(gs, col(clear, 1));
}

void gfx_end(void) {
    gsKit_queue_exec(gs);
    gsKit_sync_flip(gs);
}

void gfx_tri(float x1, float y1, int z1, u32c c1, float x2, float y2, int z2, u32c c2, float x3, float y3, int z3, u32c c3) {
    int tr = CA(c1) < 255 || CA(c2) < 255 || CA(c3) < 255;      /* triangulos con alfa (humo, fuego, trazadoras): mezcla con lo ya dibujado */
    gs->PrimAlphaEnable = tr ? GS_SETTING_ON : GS_SETTING_OFF;
    gsKit_prim_triangle_gouraud_3d(gs, x1, y1, z1, x2, y2, z2, x3, y3, z3, col(c1, !tr), col(c2, !tr), col(c3, !tr));
    gs->PrimAlphaEnable = GS_SETTING_OFF;
}

void gfx_rect(float x1, float y1, float x2, float y2, u32c c) {
    int translucent = CA(c) < 255;
    gs->PrimAlphaEnable = translucent ? GS_SETTING_ON : GS_SETTING_OFF;
    gsKit_prim_sprite(gs, x1, y1, x2, y2, 0xFFFFFF, col(c, !translucent));
    gs->PrimAlphaEnable = GS_SETTING_OFF;
}

static int waitPad(int port, int slot) {
    int s, tries = 0;
    do { s = padGetState(port, slot); } while (s != PAD_STATE_STABLE && s != PAD_STATE_FINDCTP1 && s != PAD_STATE_DISCONN && ++tries < 200000);
    return s;
}

void pad_init(void) {
    SifInitRpc(0);
    int ret = 0, id;
    scr_printf("  Cargando sio2man ...");
    id = SifExecModuleBuffer(sio2man_irx, size_sio2man_irx, 0, NULL, &ret); scr_printf(" %s (%d)\n", id >= 0 ? "ok" : "ERROR", id);
    scr_printf("  Cargando padman ...");
    id = SifExecModuleBuffer(padman_irx, size_padman_irx, 0, NULL, &ret); scr_printf(" %s (%d)\n", id >= 0 ? "ok" : "ERROR", id);
    scr_printf("  Iniciando mando ...");
    padInit(0);
    if (padPortOpen(0, 0, padBuf) == 0) { scr_printf(" ERROR\n"); return; }
    waitPad(0, 0);
    padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
    waitPad(0, 0);
    padReady = 1; scr_printf(" ok\n");
}

static float axis(unsigned char v) {
    float f = ((int)v - 128) / 127.0f;
    if (f > -0.18f && f < 0.18f) return 0;
    return f > 0 ? (f - 0.18f) / 0.82f : (f + 0.18f) / 0.82f;
}

void pad_poll(Pad *p) {
    struct padButtonStatus b;
    memset(p, 0, sizeof(*p));
    if (!padReady) return;
    int st = padGetState(0, 0);
    if (st != PAD_STATE_STABLE && st != PAD_STATE_FINDCTP1) return;
    if (padRead(0, 0, &b) == 0) return;
    unsigned m = 0xFFFF ^ b.btns; /* los botones llegan en 0 al presionar */
    unsigned d = 0;
    if (m & PAD_UP) d |= B_UP;       if (m & PAD_DOWN) d |= B_DOWN;
    if (m & PAD_LEFT) d |= B_LEFT;   if (m & PAD_RIGHT) d |= B_RIGHT;
    if (m & PAD_CROSS) d |= B_CROSS; if (m & PAD_CIRCLE) d |= B_CIRCLE;
    if (m & PAD_SQUARE) d |= B_SQUARE; if (m & PAD_TRIANGLE) d |= B_TRIANGLE;
    if (m & PAD_L1) d |= B_L1;       if (m & PAD_R1) d |= B_R1;
    if (m & PAD_L2) d |= B_L2;       if (m & PAD_R2) d |= B_R2;
    if (m & PAD_START) d |= B_START; if (m & PAD_SELECT) d |= B_SELECT; if (m & PAD_L3) d |= B_L3; if (m & PAD_R3) d |= B_R3;
    p->down = d; p->pressed = d & ~prevDown; prevDown = d;
    if (b.mode >> 4 == 0x7) { /* modo analogico */
        p->lx = axis(b.ljoy_h); p->ly = axis(b.ljoy_v);
        p->rx = axis(b.rjoy_h); p->ry = axis(b.rjoy_v);
    }
}

static int audOK = 0, audSent = 0, dbgQ = -1, dbgA = -1, dbgR = -1;
static char audMsg[72] = "NO INICIADO";
const char *audio_status(void) { static char out[96]; if (audOK) snprintf(out, sizeof out, "ACTIVO (%d) Q%d A%d R%d", audSent, dbgQ, dbgA, dbgR); else snprintf(out, sizeof out, "%s", audMsg); return out; }
static void audioFail(void) { scr_printf("\n  El sonido no se pudo iniciar. Continuando sin sonido...\n"); for (int i = 0; i < 60; i++) for (volatile int k = 0; k < 4000000; k++) {} }

void audio_init(void) {
    int ret = 0, id;
    scr_printf("  Cargando libsd ...");
    id = SifExecModuleBuffer(libsd_irx, size_libsd_irx, 0, NULL, &ret); scr_printf(" %s (%d)\n", id >= 0 ? "ok" : "ERROR", id);
    if (id < 0) { id = SifLoadModule("rom0:LIBSD", 0, NULL); scr_printf("  libsd de la consola %s (%d)\n", id >= 0 ? "ok" : "ERROR", id); }
    scr_printf("  Cargando audsrv ...");
    id = SifExecModuleBuffer(audsrv_irx, size_audsrv_irx, 0, NULL, &ret); scr_printf(" %s (%d)\n", id >= 0 ? "ok" : "ERROR", id);
    if (id < 0) { snprintf(audMsg, sizeof audMsg, "ERROR AL CARGAR AUDSRV (%d)", id); scr_printf("  Sin sonido\n"); audioFail(); return; }
    scr_printf("  Iniciando audsrv ...");
    if (audsrv_init() != 0) { snprintf(audMsg, sizeof audMsg, "ERROR INICIO: %.50s", audsrv_get_error_string()); scr_printf(" ERROR: %s\n", audsrv_get_error_string()); audioFail(); return; }
    struct audsrv_fmt_t fmt;
    fmt.freq = SND_RATE; fmt.bits = 16; fmt.channels = 2;
    if (audsrv_set_format(&fmt) != 0) { snprintf(audMsg, sizeof audMsg, "ERROR FORMATO: %.48s", audsrv_get_error_string()); scr_printf(" ERROR de formato: %s\n", audsrv_get_error_string()); audioFail(); return; }
    audsrv_set_volume(MAX_VOLUME);
    audOK = 1; scr_printf(" ok\n");
}

#define MIX_FRAMES 512
static short mixbuf[MIX_FRAMES * 2] __attribute__((aligned(64)));

void audio_pump(void) {
    if (!audOK) return;
    for (int guard = 0; guard < 8; guard++) {
        int q = audsrv_queued(), av = audsrv_available();
        dbgQ = q; dbgA = av;
        if (av < (int)sizeof(mixbuf)) break;        /* no hay lugar en el buffer de audio (audsrv_queued devuelve lo mismo que available en esta version, por eso solo se usa available) */
        snd_mix(mixbuf, MIX_FRAMES);
        int r = audsrv_play_audio((char *)mixbuf, sizeof(mixbuf));
        dbgR = r;
        if (r <= 0) break;
        audSent++;
    }
}

/* La version de PS2 no tiene opciones de video ni archivo de configuracion: funciones vacias que pide el juego compartido. */
int pc_cfg_load(char *buf, int max) { return 0; }
int pc_cfg_save(const char *buf, int len) { return 0; }
void pc_video_apply(int w, int h, int fullscreen) {}
int pc_video_poll(int *fullscreen) { return 0; }
void pc_set_brightness(int pct) {}
int pc_text_input(char *out, int max) { return 0; }
void pc_quit(void) {}

/* Reinicia el IOP y habilita la carga de modulos desde memoria. */
static void init_iop(void) {
    SifInitRpc(0);
    SifIopReset("", 0);
    while (!SifIopSync()) {}
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
}

/* Reloj: registro Count del procesador (EE, 294,912 MHz) */
#define EE_CPU_HZ 294912000.0f
static inline unsigned cpu_count(void) {
    unsigned c;
    __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    return c;
}

int main(void) {
    init_scr();
    scr_printf("PUNTO CERO para PS2 - arrancando\n\n  Reiniciando el IOP ...");
    init_iop();
    scr_printf(" ok\n");
    pad_init();
    audio_init();
    scr_printf("  Iniciando video ...\n");
    gfx_init();
    game_init();
    unsigned last = cpu_count();
    for (;;) {
        Pad p;
        pad_poll(&p);
        /* tiempo real del cuadro anterior: si los fps bajan a 30 el juego sigue a la misma velocidad */
        unsigned now = cpu_count();
        float dt = (float)(unsigned)(now - last) / EE_CPU_HZ;
        last = now;
        fpsRawDt = dt > 0.0005f ? dt : 0.0005f;      /* para el contador de FPS (sin recortar) */
        if (dt < 0.005f) dt = 0.005f;
        if (dt > 0.05f) dt = 0.05f;      /* tope: evita saltos enormes si hay una pausa larga (carga, etc.) */
        game_frame(&p, dt);
        audio_pump();
    }
    return 0;
}

/* ################ net_ps2.c ################ */
/* Red de la PS2 (Slim con puerto Ethernet integrado o adaptador de red): modulos DEV9/NETMAN/SMAP + pila lwIP de ps2ip, UDP. */
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopheap.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netman.h>
#include <ps2ip.h>


extern unsigned char DEV9_irx[], NETMAN_irx[], SMAP_irx[];
extern unsigned int size_DEV9_irx, size_NETMAN_irx, size_SMAP_irx;

static int netUp = 0, sock = -1;
char ps2net_status[64] = "";

static void alarmCb(s32 id, u16 time, void *common) { iWakeupThread(*(int *)common); }
static int waitFor(int (*chk)(void), int secs) {
    int tid = GetThreadId();
    for (int i = 0; chk() == 0; i++) {
        SetAlarm(1000 * 16, &alarmCb, &tid); SleepThread();
        if (i >= secs) return -1;
    }
    return 0;
}
static int linkUp(void) { return NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0) == NETMAN_NETIF_ETH_LINK_STATE_UP; }
static int dhcpOk(void) {
    t_ip_info ii;
    if (ps2ip_getconfig("sm0", &ii) < 0) return 0;
    return ii.dhcp_enabled && (ii.dhcp_status == DHCP_STATE_BOUND || ii.dhcp_status == DHCP_STATE_OFF);
}
/* Levanta la red una sola vez (tarda unos segundos). 0 = ok */
static int netBringUp(void) {
    if (netUp) return 0;
    int ret = 0, id;
    SifInitRpc(0);
    id = SifExecModuleBuffer(DEV9_irx, size_DEV9_irx, 0, NULL, &ret); if (id < 0) { snprintf(ps2net_status, 64, "ERROR DEV9 (%d)", id); return -1; }
    id = SifExecModuleBuffer(NETMAN_irx, size_NETMAN_irx, 0, NULL, &ret); if (id < 0) { snprintf(ps2net_status, 64, "ERROR NETMAN (%d)", id); return -1; }
    id = SifExecModuleBuffer(SMAP_irx, size_SMAP_irx, 0, NULL, &ret); if (id < 0) { snprintf(ps2net_status, 64, "ERROR SMAP (%d)", id); return -1; }
    if (NetManInit() != 0) { snprintf(ps2net_status, 64, "ERROR NETMAN INIT"); return -1; }
    NetManSetLinkMode(NETMAN_NETIF_ETH_LINK_MODE_AUTO);
    struct ip4_addr ip, nm, gw, dns;
    ip4_addr_set_zero(&ip); ip4_addr_set_zero(&nm); ip4_addr_set_zero(&gw); ip4_addr_set_zero(&dns);
    ps2ipInit(&ip, &nm, &gw);
    t_ip_info ii;
    if (ps2ip_getconfig("sm0", &ii) >= 0) { ii.dhcp_enabled = 1; ps2ip_setconfig(&ii); }
    if (waitFor(linkUp, 10) != 0) { snprintf(ps2net_status, 64, "SIN CABLE DE RED"); return -1; }
    if (waitFor(dhcpOk, 15) != 0) { snprintf(ps2net_status, 64, "NO SE OBTUVO IP (DHCP)"); return -1; }
    netUp = 1; snprintf(ps2net_status, 64, "RED LISTA"); return 0;
}

int net_open(int port) {
    net_close();
    if (netBringUp() != 0) return -1;
    sock = socket(AF_INET, SOCK_DGRAM, 0); if (sock < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr *)&a, sizeof a) < 0) { close(sock); sock = -1; return -1; }
    return 0;
}
void net_close(void) { if (sock >= 0) close(sock); sock = -1; }
int net_resolve(const char *text, unsigned *ip, int *port, int defport) {
    int p[4] = { 0, 0, 0, 0 }, n = 0, cur = -1; *port = defport;
    while (*text == ' ') text++;
    for (; *text && *text != ' '; text++) {
        if (*text >= '0' && *text <= '9') { cur = (cur < 0 ? 0 : cur) * 10 + (*text - '0'); if (cur > 255 && n < 4) return -1; }
        else if (*text == '.') { if (cur < 0 || n >= 3) return -1; p[n++] = cur; cur = -1; }
        else if (*text == ':') break;
        else return -1;
    }
    if (cur < 0 || n != 3) return -1; p[3] = cur;
    if (*text == ':') { int v = 0; text++; if (*text < '0' || *text > '9') return -1; while (*text >= '0' && *text <= '9') { v = v * 10 + (*text - '0'); if (v > 65535) return -1; text++; } if (v < 1) return -1; *port = v; }
    unsigned char b[4] = { (unsigned char)p[0], (unsigned char)p[1], (unsigned char)p[2], (unsigned char)p[3] };
    memcpy(ip, b, 4); return 0;     /* orden de red */
}
int net_send(unsigned ip, int port, const void *d, int n) {
    if (sock < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = ip;
    return (int)sendto(sock, d, n, 0, (struct sockaddr *)&a, sizeof a);
}
int net_recv(void *b, int max, unsigned *ip, int *port) {
    if (sock < 0) return 0;
    struct sockaddr_in a; socklen_t l = sizeof a;
    int r = (int)recvfrom(sock, b, max, MSG_DONTWAIT, (struct sockaddr *)&a, &l);
    if (r <= 0) return 0;
    *ip = a.sin_addr.s_addr; *port = ntohs(a.sin_port); return r;
}
void net_local_ip(char *out, int n) {
    t_ip_info ii; snprintf(out, n, "?");
    if (ps2ip_getconfig("sm0", &ii) >= 0) { const unsigned char *b = (const unsigned char *)&ii.ipaddr.s_addr; snprintf(out, n, "%d.%d.%d.%d", b[0], b[1], b[2], b[3]); }
}
const char *net_last_error(void) { return ps2net_status; }
