set pagination off

break so_trata_irq_err_cpu
commands
  printf "=== IRQ_ERR_CPU ===\n"
  printf "PID: %d\n", self->processo_atual->pid
  printf "Erro: %d\n", self->processo_atual->regERRO
  printf "Complemento: %d\n", self->processo_atual->regComplemento
  printf "TabPag: %p\n", self->processo_atual->tabpag
  continue
end

break trata_falta_de_pagina
commands
  printf "=== TRATA_FALTA_PAGINA ===\n"
  printf "PID: %d\n", self->processo_atual->pid
  printf "EndVirt: %d\n", self->processo_atual->regComplemento
  printf "Pagina: %d\n", self->processo_atual->regComplemento / 100
  printf "QuadroLivre: %d\n", quadro_livre
  continue
end

break so_despacha
commands
  printf "=== DESPACHA ===\n"
  printf "PID: %d\n", self->processo_atual->pid
  printf "PC: %d\n", self->processo_atual->regPC
  printf "TabPag: %p\n", self->processo_atual->tabpag
  continue
end

run