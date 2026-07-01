#ifndef __CHIEFTAIN_H__
#define __CHIEFTAIN_H__

    #include <semaphore.h>
    #include <pthread.h>
    #include "config.h"
    #include "valhalla.h"

    typedef struct chieftain
    {
        valhalla_t *valhalla;

        int *chairs;       /* 0 = livre, 1 = ocupada */
        int *chair_type;   /* 0 = normal, 1 = berserker, -1 = vazia */
        int *plates;       /* 1 = disponível, 0 = em uso */
        int *plate_pos_a;  /* posição do 1º prato usado por cada cadeira */
        int *plate_pos_b;  /* posição do 2º prato usado por cada cadeira */

        int normal_vikings_eating; /* vikings normais que ainda não terminaram */
        int vikings_finished;      /* total acumulado de vikings que já comeram */
        int banquet_over;          /* 1 = banquete encerrado */
        int waiters_banquet;       /* threads bloqueadas esperando o fim do banquete */

        /* * Semáforos — lógica clássica de sincronização:
         *
         * vazio     → controla cadeiras/vagas disponíveis (bloqueia se não houver)
         * cheio     → sinaliza fim do banquete
         * lock_prod → mutex para proteger a ocupação de cadeira+pratos
         * lock_cons → mutex para proteger a liberação de cadeira+pratos
         */
        sem_t vazio;
        sem_t cheio;
        sem_t lock_prod;
        sem_t lock_cons;

    } chieftain_t;

    extern void   chieftain_init(chieftain_t *self, valhalla_t *valhalla);
    extern void   chieftain_finalize(chieftain_t *self);
    extern int    chieftain_acquire_seat_plates(chieftain_t *self, int berserker);
    extern void   chieftain_release_seat_plates(chieftain_t *self, int pos);
    extern god_t  chieftain_get_god(chieftain_t *self);
    extern int    right_neighbor(int pos);
    extern int    left_neighbor(int pos);
    extern void   chieftain_wait_banquet_over(chieftain_t *self);

#endif /*__CHIEFTAIN_H__*/