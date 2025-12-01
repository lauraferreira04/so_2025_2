#ifndef METRICAS_H
#define METRICAS_H


#include <stdbool.h>


typedef struct metricas {
    int n_processos_criados;
    int tempo_total_execucao;
    int tempo_total_ocioso;

    int n_irq_reset;  
    int n_irq_err_cpu;  
    int n_irq_sistema;   
    int n_irq_teclado;
    int n_irq_tela;
    int n_irq_relogio;
    int n_irq_desconhecida;

    int n_preempcoes;
    int *tempo_retorno_processo;
    int *n_prontos;
    int *tempo_pronto;
    int *n_bloqueados;
    int *tempo_bloqueado;
    int *n_execucao;
    int *tempo_execucao;
    int *tempo_medio_resposta;

    // informação relevante sobre o estado do so
    bool so_oscioso;        // todos os processos estão bloqueados
    // informações relevante sobre o estado dos processos
    int *processos_pid;
    int *processos_estado;
    int *tempo_criacao;
    bool *processos_recem_criado;
    bool *final_ja_registrado;
     
} metricas_t;


// torna uma instância da struct globalmente acessível
extern metricas_t metricas;


// inicializa os campos da struct métricas
void inicializa_metricas(metricas_t *m);

// imprime as metricas
void metricas_imprime();

#endif  // METRICAS_H