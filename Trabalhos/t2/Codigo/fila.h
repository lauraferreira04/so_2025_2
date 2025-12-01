#ifndef FILA_H
#define FILA_H


#include <stdbool.h>


typedef struct NoFila {
    int dado;             // dado armazenado no nó
    struct NoFila *prox;  // ponteiro para o próximo nó
} NoFila;

typedef struct Fila {
    int n_elem;   // número de processos na fila
    NoFila *pri;  // ponteiro para o primeiro nó da fila   
} Fila;


Fila *fila_cria();

void fila_destroi(Fila *self);

void fila_enque(Fila *self, int dado);

int fila_deque(Fila *self);

int fila_get(Fila *self, int pos);

int fila_n_elem(Fila *self);

bool fila_vazia(Fila *self);


#endif