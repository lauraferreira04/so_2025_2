// so.c
// sistema operacional
// simulador de computador
// so25b

// ---------------------------------------------------------------------
// INCLUDES {{{1
// ---------------------------------------------------------------------

#include "so.h"
#include "dispositivos.h"
#include "err.h"
#include "irq.h"
#include "memoria.h"
#include "programa.h"
#include "fila.h"
#include "metricas.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

// ---------------------------------------------------------------------
// CONSTANTES E TIPOS {{{1
// ---------------------------------------------------------------------

// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50   // em instruções executadas
#define QUANTUM 10

#define N_MAX_PROCESSOS 5 // num máx de processos
#define N_TERMINAIS 4 
#define SEM_PROCESSO -1  // não tem processo atual
#define SEM_DISPOSITIVO -1  // não tem um dispositivo que causou bloqueio

#define SEM_ESCALONADOR 0
#define ESCALONADOR 2
#define ROUND_ROBIN 1
#define PRIORIDADE 2

// estados de um processo
typedef enum estado_t {
  PRONTO,
  EXECUTANDO,
  PARADO,
  BLOQUEADO,
  FINALIZADO
} estado_t;

typedef struct processo_t {
  int pid; // id do processo
  int regPC; // end da próxima instrução executar
  int regA; // tipo de interrupção
  int regX; 
  int regERRO;
  int terminal; // (0,1,2 ou 3)
  estado_t estado; // enum
  char executavel[100]; // nome do programa

  int pid_esperado; // pid do processo que este processo ta esperando morrer
  int dispositivo_causou_bloqueio; // id do dispositivo que causou o bloqueio (para SO_LE e SO_ESCR)

  int quantum;
  float prioridade;
} processo_t;

struct so_t {
  cpu_t *cpu;
  mem_t *mem;
  es_t *es;
  console_t *console;
  bool erro_interno;

  int regA, regX, regPC, regERRO; // cópia do estado da CPU

  // t2: tabela de processos, processo corrente, pendências, etc
  processo_t tabela_de_processos[N_MAX_PROCESSOS]; // fixa
  
  // ponteiro para o processo atual dentro da tabela de processos
  processo_t *processo_atual;

  int n_processos_tabela;

  Fila *processos_prontos;
  // id dos processos que estão usando cada terminal
  int terminais_usados[N_TERMINAIS];
};


// função de tratamento de interrupção (entrada no SO)
static int so_trata_interrupcao(void *argC, int reg_A);

// funções auxiliares
// carrega o programa contido no arquivo na memória do processador; retorna end. inicial
static int so_carrega_programa(so_t *self, char *nome_do_executavel);
// copia para str da memória do processador, até copiar um 0 (retorna true) ou tam bytes
static bool copia_str_da_mem(int tam, char str[tam], mem_t *mem, int ender);

// --------------- FUNÇÕES PROCESSOS ---------------

static bool associa_terminal_a_processo(so_t *so, processo_t *proc){
  for (int i = 0; i < N_TERMINAIS; i++)  {
    if (so->terminais_usados[i] == SEM_PROCESSO)    {
      so->terminais_usados[i] = proc->pid;
      switch (i)      {
        case 0:
          proc->terminal = D_TERM_A;
          break;
        case 1:
          proc->terminal = D_TERM_B;
          break;
        case 2:
          proc->terminal = D_TERM_C;
          break;
        default:
          proc->terminal = D_TERM_D;
      }
      return true;
    }
  }
  return false;  // não tem um terminal disponível
}

// cria um processo, retorna pid
int processo_cria(so_t *self, char *nome_do_executavel) {
    // ... (lógica de encontrar slot e tabela cheia mantém igual) ...
    int slot = SEM_PROCESSO;
    for (int i = 0; i < N_MAX_PROCESSOS; i++){
        if (self->tabela_de_processos[i].pid == SEM_PROCESSO){
            slot = i;
            break;
        }
    }
    if (slot == SEM_PROCESSO) return -1;

    // carrega o programa na memória
    int endereco_inicial = so_carrega_programa(self, nome_do_executavel);
    if (endereco_inicial < 0) return -1;

    // inicializa o novo processo
    processo_t *novo_p = &self->tabela_de_processos[slot];

    if (self->n_processos_tabela == 0) self->n_processos_tabela = 1;
    novo_p->pid = self->n_processos_tabela++;

    strncpy(novo_p->executavel, nome_do_executavel, 100);
    novo_p->executavel[99] = '\0';

    novo_p->estado = PRONTO;
    novo_p->regPC = endereco_inicial;
    novo_p->regA = 0;
    novo_p->regX = 0;
    novo_p->regERRO = 0;
    novo_p->pid_esperado = SEM_PROCESSO;
    novo_p->dispositivo_causou_bloqueio = SEM_DISPOSITIVO;
    novo_p->quantum = QUANTUM;
    novo_p->prioridade = 0.5;

    // metricas
    metricas.processos_pid[slot] = self->n_processos_tabela + 1; // PID provisório ou real
    metricas.processos_estado[slot] = PRONTO;
    metricas.n_prontos[slot]++;
    metricas.processos_recem_criado[slot] = true;

    if (!associa_terminal_a_processo(self, novo_p)){
        novo_p->pid = SEM_PROCESSO;
        return -1;
    }

    console_printf("SO: Processo PID %d criado no slot %d (Terminal %d)", novo_p->pid, slot, novo_p->terminal);
    
    metricas.n_processos_criados++;

    // retorna o endereço inicial
    return novo_p->pid; 
}

// mata um processo, liberando o slot e o terminal
void processo_mata(so_t *self, int pid){
// verifica se hà processos para serem deletados
  if (self->n_processos_tabela <= 0){
    console_printf("SO TENTOU MATAR UM PROCESSO QUANDO NÃO HÁ PROCESSOS CORRENTES\n");
    return;
  }

  self->n_processos_tabela--;

  if (pid == 0){
    // mata o processo corrente
    self->processo_atual->estado = FINALIZADO;
    int indice_finalizado = acha_indice_por_pid(self, self->processo_atual->pid);
    metricas.processos_estado[indice_finalizado] = FINALIZADO;
    self->processo_atual->pid = SEM_PROCESSO;
    self->processo_atual->terminal = -1;
    for (int i = 0; i < N_TERMINAIS; i++){
      if (self->terminais_usados[i] == self->processo_atual->pid){
        self->terminais_usados[i] = SEM_PROCESSO;
      }
    }
    fila_deque(self->processos_prontos);
  }else{
    for (int i = 0; i < N_MAX_PROCESSOS; i++){
      if (self->tabela_de_processos[i].pid == pid){
        self->tabela_de_processos[i].estado = FINALIZADO;
        self->tabela_de_processos[i].pid = SEM_PROCESSO;
        self->tabela_de_processos[i].terminal = -1;
        for (int j = 0; j < N_TERMINAIS; j++){
          if (self->terminais_usados[j] == self->tabela_de_processos[i].pid){
            self->terminais_usados[j] = SEM_PROCESSO;
          }
        }
      }

    }
  }

  // verifica se tinha algum processo esperado a morte desse
  for (int i = 0; i < N_MAX_PROCESSOS; i++)
  {
    if (self->tabela_de_processos[i].pid_esperado == pid)
    {
      self->tabela_de_processos[i].estado = PRONTO;
      // metricas
      metricas.n_prontos[i]++;
      fila_enque(self->processos_prontos, self->tabela_de_processos[i].pid);
    }
  }
}

void processo_troca_corrente(so_t *self){
  // acha o primeiro processo existente na tabela
  int i = 0;
  
  while (i < N_MAX_PROCESSOS){

    if (self->tabela_de_processos[i].pid != SEM_PROCESSO && self->tabela_de_processos[i].estado == PRONTO){
      self->processo_atual = &self->tabela_de_processos[i];
      self->processo_atual->estado = EXECUTANDO;
      break;
    }

    i++;
  }
}

bool todos_processos_encerrados(so_t *self){
  for (int i = 0; i < N_MAX_PROCESSOS; i++){
    if (self->tabela_de_processos[i].pid != SEM_PROCESSO) return false;
  }

  return true;
}

int acha_indice_por_pid(so_t *self, int pid){
  for (int i = 0; i < N_MAX_PROCESSOS; i++)
  {
    if (self->tabela_de_processos[i].pid == pid) return i;
  }
  return SEM_PROCESSO;
}

// verifica se um processo com o pid existe
static bool processo_existe(so_t *self, int pid){
  for (int i = 0; i < N_MAX_PROCESSOS; i++){
    if (self->tabela_de_processos[i].pid == pid) return true;
  }
  return false;
}

// atualiza a prioridade de um processo
static void processo_atualiza_prioridade(processo_t *proc){
  // prio = (prio + t_exec/t_quantum) / 2
  proc->prioridade = (proc->prioridade + proc->quantum / QUANTUM) / 2;
}

// ---------------------------------------------------------------------
// CRIAÇÃO {{{1
// ---------------------------------------------------------------------

so_t *so_cria(cpu_t *cpu, mem_t *mem, es_t *es, console_t *console)
{
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  self->cpu = cpu;
  self->mem = mem;
  self->es = es;
  self->console = console;
  self->erro_interno = false;

  // inicializa a tabela de processo
  for (int i = 0; i < N_MAX_PROCESSOS; i++) {
    self->tabela_de_processos[i].pid = SEM_PROCESSO;
    self->tabela_de_processos[i].estado = PARADO;
    self->tabela_de_processos[i].terminal = -1;
  }

  self->n_processos_tabela = 0;
  self->processo_atual = &self->tabela_de_processos[0];
  self->processos_prontos = fila_cria();

  // inicializa terminais
  for (int i = 0; i < N_TERMINAIS; i++){
    self->terminais_usados[i] = SEM_PROCESSO;
  }

  // Inicializa as métricas
  inicializa_metricas(&metricas);

  // quando a CPU executar uma instrução CHAMAC, deve chamar a função
  //   so_trata_interrupcao, com primeiro argumento um ptr para o SO
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);

  return self;
}

void so_destroi(so_t *self)
{
  cpu_define_chamaC(self->cpu, NULL, NULL);
  free(self);
}


// ---------------------------------------------------------------------
// TRATAMENTO DE INTERRUPÇÃO {{{1
// ---------------------------------------------------------------------

// funções auxiliares para o tratamento de interrupção
static void so_salva_estado_da_cpu(so_t *self);
static void so_trata_irq(so_t *self, int irq);
static void so_trata_pendencias(so_t *self);
static void so_escalona(so_t *self);
static int so_despacha(so_t *self);

// função a ser chamada pela CPU quando executa a instrução CHAMAC, no tratador de
//   interrupção em assembly
// essa é a única forma de entrada no SO depois da inicialização
// na inicialização do SO, a CPU foi programada para chamar esta função para executar
//   a instrução CHAMAC
// a instrução CHAMAC só deve ser executada pelo tratador de interrupção
//
// o primeiro argumento é um ponteiro para o SO, o segundo é a identificação
//   da interrupção
// o valor retornado por esta função é colocado no registrador A, e pode ser
//   testado pelo código que está após o CHAMAC. No tratador de interrupção em
//   assembly esse valor é usado para decidir se a CPU deve retornar da interrupção
//   (e executar o código de usuário) ou executar PARA e ficar suspensa até receber
//   outra interrupção
static int so_trata_interrupcao(void *argC, int reg_A)
{
  so_t *self = argC;
  irq_t irq = reg_A;
  // esse print polui bastante, recomendo tirar quando estiver com mais confiança
  console_printf("SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
  // salva o estado da cpu no descritor do processo que foi interrompido
  so_salva_estado_da_cpu(self);
  // faz o atendimento da interrupção
  so_trata_irq(self, irq);
  // faz o processamento independente da interrupção
  so_trata_pendencias(self);
  // escolhe o próximo processo a executar
  so_escalona(self);
  // recupera o estado do processo escolhido
  return so_despacha(self);
}

static void so_salva_estado_da_cpu(so_t *self)
{
  // t2: salva os registradores que compõem o estado da cpu no descritor do
  //   processo corrente. os valores dos registradores foram colocados pela
  //   CPU na memória, nos endereços CPU_END_PC etc. O registrador X foi salvo
  //   pelo tratador de interrupção (ver trata_irq.asm) no endereço 59
  // se não houver processo corrente, não faz nada

  // se for NULL ou SEM_PROCESSO, não salva
  if (self->processo_atual->pid == SEM_PROCESSO) {
    return;
  }

  // pega os valores dos registradores da memória
  if (mem_le(self->mem, CPU_END_A, &self->regA) != ERR_OK
      || mem_le(self->mem, CPU_END_PC, &self->regPC) != ERR_OK
      || mem_le(self->mem, CPU_END_erro, &self->regERRO) != ERR_OK
      || mem_le(self->mem, 59, &self->regX)) {
    console_printf("SO: erro na leitura dos registradores");
    self->erro_interno = true;
  }

  // salva reg da memória no buffer do processo atual
  self->processo_atual->regA = self->regA;
  self->processo_atual->regPC = self->regPC;
  self->processo_atual->regERRO = self->regERRO;
  self->processo_atual->regX = self->regX;

}

static void so_trata_pendencias(so_t *self)
{
  // t2: realiza ações que não são diretamente ligadas com a interrupção que
  //   está sendo atendida:
  // - E/S pendente
  // - desbloqueio de processos
  // - contabilidades
  // - etc

  // "na função que trata de pendências, o SO deve verificar o estado dos dispositivos 
  // que causaram bloqueio e realizar operações pendentes e desbloquear processos se for o caso"

  // verifica o estado dos dispositivos bloqueados
  for (int i = 0; i < N_MAX_PROCESSOS; i++){
    processo_t *p = &self->tabela_de_processos[i];
    if (p->estado == BLOQUEADO){
      // verifica o dispositivo que causou o bloqueio
      int dispositivo = p->dispositivo_causou_bloqueio;
      int operacao = dispositivo % 4;

      // verifica o estado do dispositivo
      int disponivel;
      int estado = es_le(self->es, dispositivo, &disponivel);
      if (estado != ERR_OK){
        // realiza a operação pendente
        // leitura
        if (operacao == 1){
          int dado;
          if (es_le(self->es, dispositivo-1, &dado) != ERR_OK) {
            console_printf("SO: problema no acesso ao teclado");
            self->erro_interno = true;
            return;
          }
          p->regA = dado;

          metricas.n_irq_teclado++;
        }
        // escrita
        if (operacao == 3){
          int dado = p->regX;
          if (es_escreve(self->es, dispositivo-1, dado) != ERR_OK) {
            console_printf("SO: problema no acesso à tela");
            self->erro_interno = true;
            return;
          }
          p->regA = 0;

          metricas.n_irq_tela++;
        }
      }

      // desbloqueia o processo
      p->estado = PRONTO;
      // metricas
      metricas.n_prontos[acha_indice_por_pid(self, p->pid)]++;
      p->dispositivo_causou_bloqueio = SEM_DISPOSITIVO;
      fila_enque(self->processos_prontos, p->pid);

    }
  }

}

static void so_escalona(so_t *self)
{
  // escolhe o próximo processo a executar, que passa a ser o processo
  //   corrente; pode continuar sendo o mesmo de antes ou não
  // t2: na primeira versão, escolhe um processo pronto caso o processo
  //   corrente não possa continuar executando, senão deixa o mesmo processo.
  //   depois, implementa um escalonador melhor
  
  // verifica se o processo corrente está em execução
  if (self->processo_atual->estado == EXECUTANDO) return;

  switch (ESCALONADOR){
    case ROUND_ROBIN:
      // pega o primeiro processo da fila de processos prontos
      int pid_escalonado = fila_get(self->processos_prontos, 0);
      
      for (int i = 0; i < N_MAX_PROCESSOS; i++){
      
        if (self->tabela_de_processos[i].pid == pid_escalonado && pid_escalonado != -1){
          // torna-o o processo atual
          self->processo_atual = &self->tabela_de_processos[i];
          // métricas
          metricas.processos_estado[i] = EXECUTANDO;
          metricas.n_execucao[i]++;
          
        }
      }
      break;
    
    case PRIORIDADE:
      // pega o indice do processo com a maior prioridade na tabela de processos (menor valor do campo ->prioridade)
      int indice_maior_prioridade = SEM_PROCESSO;
      float maior_prioridade = QUANTUM; 

      for (int i = 0; i < N_MAX_PROCESSOS; i++){
        if (self->tabela_de_processos[i].estado == FINALIZADO || self->tabela_de_processos[i].pid == SEM_PROCESSO) continue;

        if (self->tabela_de_processos[i].prioridade < maior_prioridade && self->tabela_de_processos[i].estado == PRONTO){
          indice_maior_prioridade = i;
          maior_prioridade = self->tabela_de_processos[i].prioridade;
        }
      }
      
      // escalona o processo de maior prioridade
      if (indice_maior_prioridade != SEM_PROCESSO){
        self->processo_atual = &self->tabela_de_processos[indice_maior_prioridade];
        // metricas
        metricas.processos_estado[indice_maior_prioridade] = EXECUTANDO;
        metricas.n_execucao[indice_maior_prioridade]++;
      }else{
        // apenas tem um processo na tabela - deixa ele corrente
        processo_troca_corrente(self);
      }
      break;

    default:
      console_printf("NENHUM\n");
      // bota o primeiro processo PRONTO para executar
      processo_troca_corrente(self);
  }

  // (métricas) verifica se o so está oscioso
  int n_proc_bloqueados = 0;
  int n_proc_vivos = 0; // NOVO CONTADOR

  for (int i = 0; i < N_MAX_PROCESSOS; i++){
    if (self->tabela_de_processos[i].pid != SEM_PROCESSO){
      n_proc_vivos++;
      if(self->tabela_de_processos[i].estado == BLOQUEADO){
        n_proc_bloqueados++;
      }
    } 
  }
  if (n_proc_bloqueados == n_proc_vivos){
    metricas.so_oscioso = true;
  } else{
    metricas.so_oscioso = false;
  }

  // (metricas) aumenta o número de preempções
  metricas.n_preempcoes++;

  console_printf("Processo escalonado!\n");
  
  // verifica se todos os processos encerraram
  if (todos_processos_encerrados(self)){
    console_printf("TODOS PROCESSOS ENCERRARAM - %d\n", metricas.n_processos_criados);
    metricas_imprime();
  }
}

static int so_despacha(so_t *self)
{
  // t2: se houver processo corrente, coloca o estado desse processo onde ele
  //   será recuperado pela CPU (em CPU_END_PC etc e 59) e retorna 0,
  //   senão retorna 1
  // o valor retornado será o valor de retorno de CHAMAC, e será colocado no 
  //   registrador A para o tratador de interrupção (ver trata_irq.asm).

  // se não tem processo válido pra rodar, retorna 1
  if (self->processo_atual->pid == SEM_PROCESSO){
    return 1;
  }
  
  if (mem_escreve(self->mem, CPU_END_A, self->processo_atual->regA) != ERR_OK
      || mem_escreve(self->mem, CPU_END_PC, self->processo_atual->regPC) != ERR_OK
      || mem_escreve(self->mem, CPU_END_erro, self->processo_atual->regERRO) != ERR_OK
      || mem_escreve(self->mem, 59, self->processo_atual->regX)) {
    console_printf("SO: erro na escrita dos registradores");
    self->erro_interno = true;
  }
  if (self->erro_interno) {
    return 1;
  } else {
    return 0;
  }
}


// ---------------------------------------------------------------------
// TRATAMENTO DE UMA IRQ {{{1
// ---------------------------------------------------------------------

// funções auxiliares para tratar cada tipo de interrupção
static void so_trata_reset(so_t *self);
static void so_trata_irq_chamada_sistema(so_t *self);
static void so_trata_irq_err_cpu(so_t *self);
static void so_trata_irq_relogio(so_t *self);
static void so_trata_irq_desconhecida(so_t *self, int irq);

static void so_trata_irq(so_t *self, int irq)
{
  // verifica o tipo de interrupção que está acontecendo, e atende de acordo
  switch (irq) {
    case IRQ_RESET:
      metricas.n_irq_reset++; // MÉTRICAS
      so_trata_reset(self);
      break;
    case IRQ_SISTEMA:
      metricas.n_irq_sistema++; // MÉTRICAS
      so_trata_irq_chamada_sistema(self);
      break;
    case IRQ_ERR_CPU:
      metricas.n_irq_err_cpu++; // MÉTRICAS
      so_trata_irq_err_cpu(self);
      break;
    case IRQ_RELOGIO:
      metricas.n_irq_relogio++; // MÉTRICAS
      so_trata_irq_relogio(self);
      break;
    default:
      metricas.n_irq_desconhecida++; // MÉTRICAS
      so_trata_irq_desconhecida(self, irq);
  }
}

// chamada uma única vez, quando a CPU inicializa
static void so_trata_reset(so_t *self)
{
  // coloca o tratador de interrupção na memória
  // quando a CPU aceita uma interrupção, passa para modo supervisor,
  //   salva seu estado à partir do endereço CPU_END_PC, e desvia para o
  //   endereço CPU_END_TRATADOR
  // colocamos no endereço CPU_END_TRATADOR o programa de tratamento
  //   de interrupção (escrito em asm). esse programa deve conter a
  //   instrução CHAMAC, que vai chamar so_trata_interrupcao (como
  //   foi definido na inicialização do SO)
  int ender = so_carrega_programa(self, "trata_int.maq");
  if (ender != CPU_END_TRATADOR) {
    console_printf("SO: problema na carga do programa de tratamento de interrupção");
    self->erro_interno = true;
  }

  // programa o relógio para gerar uma interrupção após INTERVALO_INTERRUPCAO
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
    console_printf("SO: problema na programação do timer");
    self->erro_interno = true;
  }

  // t2: deveria criar um processo para o init, e inicializar o estado do
  //   processador para esse processo com os registradores zerados, exceto
  //   o PC e o modo.
  // como não tem suporte a processos, está carregando os valores dos
  //   registradores diretamente no estado da CPU mantido pelo SO; daí vai
  //   copiar para o início da memória pelo despachante, de onde a CPU vai
  //   carregar para os seus registradores quando executar a instrução RETI
  //   em bios.asm (que é onde está a instrução CHAMAC que causou a execução
  //   deste código

  // chama processo_cria
  int pid = processo_cria(self, "init.maq"); 

  // ajusta o processo atual para EXECUTANDO
  processo_troca_corrente(self); 
  console_printf("TROCOU PRO INIT"); 
  self->processo_atual->estado = EXECUTANDO;
  self->processo_atual->regA = pid;

}

// interrupção gerada quando a CPU identifica um erro
static void so_trata_irq_err_cpu(so_t *self)
{
  // Ocorreu um erro interno na CPU
  // O erro está codificado em CPU_END_erro
  // Em geral, causa a morte do processo que causou o erro
  // Ainda não temos processos, causa a parada da CPU
  // t2: com suporte a processos, deveria pegar o valor do registrador erro
  //   no descritor do processo corrente, e reagir de acordo com esse erro
  //   (em geral, matando o processo)
  err_t err = self->processo_atual->regERRO;
  processo_mata(self, 0);
  console_printf("SO: IRQ não tratada -- erro na CPU: %s", err_nome(err));
  self->erro_interno = true;
}

// interrupção gerada quando o timer expira
static void so_trata_irq_relogio(so_t *self)
{
  // rearma o interruptor do relógio e reinicializa o timer para a próxima interrupção
  err_t e1, e2;
  e1 = es_escreve(self->es, D_RELOGIO_INTERRUPCAO, 0); // desliga o sinalizador de interrupção
  e2 = es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO);
  if (e1 != ERR_OK || e2 != ERR_OK) {
    console_printf("SO: problema da reinicialização do timer");
    self->erro_interno = true;
  }
  // t2: deveria tratar a interrupção
  //   por exemplo, decrementa o quantum do processo corrente, quando se tem
  //   um escalonador com quantum
  self->processo_atual->quantum--;
  if (self->processo_atual->quantum <= 0 && self->processo_atual->estado != BLOQUEADO){
    self->processo_atual->quantum = 10;
    processo_atualiza_prioridade(self->processo_atual);
    fila_deque(self->processos_prontos);
    // ATENÇÃO: talvez não funcione sapoha
    fila_enque(self->processos_prontos, self->processo_atual->pid);
  }
}

// foi gerada uma interrupção para a qual o SO não está preparado
static void so_trata_irq_desconhecida(so_t *self, int irq)
{
  console_printf("SO: não sei tratar IRQ %d (%s)", irq, irq_nome(irq));
  self->erro_interno = true;
}


// ---------------------------------------------------------------------
// CHAMADAS DE SISTEMA {{{1
// ---------------------------------------------------------------------

// funções auxiliares para cada chamada de sistema
static void so_chamada_le(so_t *self);
static void so_chamada_escr(so_t *self);
static void so_chamada_cria_proc(so_t *self);
static void so_chamada_mata_proc(so_t *self);
static void so_chamada_espera_proc(so_t *self);

static void so_trata_irq_chamada_sistema(so_t *self)
{
  // a identificação da chamada está no registrador A
  // t2: com processos, o reg A deve estar no descritor do processo corrente
  //int id_chamada = self->regA;
  int id_chamada = self->processo_atual->regA;
  console_printf("SO: chamada de sistema %d", id_chamada);
  switch (id_chamada) {
    case SO_LE:
      so_chamada_le(self);
      break;
    case SO_ESCR:
      so_chamada_escr(self);
      break;
    case SO_CRIA_PROC:
      so_chamada_cria_proc(self);
      break;
    case SO_MATA_PROC:
      so_chamada_mata_proc(self);
      break;
    case SO_ESPERA_PROC:
      so_chamada_espera_proc(self);
      break;
    default:
      console_printf("SO: chamada de sistema desconhecida (%d)", id_chamada);
      // t2: deveria matar o processo
      processo_mata(self, 0);
      self->erro_interno = true;
  }
}

// implementação da chamada se sistema SO_LE
// faz a leitura de um dado da entrada corrente do processo, coloca o dado no reg A
static void so_chamada_le(so_t *self)
{
  // implementação com espera ocupada
  //   t2: deveria realizar a leitura somente se a entrada estiver disponível,
  //     senão, deveria bloquear o processo.
  //   no caso de bloqueio do processo, a leitura (e desbloqueio) deverá
  //     ser feita mais tarde, em tratamentos pendentes em outra interrupção,
  //     ou diretamente em uma interrupção específica do dispositivo, se for
  //     o caso
  // implementação lendo direto do terminal A
  //   t2: deveria usar dispositivo de entrada corrente do processo
  int terminal_base = self->processo_atual->terminal;
  int disp_teclado_ok = terminal_base + TERM_TECLADO_OK;

  for (;;) {  // espera ocupada!
    int estado;
  
    if (es_le(self->es, disp_teclado_ok, &estado) != ERR_OK) {
      console_printf("SO: problema no acesso ao estado do teclado no Terminal %d", terminal_base);
      // bloqueia o processo
      self->processo_atual->estado = BLOQUEADO;
      self->processo_atual->dispositivo_causou_bloqueio = disp_teclado_ok;

      // volta o PC
      self->processo_atual->regPC--;

      self->erro_interno = true;
      return;
    }

    if (estado != 0) break;
    // como não está saindo do SO, a unidade de controle não está executando seu laço.
    // esta gambiarra faz pelo menos a console ser atualizada
    // t2: com a implementação de bloqueio de processo, esta gambiarra não
    //   deve mais existir.
    console_tictac(self->console);
  }
  int disp_teclado = terminal_base + TERM_TECLADO;
    
  int dado;
  if (es_le(self->es, disp_teclado, &dado) != ERR_OK) {
      console_printf("SO: problema no acesso ao teclado no Terminal %d", terminal_base);
      self->erro_interno = true;
      return;
  }
  // escreve no reg A do processador
  // (na verdade, na posição onde o processador vai pegar o A quando retornar da int)
  // t2: se houvesse processo, deveria escrever no reg A do processo
  // t2: o acesso só deve ser feito nesse momento se for possível; se não, o processo
  //   é bloqueado, e o acesso só deve ser feito mais tarde (e o processo desbloqueado)
  self->regA = dado;
  self->processo_atual->regA = dado;
}

// implementação da chamada se sistema SO_ESCR
// escreve o valor do reg X na saída corrente do processo
static void so_chamada_escr(so_t *self)
{
  // implementação com espera ocupada
  //   t2: deveria bloquear o processo se dispositivo ocupado
  // implementação escrevendo direto do terminal A
  //   t2: deveria usar o dispositivo de saída corrente do processo
  
  int terminal_base = self->processo_atual->terminal;
  int disp_tela_ok = terminal_base + TERM_TELA_OK;
  
  for (;;) {
    int estado;
    if (es_le(self->es, disp_tela_ok, &estado) != ERR_OK) {
      console_printf("SO: problema no acesso ao estado da tela no Terminal %d", terminal_base);
      // bloqueia o processo
      self->processo_atual->estado = BLOQUEADO;
      self->processo_atual->dispositivo_causou_bloqueio = disp_tela_ok;

      // volta o PC
      self->processo_atual->regPC--;

      self->erro_interno = true;
      return;
    }
    if (estado != 0) break;
    // como não está saindo do SO, a unidade de controle não está executando seu laço.
    // esta gambiarra faz pelo menos a console ser atualizada
    // t2: não deve mais existir quando houver suporte a processos, porque o SO não poderá
    //   executar por muito tempo, permitindo a execução do laço da unidade de controle
    console_tictac(self->console);
  }
  
  int disp_tela = terminal_base + TERM_TELA;
  int dado = self->processo_atual->regX;
  
  // está lendo o valor de X e escrevendo o de A direto onde o processador colocou/vai pegar
  // t2: deveria usar os registradores do processo que está realizando a E/S
  // t2: caso o processo tenha sido bloqueado, esse acesso deve ser realizado em outra execução
  //   do SO, quando ele verificar que esse acesso já pode ser feito.
  if (es_escreve(self->es, disp_tela, dado) != ERR_OK) {
    console_printf("SO: problema no acesso à tela no Terminal %d", terminal_base);
    self->erro_interno = true;
    return;
  }

  self->regA = 0;
  self->processo_atual->regA = 0;
}

// implementação da chamada se sistema SO_CRIA_PROC
// cria um processo
static void so_chamada_cria_proc(so_t *self)
{
  // ainda sem suporte a processos, carrega programa e passa a executar ele
  // quem chamou o sistema não vai mais ser executado, coitado!
  // t2: deveria criar um novo processo

  // em X está o endereço onde está o nome do arquivo
  int ender_proc;
  // t2: deveria ler o X do descritor do processo criador
  ender_proc = self->processo_atual->regX;

  char nome[100] = {0}; 
  int novo_pid = -1; // inicializa com erro
  
  if (copia_str_da_mem(100, nome, self->mem, ender_proc)) {
    // cria o novo processo (Item 7 e 9)
    novo_pid = processo_cria(self, nome);
    
    // processo_cria ta responsável disso
    // int ender_carga = so_carrega_programa(self, nome);
    if (novo_pid > 0) {
      // deu certo
      console_printf("SO: PID %d (pai) criou novo PID %d ('%s').", 
                           self->processo_atual->pid, novo_pid, nome);
      // t2: deveria escrever no PC do descritor do processo criado
      // isso está sendo feito em processo_cria
    }
  }
  // deveria escrever -1 (se erro) ou o PID do processo criado (se OK) no reg A
  //   do processo que pediu a criação
  self->processo_atual->regA = novo_pid;
}

// implementação da chamada se sistema SO_MATA_PROC
// mata o processo com pid X (ou o processo corrente se X é 0)
static void so_chamada_mata_proc(so_t *self)
{
  // t2: deveria matar um processo
  // ainda sem suporte a processos, retorna erro -1
  processo_mata(self, self->processo_atual->regX);  // mata o processo atual
}

// implementação da chamada se sistema SO_ESPERA_PROC
// espera o fim do processo com pid X
static void so_chamada_espera_proc(so_t *self)
{
  // t2: deveria bloquear o processo se for o caso (e desbloquear na morte do esperado)
  // ainda sem suporte a processos, retorna erro -1
  //console_printf("SO: SO_ESPERA_PROC não implementada");
  //self->regA = -1;

  // verifica se o processo a se esperar é válido
  if (self->processo_atual->regX == self->processo_atual->pid || 
    !processo_existe(self, self->processo_atual->regX)){
    console_printf("PROCESSO INVALIDO");
    self->erro_interno = true;
    // erro aqui !!!!!!
    return;
  }

  console_printf("[%d] vai esperar o fim de [%d]", self->processo_atual->pid, self->processo_atual->regX);

  // bloqueia o processo chamador
  self->processo_atual->estado = BLOQUEADO;
  self->processo_atual->pid_esperado = self->processo_atual->regX;
}


// ---------------------------------------------------------------------
// CARGA DE PROGRAMA {{{1
// ---------------------------------------------------------------------

// carrega o programa na memória
// retorna o endereço de carga ou -1
static int so_carrega_programa(so_t *self, char *nome_do_executavel)
{
  // programa para executar na nossa CPU
  programa_t *prog = prog_cria(nome_do_executavel);
  if (prog == NULL) {
    console_printf("Erro na leitura do programa '%s'\n", nome_do_executavel);
    return -1;
  }

  int end_ini = prog_end_carga(prog);
  int end_fim = end_ini + prog_tamanho(prog);

  for (int end = end_ini; end < end_fim; end++) {
    if (mem_escreve(self->mem, end, prog_dado(prog, end)) != ERR_OK) {
      console_printf("Erro na carga da memória, endereco %d\n", end);
      return -1;
    }
  }

  prog_destroi(prog);
  console_printf("SO: carga de '%s' em %d-%d", nome_do_executavel, end_ini, end_fim);
  return end_ini;
}


// ---------------------------------------------------------------------
// ACESSO À MEMÓRIA DOS PROCESSOS {{{1
// ---------------------------------------------------------------------

// copia uma string da memória do simulador para o vetor str.
// retorna false se erro (string maior que vetor, valor não char na memória,
//   erro de acesso à memória)
// t2: deveria verificar se a memória pertence ao processo
static bool copia_str_da_mem(int tam, char str[tam], mem_t *mem, int ender)
{
  for (int indice_str = 0; indice_str < tam; indice_str++) {
    int caractere;
    if (mem_le(mem, ender + indice_str, &caractere) != ERR_OK) {
      return false;
    }
    if (caractere < 0 || caractere > 255) {
      return false;
    }
    str[indice_str] = caractere;
    if (caractere == 0) {
      return true;
    }
  }
  // estourou o tamanho de str
  return false;
}

// vim: foldmethod=marker
