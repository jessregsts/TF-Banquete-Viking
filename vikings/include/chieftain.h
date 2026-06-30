#ifndef __CHIEFTAIN_H__
#define __CHIEFTAIN_H__

    #include <semaphore.h>
    #include <pthread.h>
    #include "config.h"
    #include "valhalla.h"

    /* [DEBUG-STATE] Estados possíveis de uma thread viking, do ponto de
       vista de "o que ela está fazendo agora". Não interfere em nenhuma
       lógica de sincronização — é só um rótulo que a própria thread
       atualiza sobre si mesma, sob lock, para podermos "fotografar"
       o que todo mundo está fazendo num instante qualquer. */
    typedef enum {
        VSTATE_UNKNOWN = 0,     /* ainda não rodou nada */
        VSTATE_WAITING_SEAT,    /* esperando cadeira/pratos pra comer */
        VSTATE_EATING,          /* comendo de fato */
        VSTATE_WAITING_BARRIER, /* já comeu (ou é atrasado), esperando o banquete acabar */
        VSTATE_PRAYING,         /* rezando */
        VSTATE_DONE             /* terminou tudo */
    } viking_state_t;

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

        /* [DEBUG-TIME] Timestamp (ns, CLOCK_MONOTONIC) do instante exato
           em que banquet_over foi setado para 1. Serve para comparar
           objetivamente "quando a barreira realmente abriu" contra
           "quando cada viking realmente começou a rezar", sem depender
           da ordem de impressão dos plogs. */
        long long barrier_released_at;

        /* [DEBUG-STATE] Quadro de estados: states[slot] = o que a thread
           dono daquele slot está fazendo agora. state_owner guarda o
           pthread_t real, para detectar colisão de hash entre threads
           diferentes. total_vikings é o tamanho da tabela. */
        viking_state_t *states;
        pthread_t *state_owner;
        int total_vikings;
        sem_t lock_states; /* protege leitura/escrita do quadro de estados */

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

    /* [DEBUG-STATE] Funções extras de inspeção — não fazem parte da
       lógica de sincronização original, só "cutucam" o quadro de
       estados para ver o que cada thread está fazendo agora. */
    extern void   chieftain_set_state(chieftain_t *self, int id, viking_state_t state);
    extern void   chieftain_snapshot_states(chieftain_t *self);

#endif /*__CHIEFTAIN_H__*/