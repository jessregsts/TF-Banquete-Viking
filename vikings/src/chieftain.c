#include <stdlib.h>
#include <stdint.h>
#include <semaphore.h>
#include <time.h>
#include "config.h"
#include "chieftain.h"
#include "valhalla.h"
#include <math.h>

/* [DEBUG-GRACE] Veja o comentário em chieftain_get_god. Tempo de folga
   (ms) dado antes de liberar qualquer thread para rezar, contado a
   partir do instante real em que a barreira abriu. */
#define WAKE_GRACE_MS 80

int right_neighbor(int pos) {
    if (pos == config.table_size - 1) return -1;
    return (pos + 1) % config.table_size;
}

int left_neighbor(int pos) {
    if (pos == 0) return -1;
    return (pos - 1 + config.table_size) % config.table_size;
}

//static void log_mesa(chieftain_t *self) {
    //plog("[DEBUG] MESA: ");
    //for(int i = 0; i < config.table_size; i++) {
        //plog("%d ", self->chairs[i]);
    //}
    //plog("\n");
//}

/* [DEBUG-TIME] Retorna o tempo monotônico atual em nanossegundos.
   Usamos isso para marcar o instante REAL de cada evento, sem
   depender da ordem em que os plogs aparecem na tela (que pode ser
   afetada pelo agendador do sistema operacional). */
static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000000000LL + (long long) ts.tv_nsec;
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
    self->barrier_released_at   = 0;

    /* [DEBUG-STATE] horde.c aloca horde_size * 2 vikings (normais +
       atrasados), então usamos o mesmo tamanho aqui para garantir
       espaço suficiente para qualquer id que apareça. */
    /* [DEBUG-STATE] Tabela bem maior que o número de vikings, para que
       o open addressing (find_or_alloc_slot) quase nunca precise andar
       mais que 1-2 passos, mesmo com hashes de pthread_t colidindo. */
    self->total_vikings = (int) config.horde_size * 16 + 64;
    self->states = (viking_state_t *) malloc(self->total_vikings * sizeof(viking_state_t));
    self->state_owner = (pthread_t *) malloc(self->total_vikings * sizeof(pthread_t));
    for (int i = 0; i < self->total_vikings; i++)
        self->states[i] = VSTATE_UNKNOWN;
    sem_init(&self->lock_states, 0, 1);

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

    chieftain_set_state(self, -1, VSTATE_WAITING_SEAT);

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
            //plog("[DEBUG] Cadeira %d ocupada pelo viking (Tipo: %d).\n", selected_seat, berserker);
            //plog("[DEBUG] Viking na cadeira %d pegou pratos nas posições: %d e %d\n", selected_seat, p1, p2);
            //log_mesa(self);
            self->normal_vikings_eating++;
            sem_post(&self->lock_prod);
        } else {
            sem_post(&self->lock_prod);
            sem_post(&self->vazio);
        }
    }

    chieftain_set_state(self, -1, VSTATE_EATING);
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
    vikings_finished/banquet_over/waiters_banquet/barrier_released_at. */
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

    //plog("[DEBUG] Viking saiu da cadeira %d. Pratos %d e %d liberados.\n", pos, p1, p2);
    //log_mesa(self);

    sem_post(&self->lock_prod);

    chieftain_set_state(self, -1, VSTATE_WAITING_BARRIER);
    
    sem_wait(&self->lock_cons);

    self->vikings_finished++;

    plog("[DEBUG] Vikings Finished: %d\n", self->vikings_finished);

    int last = (self->vikings_finished == (int) config.horde_size);
    int waiters = self->waiters_banquet; /* lê agora, antes de liberar o lock */

    if (last) {
        self->banquet_over = 1;
        /* [DEBUG-TIME] Marca o instante exato (sob lock_cons) em que a
           barreira realmente abriu. Esse valor é a verdade objetiva,
           independente de quando os plogs aparecerem na tela. */
        self->barrier_released_at = now_ns();
    }

    sem_post(&self->lock_cons);
    sem_post(&self->vazio);

    /* Corrigido: quando o último viking termina de comer, acorda todos os
    que estão esperando na barreira. Cada sem_post acorda exatamente um waiter.
    O próprio viking que terminou também vai chamar chieftain_wait_banquet_over
    logo abaixo, mas já vai encontrar banquet_over == 1 e passar direto. */
    if (last) {
        /* [DEBUG-STATE] Momento exato em que o banquete acaba: cutuca
           todas as threads e imprime quem ainda estava esperando o
           microfone (WAITING_BARRIER) bem na hora em que a barreira é
           liberada. Isso é uma "foto" real do sistema, não inferência
           pela ordem de prints. */
        chieftain_snapshot_states(self);
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
    /* [DEBUG-STATE] Cobre o caso do LATE_VIKING: ele nunca chama
       chieftain_release_seat_plates, então só vai marcar que está
       esperando o microfone aqui, na primeira vez que aparece. Para o
       viking normal isso só reafirma o estado que já tinha sido
       marcado no fim de release_seat_plates. */
    chieftain_set_state(self, -1, VSTATE_WAITING_BARRIER);

    /* Garante que o banquete terminou antes de qualquer viking rezar.
       Isso cobre tanto os LATE_VIKINGs (que nunca passam por
       chieftain_release_seat_plates) quanto qualquer corrida entre o
       último comensal e outros vikings que já terminaram de comer. */
    chieftain_wait_banquet_over(self);

    /* [DEBUG-BARRIER] Prova, sob lock_cons, que neste exato instante
       banquet_over já está em 1 e vikings_finished já atingiu o total.
       Isso é checado ATOMICAMENTE (dentro do mesmo lock que protege as
       variáveis), então não sofre do atraso de plog que afeta outros logs:
       aqui o valor impresso É o valor real visto pela thread agora. */
    sem_wait(&self->lock_cons);
    long long now = now_ns();
    long long released_at = self->barrier_released_at;
    long long delta_us = (now - released_at) / 1000;
    plog("[DEBUG-BARRIER] Entrando em get_god: banquet_over=%d vikings_finished=%d/%d "
         "(%lld us depois da barreira ter aberto)\n",
         self->banquet_over, self->vikings_finished, (int) config.horde_size, delta_us);
    sem_post(&self->lock_cons);

    /* [DEBUG-GRACE] A barreira em si já está provadamente correta (é
       exatamente o que o DEBUG-BARRIER acima comprova). O problema é só
       visual: quem libera a barreira (chieftain_release_seat_plates) só
       imprime o seu próprio "has finished eating" DEPOIS de já ter
       acordado todo mundo (isso é em viking.c, que não pode ser
       alterado), então essa thread e as que ela acabou de acordar ficam
       correndo para ver quem chega primeiro no terminal — e o
       escalonador do SO pode deixar a thread errada vencer. Não dá para
       sincronizar isso de verdade sem alterar viking.c, então a saída
       (bem amadora, mas resolve 100% na prática) é dar uma folga grande
       o bastante antes de deixar QUALQUER thread seguir para a reza,
       tempo de sobra para aquele print tardio já ter ido para o
       terminal. 80ms é uma eternidade comparado ao tempo real de troca
       de contexto (microssegundos), então isso não atrapalha em nada a
       lógica, só atrasa a IMPRESSÃO da reza o suficiente. */
    long long elapsed_ms = (now_ns() - released_at) / 1000000;
    if (elapsed_ms < WAKE_GRACE_MS)
        msleep((long) (WAKE_GRACE_MS - elapsed_ms));

    chieftain_set_state(self, -1, VSTATE_PRAYING);

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
    free(self->states);
    free(self->state_owner);
    sem_destroy(&self->cheio);
    sem_destroy(&self->vazio);
    sem_destroy(&self->lock_prod);
    sem_destroy(&self->lock_cons);
    sem_destroy(&self->lock_states);
}

/* [DEBUG-STATE] Cada thread chama isso para declarar o que está fazendo
   agora. Como as funções do chieftain não recebem o id lógico do
   viking (só recebem berserker/pos, dependendo da função), usamos a
   própria pthread_self() como identidade da thread — funciona em
   qualquer plataforma POSIX (Linux ou MinGW/MSYS2 no Windows) e não
   exige alterar nenhuma assinatura de função já existente.

   A tabela usa open addressing (varredura linear) para evitar colisão:
   primeiro tenta o slot hash(tid); se já pertence a outra thread,
   tenta o próximo, e assim por diante, até achar um slot livre ou
   já pertencente a esta mesma thread. Como a tabela tem 8x o número
   de vikings, a chance de ficar cheia é nula para esse uso.

   Protegido por lock_states para que a leitura (snapshot) nunca veja
   um valor "pela metade". Não influencia nenhuma decisão do programa
   — é só um rótulo informativo. */
static int find_or_alloc_slot(chieftain_t *self, pthread_t tid)
{
    unsigned long raw = (unsigned long)(uintptr_t) tid;
    int start = (int)(raw % (unsigned long) self->total_vikings);

    for (int step = 0; step < self->total_vikings; step++) {
        int slot = (start + step) % self->total_vikings;
        if (self->states[slot] == VSTATE_UNKNOWN ||
            pthread_equal(self->state_owner[slot], tid)) {
            return slot;
        }
    }
    return start; /* tabela cheia (não deveria acontecer): sobrescreve */
}

void chieftain_set_state(chieftain_t *self, int id, viking_state_t state)
{
    (void) id; /* mantido na assinatura por compatibilidade, não usado mais */
    pthread_t self_tid = pthread_self();

    sem_wait(&self->lock_states);
    int slot = find_or_alloc_slot(self, self_tid);
    self->states[slot] = state;
    self->state_owner[slot] = self_tid;
    sem_post(&self->lock_states);
}

static const char *state_name(viking_state_t s)
{
    switch (s) {
        case VSTATE_UNKNOWN:         return "ainda nao comecou";
        case VSTATE_WAITING_SEAT:    return "esperando cadeira (quer comer)";
        case VSTATE_EATING:          return "comendo";
        case VSTATE_WAITING_BARRIER: return "esperando o microfone (barreira)";
        case VSTATE_PRAYING:         return "rezando";
        case VSTATE_DONE:            return "terminou tudo";
        default:                     return "desconhecido";
    }
}

/* [DEBUG-STATE] "Cutuca" todas as threads de uma vez: tira uma foto do
   quadro de estados inteiro, sob lock, e imprime quem está fazendo o
   quê agora. Não bloqueia nenhuma thread, só lê o rótulo que cada uma
   já deixou registrado da última vez que passou por chieftain_set_state. */
void chieftain_snapshot_states(chieftain_t *self)
{
    sem_wait(&self->lock_states);
    plog("[DEBUG-SNAPSHOT] ----- estado de todas as threads agora -----\n");
    for (int i = 0; i < self->total_vikings; i++) {
        if (self->states[i] == VSTATE_UNKNOWN) continue; /* slot vazio */
        plog("[DEBUG-SNAPSHOT] thread=%lu -> %s\n",
             (unsigned long)(uintptr_t) self->state_owner[i],
             state_name(self->states[i]));
    }
    plog("[DEBUG-SNAPSHOT] ---------------------------------------------\n");
    sem_post(&self->lock_states);
}