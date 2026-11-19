#ifndef MODELS_H
#define MODELS_H
typedef struct { int nv, nf; const float *v; const unsigned short *f; const unsigned char *c; const unsigned char *p; const unsigned char *vb; } Model;  /* p: 5 = cara del arma. vb: 3 bytes por vertice = hueso A, hueso B, peso de A (0..255) */
extern const Model model_soldado_tactico, model_superviviente, model_zombi_comun;
#endif
