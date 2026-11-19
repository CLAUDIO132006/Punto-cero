/* Sonido: sintesis de efectos y mezclador estereo (independiente de la plataforma). */
#ifndef SOUND_H
#define SOUND_H

#define SND_RATE 44100

enum {
    SND_PISTOL, SND_RIFLE, SND_SHOTGUN, SND_BOTSHOT,
    SND_HIT, SND_HEAD, SND_DEATH0, SND_DEATH1, SND_HURT,
    SND_RELOAD, SND_EMPTY, SND_STEP, SND_TICK, SND_WIN, SND_LOSE, SND_PDEATH,
    SND_BOOM, SND_PIN, SND_BOUNCE,
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
