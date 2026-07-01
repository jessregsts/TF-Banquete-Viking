#include <stdlib.h>
#include <stdint.h>
#include <semaphore.h>
#include "config.h"
#include "chieftain.h"
#include "valhalla.h"
#include <math.h>

int right_neighbor(int pos) {
    if (pos == config.table_size - 1) return -1;
    return (pos + 1) % config.table_size;
}

int left_neighbor(int pos) {
    if (pos == 0) return -1;
    return (pos - 1 + config.table_size) % config.table_size;
}

void chieftain_init(chieftain_t *self, valhalla_t *valhalla)
{
    self->valhalla = valhalla;

    self->chairs      = (int *)malloc(config.table_size * sizeof(int));
    self->chair_type  = (int *)malloc(config.table_size * sizeof(int));
    self->plates      = (int *)malloc(config.table_size * sizeof(int));
    self->plate_pos_a = (int *)malloc(config.table_size * sizeof(int));
    self->plate_pos_b = (int *)malloc(config.table_size * sizeof(int));

    for (int i = 0; i < config.table_size; i++) {
        self->chairs[i]      = 0;
        self->chair_type[i]  = -1;
        self->plates[i]      = 1;
        self->plate_pos_a[i] = -1;
        self->plate_pos_b[i] = -1;
    }

    self->normal_vikings_eating = 0;
    self->vikings_finished      = 0;
    self->banquet_over          = 0;
    self->waiters_banquet       = 0; /* contador de threads bloqueadas na barreira */

    sem_init(&self->cheio,     0, 0);
    sem_init(&self->vazio,     0, config.table_size / 2);
    sem_init(&self->lock_prod, 0, 1);
    sem_init(&self->lock_cons, 0, 1);

    plog("[chieftain] Initialized\n");
}

static int is_safe_seat(chieftain_t *self, int pos, int berserker)
{
    if (self->chairs[pos] == 1) return 0;
    int left = left_neighbor(pos);
    if (left != -1 && self->chairs[left] == 1 && self->chair_type[left] != berserker) return 0;
    int right = right_neighbor(pos);
    if (right != -1 && self->chairs[right] == 1 && self->chair_type[right] != berserker) return 0;
    return 1;
}

static int try_get_plate_positions(chieftain_t *self, int pos, int *p1, int *p2)
{
    int candidates[3] = {
        pos,
        (left_neighbor(pos)  == -1) ? (int)config.table_size - 1 : left_neighbor(pos),
        (right_neighbor(pos) == -1) ? 0 : right_neighbor(pos)
    };
    int found = 0;
    for (int i = 0; i < 3 && found < 2; i++) {
        if (self->plates[candidates[i]] == 1) {
            if (found == 0) *p1 = candidates[i]; else *p2 = candidates[i];
            found++;
        }
    }
    return (found == 2);
}

int chieftain_acquire_seat_plates(chieftain_t *self, int berserker)
{

    int selected_seat = -1;
    int p1 = -1, p2 = -1;

    while (selected_seat == -1) {
        sem_wait(&self->vazio);
        sem_wait(&self->lock_prod);

        int start_seat = rand() % config.table_size;
        for (unsigned int i = 0; i < config.table_size; i++) {
            int pos = (start_seat + i) % config.table_size;
            if (is_safe_seat(self, pos, berserker) && try_get_plate_positions(self, pos, &p1, &p2)) {
                selected_seat = pos;
                break;
            }
        }

        if (selected_seat != -1) {
            self->chairs[selected_seat]      = 1;
            self->chair_type[selected_seat]  = berserker;
            self->plates[p1]                 = 0;
            self->plates[p2]                 = 0;
            self->plate_pos_a[selected_seat] = p1;
            self->plate_pos_b[selected_seat] = p2;
            //plog("[DEBUG] Cadeira %d ocupada pelo viking (berserker=%d).\n", selected_seat, berserker);
            plog("[DEBUG-PLATES] Giving plates %d and %d to chair=%d\n", p1, p2, selected_seat);
            self->normal_vikings_eating++;
            sem_post(&self->lock_prod);
        } else {
            sem_post(&self->lock_prod);
            sem_post(&self->vazio);
        }
    }

    return selected_seat;
}

void chieftain_release_seat_plates(chieftain_t *self, int pos)
{
    /* Corrigido (race real, confirmado com ThreadSanitizer): chairs[],
    chair_type[], plates[], plate_pos_a/b[] e normal_vikings_eating são
    os MESMOS dados que chieftain_acquire_seat_plates lê/escreve sob
    lock_prod (is_safe_seat / try_get_plate_positions). Aqui eles eram
    alterados sob lock_cons, um semáforo diferente, então acquire e
    release não tinham exclusão mútua nenhuma entre si sobre essas
    estruturas. Por isso o acesso a essas estruturas agora também é
    protegido por lock_prod, igual ao acquire. lock_cons continua
    protegendo, sozinho, apenas o que só o release toca:
    vikings_finished/banquet_over/waiters_banquet. */
    sem_wait(&self->lock_prod);

    int p1 = self->plate_pos_a[pos];
    int p2 = self->plate_pos_b[pos];

    self->chairs[pos]     = 0;
    self->chair_type[pos] = -1;
    self->plates[p1]      = 1;
    self->plates[p2]      = 1;

    /* Corrigido: limpa plate_pos após liberar os pratos.
    Sem isso, se outro viking sentar nesta cadeira depois,
    plate_pos ainda teria o índice antigo e causaria segfault. */
    self->plate_pos_a[pos] = -1;
    self->plate_pos_b[pos] = -1;

    self->normal_vikings_eating--;

    plog("[DEBUG-PLATES] Receiving plates %d and %d from chair=%d\n", p1, p2, pos);

    sem_post(&self->lock_prod);

    sem_wait(&self->lock_cons);

    self->vikings_finished++;

    int last = (self->vikings_finished == (int) config.horde_size);
    int waiters = self->waiters_banquet; /* lê agora, antes de liberar o lock */

    if (last) {
        self->banquet_over = 1;
    }

    sem_post(&self->lock_cons);
    sem_post(&self->vazio);

    /* Corrigido: quando o último viking termina de comer, acorda todos os
    que estão esperando na barreira. Cada sem_post acorda exatamente um waiter.
    O próprio viking que terminou também vai chamar chieftain_wait_banquet_over
    logo abaixo, mas já vai encontrar banquet_over == 1 e passar direto. */
    if (last) {
        for (int i = 0; i < waiters; i++)
            sem_post(&self->cheio);
    }
}

void chieftain_wait_banquet_over(chieftain_t *self)
{
    /* Corrigido: a versão anterior tinha race condition.
    O if(!banquet_over) seguido de sem_wait não é atômico: entre o if e o
    sem_wait, o último viking poderia terminar e fazer os sem_post todos,
    e esta thread ficaria bloqueada para sempre perdendo o sinal.
    A correção é: verificar banquet_over e registrar como waiter dentro
    do lock_cons, garantindo atomicidade. */
    sem_wait(&self->lock_cons);

    if (self->banquet_over) {
        /* Banquete já acabou antes desta thread chegar: passa direto. */
        sem_post(&self->lock_cons);
        return;
    }

    /* Registra-se como waiter antes de liberar o lock.
    Assim o último viking a terminar de comer vai contar este waiter
    e fazer o sem_post correspondente. */
    self->waiters_banquet++;
    sem_post(&self->lock_cons);

    /* Bloqueia até que chieftain_release_seat_plates faça sem_post. */
    sem_wait(&self->cheio);
}

god_t chieftain_get_god(chieftain_t *self)
{
    /* Garante que o banquete terminou antes de qualquer viking rezar.
       Isso cobre tanto os LATE_VIKINGs (que nunca passam por
       chieftain_release_seat_plates) quanto qualquer corrida entre o
       último comensal e outros vikings que já terminaram de comer. */
    chieftain_wait_banquet_over(self);

    valhalla_t *valhalla = self->valhalla;
    god_t chosen = (god_t) -1;

    pthread_mutex_lock(&valhalla->mutex_prayers);

    int total_normal = 0;
    for (int g = 0; g < NUMBER_OF_GODS; g++) {
        if (!valhalla_is_super((god_t) g))
            total_normal += (int) valhalla->prayers[g];
    }

    god_t candidates[NUMBER_OF_GODS];
    int num_candidates = 0;

    for (int g = 0; g < NUMBER_OF_GODS; g++) {
        god_t current = (god_t) g;
        int count = (int) valhalla->prayers[current];
        int ok = 1;

        if (valhalla_is_super(current)) {
            int limit = (int) ceil(total_normal * (1.0 + SUPER_GOD_TOLERANCE_RATE));
            if (count + 1 > limit)
                ok = 0;
        } else {
            god_t rival = valhalla_get_rival(current);
            int rival_count = (int) valhalla->prayers[rival];
            int max_allowed = (int) ceil(rival_count * (1.0 + RIVAL_TOLERANCE_RATE));
            if (max_allowed < 1) max_allowed = 1;
            if (count + 1 > max_allowed)
                ok = 0;
        }

        if (ok) {
            candidates[num_candidates] = current;
            num_candidates++;
        }
    }

    if (num_candidates == 0) {
        for (int g = 0; g < NUMBER_OF_GODS; g++) {
            candidates[num_candidates] = (god_t) g;
            num_candidates++;
        }
    }

    chosen = candidates[rand() % num_candidates];

    // Incrementa prayers[chosen] DENTRO do mutex, junto com a decisão.
    // É fundamental que decisão e incremento sejam atômicos: se outra
    // thread entrar em chieftain_get_god entre o unlock e o incremento
    // de valhalla_pray, ela leria prayers[] desatualizado e poderia
    // escolher o mesmo deus, violando os limites de 5% e 10%.
    valhalla->prayers[chosen]++;

    pthread_mutex_unlock(&valhalla->mutex_prayers);

    return chosen;
}

void chieftain_finalize(chieftain_t *self)
{
    free(self->chairs);
    free(self->chair_type);
    free(self->plates);
    free(self->plate_pos_a);
    free(self->plate_pos_b);
    sem_destroy(&self->cheio);
    sem_destroy(&self->vazio);
    sem_destroy(&self->lock_prod);
    sem_destroy(&self->lock_cons);
}