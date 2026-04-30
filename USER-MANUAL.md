# MEI Orquestrador - Manual do Usuário

O **MEI** (Multi-Agent Environment Integrator) é um orquestrador C99 de alta performance desenhado para governar sistemas de inteligência artificial baseados em processos de linha de comando (CLI), rodando de forma resiliente e isolada através de sessões **Tmux**, e utilizando um repositório **Fossil** como sua *única fonte de verdade*.

---

## 1. Princípios do Sistema

- **Arquitetura Orientada a Heartbeat (Tick-based):** Agentes não operam em loop infinito desgovernado. Eles são amostrados periodicamente pelo Orquestrador, que distribui pacotes estruturados (PULSE) apenas se o agente estiver apto a executar o próximo passo.
- **Fossil First:** Todo o estado, configuração e histórico (Timeline) vivem no repositório Fossil local (`.fossil`). O MEI só carrega o que estiver lá.
- **Isolamento de Workspaces:** Cada agente ganha uma cópia do repositório Fossil em `/tmp/workspaces/<agente>` onde as modificações ocorrem sem colidir com outros atores do sistema.
- **Protocolo PULSE:** Todas as instruções são empacotadas de maneira previsível.

---

## 2. Compilando e Executando

### Pré-requisitos
- Compilador GCC (`-std=c99`)
- Ncurses (`libncurses-dev` ou equivalente no macOS)
- `tmux` instalado no sistema
- `fossil` instalado no sistema

### Build
Para construir o binário final `orchestrator_ui`:
```bash
make clean && make
```

### Inicialização
O programa necessita de um arquivo de repositório Fossil ativo.
```bash
./bin/orchestrator_ui <caminho_para_repositorio.fossil>
```
*Exemplo:*
`./bin/orchestrator_ui /Users/viniciusmaciel/projs/MEI/repo.fossil`

---

## 3. Configurando Agentes (Diretório `/.agents/`)

Os agentes são definidos por arquivos Markdown colocados no diretório `/.agents/` do seu projeto. O Orquestrador faz a leitura desses arquivos na inicialização para montar o pool de atores disponíveis.

**Formato Esperado (`/.agents/planner.md`):**
```markdown
name: planner-1
role: planner
cli: claude
cmd: "claude-cli start --mode planner"
```
*(Nota: a propriedade `name` não deve conter espaços e será o ID base para o workspace e o nome da sessão no tmux).*

---

## 4. Trabalhando na Interface Visual (Ncurses)

A interface do Orquestrador possui três painéis principais:
- **Painel de Agentes (Esquerda):** Mostra os processos ativos e seus respectivos estados (`IN_PROGRESS`, `BLOCKED`, `PAUSED`).
- **Painel de Detalhes (Direita):** Estatísticas detalhadas (tickets, CPU mockada/real, status do passo atual) sobre o agente destacado.
- **Painel de Ações e Logs (Inferior):** Log do sistema em tempo real refletindo batimentos de Heartbeat e tráfego PULSE.

### Atalhos Globais:
- **`Seta Cima` / `Seta Baixo`**: Navega entre os agentes listados.
- **`p` ou `P`**: **Pausa** o agente selecionado. Ele continuará vivo no Tmux, mas deixará de receber excitação do Heartbeat.
- **`r` ou `R`**: **Retoma** a execução do agente selecionado.
- **`k` ou `K`**: **Mata** o processo do agente selecionado, forçando o término da sessão `tmux`.
- **`a` ou `A`**: **Anexa** a sua janela atual diretamente ao console `tmux` onde o agente roda. Excelente para *live debug* do output real gerado pelo LLM. Para desanexar e voltar ao Orquestrador, pressione o atalho local de detach do seu Tmux (geralmente `Ctrl+B, d`).
- **`q` ou `Q`**: Desliga o orquestrador graciosamente, matando todas as sessões Tmux agregadas.

---

## 5. Lidando com Falhas e o `MAX_STEPS_PER_TICKET`

Se um agente entra em loop contínuo e não resolve a demanda após um número limite de transições (por padrão, **50 pulsos**), a trava de segurança `MAX_STEPS_PER_TICKET` do MEI é acionada. 

**O que acontece:**
1. O estado do Agente muda para `BLOCKED`.
2. Uma instrução CLI é disparada para o Fossil, atualizando a *tag/status* do ticket correspondente para travado.
3. Caberá a um agente **Revisor** ou a você (humano) acessar a thread e intervir com novas instruções manuais ou reassinar o ticket.

## Nota

Se você criar um ticket usando a linha de comando do fossil: fossil ticket add title "Criar nova interface" comment "Mudar as cores para azul".

Não utilize o campo sub-sistema do fossil para colocar o nome do agente. Este campo não está sendo utilizado pelo MEI. 
O MEI busca o agente para quem ele está designado através do campo "private-contact".