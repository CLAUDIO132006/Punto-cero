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
"JRe2En4TiRIuDgUOCA2PE5UdVBb4CpoSHiMyJfkdYxyvHDwYxxFFESwVfRq+HPAUDArEAKkFLRn9IHcYcAyyBSoBzgJeCtIE+PeR"
"73LpouYD6HrvkvH36Crn9u4//9kOZhDnDR8MNAc2BpsQ3BZuFVEUKw5nBlMCkAfQCaD9EvKy8Rz2yffF8Abrh/x6Cb8BiQUEGK4h"
"rCEJJhgleR0VF9oQxQoCA6L+aPl77NnrNPUt74DibNjN2yLp5++b+Kv+8wSIEQEaExudGCkVuxUVFqUOlhG6Ga0UmAk8/gP3BPrJ"
"/Zv6ePkX+vUGDRxzHoAaGxxSGpEbJiGjJCIeFg9rDH4NeAzoEkkTexI+F3kWpg+mC6sKkQmDCLP+o/Do78zxSe/09W/49vNH94X8"
"Vf94AdYDpwRpAbsAJwNRAUD8OPAZ50TpgfGs9M/ryOoL6DnpSOw84bLnsfWy+UPvFuRg52XoBOoA6iDq++8r9+T5Nfva/dP+C//7"
"9hjpydnM2hvoHOjK47znovGW7bvjwOio8Hr3YPn9+hAIYAxKDY4Tvwnv9ljsdv7kEUwK9gVEBIMEqAYQCWERuRd7Ej8Auf+vB30A"
"mfVX7KTrtejB5wb49Ad1FUYoFC8VKKYeohK9FAQlHiTbD2IASv4x/eTu+Og893T1ken+7Bn8vQe6BXEGbwfaB3UNIgyFDSkW6RZ7"
"ETESChTsDVEIkgRR/lr5AvZt/mMH+ABX9w/58A1ZExUIvwruDgsSpxZZFmENngfuDS0dtSjsHV4QNQ1iC/ULHwZmAB0BRPIF6H7z"
"v/VR7+Tyjv38+/v4ggO0Bs4KsgzBAov/MwCbATv8lQLzDZf+Qf15/gbz3feFBdgMcQptDRMI3AbtCGEAYAVLBLQJgxHTBx8J8w+P"
"D34LYwS5+wkEsQ4BA2z6tfEW6871OfhlAwUW/guEBG4LOAyhBMj/6AcHDyIIrgR0FNkcHR3CFH0GMQxeEmsRGgUP/ToFTgoyFpsZ"
"3hKlFP8Y9CAfJsgptTT+LIEVywYN+eT1bfpX/0kDXP9Q9tDsivOt/YIE2gcdCAcIoftECkMhSQ2d6bDTBNbb4CvjCN2+30/n7+Bn"
"3HXZpN565rjpqvCA6yDskvM67z/zXQWkAvHywfbF+OAEFBipFsISpA4qCqMPYhZiCyMC6Ai+Ac3ztff8ApsQDx/BLadB3D6UMCMt"
"UiTOHlIe2B0+F3wN6wOJ/z4EtP1Q/YAHGhFDF+cM5wV5AE362ApQFIoFyREOLQQiVBAlEe4IyfVn+qYS+RJWCGcLUBEzEBwMggEO"
"94H/uvxe97r9sfozAVYBZPEw4gnaUN1n6CP++gX8+tD4NAC7BEcDFQZWCDELdiJII9//M/n5/R7wmvAg/if8KPreBpYCzPiE+7L+"
"FAWJBiMBCPv0/KAGHgupDysQ/Aa5/2gEpw6eCLfzMuhU9RwKKRCtCPoBpQX0Afr8cwTABGkDiwbCBLYQBCGpHeIc7h9WF60PPA9d"
"CvMF6wfgBqkGagVPAHL+CgfDDmQKBwcEBmEDMvlL79L0Qfv8+MbyyfbdCZwPChRvIgoVpwFlCDgThwx/AAsDFAZMAWP7vPpZALP/"
"Tvwf/cn40vTD/FQHjQh+/gLvFO43/hoIRAybCQ4HIxGLEXkJfAa7CzoLEftl7o3lAeJi5dLuk+7T3ljjI+617czvJff7AA8DAwOn"
"AmcDrgSm///5Z/OP8Tz3DPsV/xoBsACOBbUEw/sx9kH//gouAZvwSedI5pnsx+tP4L3bitte1q/Vbt1p5u7cRNDOztnNXtZm3wHf"
"WOC/3nfWVdNB00XRY8550l/bB95r4Efqfu/16dfuK/ZD9+D0yPKD9Kzy1/q7/D70cPCB5t/qgf/IBWcDBQE9AKcBAvxd+9r6dPc3"
"A7UGjvlA+6X8/ezY4t/lWvA4/X0EY/td9k37xPuV/Az6iPaS9yv7vPi69tv5DQD6/5H3XfN18V78w/6Z7orqLe8C7KfqzPLQ9gz4"
"Zv3gBKYJiwe1Acj9NPxSBpcOiQ1fD4EEr/d19Jz27PshAK7+fARwEiEYSRhKDmgVkh/3G34jsSbcJPsj4iBmGG4J+/uB/0gErP5d"
"/QD8fP5+BT8PjRaPFNELgAuMF7IgQSAfFtQQvQ9mCkcApvTm6ubfPdvV25nd/97D4sHw8PB57Gb5Vvty/FQFzgvQE3EXUxQcDsAN"
"NwybCQ8JHgQN9bnqkPId9774F/gg9VL6FgT+Cw0Omg00FM8ftiFlHV4WqBG9DswJfghlAhL+NP+DAV0JPwy0BfsB0gJyAxUKAhZ6"
"Hwwm5CUYIs0gpx1UHgAd/hQOFgwatBnDFxkUgA07CCcJcAdMCh8VMhxJGxUZeR+wJBonJiqMKKMi/R5nIJYbNhFUCGwCs/wS91r8"
"xgPmAZMAjQYfDQ8KbQcmDEUPERCBCeEE3AcbBDv/ZQO6B+4A7PcA82Xx3PQR9TPzvvFj7o3t+Oqv6i/t+uot7l3wI++Z8dTux+vm"
"7ZXtVfL+/5AG/wZNBeYAePhs6iPj6uPE4kHgF+J15IPlW+ai6SjwqPbk/qwBOfqV88PxPu0K5ovfs92+3JLbIt2q3gzlpuvn6x7r"
"3uqf5mDjYuHn5VPvBe3O5kbhXd/P44fntuXt6z72Q/UT+MT6fPpO+9r82P0d/yn/U/oa9zbwVuso60Dsy/F8+GD3i/YC/I/8Efzx"
"9zfyouoT5IniF99631XlZO0X9Bv9ngRICXISuhnhFSMThxaDA/Ppbug+88sAQQHc87/vdfJL9gb53vS38m743v0m91PpPONJ56vq"
"CenK6XHwyPVb7pLdy9aw3VrgM9y91qrZFeI95EjmVecO7MT3zP2V+vj82P5I/2wF7QJVBIMHcAU/BLYGSgznCQ8Kqgo9CdYMMg9V"
"D8MQURqnJvUqTiwDLvAu8zEcNDU4Kzu/M0AqzyhCLJEqXiRgIOEacBZOHmIo7CslLmgwojLRM541ojpcPII0jzLXMMUpXSfYJAgo"
"tiY3Ij4jgCGxHB4YLR3vJbUmAR/8GcMZ2xc4FZwNswTuASIDCgMdAKP7/vuC/eL52Piv+Tf2hPKd8VTwlPHK8lTx5fGU9Jr0vPIn"
"9H72JvjX91P3sfeS9lDxQe5W70XtIu1L7UHtI+oR5XbmHubB42zobu4W71ft7Ofb46XlSuuX66nmO+R14SDjr+Vn6t3tLOzM7Dvs"
"cO798s3zZ/Ej8GvuA+3+6/3pKfBr+CX7+Pli94X6IP8E/YX6yvlR/NAApADCAV4CQQJrArkDwgquDvoNigmbBDsFQAXyBw8N1QvC"
"CkcOUw+iB3IB7gRuChkLigbBBMcDngSeDCIT+xNnE9cTqBUeFIENxQkSClsS3xZIDj8Kkwm1A0H+RQEGBp0LkBKqFKUWkhhBGLwY"
"DiMDKuEpiiz1K2Mtmyt/JZMaTwks/V71AuzC4tff9t3z263Wu8xpy73Uu+Mp92kJWAy3EHkbLRPYB/8D6veG5PbZwdeI0m3QQtDB"
"0evXJt2M4oHmhesk9Hb5ZP1O/vP3M/Im8dHxsvwyCqUBU/c69xbwfef13UXUZtJ01JjVM9vt5ULrI+hd5hfmueHo40Tph+Yn46fk"
"FeO43sbbDtgM1ajSJdbG1qHUbtsw4YXn7+9g+HwB1wklGPsmBS3dMLEzAS8uKM8i4RtRGnMdEhiCDugKwgeI+8D3W/7O9pz1fgDq"
"BjIJ1gW//hH1fvrhBlYEXwOmBbcDtAZYCVMIdxMcHXMQYgn3D1cNFgdGBwYNbQyyBYYF8AqVEFkQVwxSBWwB+gOJCzUQoQoUDqwW"
"oBhkHFsljCfyJ8kwBy6QI8IdHx37G6QTaxGXHtMsNDJ0NBQwPC6gJ+4RfgXlBQ4DRvyV+G70KfdY/oH61O9f7rz87gkGEZwUJBbp"
"EpII0ADy9Xzqjeag5IDfp9kn1sbUytac2NPbDt263eXewdef0VjO4cskzzfTrtP91BPYsN5+6CHmH+il/eoJEAOV/1kIKRLCFxIU"
"LA6gCrP/5vslAxD+L/KA7iLqAucJ6YzvgvnB/Xz+7P8JCEIW1hVZDeUK2QN7ACUDZwY8CxgH0vzY+4L/NPb09QwGwQysESgdxCWK"
"KKQnbB61GVUZxQtXBJ8LBxAbD4kVVCLTJsUkqCTCI80kUCrNJQgUqgadAswBAgKdAs0Befr4/BoK4hJsGsYaPhdkE/sOmAx2EEQU"
"jw3aCEAKBRB4FeMV9hKnDXgF5QByAbj7bvMc7GTvf/wnAHD8bwBxCpwNWgm6BosLRgYS+pr2EfRG8a3rQuOb3L/ZG96J6QbvZexN"
"7eLxEvEm6XDtxfmj+DLyoPAp7kntJO5Q7Zby0fiX+Qv5Ev0kAPj4G/Q38vHpruTK5nrsvPLM/E8DqPsC/isG3f83/B4BCAXnAukB"
"FgJs/GXzd+/O9Pf3Q/lq/aL99/ie9Qv5HPoE8vrqleeF6W7sL+iN51buPfWb+HoBQQtLCIsG6Qi7BhcGXwpeDgkQpBOJGMMachui"
"HW4mLC79KpIm6ShgKJwbZRadHEodbhs5HBccRhY4Eu4MXgkNDPUHagV6CEIJdAgrB+wGRAZzAwP/8PylBcoQBRM6DvsIbgeqAx4C"
"WQL+/un/eAcCCKAAMfzA95r5WwBLBj0LmxCyFDcS/BicIQIc9BQSE4kSDxJuE2sTLg3CCksLjQj7B00H0gmIBun8YflY+sz5kfM7"
"8dXtoOfQ5MjlOOn/6hrtSus+6GTmHOap56vlmeBn3LjbgNxI3vXgNOCY3JbZVNWy1eLbFODv3aLWrNX221vg3+GI42rgRth/13/i"
"ruyV7YjugfO59sT0S+786M3ouekJ6hXqxeoz6prmq+fC7r31q/I563/sHPGM8HntDe/W8HT1uvw5+2X6PwA/Ai4ELwXpAQMBYQAp"
"BBAItQcoCEIH7gfpC/QQ4xJ+EgQStBI2FWoXohmUGqAZERe/E/EPiAzrBx4E4wakC7gO2Q+TDQcNuBA/FdIXhhQ3FZAa1xT2DfsQ"
"UBWyGegd8hhsEqMPvAeSBHsE7f2L+wz7KPeO+MT9df3D/ScCNANABtcErPm19CD7UAMyAGP7Lf6A+iz0LvOb7sPoYurQ8Fb4if6H"
"AfMA5vx/95/2K/gu9ebzK/bZ9NXw9PSD/ZgA4AKkABr9fv99Af0BsAQQBx0BqvmR+Vf51Paa9zj7Qf2g/RP72/h9/VQAx/6j/lr9"
"z/4dBPsGzQiwChcO6hWIGoEYYBdYGe0fkSK6FWQJlQb5BIsEjANOAc7+q/pL+Ef8kP1D9xr2cftyAiwJKw2YDIwIkAYIBR8FogrF"
"ESsWthTzC/7/efxaA70GGgJm+sbw0ewD8kv28fpVAoMEAQVPBg0HKgdIBoULPhKPFKMW+xNJCnwDIgMRALf9JQPYCAoMtQmqA44C"
"oP/R+x78s/3//7v7x/SL+GYCrAS5Al0EPQZFBe4AvPlc83H0Lvn2+uz7AP3G/Gb6e/bA9E31e/OH9GD2ie5B6qbz7/g39gb2ovre"
"AIH/x/iR9DL3evsf+sjzT+2f7vLt5Ord6ovooei37Xjzf/O270vu8+3I6njsp/Ws+fv5zvlr+BT8+AHrBK0FpgLMAGYDHQKf/pr/"
"jgCi/6r+PgHqB0cLOAoFBAH95foR/r0DJQSuAIH+0v2S/Dj/GQLUAbIDMAYhDJAOlwohCfYKGwv+CD0IXgn/C+4I+wNRBhkJvASL"
"AHUDNQVwAL75CPpr/fn7kv0GApgB1gGbBpcLTg+pEEMOrQqACB0GJARPARX+pv26+7/45fmf/VsA9APdB+8Iewd3B/YL5A7xCrMF"
"dAUaBnUEhATXB6ILAA7xDSwM7guADPANBRH5FKQVbhLHE/UXCxnWGPMZlx10IkImUCcqJdMhJR75G68d5h2IGTwU3g0TCXQG6gKR"
"BKQH+AidC3kHPAFZARwDLQHk/1kC2wGh/NX1GfWX+Nr36fLP69rnM+cF5tjhlt3B3j7huONp5kzps+718SnyYvNa9EPyqfFl9Fj2"
"hvXM757rHOt56jXs6+9v8oPwP++u8V3xU/E/9y78Bv3C/+wDZAWgAzoCOgIFAgAB8P+RAfIDogIPAmMECQJh/Df4mvcL9eHvHvIN"
"95X9IgRNBjMG7QJxAB7+Nv0Z/3z+aPqF9gL6OgLfBKb/ivpb+n36zPl7/UIEHwMA/oj+If9h/sf9FP6cAYkEdASaBAcFqQbKDCgR"
"/hBGDe8HDgeOCJQJ0gi5CAIICwTWAkQETwWDBDsGZgqwCZwHagi8CWAK9gzID8UN3wiNB+0J9AoHCusHBAPT+3/2m/Zu/BQAD/7T"
"/DP9H/0l/REBTgSHAe3+3v1j/Pn70v2N/uj70frr/+sEugL4/ZP6Afge9VL13fjU9pjwge7l7WjqVecd6NLsavCw8NPta+ck43zi"
"ueON5SPkK+LU51bvfO4o7lDyz/PC85fymPIA9v73C/xzAKv9UPvL/dwAdQQqCBIJ2wo5DkkOEg2WCkkMsBF8D1AMgw3wEF0T+Q62"
"CcYGkgfoDKcPKw7RCowHugTVBO4DGQEnBDUEXwC7AWYEQgSGA8kGYAttDbAN7QtwBsr+GvoB+Qf7MP6+/V7+ZwMiB2UIRAZdBMwG"
"pgijCPIFaAC5/VUAwgM/BwcLSgrJCPYMDhCcDTUM/Qo9B08DvwBAARkGBAxcC1wKWw6HD3wKwwM4BBYIHwj6B4IJlwgMBV0FBQeB"
"BNEC3ATnBNEDHAOd/rr8sv/0AXsEIwZSBfgFTQZYA/4DqQioCnEILwe0CMcGWgZqCcYIxAfXCccL4AgiBrYIkgjZArr7EPm0+1/+"
"ev+2/lr+SP7I/tkBAQQOAu78aPro+2f8k/gS9E31dPkA+GnwOO3Y8VH1kvbe+QP7nPdC76flF+O/5I/msufd5pzmx+dy5znoF+dE"
"4kXkp+RJ4hblbOWs5h/nXeGb29PYpdvH4gznUOp58PDzXPLp8RLzGvXl+xcAwvzt9vDxDPNi+kH/LwGhA+wC+gRaCjALkwmjBxYG"
"lALC/BT8Nv8v/nT+4wAbAg4HFAsMC0oINAXIA2MDKgbbCDYM/hDtEJcPvhCcEGkQ+xA+D3gKSAa8B7AMeg2sCUEGVgXWB0oJkwmv"
"CnAIsAV5BUIHYwfYBVEIGAx8DuwRVBYBF9oUExYGGAIY8xf1FgUWBRY4FY0TmBIhE7ESkhAEDjYLNAi3BYYEuAOxBIAGBAUSAQj/"
"jv/e/8UAeAFQ//j8i/xW/az+zv3h+uT5/PwFAmgEtAN0/0v7APtM+ij5APeE9br3Gfrp+hr6Nvdu9Hr3x/vq+SL3rfSf8IvwNvQR"
"99361vvk+NL5V/3p/tz+lf3r+m31Q+437BHwOvPg8uHyfvXi9A/yuPO497r5Ifdd8n3xLPRV9qn2ZfYf9H3wg+4J7x3wOO7n7Rbw"
"f/Fk8uzv6u6V84D2OPen+Nz0dPEo9RT70gJWBqYFRAi5C+YNCAsBBZkDkQUGBS4CIgWhCcgKjQuhCBgHZAbOAgoEogl3DasNGwxQ"
"CtEHQAbGCR0RCBa5FvgUcBIkEnESQBBmDbAJBQhCCe4KiAsqDNgPQxLHET4P7AvxB7QDOALRAaMBgQHiAeEDvAjoDyUV2Bf+GBUW"
"YRHyDnoNPAydCuUIDAgECCAHwAb9B2EIeQdyBL/+5vYT8SzuY+0P7g7tFuwM7azvQ/Iz9WT3sPdJ+H33DvUX9U/28vNn8d/ykPRa"
"89L0tPe89e3wme0Q76PymPW69VvzJPSK+TT+4P64/sX+sf9SACIB4gS0CR4L0Qh9B8UHKArBCqkHMgdgCYsLmAozB7kFzwZNCa4I"
"YQX5A/oDWQOnAjoD7gQOCFEGIAGr/9P++/8wAQH/rP2e/eH8wvxP/X39cf6b/hT9G/sw+4j86PvM+2z9NQB0AjkCqAEbAAf/d//U"
"/Hr69vsN/5oAQwA+AdcDqQVLBJwCHQKXAf4Ab/+9+3j40/lw/TIBwAPMA90CSgTLBnwG3wR5A7cDbQUABG4AWgCQ/2j/XQFe/yT8"
"Fvop+u/63/v3+8T6s/pm/An9lPiW9bnzsu/O7rXvh+yS51LnZum76fnqhvDx9OfzfPJR9RT6Mvz3+Xz3ZvaL9Ib2UPrn+4T9mgFW"
"BaQGFghiCN4I6gZBAzoCtgPIBw4LAQ0KDl8Nsg2GDxUQnhBQEbwQ3A34CcYIdQqIDBAMEgozCFQJGQ1zEsoVUxKaDWoLhQ3FDm0L"
"xwYkA90B5/6N/rABCQLOAS8Adf1y/rcB/wL2ANv9TvvY+uT7b/3jAFAC5gBQ/6L/V//W+cD0kPRy9Vn0PfSp9gT6J/oU9w73Mfmt"
"+dX4zvkx/BP8Ufp1+iT6gvjK9173OPg/+bn62v3yANECewIDADL/+wBPAbL/4/xH+4j7iftL/Gv+AwDmAekEgQX4AyIDcgOXAhQA"
"tv7p/48BwQBL/r78Jf4wAYgEXwTQAf8C5AM+AiUBBQGNAf0C3ANmA3MDPgRCBCoDRwAL/b78zPs1+S/3w/Ww9ALzT/Lp88/2D/n/"
"+oz5QfYT+Gn5qfUj85X1JvhV+H34mPr6/hUCuQEa/9n84Pxp/mEApwHcAPH/ZgBgAe0CfQOiAs4CEAMtAZoAtwIrBaoGpwZ5CHUM"
"lg8/EOQOmA0UDcAMGwuICP4FjwR6A3cCZASjBdQCq/9E/jL/u/8l/cb7bPwN/hUBxARUCWELwwuiDfUQ9RPCFToYqRg4GFEYTRai"
"EAgLgAutDHANug4VDL4JdQnyBz0FoAE9/o78af5nAhwC6f7T/ET5RfRr8FLx6/c8+175w/lr+L7yPOxL5jLlC+gk6nfrWuwX6ijn"
"Bee65EDiB+cW7gXxRO4j5wfmeett7YHsEe0M8bX2hPh1+ZL71Pmo+K/6BP6QABL/5P10/0kDsQhTDpURcRJCFCYSmA0kDXoNjQxH"
"C3wHCgTqBwcMlQgrBcoFKgcXBZ7/Wf2s/RcCtQqaDcUOphGNDw8KGQc/CUsOQRImEE4HyfwZ9HTrjeEc3LLbMdo/2ETaZtuR12XV"
"S9YB2EzZItn32nLfo+Sb6SftZ/K/+kj/GwGOBBcGignhDyMU5hMyDGsEmQEEAOT+r/8SBG4HIAbYAaP/XQEYA+ADaQNsBIEGowh0"
"DDQPtxGxFMwWPBiQGg8daR2yHsUimSccKXMk5h+DH70f0B7NHFYeyiAIHKkUShEnEZ4Q7Q1PCcwH0wjwBTICVgECBDkF8gEXAHAB"
"4gNdBIAEIAfUCCsJpwfjAyACMgJIAfH+U/1+/YD8vPo7/O0AewOLAJz7aPkx+LH1mPOF8cLtm+mW507mh+Mp4WzgleEa5bDmDeco"
"6W/r4e7079TuKPII+Rn98vwR/qYCeAdbCNYFkgNlBDUHowdtBm0EegI/AQD/Av44/3MAZACMANcBOwKJAskC8QBB/cb6UfrT+af5"
"4/iM+G/4svea+Wb5q/Us9P7z1PJr8eHyWfXg9HH0EPXs9Tr3/Pi8+vj5Hfof/n8BMAHd/pz8FvsO/IL/UgMMBdAFGAiiCosOQRRU"
"GBoYHBaLFVgVQhbGGIAchiBtImUicyEzHisacBmYGBkXBBcyFIIQ8A2xDBUOqw5QDPcHCwQ+AaT+T/3o/Jf8v/ze/Ff96/2/+yD5"
"QvrX+3b70frT+Uv5LvlH9wz1RPXx9pD3Rff599f5r/o/+rP5k/jj+ML6gfv8+jz68/rP+zL+zwPuB+4JTAs3DPMMRA0LD04RWhBH"
"DSUMXg2wDnwOEA3EC64KJQpVCjQJTgcFB5IGsgbtB6YGIQQ1A/4DuAP4AXUCDATEA8AAoPz9+of74/qJ+dv39vVY90P4RfZg9T/1"
"F/Zy95f3jvbl9kX6Vfxi+7j56Ph++b76Wfvu+fH3v/ZU9kn3vvfJ95n4VfjM96j3ZfeQ9vXzBvMV9Ojyj/Ad8JnxkfNr9Dfz0/Pw"
"9c/0RvO28mXzIPcn+439Ff6l/SH91Pw7/nEA9QHFAT3/nPw7+x/7kPt5+wb8LP3a+vL0KfRf+D35u/eY9xH76v+NAkIEPgU1BooH"
"+QiFCe8HegUoBfsH9AhHCWkLkgrcB3UF3gO4BLAGawdHB0UIZgjxBqsFzwPXA54GaggaCYgIXQfVBuUENAI4AM8ARgNjA98CRwO7"
"AjABnv/f/K/4rPZ99R/07vOL8vTxdPHU753w9vGz8yn0yfJi9fT4OfsA/3oCjgI+AEUA+QHMApkCkQJyA6UB1Pzz+Rf77/oc+Af3"
"y/Xe8+306vgC+yf5svcX+dX7kf7X/93+NP8xAFn+of2A/q38Dvjs9Lb1VffO+K77gv0f+3X34PYE+Bn2BfMg9mb8U/8nAi4I0AzE"
"DKsKXgj8BggHNQdgB1UH3QWnA7oCugGN/w7+Nf1G+5n51/oE/f/86/o7+tj81f7I/U/8Yv2MAGwDXgUFBhsFTAR/BcMG2wV+BGwE"
"0wWUBgcFmAKCALH+Bvyx+r77jfpE+JL4jvn3+f768Px1/oT+Sf4SAIYBOwBr/3P/Yv5q/Dv8IP/7AJQAUgE2Ay8C3P4X/nUAFgOB"
"A/QBKAC/AEADBQTxA2ME3gSNBB8DxgI+BDEHKArDClwKQAqBCUcJCAvLDfYPgBGzEkASqRBYD9IN7gssCgAJrQcBBiQDvP5k/VP/"
"qf+H/cj5MPaX9PDz6PIM8wb2nPk8/JT9tv0P/wMBZwLLA1gD8ACH/zAARwBp/0v/Xv++/3cARAC7/ZX6RflW+ET3aPeB+WX6AfgB"
"9qH2lPqHAKwEgAV4BKIDqQRZBrUFbAXpBgUHgwYUB+8IeQtYDWQNnAvnCVoK0QvkC2ELAgo/CP4HDwitCPoJ5wogC6AK5AnICOYH"
"gAcpBYQA9v1i/dj7TvwF/rr8VPo8+vL6fvqa+uf6uvlG+Gf4z/kV+sP4MPdA9vr1yPZH+Kr3VvWk85/zsPTb9AL2cPgR+NL14fRX"
"9XH3YPrA+kv4AvZJ9d/1+Paz+Jr7ZP6U/93/jQD4AMEAvwGnA8oFwwhjCqAJFAjdBrEGvAZdBhsG5AVnBcQFkgc4CbsK5wskDIoM"
"3QxeDAkM2wxgDVYNwg2CDaYMCAw1C9YIPwb5BpkJAgrVByYFkALt/wb9/flp+Ij36/Sy8Qrxx/Fb8UvxWvK08yn1DvbA9fb1zfcd"
"+fP5qPsn/cH8wvp6+ZH6Hf4pASkBWv/n/Tz+ef9rAGcBZwK3AhYC6wFvAmYBc/4w/Db8a/1k/TD7Afod/K7+rv+t//3+Af62/Nf7"
"ffyk/eH+jwCRAosEfwUXBhwHmwe0B/QIxQpsC4kLJAt+CU0HgwbyB0EJpAlHCbAHrgY7B8sIkQrFCfwF/QOTBXIGMgRUAfv/0f4U"
"/d/8xv5t/yX9ffqc+d/6KPwM/Kz76Pq2+dz4Qflb+tz65/tY/SD+VP5M/jf/DQHwAvUEuAatBiYGcwcSCF4HlAfaB4oIkgneB3IF"
"HQbIB+4HUgYdA9gADwD1/Kz4C/cZ9Q7xru/58cvzmPMQ87rzsfQF9U30C/IK8fnxy/ES8XrxxPHk8Qf0MvbJ9rv49/pm+5L8t//H"
"AfIBpQAi/AH4h/dR9xf3BflE+yb8p/ze/V3/fP7e++b6uPts/Oj6/fnA+3v9ZQCqBWQJ3wh+CDcKogptCi0KpArQDH4PcBEYEX0P"
"+Q3KDHgK9AXTAucBdAC5/b37Vfzx/Uz/DAG+ATgAV//n/8UBDQX+BVUEAATeBGkDCgIJBaoItQj2BFIB2ADN/2j7P/b+8yv0n/RK"
"8+ntbOeg5Kfmy+lC6p7qseyo7EHpu+ci7ZP2V/xO/lQBEAToAxsCYgLIBQIH+wIb/h//jwVICTUH+gMOAuMBiAZZDIILngdTB4EK"
"jA46DwgMZAlECBwHSAenCgoP1hHaElUSURLjEd0NgQmlBwcH8QbHBbIDMgM2BKoEkgOeATMCdwYnCLEFcwTVBC8H/Qu7DaMJbwR6"
"AwIFywMiAFL+dADEAqoCkQNMBPoBsgAVAssC6wIaBCYGFAglB1YDhgCR/0sAnQBp+6b14vYJ+hT6vPer89Tw2/Gm89rxG+6R68nq"
"tOyv8LfzG/Rt8vTwSvSf+7L+IP6sABgDagDm+wX53veL92L3BfiN+KX5svxh/k/+9v6I/8T+7P3x/pMA0v8Q/e/7hPyW/ar/yQA2"
"AnIGQwsvD24RAxMhFlMYchhNGVwZTxffFdoUmRLsDjgLKAl9CJ4IkAgFB/QDtgG2AdgB1P9I/IX5Bff39e/3Gvh29fDzZvVE+mP/"
"NgBu/Xb8SP+WAGD80/d6+OX5OvfB8xzzQPMv8qzx1/Es8SLxAvKw8Fjtluuo7BPtMunp5GjlXOnU7brw5/JF9xT84f6HARgF4ghs"
"DIsOQxC2EqkTahPtFN4X1hkRGmUa3RtgHPMb9xvJGZ8U8Q/XC30GqAHZ//wAFQJMAB7+Ev6Z/Kr4MPey+Av5UPcn9v73z/vo/Rj8"
"//eY9b32Jfnt+OH1//QQ+Cv6pPny+vX+3wFJA30F0gcMCH4G/wOyAHf+lf1Z++z3GvYc9uD1xfTo9Db3IvmI+EH2p/TU9Dr2Ffgp"
"+Y/5ivv9/gMCXQX4CTwOExAYD/YMKgt/CVQHBgS3ACYAWQHj/4P7h/lh/Ov/RAGMAnYFHAlsC5QL2QukDQQORQlNAVP9a/+rAToB"
"XQETBHYHvggICMEGTATXAD/90PiW9PfyLPP78RXvj+4k8RvypvAm8JnxvvMv9Pvxlu8I7zHwsPFF8mrzBvbu+If98wR9DHMSsBad"
"F70ViRRNFJQRCgsjA3P+x/65/nv6qvbV9Qj1E/QG9ej3+PqY/Pv89/yK/en/DANyBXEHqwhlCJoH3Ad7CdgKtwu5Da4P/A9cEEAR"
"9RBlD2sN+AqMBxQEkwKYApAClAMiBuIHSAjtCG0JSgjDBUUDPAEU/lb5kfUG87Tu+Ohf5f3kkOW+5N/iwuHo4bni7+Mx5jTpl+ry"
"6TjqVezb7UruKPAf88rzM/P/9IL4bvyRAeUGFQoqC74M8g8MEg4RWQ4RDCMLFArqB68GcgePCOcHWAXEA3kDgQHZ/jD+rf6M/q39"
"rf2M/i7+Fv4pAAwCWAPvBasIWwmyCLUJDgzeC/gJDwqpC1INsg4iD0gPJhASEfkQ4g++Dt0N1gw9C9MIcwY4BSgFFgYoBxMHKgYS"
"BXYDqAF/ANb/z/50/VP8rvoX+AD2mvWC9pj3Rfg3+UH7b/27/SH8cfon+An0o/DO8M/ySvPz8in1nPh7+UP5q/oW/O773fqs+cH4"
"NPjb97P3Svig+XH6IPvE/Fv9ifxQ/hgDmgUyA0D/7fyX+576hvpF+wL9FP8gAC4BYwMhBlcJ2Qz8D2gSYxKsDwkO1g58D2oPLw/p"
"DQQMWAsJDPgLEQraCAkJFAddA4UBsgAw/+n9Tv1x/cL93vwe+2765Pun/Yf97fxm/Lf6k/k4+d33j/bL9AHxDO5L7jHwcPEr8Q/x"
"GPMi90r7hf15/kT/Mf/W/z4CsQM0BIUFBAasBOcCKwLiAqEDSQMoAmcAkv71/M36XPg+9nD0gfO387v01vaG+Sv71/vH/Nf92P44"
"AKkBVwPCBQ8I2AnnC2oOvxCjEZ0Qrw9GEHAQIw5iCh4H0gTcAykEvgPjAUgAWf8C/v77Afqx+EH3WfXe9In2APmu++f9z/5n/5cA"
"iwGXAuYETgc8CPgHDgcmBi0FSAIY/uH7IPvL+Ez1CfOY8iDzmfOY8/PzI/Xc9XX13/RX9MHzzPPd9JT2S/hG+iv9LwBQA8kHFQyz"
"Dn8R3RQ3F74YhRktGV8YtxbSE4cR8xCXEZkS0xF3Di4LewmCCFUIyggVCC0F0AFaAOr/Df94/8cAFQAE/pD8AvzR/In+6P/LAFgB"
"jgEuAR0APv9c/pj8/PqU+lL6bvgd9QHzUfLb8D/wDPPn9gH4V/W98Cftweuj653rUeuY69DsMe458M3zV/d6+Yj7af6UARcF+wjb"
"DHQQpxLaElwSvBGlEC0Q7hA3EZ8PPA37C6cLnQqgCDEHhwdGCOcGEgR+ArECjgPVA3gC0QB4AacDWQTjA3MFxgi6CXkHfgU/BR4F"
"GgSBAiUB3v8P/Vn50feK+Lj4Xfdf9abzmPI58u7y3fTC9iz45/nj+tD51/hw+pj9WwD1AkQGTwmFCoEK7QpQDF4OXRCgEckRDBBi"
"DK0IsgUAAhz9cfg39UTz7fGs8O7uEu097HXsWe3B7r3vL/DT8BzxLPFq8sn09fcg/NT/fQEtAm8EKAksDtYQ5RFAFEMXcRcNFmUX"
"/xkxGswYYhYbEQEJHQAV+eX1z/WQ9oL2nvXt9PfzkvHd7r7tU+7n7hbtW+gh43fhn+Xm6wzvcvC28wb3m/is+74AlgP8AzsF7wUi"
"A0n/5v5AAjAGaAjjCK8InglFDCYOsQ3HC6cI9wXZBQUF3f+4+Xz3p/kc/XH+jP2y/Cj9rf4e/738bvqO+07+IP/Q/af8tPwy/cv9"
"vv4TASoFAQefA1D/Av/AAc8CpP+S+y77+v0KAEwAdwHOA9MFHgn1DeoQQw4UBtwA7wRdCWoEpPsS+Vv80/7y/+sDOQr1DtYPbgsH"
"A//8j/3rAGcCnAH8/m/7Ffko+Vb6gvqL+vn7Sf4zAX0BCvvV8pbxp/Yo+mL5SPqy/xkFaAjYCjsN8xBZFJsTTw8YCkYFNwJ6AfMB"
"YABU+S7wPuy97Tvvg+5j7prxgfUC9ln0u/MC9eH3vPvy/5QDwwW3B1wLVRCBFDoWFBbNFY4V0ROCD+YJ8wVFAy3+sPYu8O3rqumn"
"6GLnjuUr5Hfjn+PO5kruIvZ7+Vj5w/nW+13+LAGsBIAH1gfPBb4DYwTSBrIHrgfQCFgJAggNBpoE4wMiAwYDsQQFBroFJAXEBPYE"
"swUbBxIKeQxDDBAMxw3TD6EQ8Q96DhgNrAuECjYLEQ1uDcYMqAx9DAkMZguxCSoGegKsAdgClQJRAIj9C/x4/B3+0ADsAscB8f0o"
"+lP4PPhI+DP3ZfQH8WTvvu/T8LzxovE/8KHu4+227RXtwuu76vfq3euF7FTsMus469PtU/Fg9Jb3XfzuAv8IwQyyDm0P1Q4ODVYK"
"bAcvBuwG/QdqCSgLfAurCpwKFAybDUYNswvoCTQIPwfVBvcH1Az2E34Z5Bt3HXkgnSOYJdIl0iILHvsaWxlYFqwRuw20C0YLCAyX"
"DQMQcxI9E9MR0A5aDFoL1wkVCBQHEAXhAaT+yfpU9gfyg+4h7Wfuz++C7vTroOph6WLn9uQv4c7dz9183yTgQt8V3QPcON4y4vLl"
"jOls7Y/xM/aT+yMAgwGS/yn8SPkT+Bj4u/jJ+RX6e/kd+o/8C//IADcCMALu/vz61Ppj/aP+//25/Wb/pAEZA3wF1givCpQLUA0U"
"DrQLoQiKBysGowIz/zP9ivvo+Z75KPx1AL4C6QGnAJP/e/37+rD52PkE+ST3zPcj+oj74Ptz+tD5PvzT/HX4QvOM8j71GPWm8azv"
"1vFl9tj3MfdW+pv+jP4R+2X5Vvxv/t36k/W39Qr8bf6/+av5AgBcA3ICkAI0BoAJugiXB1IJoAuYDGoNvQ9wEg8UUBUzFg8W7BRr"
"EtsNBAmnB30ISQd2BoQJmA2bD24P+w2yDXoP9A+VDVQLLgmuBIEAGgCQAWgCyQJ2A44EogTtAZr9jPqv+Dr1b/Bn7TXsFOvA6Yvo"
"Jul47JfuZO3M7UnyNfV28ubtY+zD7e/ukO577rLvh/Ev9Wj6I/51ABwDaAWqBq4G9wQlAn//Q/5I/z4BSgIgA1UFjgiZCpQKGgtD"
"DeQN3Qx3DTMPfw8FDtALpwroC0cOdA/vDtANSQ0aDiwQoxFBEL0MXAnkBt4FJQa6BZgDwQB//qT8gfq++GL3GvYP9Xnz0PLn9DH3"
"gvii+Tf5u/fo9yr6Ffxg/Pj8rP5GAHQBmgHnAG4ALP+O/IP6bvlE98Xzw/CW7knsr+mD5xrmuuSH47TjgeV+6ETrKuza63fshe9m"
"9G/4Nfqp+iD7PPwf/gkBTQQ3BgcHWgi2CW4KzAtqDkoRKBOfEoIQRxBxE5cXRhkvGGYWVhROE4AVjBg8GbIXJRXoEQoNwQckBHgA"
"1/v696L1rPTb87byjfJV9IX4l/1tASEEigV4BcUECATbA/EDegMeAjn/Rfzu+6D9kP/nAFsBAQLiAtICKwK8AIH+6/wu+w/5C/k3"
"+5b9+f5gAI8DpgYGCL8JRgweDpkOJg7pDSgNuAphB/oDZgFmAJwAlAE0AjICaALRAW4Afv9b/vT9rP7b/aX80P1X/6f+wfzp+yX8"
"pfsh+uz3tfT/8e7xnvMJ9Xz1ifSQ8tjwv+8p8E/zkPc6+rb62/k4+dP59/oa/Nf8UPwz+xf67/hH+UP7O/1o/xMCvgOqA/oC9wKG"
"BAgIKAzJDgIOogqlCGoIOgiFCZYLpAu1CZQH+QY0BysG/gN4Ar0CIQPLAVkAdgBtAdcBPAGQAJAAVgFbAksCcgE0AVgBFQH9/yL+"
"iP1u/0EBwAFjA+wF3gXZArj/p/4g/w7/Xv7L/f37ivgH9fTyCvPl9NT38vlR+dD3xvdN+eH7K/1F/FX8d/80BIwInwzpD3IQbw+i"
"DisNJQtGCcMHQgdaBw0HUAWJATr+x/2v/uj+Zf2s+pn4svYR9Uj1C/YS98P4wvjW9xr5VPtd+9j5yvij9gPzXfBH7rLsOu0L74Dx"
"bvR69qL3Fvn8+sT8Bf6j/ob+l/0Q/e398/4fAOQBNAI3AXsBpQL1AtsB9v/q/nn/NgDX/8v/VAHOAoIDhQQFBtMIIA1vELkRqhFv"
"EHMPow+CD2kNrAm7Bl8FZATdAxAExAW4CPQJUgnsCIgJuAsdDpkOeQ1zDK0MTw3JDe0OFBAYEAUPXg0JDL8KxwdUA8v/Zf7K/Rz8"
"Qfqe+Zj4VveS95/3//ZR96n3Cfdy9uH2qfdG9x32pvVv9crzbfHp8A/yVfPs9LH2APgy+S76P/pC+rn7Ef4v/zz+B/0S/WT9I/1k"
"/Bb8zPwK/aL8X/x7+0j6KPmL+AD6YPwQ/m0AHQTSB7sKyg0lEnkWgBjgFzYWhRU5FQcTYQ+SDJELlgvuCmcJ6ge0BuQFiQQfAvn/"
"sP4S/i79Z/tX+uz6v/ww/wQBLgKcA2QF4AWsAwsBfQBOAVwCYQNoBNsEegSWBIIFhQaXByAIDgjeCNUJ9Qh0BwMHdgcNCLMHEgdD"
"BhQDFf8T/qH/VQDS/ZX5GPfy9pz3C/i496n2mPWy9PHzUPQj9iv4Kvn0+ez7XP2A/GX6ffhY+Lv54/nD98P0qfL28UPyN/Pg8wH0"
"WvTX9F/1JvYe99P3Pfcv9UTzC/PM9DD3CPjl93n5+fte/Lf6v/lr+7H+0wA5AO/9xvzw/WkANQL4Ad4ADAC9/5IAAAJ3A0IEIAOn"
"AeMBPANXBLoEIwWcBW0FfQViBhEHnwdQCewLDA5/DwQRUxLCEuwSqxL2EHwOAgx5CU8HbwX5A7ACQQAx/Qj7m/kZ+KD2n/X79MP0"
"EPWD9cb12vUH9kz2ePYl9934z/p3+wT70/rP+in7TvyD/WL+iv6K/tL+Hf7K/Av8GPyX/c3/ZQAL/9n9vf53AVYEXgZ2B9wHGQi5"
"CIcKRQ2aDjMNWAqpByUGngUiBjgH4AUeAu7+rPyA+5X7vPva+5v7i/ro+TX6bPoi+oX5dPi59xb4CPnC+uj8pP0s/EH5n/ac9bP2"
"evgi+Gz1ZvJb8I7v4+9S8XDz1fS89JPz/fGj8afzdvZF+PD41fif+OX4UPrC/ZcBJQSUBsQI8gmaCkoLXgwpDeYMCAzkC7gMjAyA"
"CgEIAwZjBPECpAE7AOj+GP4J/W77nPrk+lD7M/sg+vL3+/UZ9qX38Pdz9n71vvWc9Wb1UPYn97L2yfXv9dP36vl9+pb6//t5/m4A"
"fAF2ApoD8gPDA/8DswP6Ap4CdQIEAwYERAR1A7cCfQLNAQMBhQHVAh4EDwaHCGcKKQvCC4EN4A8rEvMTTBRDE6gRNhCuD5IPLA7L"
"CxYKNAkpCFgGOQRZArUA6/7T/Bf7A/qz+Z76FvzD/HD8KPzb/A7+N//d/4n/Q/9K/5P+PP04/F77I/pc+Rz5bvjZ97/4E/u1/bf/"
"EgC0/iL9Ev1P/vH+B/5v/HT7W/vJ+9f7KfvN+iH76Pv9/KP+ggBlAQ4BOwAdAF8Asf+g/rL+SgAzArgD+QSlBbUFiQU8BbkE8wNc"
"AwMDbAI7AmwCBAIAAY//IP7t/OD7gvvE+xz8+/yx/n4A4gECA40DfQM8BAQGQwd/B3YHAwjkCNYIoAfNBkkHGwhzCLkHxQaZBs0F"
"MgSwAq4B/wBs//L8E/tT+hf62Pk++Xj4s/ca9/32Gff59+75m/vU+1n7ofsH/Sb/AgE4AtMCtwI8AlYB8gASAlkDRQPUAfD/j/6P"
"/Wj8BvvK+c75LvvP/ID+WgCCAcQB6AFGAhwDeAMKAur/8/7K/y4BrgGJAQMB4P9T/0sAYQHSAUYCvQLwAYv/gP0X/Xz+3QAFA3QE"
"4ASYBMsEHgeRChcM7ArVCKkHRwc3B/sHsglXC+QLYwvkCtUL+gx+DGALjArECekI8gf2Bp0GfQaBBVcEywN3AzcDAANqAuoB8wFy"
"AY7/Q/2v+6n6x/kT+c34/fi5+Zv7of4cAQcBHP/e/X3+GAAuAasBSgL5AhADCQOqA3gEnASYA+QB0//r/fv8ePxh+0X5dPck9wf4"
"ovlL+6X8Sv3I/D37Mfox+wv9Jv4O/7EALwIvArwAcP+F//j/wf8x/1j/ZwA8AYwBlwHqAbkCJwNdA0IEygQdAyoAZf41/hr+RP2U"
"/Jb8SPx+++f6xfop/Kj+IwBVAC0ANgCyAPoAAgC3/hD+nf04/ev8z/wR/Xf9/PxS+wL5l/aI9fT19PW09H/zUPJL8NPuI++V8f30"
"Wfcp+BP48vc39+z14vW299r5fPpd+tH7qv6tAPcBiwNkBfEGjgbABPcDAwR4AycDUgTGBp0JAQtDCtYIYQewBeUDXQKNAQQBPQBL"
"/+T9AP2x/A779fgl+Z376/1M/sP8r/px+Yf5x/qK/Lz+ZwCSAMkAKgKiA5QElAW8BS0EgQINAmcCIgPlBMUGTAZBA04Abf8U//D9"
"k/w1/Jn8rvuR+fz2M/RW84H1fPjb+VX5Xvcf9a/0AvdB+p78qf4UAYoDsARXBGYEQwboCBkK4wn6CZkKbQroCAcHagYPBxcHHgY+"
"BmMHIgaoAmMAsQChAlYEswXEB/EJBgpgBzUEJQMMBIIEFgSjA2cDvwL0ABH/Bf6e/Z796v2w/SP88vkf+Bv3ofb79RL1TPWK9935"
"Nvpf+IP2SvbN9Yj00vOU87bzcvTX9Z33HPkK+gT69vi7+Ej63vsl/eT+/P9k/5n+uv7s/jb/JQC/AQIEFQZ+BskFdQblCD8LPQ09"
"DxoQcw8HDnsMyArsCK8H7geQChcOCxCHEBcQ7g4mDq0NAgy6CRYHKAN8/kL6hfcQ9iX2GPi4+Yz4q/St71LrPemN6HbnX+X+4mji"
"QuQJ5xvpiOow60Tq1ugE6E3o0OrC7kbyQPbj+/0B6wZVCusL9AqhCAUH9wbICPQLwg5lEGkRvhHXEG8Pzw6eDhoOHQ2aC6cJGgjl"
"CCsMog9REqMUYBa7FpsVeBO7EMwOrg6oDx8RmRIOE/URqg+mDEYJhAXdASn/tvzz+fn2jPTT8/X01fZ799P22/bI9z/3OPVl9Kj1"
"2vcL+hD9EwD/AFsAH/9M/Un79/lT+Rb5mfmT+tT6avqm+oX7jPyk/db+hP/Q//EAcQLgAlcCxAENAYQA/ADfAeUBIQGbALEAdAFr"
"Al8DPgQXBVoGVAeoB+gH0gi6CrIMWg19DF4LVwuEDIUNfw35DK0MbQyWC2IKXgnSCOoIBQlWCAcHewWPBOMEpgWvBdYE+gOGA+MC"
"twFFAHL+8/yu/OL87/xd/FX7i/rf+TP5KPl++fb4HPhq+Nf5CvsG+5T6gPuK/df+o/+cAEwBkwBW/rz8IP1q/sr/dAHPAoYDagPh"
"At8C8AJCAikBxwBeAf4BYAEBABT/TP6h/Yj8Avtd+qT6I/v1+0/8hfs/+wL8sPye/B/8gvyC/h8BcwJ/AtsCHAQYBQsFXgToA7oD"
"sgK2AA//g/5M/jT9Mful+Yb58fmN+U34O/dY9vP1wfZ298H27vQL8yjyHfNL9Sf3Dvid+GT5UPpe+8j7qvq9+ED4C/rz+8/7D/qH"
"+Gn4kvlE+tj5lvlK+n/7e/wF/bb9of/nAeYCVgILAVgA+wBVAhwD2gNEBcsGKAh2CbkK1gv5CzUKQQjqB3oI9ggfCeMIdgiKCP0I"
"/ggJCJIGjAV8BR4GdAYaBjQFnQQEBawFrwWTBXEFdASkA7kDhQN+AioBd/+i/U38Iftj+kH6dvrt+hX7sfpU+vT5fPmi+eP5zvnb"
"+VP6EPso+1v6oPkc+T34nPet9234i/kx+l/6L/rh+f35UPpr+o36d/pB+uH61/t7/A/9I/6//woBUgHJAC0AIgDMAGwB9gG5AmID"
"jgM5AxUDTANiA0wDhAPoAw4ErgO1AtIBgAGOAUMBwQDNAEUBqwHxAfwBkQEoAYQAdP95/pP9bPyd+8v7Ofwu/JL7AvvU+mb6dPmt"
"+KH4BvnH+az6V/uM+4X7o/vS+/n75Puw+4n7o/vv++f7W/u4+lP6kfk8+K/2uvWb9bv14/Uz9qr20var9rv2SfcD+Pf4ePoe/Ff9"
"0/0j/rv+Kf89/3P/u//m/3YAXgGhAh8ETQUvBhMH8AeDCM4I3AjwCE0JDApdCt8JaAntCEoIAgglCF8Iugj5CNAIWghHCGMJvgoy"
"C5MKcwmqCGkIVgjlB/4Gdwa+BtAGIAagBKsCRAFoAKr/i/7v/Jn7/frR+nz73fxb/a387vvd++H7i/uC+7H7WPuK+gT6OPpI+2b8"
"Fv2U/bT9cP0n/QX9ovwZ/On7Nvyf/N/89fzl/Fb9Lv6n/r7+3f4z/+L/mQC8AHcATwCRANsAwwBaAGQApQHzAi4DywKSAscCcAMa"
"BHIEIAUZBysKEQxNC9UIZwbcBFkEDwUsBggGDARcAfj/YgGeA2QDUwE5/zj9Tfsx+vD5nfn2+Mf4ifkf+j/5VPfk9YH1nvbs+Jj6"
"6/ot+uv4Uvgq+Zf6DPzi/eH/rAHKA70GLQkdCjIKmglKCJIGeATKAk4DCAa0CBgJvAivCpwMYwpaBkwFlAY0B6wGeQaEBw8J3QmE"
"CX8JnQm+Bo4BYv5//eT8sPyd/Bn8rfyH/7UBhwAR/ur8/Pz5/K38Ov2p/jn/0f44/0QBMQMqAoz/P/5a/V77pfgJ9rj0lPQD9c72"
"KPnI+gv8A/0P/a37jPkn+I73OPe9+G38pP8pAXUCSAXhCIIKRgpxCm8J6wQJ/5L7yPvr/V8AMANBBoII7ghfCEcJHAvCCRsFbgFQ"
"AGQANgFZAq0C2gKyA5cDSgHh/bP6EPnl+SL8tv3m/fb9nf5l/9b/UP+j/SD8Tvx3/UL+P/5w/Pf4S/Yf9YL0f/Ql9d72g/nI+3/9"
"y/6e/hb8nPdG87PxCPMW9l/6y/4hARMAzv00/Vr94/v4+Nv1qvPs8wv3i/sTAKEDwwQiBMkDEQROBEkEfwT1BGgFVQb8B/oJhwy8"
"D3QScxNLEqAPxgyOCuMIBAg4CPQI8gn3Cs8L0Qx5DUgMRAkNBlcD+wCl/zQAiwERAu4BnQFJAesA4f8R/gL8Gfrb+Fb4jPiq+RT7"
"OPzg/EX9gf1I/ev8xfzD/Gj9Ev+0AFcBaQF5ASMBMwAx/4P+G/6X/WX9Zv4oAGYBCwIlA7IETAVUBN8CwgEpAVEB/QHBAoUDVwQo"
"BSQGrQf8CAkJWAg9B4QF+gMzA8YC3QGFAO7/mwD0AWYDngQIBcAEGAUbBlAGTAV6A1UB3P9f/7v+cf2R/FL83vtC+5j6f/mz+A/5"
"tvk9+Wr3+vR38jnwUO/Z75/wbfGr8jv0DvYj+Hf6kfzU/X3+Ff+V/4T/9f7U/nT//v/n/00ABQKWA7MDBwOSAocCtgIjA50DuQOt"
"A/oDyAR5BXkFYwWHBS0FvwQXBVUFiwRjA94C8AK7AikCwAGCARkB4/8j/hr9x/x2/DL8Zvxi/aP+Uv+G/wgA+gC7AQoCNQKSAuAC"
"cAJJAdT/Tf4E/dD7xfoW+l/57/hQ+fL5WPr3+lT8L/6u/2QAqQDXACgBnAHXAagBLAGOANX/R/+4/x8BXQLkAsgCmgLtAmADLQM+"
"AogBcgFOAfwAJgEpAqYD5gSBBUsFYgQqAxAClwHMAUMCTgKOAWAAIf8S/ir96/uU+uL5w/k++r372f2V/0oAGQDm/1kAIQFAAREA"
"dP6h/VD9s/wB/CP8IP0e/vL+6//ZAD0BIQEUAW4BDAJoAjMCxgFoAaYB4gJKBEIF2AXMBewEjQOTAmMCMgJBAb3/Jf4e/Qr9Tv1m"
"/Xb9h/3n/fD+9/85AKr/fP4h/db7fvpW+a74YviB+P74uPkl+zH9Bv/d/4H/wv5K/of9Lfzn+k36NPrw+Wj5VflW+vD7HP2j/Sf+"
"P/+fAGgBHQFMAMb/W/+p/tb9JP3y/KD9x/51/6b/OABbAV8C2wI8A+8DQwSSA0YCHwHIAIEBlAIgA2MD8wPlBOYFdgasBtoGxQZY"
"BoMFLQQmA7wCOwKXAWsB9QHrAvQD7QSKBVEFYgRMAxwC1wDP/yL/rf5C/u79Kv7y/mv/Kf+K/gr+/P2H/gv/+v6X/kb++P2B/Qv9"
"j/wl/CH8TfxP/Dn8TfyB/EX8TvtD+vX5j/pm+9373Ptf+8L6fvrZ+pv7Avzf+9L70/uJ+2L7o/sL/Jb8Of19/Tb9I/0V/pj/jwAP"
"AeQBHAMeBNMEhwVJBgAHPQemBlcFjgOPARwAjf/r/uP9Y/2b/fP9af46/zMAnwBPAAoACQDw/5j/Tf8s/wr/1v40/u38e/t4+tv5"
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
    const float cx = -0.110f, cy = -0.085f, cz = 0.020f, hw = 0.150f, hh = 0.175f, hd = 0.010f, fz = cz - hd - 0.001f;
    wbox(cx, cy, cz, hw, hh, hd, FACE);
    wbox(cx, cy + hh + 0.020f, cz, hw * 0.74f, 0.020f, hd, FACE);                                 /* hombros recortados */
    wbox(cx - hw, cy, cz, 0.004f, hh, hd + 0.002f, EDGE); wbox(cx + hw, cy, cz, 0.004f, hh, hd + 0.002f, EDGE);
    wbox(cx, cy - hh, cz, hw, 0.004f, hd + 0.002f, EDGE); wbox(cx, cy + hh + 0.040f, cz, hw * 0.74f, 0.004f, hd + 0.002f, EDGE);
    for (int i = -4; i <= 4; i++) wbox(cx + i * 0.030f, cy + 0.005f, fz, 0.0022f, hh * 0.97f, 0.0008f, RIB);   /* fibras verticales */
    wbox(cx - 0.040f, cy - 0.040f, fz - 0.001f, 0.050f, 0.032f, 0.0012f, TAG);                    /* etiqueta plateada */
    wbox(cx - 0.040f, cy - 0.030f, fz - 0.002f, 0.032f, 0.003f, 0.0008f, TAG2); wbox(cx - 0.040f, cy - 0.047f, fz - 0.002f, 0.032f, 0.003f, 0.0008f, TAG2);
}
static void plateHand(const float *p, int withPlate) {
    u32c GLV = RGBA(34, 36, 40, 255), KNU = RGBA(70, 72, 78, 255), SK = RGBA(30, 32, 36, 255), SK2 = RGBA(24, 26, 30, 255), SKIN = RGBA(176, 128, 96, 255);
    float a0[3];
    nadeSpace(p, p[3], p[4], p[5]); wpt(0.0f, -0.056f, -0.052f, a0);
    { const float anc[3] = { 0.60f, -0.90f, 0.12f }; limb(anc, a0, 0.040f, SK, SK2); }            /* manga oscura como en el video */
    if (withPlate) plateBody();
    wbox(0, -0.048f, -0.010f, 0.034f, 0.012f, 0.034f, GLV);                                        /* palma sobre la placa */
    wbox(0.041f, 0.000f, -0.010f, 0.010f, 0.040f, 0.032f, GLV); wbox(0.046f, 0.030f, -0.010f, 0.006f, 0.010f, 0.026f, KNU);   /* dedos y nudillos por el borde */
    wbox(0.041f, -0.020f, -0.040f, 0.007f, 0.012f, 0.006f, SKIN);                                  /* yema del pulgar/dedo (piel, se ve en el video) */
    wbox(-0.030f, 0.004f, -0.036f, 0.012f, 0.022f, 0.010f, GLV);                                   /* pulgar */
}
static void plateHandL(const float *p) {   /* mano izquierda: sujeta abierta la solapa del chaleco; casi siempre solo asoma la punta por la esquina inferior izquierda */
    u32c GLV = RGBA(34, 36, 40, 255), SK = RGBA(30, 32, 36, 255), SK2 = RGBA(24, 26, 30, 255), SKIN = RGBA(176, 128, 96, 255), FLAP = RGBA(40, 46, 56, 255);
    float a0[3];
    nadeSpace(p, p[3], p[4], p[5]); wpt(0.0f, -0.030f, -0.045f, a0);
    { const float anc[3] = { -0.90f, -1.20f, 0.12f }; limb(anc, a0, 0.040f, SK, SK2); }
    wbox(0.0f, 0.0f, -0.020f, 0.022f, 0.030f, 0.012f, GLV); wbox(0.022f, 0.010f, -0.004f, 0.010f, 0.007f, 0.018f, GLV); wbox(0.022f, -0.010f, -0.004f, 0.010f, 0.007f, 0.018f, GLV);
    wbox(0.034f, 0.010f, -0.004f, 0.005f, 0.006f, 0.015f, SKIN);                                   /* yema del dedo (piel) */
    wbox(0.060f, 0.000f, -0.010f, 0.045f, 0.050f, 0.003f, FLAP);                                   /* solapa del chaleco */
}
static void drawPlateVM(void) {
    /* Linea de tiempo (u = (t_video - 0.40) / 1.65): 0-0.27 la derecha entra abajo a abrir el chaleco y se hunde a por la placa; la izquierda asoma
     * y aguanta la solapa; 0.46-0.60 sube la placa; 0.60-0.85 la muestra; 0.85-0.97 la empuja hacia abajo con la palma dentro del chaleco. */
    static const float KR[][7] = {      /* mano derecha: { instante 0..1, x, y, z, giro, cabeceo, alabeo } */
        { 0.00f, 0.60f, -0.70f, 0.30f, 0.30f, -0.50f,  0.40f }, { 0.06f, 0.42f, -0.30f, 0.30f, 0.30f, -0.50f,  0.40f },
        { 0.12f, 0.30f, -0.18f, 0.30f, 0.30f, -0.50f,  0.30f }, { 0.20f, 0.30f, -0.40f, 0.30f, 0.30f, -0.50f,  0.30f },
        { 0.27f, 0.45f, -0.65f, 0.31f, 0.30f, -0.55f,  0.50f }, { 0.42f, 0.55f, -0.75f, 0.30f, 0.30f, -0.60f,  0.55f },
        { 0.50f, 0.42f, -0.50f, 0.28f, 0.30f, -0.60f,  0.55f }, { 0.55f, 0.27f, -0.18f, 0.27f, 0.25f, -0.60f,  0.50f },
        { 0.61f, 0.15f, -0.12f, 0.26f, 0.25f, -0.65f,  0.15f }, { 0.67f, 0.14f, -0.16f, 0.26f, 0.25f, -0.65f, -0.20f },
        { 0.72f, 0.15f, -0.18f, 0.26f, 0.25f, -0.65f, -0.20f }, { 0.79f, 0.10f, -0.21f, 0.26f, 0.20f, -0.62f, -0.18f },
        { 0.85f, 0.08f, -0.29f, 0.27f, 0.20f, -0.60f, -0.15f }, { 0.90f, 0.10f, -0.40f, 0.27f, 0.20f, -0.85f, -0.10f },
        { 0.94f, 0.12f, -0.52f, 0.27f, 0.20f, -1.00f, -0.05f }, { 0.98f, 0.25f, -0.75f, 0.30f, 0.30f, -0.80f,  0.20f },
        { 1.00f, 0.60f, -0.80f, 0.30f, 0.30f, -0.50f,  0.40f } };
    static const float KL[][7] = {      /* mano izquierda: casi quieta en la esquina inferior izquierda; tira de la solapa y la sujeta al meter la placa */
        { 0.00f, -0.45f, -0.60f, 0.28f, 0.20f, -0.20f, 0.10f }, { 0.10f, -0.34f, -0.14f, 0.28f, 0.20f, -0.20f, 0.10f },
        { 0.25f, -0.27f, -0.10f, 0.28f, 0.20f, -0.20f, 0.10f }, { 0.33f, -0.30f, -0.14f, 0.28f, 0.20f, -0.20f, 0.15f },
        { 0.45f, -0.27f, -0.10f, 0.28f, 0.20f, -0.20f, 0.10f }, { 0.84f, -0.27f, -0.11f, 0.28f, 0.20f, -0.20f, 0.10f },
        { 0.91f, -0.20f, -0.17f, 0.27f, 0.20f, -0.25f, 0.10f }, { 0.96f, -0.22f, -0.14f, 0.28f, 0.20f, -0.20f, 0.10f },
        { 1.00f, -0.45f, -0.60f, 0.28f, 0.20f, -0.20f, 0.10f } };
    float u = bfPlateU, R[6], L[6];
    plateKey(KR, 17, u, R); plateHand(R, u >= PLATE_IN && u < PLATE_OUT);
    if (u >= 0.06f && u < 0.98f) { plateKey(KL, 9, u, L); plateHandL(L); }
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
