# MEI — Manual do Usuário: Backend GitHub

MEI orquestra agentes de IA autônomos em repositórios GitHub. Os agentes trabalham em
paralelo — planejando, implementando e revisando código — coordenados pelo orquestrador
usando GitHub Issues como fonte única de verdade.

---

## Pré-requisitos

| Ferramenta | Verificação            | Instalação                          |
|------------|------------------------|-------------------------------------|
| `mei`      | `mei --version`        | `make && make install` neste repo   |
| `gh`       | `gh auth status`       | `gh auth login` se necessário       |
| `git`      | `git config user.email`| Necessário para commits dos agentes |
| `tmux`     | `tmux -V`              | Cada agente roda em um pane próprio |

---

## Conceitos fundamentais

### Issues como tickets

Cada GitHub Issue é um ticket MEI. O estado é armazenado em labels — nunca no campo
nativo de assignee:

| Label GitHub              | Significado                                         |
|---------------------------|-----------------------------------------------------|
| `status:Open`             | Disponível para o planner processar                 |
| `status:Planned`          | Planner designou a um agente                        |
| `status:In Progress`      | Agente trabalhando                                  |
| `status:Review`           | Submetido; aguardando revisão                       |
| `status:Rework`           | Revisor rejeitou; coder refatora                    |
| `status:Blocked`          | Bloqueio legítimo; planner resolve automaticamente  |
| `status:Done`             | Concluído (issue fechada automaticamente)           |
| `status:Pending Approval` | Planner escreveu plano; aguarda aprovação humana    |
| `status:Verified`         | Humano aprovou o plano                              |
| `status:Delegated`        | Sub-issues criados; execução em andamento           |
| `status:QA Ready`         | Sub-issues concluídos; aguardando QA do planner     |

O **assignee real do GitHub não é usado**. O MEI usa a label `assignee:<hash>` para
designar agentes sem interferir na interface nativa.

### Definição de agentes (`.mei/`)

O diretório `.mei/` na raiz do repositório contém um arquivo `.md` por agente:

```
.mei/
  planner.md    — analisa issues, decompõe em sub-issues
  coder.md      — implementa, commita, submete para revisão
  reviewer.md   — valida entregáveis, aprova ou solicita rework
```

Formato de cada arquivo:

```
name: coder
role: coder
cli: claude
cmd: claude --dangerously-skip-permissions
capabilities: [coding, git-commit, debugging]
description: Coding agent. Picks up Planned issues, implements the required
  changes on a dedicated branch, and submits for review.
```

> `description:` deve ser o **último campo** — o parser acumula todas as linhas seguintes.

---

## Referência de comandos

```
mei github new    <owner/repo>              Criar labels + scaffold .mei/ no repo
mei github status <owner/repo>              Listar issues agrupados por status
mei github agents <owner/repo>              Listar agentes definidos em .mei/
mei github run    <owner/repo> [--clean]    Iniciar o orquestrador (TUI ncurses)
```

---

## Fluxo 1 — Configurar um repositório existente

Execute uma única vez por repositório. Requer permissão de escrita (`push`).

```
$ mei github new trumae/valente
Setting up MEI on github.com/trumae/valente...

  [1/4] Verifying repository access...     ok
  [2/4] Creating MEI labels...              ok
  [3/4] Creating agent scaffolds...         ok
  [4/4] Committing and pushing scaffolds... ok

Repository ready: github.com/trumae/valente

Next steps:
  1. Customize agents:
       git clone https://github.com/trumae/valente && cd valente
       $EDITOR .mei/planner.md .mei/coder.md .mei/reviewer.md
       git commit -am "Customize agents" && git push

  2. Create your first issue on GitHub (set label status:Open).

  3. Run the orchestrator:
       mei github run trumae/valente
```

O que `new` faz internamente:
1. Verifica acesso via `gh api repos/<owner/repo>`
2. Cria as 12 labels `status:*` com `gh label create --force` (seguro re-executar)
3. Clona o repositório em `/tmp/mei_gh_XXXXXX`
4. Escreve `.mei/planner.md`, `.mei/coder.md`, `.mei/reviewer.md`
5. Commita e faz push; remove o diretório temporário

---

## Fluxo 2 — Personalizar os agentes

Após o scaffold, edite `.mei/` para definir a persona, o stack e o estilo de cada agente:

```bash
git clone https://github.com/trumae/valente && cd valente
```

Exemplo de `.mei/coder.md` personalizado:

```
name: coder
role: coder
cli: claude
cmd: claude --dangerously-skip-permissions
capabilities: [coding, git-commit, debugging]
description: Coding agent especializado em TypeScript/React com Vitest para testes.
  Sempre lê os testes existentes antes de escrever código novo. Segue o Airbnb
  style guide. Cria branches com prefixo mei/issue-<número>.
```

```bash
git commit -am "Customize agents for this project"
git push
```

---

## Fluxo 3 — Criar issues para o orquestrador processar

Issues precisam da label `status:Open` para o MEI reconhecê-las.

```bash
# Issue simples — coder pega diretamente
gh issue create -R trumae/valente \
  --title "Adicionar paginação na listagem de usuários" \
  --body "A endpoint /users retorna todos os registros.
Adicionar parâmetros limit e offset com validação de range." \
  --label "status:Open"

# Issue pai — planner decompõe em sub-issues
gh issue create -R trumae/valente \
  --title "[FEATURE] Sistema de notificações por email" \
  --body "Implementar envio de emails transacionais via SendGrid:
- Confirmação de cadastro
- Reset de senha
- Notificações de atividade semanal" \
  --label "status:Open"
```

Você também pode criar issues pela interface web do GitHub e aplicar a label `status:Open`
manualmente.

---

## Fluxo 4 — Inspecionar o estado atual

```
$ mei github status trumae/valente

  trumae/valente

  Blocked       (1)
    #12         Integrar com API de pagamentos — falta token  [coder]

  In Progress   (2)
    #8          Adicionar paginação na listagem de usuários   [coder]
    #9          Refatorar módulo de autenticação              [planner]

  Review        (1)
    #7          Corrigir validação de CPF no cadastro

  Open          (3)
    #13         Implementar export CSV
    #14         Dashboard de métricas
    #15         Testes de integração do módulo de pedidos

  Done          (5)
```

Issues são agrupados por urgência: Blocked → In Progress → Review → Rework → Open →
Planned → Done (exibido apenas como contagem).

---

## Fluxo 5 — Rodar o orquestrador

```bash
# Iniciar normalmente
mei github run trumae/valente

# Forçar limpeza de workspaces anteriores em /tmp/workspaces/
mei github run trumae/valente --clean
```

O orquestrador:
1. Carrega agentes de `.mei/` via API do GitHub
2. Inicia sessão tmux `mei` com um pane por agente
3. Monta o loop de tick: sincroniza issues, despacha trabalho, detecta stalls
4. Exibe a TUI ncurses no terminal corrente

**Cache adaptativo**: para reduzir chamadas à API, o backend usa cache local com
intervalo de `max(5, agentes_ativos × 2)` ticks entre polls. Operações de escrita
(mudança de status, comentários) invalidam o cache imediatamente.

---

## Referência da TUI

### Teclas universais

| Tecla     | Ação                                          |
|-----------|-----------------------------------------------|
| `q` / `Q` | Encerrar o orquestrador (shutdown limpo)      |
| `Tab`     | Alternar entre tela de Agentes e de Tickets   |
| `1`       | Ir para tela de Agentes                       |
| `2`       | Ir para tela de Tickets                       |

### Tela de Agentes (`1`)

| Tecla     | Ação                                            |
|-----------|-------------------------------------------------|
| `↑` / `↓` | Navegar entre agentes                           |
| `a`       | Attach no tmux do agente (observe em tempo real)|
| `p`       | Pausar agente (suspende ticks para ele)         |
| `r`       | Retomar agente pausado                          |
| `k`       | Matar agente (estado OFFLINE)                   |

> Ao fazer **attach** (`a`): os ticks do orquestrador ficam suspensos enquanto você observa.
> Detache com **Ctrl+B, D** para retomar.

### Tela de Tickets (`2`)

| Tecla     | Ação                                                     |
|-----------|----------------------------------------------------------|
| `↑` / `↓` | Navegar entre issues                                     |
| `s`       | Avançar status (Open → Planned → In Progress → ...)      |
| `S`       | Retroceder status                                        |
| `r`       | Recarregar issues do GitHub                              |
| `o`       | Ciclar ordenação: Status → Título → Assignee             |
| `d`       | Redirecionar issue para outro agente                     |

---

## Ciclos de vida completos

### Issue simples (executor direto)

```
status:Open
    │  planner analisa, designa ao coder
    ▼
status:Planned          [assignee:coder-hash]
    │  coder clona workspace, cria branch mei/issue-8
    ▼
status:In Progress
    │  coder termina, limpa assignee e submete
    ▼
status:Review           (assignee vazio — revisor pega)
    │  revisor aprova
    ▼
status:Done             (issue fechada automaticamente)
```

Se o revisor rejeitar:

```
status:Rework           [assignee:coder-hash]
    │  coder corrige com base no feedback [review]
    ▼
status:Review           (ciclo repete até aprovação)
```

### Issue pai com gate de aprovação humana

Issues complexas passam por validação humana antes da execução distribuída:

```
status:Open
    │  planner escreve plano completo em comentário [orch]
    ▼
status:Pending Approval
    │  ← INTERVENÇÃO HUMANA ←
    │  Aprovar: mude label para status:Verified no GitHub
    │  Rejeitar: volte para status:Open  (planner re-planeja)
    ▼
status:Verified
    │  planner cria sub-issues com status:Open
    ▼
status:Delegated
    │  sub-issues processados em paralelo pelos executores
    │  quando todos em Done:
    ▼
status:QA Ready
    │  planner faz revisão geral de qualidade
    ▼
status:Done             (issue pai fechada)
```

Se o QA reprovar, o planner cria sub-issues de correção e o status volta para `Delegated`.

### Bloqueio e desbloqueio automático

```
status:In Progress      [assignee:coder-hash]
    │  coder encontra bloqueio real (credenciais ausentes,
    │  conflito de escopo, dependência externa)
    │  escreve: [log] BLOCKED: <razão>
    ▼
status:Blocked          [assignee mantido]
    │  planner detecta automaticamente e resolve
    │  escreve resolução em [orch]
    ▼
status:Planned          [assignee:coder-hash restaurado]
    │  coder retoma de onde parou
    ▼
  ... continua fluxo normal ...
```

Se o planner também não conseguir resolver:

```
[orch] ESCALATION: <razão detalhada>
status:Blocked permanece — aguarda intervenção humana
```

---

## Inspecionar logs de um issue

Todo o histórico do orquestrador fica nos comentários do próprio issue:

```bash
# Ver todos os comentários
gh issue view 8 -R trumae/valente --comments

# Apenas entradas de audit log dos agentes
gh issue view 8 -R trumae/valente --json comments \
  --jq '[.comments[] | select(.body | startswith("[log]"))] | .[].body'

# Apenas notas do orquestrador
gh issue view 8 -R trumae/valente --json comments \
  --jq '[.comments[] | select(.body | startswith("[orch]"))] | .[].body'

# Apenas feedback do revisor
gh issue view 8 -R trumae/valente --json comments \
  --jq '[.comments[] | select(.body | startswith("[review]"))] | .[].body'

# Comentários humanos (sem prefixo automático)
gh issue view 8 -R trumae/valente --json comments \
  --jq '[.comments[] | select(.body | test("^\\[log\\]|^\\[orch\\]|^\\[review\\]") | not)] | .[].body'
```

| Prefixo    | Origem                                            |
|------------|---------------------------------------------------|
| `[log]`    | Audit log dos agentes: `timestamp \| agente \| msg` |
| `[orch]`   | Orquestrador: resoluções de bloqueio, observações |
| `[review]` | Notas de rejeição do revisor                      |
| *(nenhum)* | Comentário humano — lido pelo agente como contexto |

---

## Resolução de problemas

### "No tickets found in owner/repo"

O repo não tem issues com labels `status:*`:

```bash
mei github new owner/repo        # Cria as labels (seguro re-executar)
# Depois crie issues com a label status:Open
```

### "Error: cannot access 'owner/repo'"

```bash
gh auth status                   # Verificar login
gh api repos/owner/repo          # Testar acesso direto
gh auth refresh -s repo          # Re-autenticar com escopo de repo
```

### Agente travado sem progresso

```bash
# Na TUI: selecione o agente, pressione 'a' para ver o que ele está fazendo
# Detach com Ctrl+B, D

# Log de crashes do processo
cat /tmp/mei.log

# Reiniciar limpando workspaces
mei github run owner/repo --clean
```

### Issue não é pego por nenhum agente

Verifique:
1. Label `status:Open` presente (cheque em `gh issue view <n> -R owner/repo`)
2. Ausência de label `assignee:*` (se já atribuído, o orchestrador não re-atribui)
3. Arquivo `.mei/<role>.md` existe e tem `role:` compatível com o tipo de ticket
4. Agente não está em estado `PAUSED` ou `OFFLINE` na TUI

### Labels duplicadas ou com cores erradas

```bash
# --force faz upsert, seguro re-executar
mei github new owner/repo
```
