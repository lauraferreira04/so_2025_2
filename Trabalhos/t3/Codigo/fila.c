#include "fila.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>


static NoFila *no_cria(int dado) {
    NoFila *n = (NoFila*) malloc(sizeof(NoFila));
    assert(n != NULL);

    n->dado = dado;
    n->prox = NULL;

    return n;
}


Fila *fila_cria() {
    Fila *f = (Fila*)malloc(sizeof(Fila));
    assert(f != NULL);

    f->n_elem = 0;
    f->pri = NULL;

    return f;
}


void fila_destroi(Fila *self) {
    NoFila *p = self->pri;
    while (p != NULL) {
        NoFila *temp = p;
        p = p->prox;
        free(temp);
    }
    free(self);
}


void fila_enque(Fila *self, int dado) {
    NoFila *novo_no = no_cria(dado);

    if (fila_vazia(self)) {
        self->pri = novo_no;
    } else {
        NoFila *p = self->pri;
        while (p->prox != NULL)
        {
            p = p->prox;
        }
        p->prox = novo_no;
    }

    self->n_elem++;
}


int fila_deque(Fila *self) {
    if (fila_vazia(self)) return -1;

    NoFila *no_removido = self->pri;
    int dado_removido = no_removido->dado;
    self->pri = self->pri->prox;
    free(no_removido);

    self->n_elem--;
    return dado_removido;
}


int fila_get(Fila *self, int pos) {
    if (fila_vazia(self)) return -1;

    NoFila *p = self->pri;
    for (int i = 0; i < pos; i++) {
        p = p->prox;
    }

    return p->dado;
}


int fila_n_elem(Fila *self) {
    return self->n_elem;
}


bool fila_vazia(Fila *self) {
    return self->n_elem == 0;
}