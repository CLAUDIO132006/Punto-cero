/*
 * PUNTO_CERO.ELF (PS2, MIPS R5900) - reconstruccion de main()
 *
 * Direccion de main: 0x168738
 * Ruta: entry 0x1030f0 -> 0x100730 (crt0) -> main(argc, argv) -> exit()
 *
 * El ELF no tiene simbolos. Los nombres sub_XXXXXX son direcciones reales;
 * los comentarios "probable ..." son suposiciones.
 * Reconstruccion a mano a partir del desensamblado (ver main_punto_cero.asm).
 */

int main(int argc, char **argv)
{
    sub_106d50();
    sub_106ff0("PUNTO CERO para PS2 - arrancando\n\n  Reiniciando el IOP ...");  /* 0x3a7df0 */

    sub_158398(0);              /* probable SifInitRpc(0) */
    sub_158b78("", 0);          /* probable SifIopReset("", 0) */
    while (!sub_158ca0());      /* probable SifIopSync() */
    sub_158398(0);              /* SifInitRpc(0) */

    sub_1074a0();
    sub_107860();
    sub_106ff0(" ok\n");        /* 0x3a7c88 */
    sub_108a80();
    sub_109098();
    sub_106ff0("  Iniciando video ...\n");  /* 0x3a7e30 */
    sub_1086a0();
    sub_121580();

    float f = *(float *)0x3a99cc;
    char buf[...];              /* en el stack ($sp) */

    for (;;) {                  /* bucle principal, nunca retorna */
        do {
            sub_108c70(buf);
            /* f13 = f */
            sub_1216a8(buf);    /* probable update/render por frame */
        } while (*(int *)0x3b4a04 == 0);

        do {
            sub_1085e0();
            sub_108c70(buf);
            /* f13 = f */
            sub_1216a8(buf);
        } while (*(int *)0x3b4a04 != 0);
    }
}
