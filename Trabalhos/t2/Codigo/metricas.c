// metricas.c
#include "metricas.h"
#include "console.h"
#include <assert.h>
#include <stdlib.h> 
#include <stdio.h>


// em so.c:
//    #define N_PROCESSOS 5   // número máximo de processos
#define N_PROCESSOS 5

metricas_t metricas;


void inicializa_metricas(metricas_t *m)
{
    m->n_processos_criados = 0;
    m->tempo_total_execucao = 0;
    m->tempo_total_ocioso = 0;

    m->n_irq_reset = 0;
    m->n_irq_err_cpu = 0;
    m->n_irq_sistema = 0;
    m->n_irq_teclado = 0;
    m->n_irq_tela = 0;
    m->n_irq_relogio = 0;
    m->n_irq_desconhecida = 0;

    m->n_preempcoes = 0;
    // processos
    m->processos_pid = (int*) malloc(N_PROCESSOS * sizeof(int));
    assert(m->processos_pid != NULL);
    // tempo de retorno de processos
    m->tempo_retorno_processo = (int*) malloc(N_PROCESSOS * sizeof(int));
    assert(m->tempo_retorno_processo != NULL);
    // estado dos processos
    m->processos_estado = (int*) malloc(N_PROCESSOS * sizeof(int));
    assert(m->processos_estado != NULL);
    // tempo de criação do processo
    m->tempo_criacao = (int*) malloc(N_PROCESSOS * sizeof(int));
    assert(m->tempo_criacao != NULL);
    // processos recem criados
    m->processos_recem_criado = (bool*) calloc(N_PROCESSOS, sizeof(bool));
    assert(m->processos_recem_criado != NULL);
    // final do processo ja registrado
    m->final_ja_registrado = (bool*) calloc(N_PROCESSOS, sizeof(bool));
    assert(m->final_ja_registrado != NULL);

    // n vezes prontos
    m->n_prontos = (int*) malloc(N_PROCESSOS * sizeof(int));
    assert(m->n_prontos != NULL);
    // tempo pronto
    m->tempo_pronto = (int*) malloc(N_PROCESSOS * sizeof(int));
    assert(m->tempo_pronto != NULL);
    // n vezes bloqueado
    m->n_bloqueados = (int*) malloc(N_PROCESSOS * sizeof(int));
    assert(m->n_bloqueados != NULL);
    // tempo bloqueado
    m->tempo_bloqueado = (int*) malloc(N_PROCESSOS * sizeof(int));
    assert(m->tempo_bloqueado != NULL);
    // n execuções
    m->n_execucao = (int*) malloc(N_PROCESSOS * sizeof(int));
    assert(m->n_execucao != NULL);
    // tempo execução
    m->tempo_execucao = (int*) malloc(N_PROCESSOS * sizeof(int));
    assert(m->tempo_execucao != NULL);
    // tempo resposta
    m->tempo_medio_resposta = (int*) malloc(N_PROCESSOS * sizeof(int));
    assert(m->tempo_medio_resposta != NULL);

    for (int i = 0; i < N_PROCESSOS; i++)
    {
        m->processos_pid[i] = -1;  // sem processo
        m->tempo_retorno_processo[i] = 0;
        m->n_prontos[i] = 0;
        m->tempo_pronto[i] = 0;
        m->n_bloqueados[i] = 0;
        m->tempo_bloqueado[i] = 0;
        m->n_execucao[i] = 0;
        m->tempo_execucao[i] = 0;
        m->tempo_medio_resposta[i] = 0;
    }

    m->so_oscioso = false;
}


void metricas_imprime()
{
    FILE *f = fopen("relatorio.txt", "w");
    if (f == NULL) 
    {
        perror("Erro ao abrir relatorio.txt");
        return;
    }

    fprintf(f, "- n processos criados: %d\n", metricas.n_processos_criados);
    fprintf(f, "- tempo total de execução: %d\n", metricas.tempo_total_execucao);
    fprintf(f, "- tempo total ocioso: %d\n", metricas.tempo_total_ocioso);
    fprintf(f, "- irqs: reset[%d], err_cpou[%d], sistema[%d], teclado[%d] tela[%d] relogio[%d], desconhecida[%d]\n",metricas.n_irq_reset, metricas.n_irq_err_cpu, metricas.n_irq_sistema, metricas.n_irq_teclado, metricas.n_irq_tela, metricas.n_irq_relogio, metricas.n_irq_desconhecida);
    fprintf(f, "- n preempções: %d\n", metricas.n_preempcoes);

    fprintf(f, "\nMétricas de processos:\n");
    for (int i = 0; i < 4; i++) 
    {
        fprintf(f, "Processo %d\n", i);
        fprintf(f, "- tempo de retorno proc %d: %d\n",i - 1, metricas.tempo_retorno_processo[i]);
        fprintf(f, "- n vezes em cada estado proc %d : pronto[%d], block[%d], exec[%d]\n", i - 1, metricas.n_prontos[i], metricas.n_bloqueados[i], metricas.n_execucao[i]);
        fprintf(f, "- tempo em cada estado do proc %d : pronto[%d], block[%d], exec[%d]\n", i - 1, metricas.tempo_pronto[i], metricas.tempo_bloqueado[i], metricas.tempo_execucao[i]);
    }

    fclose(f);
}