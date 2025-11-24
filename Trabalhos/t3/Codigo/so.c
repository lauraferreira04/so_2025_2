// so.c
// sistema operacional
// simulador de computador
// so25b

// ---------------------------------------------------------------------
// INCLUDES {{{1
// ---------------------------------------------------------------------

#include "so.h"
#include "cpu.h"
#include "dispositivos.h"
#include "err.h"
#include "irq.h"
#include "memoria.h"
#include "programa.h"
#include "tabpag.h"
#include "fila.h"
#include "metricas.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>


// ---------------------------------------------------------------------
// CONSTANTES E TIPOS {{{1
// ---------------------------------------------------------------------

#define MEM_TAM 10000
// tempo de transferência de uma página entre a memória principal e a secundária
#define TEMPO_SWAP 0  // 0 por enquanto para testar
#define PROTEGIDO 100 // pid de uma página protegida

// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50   // em instruções executadas
// definidas no t2
#define QUANTUM 10

#define N_MAX_PROCESSOS 5 // num máx de processos
#define N_TERMINAIS 4 
#define SEM_PROCESSO -1  // não tem processo atual
#define SEM_DISPOSITIVO -1  // não tem um dispositivo que causou bloqueio

#define SEM_ESCALONADOR 0
#define ESCALONADOR 2
#define ROUND_ROBIN 1
#define PRIORIDADE 2

// Não tem processos nem memória virtual, mas é preciso usar a paginação,
//   pelo menos para implementar relocação, já que os programas estão sendo
//   todos montados para serem executados no endereço 0 e o endereço 0
//   físico é usado pelo hardware nas interrupções.
// Os programas estão sendo carregados no início de um quadro, e usam quantos
//   quadros forem necessárias. Para isso a variável quadro_livre contém
//   o número do primeiro quadro da memória principal que ainda não foi usado.
//   Na carga do processo, a tabela de páginas (deveria ter uma por processo,
//   mas não tem processo) é alterada para que o endereço virtual 0 resulte
//   no quadro onde o programa foi carregado. Com isso, o programa carregado
//   é acessível, mas o acesso ao anterior é perdido.
// estados de um processo
typedef enum estado_t {
  PRONTO,
  EXECUTANDO,
  PARADO,
  BLOQUEADO,
  FINALIZADO
} estado_t;

typedef struct quadro {
  int pid;
  int pagina;
} quadro_t;

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

  // T3
  tabpag_t *tabpag;
  int regComplemento; 
  int quadro_mem2;  // quadro a partir do qual o programa foi carregado em memória secundária
  int data_desbloqueio;  // data até desbloquear um processo
} processo_t;

// t3: a interface de algumas funções que manipulam memória teve que ser alterada,
//   para incluir o processo ao qual elas se referem. Para isso, é necessário um
//   tipo de dados para identificar um processo. Neste código, não tem processos
//   implementados, e não tem um tipo para isso. Foi usado o tipo int.
//   É necessário também um valor para representar um processo inexistente.
//   Foi usado o valor -1. Altere para o seu tipo, ou substitua os usos de
//   processo_t e NENHUM_PROCESSO para o seu tipo.
//   ALGUM_PROCESSO serve para representar um processo que não é NENHUM. Só tem
//   algum sentido enquanto não tem implementação de processos.

/*typedef int processo_t;
#define NENHUM_PROCESSO -1
#define ALGUM_PROCESSO 0*/

struct so_t {
  cpu_t *cpu;
  mem_t *mem;
  mmu_t *mmu;
  es_t *es;
  console_t *console;
  bool erro_interno;

  int regA, regX, regPC, regERRO, regComplemento; // cópia do estado da CPU
  
  // t2: tabela de processos, processo corrente, pendências, etc
  processo_t tabela_de_processos[N_MAX_PROCESSOS]; // fixa
  // ponteiro para o processo atual dentro da tabela de processos
  processo_t *processo_atual;
  int n_processos_tabela;
  Fila *processos_prontos;
  // id dos processos que estão usando cada terminal
  int terminais_usados[N_TERMINAIS];

  // primeiro quadro da memória que está livre (quadros anteriores estão ocupados)
  // t3: com memória virtual, o controle de memória livre e ocupada deve ser mais
  //     completo que isso
  int quadro_livre_mem;
  // vetor de quadros com o pid do dono do quadro e o número da página que o ocupa
  quadro_t *tabquadros;

  // memória secundaria
  mem_t *mem2;
  // indica se a memória secundária está liberada
  bool mem2_livre;
  // tempo até liberar a memória secundária
  int mem2_tempo_ate_livre;
  // primeiro quadro da memória secundária que está livre
  int quadro_livre_mem2;
};


// função de tratamento de interrupção (entrada no SO)
static int so_trata_interrupcao(void *argC, int reg_A);

// funções auxiliares
// no t3, foi adicionado o 'processo' aos argumentos dessas funções 
// carrega o programa contido no arquivo para memória virtual de um processo
// retorna o endereço virtual inicial de execução
static int so_carrega_programa(so_t *self, processo_t *processo,
                               char *nome_do_executavel);
// copia para str da memória do processo, até copiar um 0 (retorna true) ou tam bytes
static bool so_copia_str_do_processo(so_t *self, int tam, char str[tam],
                                     int end_virt, processo_t processo);
                                   
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

// t3: precisa criar tab de pag
// cria um processo, retorna pid
int processo_cria(so_t *self, char *nome_do_executavel, int *ender_carga) {
    // ... (lógica de encontrar slot e tabela cheia mantém igual) ...
    int slot = SEM_PROCESSO;
    for (int i = 0; i < N_MAX_PROCESSOS; i++){
        if (self->tabela_de_processos[i].pid == SEM_PROCESSO){
            slot = i;
            break;
        }
    }
    if (slot == SEM_PROCESSO) return -1;

    // inicializa o novo processo
    processo_t *novo_p = &self->tabela_de_processos[slot];

    if (self->n_processos_tabela == 0) self->n_processos_tabela = 1;
    novo_p->pid = self->n_processos_tabela++;

    strncpy(novo_p->executavel, nome_do_executavel, 100);
    novo_p->executavel[99] = '\0';

    novo_p->estado = PRONTO;
    novo_p->regA = 0;
    novo_p->regX = 0;
    novo_p->regERRO = 0;
    novo_p->pid_esperado = SEM_PROCESSO;
    novo_p->dispositivo_causou_bloqueio = SEM_DISPOSITIVO;
    novo_p->quantum = QUANTUM;
    novo_p->prioridade = 0.5;
    novo_p->tabpag = tabpag_cria();
    novo_p->quadro_mem2 = 0;
    novo_p->data_desbloqueio = 0;

    // carrega o programa na memória
    int endereco_inicial = so_carrega_programa(self, novo_p, nome_do_executavel);
    if (ender_carga != NULL) memcpy(ender_carga, &endereco_inicial, sizeof(int));
    novo_p->regPC = endereco_inicial;
    
    if (endereco_inicial < 0) return -1;

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

    // retorna o PID
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

so_t *so_cria(cpu_t *cpu, mem_t *mem, mem_t *mem_secundaria, mmu_t *mmu,
              es_t *es, console_t *console)
{
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  self->cpu = cpu;
  self->mem = mem;
  self->mem2 = mem_secundaria;
  self->mmu = mmu;
  self->es = es;
  self->console = console;
  self->erro_interno = false;
  self->mem2_livre = true;
  self->mem2_tempo_ate_livre = 0;

  self->tabquadros = malloc(MEM_TAM / TAM_PAGINA * sizeof(quadro_t));
  assert(self->tabquadros != NULL);
  for (int i = 0; i < MEM_TAM/TAM_PAGINA; i++){
    self->tabquadros[i].pagina = -1;
    self->tabquadros[i].pid = SEM_PROCESSO;
  }

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

  // quando a CPU executar uma instrução CHAMAC, deve chamar a função
  //   so_trata_interrupcao, com primeiro argumento um ptr para o SO
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);

  // inicializa a tabela de páginas global, e entrega ela para a MMU
  // t3: com processos, essa tabela não existiria, teria uma por processo, que
  //     deve ser colocada na MMU quando o processo é despachado para execução
  // acho que não vai precisar dessa parte abaixo
  // self->tabpag_global = tabpag_cria();
  // mmu_define_tabpag(self->mmu, self->tabpag_global);

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
  // ATENÇÃO CPU_END_complemento É NOVO, NÃO TINHA NO T2
  if (mem_le(self->mem, CPU_END_A, &self->regA) != ERR_OK
      || mem_le(self->mem, CPU_END_PC, &self->regPC) != ERR_OK
      || mem_le(self->mem, CPU_END_erro, &self->regERRO) != ERR_OK
      || mem_le(self->mem, CPU_END_complemento, &self->regComplemento) != ERR_OK
      || mem_le(self->mem, 59, &self->regX)) {
    console_printf("SO: erro na leitura dos registradores");
    self->erro_interno = true;
  }

  // salva reg da memória no buffer do processo atual
  self->processo_atual->regA = self->regA;
  self->processo_atual->regPC = self->regPC;
  self->processo_atual->regERRO = self->regERRO;
  self->processo_atual->regComplemento = self->regComplemento; // NOVO NO T3
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
    
      // metricas ATENÇÃO, VAI FICAR SEM MÉTRICAS AQUI?
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
          // métricas ATENÇÃO, TEM MÉTRICAS AQUI AINDA?
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
        // t3, ainda nao implementado
        //mmu_define_tabpag(self->mmu, self->processo_corrente->tabgpag);
        
        // metricas ATENÇÃO
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
  if (self->processo_atual->pid == SEM_PROCESSO) return 1;

  // NOVO: configura a MMU para o processo atual
  // o processo_corrente->tabpag contém a tabela de paginas individual
  mmu_define_tabpag(self->mmu, self->processo_atual->tabpag);

  if (mem_escreve(self->mem, CPU_END_A, self->regA) != ERR_OK
      || mem_escreve(self->mem, CPU_END_PC, self->regPC) != ERR_OK
      || mem_escreve(self->mem, CPU_END_erro, self->regERRO) != ERR_OK
      || mem_escreve(self->mem, 59, self->regX)) {
    console_printf("SO: erro na escrita dos registradores");
    self->erro_interno = true;
  }
  if (self->erro_interno) return 1;
  else return 0;
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
  processo_t *p = (processo_t*) malloc(sizeof(processo_t));
  p->pid = SEM_PROCESSO;

  int ender = so_carrega_programa(self, p, "trata_int.maq");
  if (ender != CPU_END_TRATADOR) {
    console_printf("SO: problema na carga do programa de tratamento de interrupção");
    self->erro_interno = true;
  }

  // programa o relógio para gerar uma interrupção após INTERVALO_INTERRUPCAO
  if (es_escreve(self->es, D_RELOGIO_TIMER, INTERVALO_INTERRUPCAO) != ERR_OK) {
    console_printf("SO: problema na programação do timer");
    self->erro_interno = true;
  }

  // define o primeiro quadro livre de memória como o seguinte àquele que
  //   contém o endereço final da memória protegida (que não podem ser usadas
  //   por programas de usuário)
  // t3: o controle de memória livre deve ser mais aprimorado que isso  
  self->quadro_livre_mem = CPU_END_FIM_PROT / TAM_PAGINA + 1;
  self->quadro_livre_mem2 = 0;
  // marca os quadros de memória protegida como nao livres;
  for (int i = 0; i < self->quadro_livre_mem + 1; i++) self->tabquadros[i].pid = PROTEGIDO;

  // t2: deveria criar um processo para o init, e inicializar o estado do
  //   processador para esse processo com os registradores zerados, exceto
  //   o PC e o modo.
  // como não tem suporte a processos, está carregando os valores dos
  //   registradores diretamente no estado da CPU mantido pelo SO; daí vai
  //   copiar para o início da memória pelo despachante, de onde a CPU vai
  //   carregar para os seus registradores quando executar a instrução RETI
  //   em bios.asm (que é onde está a instrução CHAMAC que causou a execução
  //   deste código

  // coloca o programa init na memória
  // chama processo_cria
  int pid = processo_cria(self, "init.maq", NULL); 

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
  
  // VERIFICA PAGEFAULT
  // ...
  
  err_t err = self->regERRO;
  console_printf("SO: IRQ nao tratada -- erro na CPU: %s (%d)",
                 err_nome(err), self->regComplemento);
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

  // talvez seria melhor não tratar
  /*console_printf("SO: interrupção do relógio (não tratada)");*/
  self->processo_atual->quantum--;
  if (self->processo_atual->quantum <= 0 && self->processo_atual->estado != BLOQUEADO){
    self->processo_atual->quantum = 10;
    processo_atualiza_prioridade(self->processo_atual);
    fila_deque(self->processos_prontos);
    // ATENÇÃO
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
  int id_chamada = self->regA;
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
  for (;;) {  // espera ocupada!
    int estado;
    if (es_le(self->es, D_TERM_A_TECLADO_OK, &estado) != ERR_OK) {
      console_printf("SO: problema no acesso ao estado do teclado");
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
  int dado;
  if (es_le(self->es, D_TERM_A_TECLADO, &dado) != ERR_OK) {
    console_printf("SO: problema no acesso ao teclado");
    self->erro_interno = true;
    return;
  }
  // escreve no reg A do processador
  // (na verdade, na posição onde o processador vai pegar o A quando retornar da int)
  // t2: se houvesse processo, deveria escrever no reg A do processo
  // t2: o acesso só deve ser feito nesse momento se for possível; se não, o processo
  //   é bloqueado, e o acesso só deve ser feito mais tarde (e o processo desbloqueado)
  self->regA = dado;
}

// implementação da chamada se sistema SO_ESCR
// escreve o valor do reg X na saída corrente do processo
static void so_chamada_escr(so_t *self)
{
  // implementação com espera ocupada
  //   t2: deveria bloquear o processo se dispositivo ocupado
  // implementação escrevendo direto do terminal A
  //   t2: deveria usar o dispositivo de saída corrente do processo
  for (;;) {
    int estado;
    if (es_le(self->es, D_TERM_A_TELA_OK, &estado) != ERR_OK) {
      console_printf("SO: problema no acesso ao estado da tela");
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
  int dado;
  // está lendo o valor de X e escrevendo o de A direto onde o processador colocou/vai pegar
  // t2: deveria usar os registradores do processo que está realizando a E/S
  // t2: caso o processo tenha sido bloqueado, esse acesso deve ser realizado em outra execução
  //   do SO, quando ele verificar que esse acesso já pode ser feito.
  dado = self->regX;
  if (es_escreve(self->es, D_TERM_A_TELA, dado) != ERR_OK) {
    console_printf("SO: problema no acesso à tela");
    self->erro_interno = true;
    return;
  }
  self->regA = 0;
}

// implementação da chamada se sistema SO_CRIA_PROC
// cria um processo
static void so_chamada_cria_proc(so_t *self)
{
  // ainda sem suporte a processos, carrega programa e passa a executar ele
  // quem chamou o sistema não vai mais ser executado, coitado!
  // t2: deveria criar um novo processo
  // t3: identifica direito esses processos
  //processo_t processo_criador = ALGUM_PROCESSO;
  //processo_t processo_criado = ALGUM_PROCESSO;

  // em X está o endereço onde está o nome do arquivo
  int ender_proc;
  // t2: deveria ler o X do descritor do processo criador
  ender_proc = self->processo_atual->regX;
  char nome[100];
  int pid;
  if (so_copia_str_do_processo(self, 100, nome, ender_proc, *self->processo_atual)) {
    int ender_carga = -1;
    pid = processo_cria(self, nome, &ender_carga);

    // usado aqui para não gerar warning
    int indice_proc_criado = acha_indice_por_pid(self, pid);
    console_printf("indice proc criado: %d\n", indice_proc_criado);

    if (ender_carga != -1) {
      // t2: deveria escrever no PC do descritor do processo criado
      self->processo_atual->regA = pid;
      //self->regPC = ender_carga;
      return;
    } // else?
  }
  // deveria escrever -1 (se erro) ou o PID do processo criado (se OK) no reg A
  //   do processo que pediu a criação
  self->regA = -1;
}

// implementação da chamada se sistema SO_MATA_PROC
// mata o processo com pid X (ou o processo corrente se X é 0)
static void so_chamada_mata_proc(so_t *self)
{
  // t2: deveria matar um processo
  // ainda sem suporte a processos, retorna erro -1
  console_printf("SO: SO_MATA_PROC não implementada");
  self->regA = -1;
  processo_mata(self, self->processo_atual->regX);  // mata o processo atual
}

// implementação da chamada se sistema SO_ESPERA_PROC
// espera o fim do processo com pid X
static void so_chamada_espera_proc(so_t *self)
{
  // t2: deveria bloquear o processo se for o caso (e desbloquear na morte do esperado)
  // ainda sem suporte a processos, retorna erro -1
  // console_printf("SO: SO_ESPERA_PROC não implementada");
  // self->regA = -1;

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

  // processo_atualiza_prioridade(self->processo_atual);
  // fila_deque(self->processos_prontos);
}


// ---------------------------------------------------------------------
// CARGA DE PROGRAMA {{{1
// ---------------------------------------------------------------------

// funções auxiliares
static int so_carrega_programa_na_memoria_fisica(so_t *self, programa_t *programa);
static int so_carrega_programa_na_memoria_virtual(so_t *self,
                                                  programa_t *programa,
                                                  processo_t *processo);

// carrega o programa na memória
// se processo for NENHUM_PROCESSO, carrega o programa na memória física
//   senão, carrega na memória virtual do processo
// retorna o endereço de carga ou -1
static int so_carrega_programa(so_t *self, processo_t *processo,
                               char *nome_do_executavel)
{
  console_printf("SO: carga de '%s'", nome_do_executavel);

  programa_t *programa = prog_cria(nome_do_executavel);
  if (programa == NULL) {
    console_printf("Erro na leitura do programa '%s'\n", nome_do_executavel);
    return -1;
  }

  int end_carga;
  if (processo->pid == SEM_PROCESSO) {
    end_carga = so_carrega_programa_na_memoria_fisica(self, programa);
  } else {
    end_carga = so_carrega_programa_na_memoria_virtual(self, programa, processo);
  }

  prog_destroi(programa);
  return end_carga;
}

static int so_carrega_programa_na_memoria_fisica(so_t *self, programa_t *programa)
{
  int end_ini = prog_end_carga(programa);
  int end_fim = end_ini + prog_tamanho(programa);

  for (int end = end_ini; end < end_fim; end++) {
    if (mem_escreve(self->mem, end, prog_dado(programa, end)) != ERR_OK) {
      console_printf("Erro na carga da memoria, endereco %d\n", end);
      return -1;
    }
  }

  console_printf("SO: carga na memoria fisica %d-%d", end_ini, end_fim);
  return end_ini;
}

// ATENÇÃO, TEM QUE MEXER AQUI!
static int so_carrega_programa_na_memoria_virtual(so_t *self,
                                                  programa_t *programa,
                                                  processo_t *processo)
{
  // t3: isto tá furado...
  // está simplesmente lendo para o próximo quadro que nunca foi ocupado,
  //   nem testa se tem memória disponível
  // com memória virtual, a forma mais simples de implementar a carga de um
  //   programa é carregá-lo para a memória secundária, e mapear todas as páginas
  //   da tabela de páginas do processo como inválidas. Assim, as páginas serão
  //   colocadas na memória principal por demanda. Para simplificar ainda mais, a
  //   memória secundária pode ser alocada da forma como a principal está sendo
  //   alocada aqui (sem reuso)

  // calcula o tamanho de páginas necessárias para o programa
  int end_virt_ini = 0;
  int end_virt_fim = prog_tamanho(programa) - 1;
  int pagina_ini = end_virt_ini / TAM_PAGINA;
  int pagina_fim = end_virt_fim / TAM_PAGINA;
  int n_paginas = pagina_fim - pagina_ini + 1;
  int quadro_ini = self->quadro_livre_mem2;
  int quadro_fim = quadro_ini + n_paginas - 1;

  // carrega o programa na memória secundária
  int end_fis_ini = quadro_ini * TAM_PAGINA;
  int end_fis = end_fis_ini;
  for (int end_virt = end_virt_ini; end_virt <= end_virt_fim; end_virt++) {
    if (mem_escreve(self->mem2, end_fis, prog_dado(programa, end_virt)) != ERR_OK) {
      console_printf("Erro na carga da memoria, end virt %d fis %d\n", end_virt, end_fis);
      return -1;
    }
    end_fis++;
  }

  // atualiza o quadro livre inicial da memória secundária
  self->quadro_livre_mem2 = quadro_fim + 1;
  // guarda o quadro de mem2 no qual o programa do processo foi carregado
  processo->quadro_mem2 = quadro_ini;

  console_printf("SO: carga na memoria secundaria V%d-%d F%d-%d npag=%d",
                 end_virt_ini, end_virt_fim, end_fis_ini, end_fis - 1, n_paginas);
  return end_virt_ini;
}


// ---------------------------------------------------------------------
// ACESSO À MEMÓRIA DOS PROCESSOS {{{1
// ---------------------------------------------------------------------

// copia uma string da memória do processo para o vetor str.
// retorna false se erro (string maior que vetor, valor não char na memória,
//   erro de acesso à memória)
// O endereço é um endereço virtual de um processo.
// t3: Com memória virtual, cada valor do espaço de endereçamento do processo
//   pode estar em memória principal ou secundária (e tem que achar onde)
static bool so_copia_str_do_processo(so_t *self, int tam, char str[tam],
                                     int end_virt, processo_t processo)
{
  if (processo.pid == SEM_PROCESSO) return false;
  
  // ATENÇÃO DÚVIDA SE PRECISA DISSO MESMO
  mmu_define_tabpag(self->mmu, processo.tabpag);

  for (int indice_str = 0; indice_str < tam; indice_str++) {
    int caractere;
    // não tem memória virtual implementada, posso usar a mmu para traduzir
    //   os endereços e acessar a memória, porque todo o conteúdo do processo
    //   está na memória principal, e só temos uma tabela de páginas
    if (mmu_le(self->mmu, end_virt + indice_str, &caractere, usuario) != ERR_OK) {
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
