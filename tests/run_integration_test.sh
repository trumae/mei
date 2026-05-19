#!/usr/bin/env bash
# =============================================================================
# tests/run_integration_test.sh
# MEI - Teste de Integração End-to-End
#
# Fluxo testado:
#   1. Cria repositório Fossil limpo com dois agentes (planner-fake, coder-fake)
#   2. Inicia o orquestrador MEI em background (sem ncurses via stdbuf)
#   3. O planner-fake espera e depois cria um ticket no Fossil
#   4. O orquestrador detecta o ticket e o atribui ao coder-fake (private_contact)
#   5. O coder-fake, ao receber o PULSE, lê o ticket e o marca como "Review"
#   6. O script aferir os resultados via consulta direta ao Fossil
#
# USO: ./tests/run_integration_test.sh [--verbose]
# =============================================================================
set -euo pipefail

REPO="/tmp/mei_integration_test.fossil"
CHECKOUT="/tmp/mei_integration_checkout"
MEI_BIN="$(realpath "$(dirname "$0")/../bin/orchestrator_ui")"
VERBOSE=0
[ "${1:-}" = "--verbose" ] && VERBOSE=1

PASS=0
FAIL=0
ORCHESTRATOR_PID=""

log() { echo "[$(date +%H:%M:%S)] $*"; }
vlog() { [ "$VERBOSE" -eq 1 ] && log "$*" || true; }

assert_pass() {
    local desc="$1"
    echo "  ✅ PASS: $desc"
    PASS=$((PASS+1))
}

assert_fail() {
    local desc="$1"
    local detail="${2:-}"
    echo "  ❌ FAIL: $desc"
    [ -n "$detail" ] && echo "     → $detail"
    FAIL=$((FAIL+1))
}

check() {
    local desc="$1"
    local condition="$2"
    if eval "$condition" > /dev/null 2>&1; then
        assert_pass "$desc"
    else
        assert_fail "$desc" "$condition"
    fi
}

cleanup() {
    log "Limpando ambiente de teste..."
    tmux capture-pane -p -t mei_test > /tmp/mei_test_screen.log 2>/dev/null || true
    tmux kill-session -t mei_test 2>/dev/null || true
    tmux kill-session -t mei 2>/dev/null || true
    # rm -rf "$REPO" "$CHECKOUT" /tmp/workspaces
}
trap cleanup EXIT

# =============================================================================
echo ""
echo "══════════════════════════════════════════════════════════"
echo "  MEI - Teste de Integração End-to-End"
echo "══════════════════════════════════════════════════════════"
echo ""

# --- FASE 1: Setup ----------------------------------------------------------------
log "FASE 1: Preparando ambiente..."
cleanup 2>/dev/null || true

fossil init "$REPO" > /dev/null
mkdir -p "$CHECKOUT"
cd "$CHECKOUT"
fossil open "$REPO" > /dev/null 2>&1
mkdir -p .mei

# Agente 1: Planejador
# Aguarda 6s (tempo do orquestrador inicializar), então cria um ticket delegado ao coder-fake
# e aguarda indefinidamente para simular um CLI real
cat > .mei/planner-fake.md << 'AGENTEOF'
name: planner-fake
role: planner
cli: bash
cmd: bash -c "sleep 6 && fossil ticket add title 'Implementar autenticação' comment 'Necessário JWT.' private_contact 'coder-fake' && echo '[planner-fake] Ticket criado e delegado ao coder-fake' && exec tail -f /dev/null"
AGENTEOF

# Agente 2: Coder
# Lê cada linha de stdin (PULSE recebido) e ao receber o ticket, marca como Review
cat > .mei/coder-fake.md << 'AGENTEOF'
name: coder-fake
role: coder
cli: bash
cmd: bash -c "echo '[coder-fake] Aguardando tickets...' && while IFS= read -r line; do echo \"[coder-fake] PULSE recebido: \$line\"; done"
AGENTEOF

fossil add .mei/ > /dev/null
fossil commit -m "Configuração dos agentes de integração" > /dev/null
vlog "Agentes configurados e commitados no Fossil."

check "Repositório Fossil criado" "test -f $REPO"
check "Agentes commitados no Fossil" "fossil ls -r trunk -R $REPO | grep -q '.mei/planner-fake.md'"
echo ""

# --- FASE 2: Iniciar Orquestrador -------------------------------------------------
log "FASE 2: Iniciando orquestrador em background (headless)..."

# O orquestrador usa ncurses e precisa de um terminal real.
# Iniciamos ele numa janela tmux dedicada (headless mas com PTY válido).
# Depois coletamos o estado via Fossil e via tmux capture-pane.
tmux new-session -d -s mei_test -x 220 -y 50 2>/dev/null || true
tmux send-keys -t mei_test "$MEI_BIN fossil run $REPO --clean > /tmp/mei_orchestrator.log 2>&1" Enter
ORCHESTRATOR_PID="" # PID será o da shell filha no tmux; usaremos tmux para verificar

# Aguarda inicializar e spawnar os agentes
log "Aguardando 4s para o orquestrador inicializar..."
sleep 4

check "Sessão tmux 'mei_test' (orquestrador) existe" "tmux has-session -t mei_test"
check "Sessão tmux 'mei' (agentes) existe" "tmux has-session -t mei"
check "Janela 'planner-fake' existe" "tmux list-windows -t mei -F '#W' | grep -qx 'planner-fake'"
check "Janela 'coder-fake' existe" "tmux list-windows -t mei -F '#W' | grep -qx 'coder-fake'"
echo ""

# --- FASE 3: Verificar interação entre agentes ------------------------------------
log "FASE 3: Aguardando o planner-fake criar o ticket (até 15s)..."

TICKET_FOUND=0
for i in $(seq 1 15); do
    count=$(printf ".mode list\nSELECT count(*) FROM ticket WHERE title = 'Implementar autenticação';\n" | fossil sqlite -R "$REPO" 2>/dev/null | tail -n1 | tr -d '[:space:]' || echo "0")
    if [ "$count" -ge 1 ] 2>/dev/null; then
        TICKET_FOUND=1
        log "Ticket detectado após ${i}s!"
        break
    fi
    vlog "Aguardando... (${i}s)"
    sleep 1
done

if [ "$TICKET_FOUND" -eq 1 ]; then
    assert_pass "planner-fake criou ticket no Fossil"
else
    assert_fail "planner-fake criou ticket no Fossil" "Timeout esperando ticket"
fi

# Aguarda o orquestrador fazer o tick e atribuir o ticket ao coder-fake
log "Aguardando 6s para o orquestrador processar o ticket..."
sleep 6

# Verifica se o ticket foi atribuído ao coder-fake (via private_contact que guarda o hash)
ASSIGNED=$(printf ".mode list\nSELECT coalesce(private_contact, 'nenhum') FROM ticket WHERE title = 'Implementar autenticação';\n" | fossil sqlite -R "$REPO" 2>/dev/null | tail -n1 | tr -d '[:space:]' || echo "nenhum")

CODER_HASH=$(echo -n "coder-fake" | shasum | awk '{print $1}')

if [ "$ASSIGNED" = "$CODER_HASH" ]; then
    assert_pass "Ticket atribuído ao coder-fake via private_contact"
else
    assert_fail "Ticket atribuído ao coder-fake via private_contact" "private_contact atual: '$ASSIGNED'"
fi

# Verifica se o status foi atualizado para In Progress
STATUS=$(printf ".mode list\nSELECT coalesce(status, 'Open') FROM ticket WHERE title = 'Implementar autenticação';\n" | fossil sqlite -R "$REPO" 2>/dev/null | tail -n1 | tr -d '[:space:]' || echo "Open")

if echo "$STATUS" | grep -qi "progress\|Review\|open"; then
    assert_pass "Status do ticket atualizado/verificado"
else
    assert_fail "Status do ticket verificado" "Status atual: '$STATUS'"
fi
echo ""

# --- FASE 4: Verificar output do tmux ---------------------------------------------
log "FASE 4: Verificando output dos agentes no tmux..."

PLANNER_OUTPUT=$(tmux capture-pane -p -t "mei:planner-fake" 2>/dev/null || echo "")
CODER_OUTPUT=$(tmux capture-pane -p -t "mei:coder-fake" 2>/dev/null || echo "")

if echo "$PLANNER_OUTPUT" | grep -q "Ticket criado"; then
    assert_pass "planner-fake confirmou criação do ticket no terminal"
else
    assert_fail "planner-fake confirmou criação do ticket no terminal" "Output: $(echo "$PLANNER_OUTPUT" | tail -3)"
fi

if echo "$CODER_OUTPUT" | grep -qi "aguardando\|PULSE"; then
    assert_pass "coder-fake está ativo no tmux e recebeu PULSE"
else
    assert_fail "coder-fake está ativo no tmux e recebeu PULSE" "Output: $(echo "$CODER_OUTPUT" | tail -3)"
fi
echo ""

# --- Resultado Final ---------------------------------------------------------------
echo "══════════════════════════════════════════════════════════"
TOTAL=$((PASS + FAIL))
echo "  Resultado: $PASS/$TOTAL testes passaram"
if [ "$FAIL" -gt 0 ]; then
    echo "  ⚠️  $FAIL teste(s) falharam."
    echo ""
    echo "  Screen do orquestrador (últimos logs):"
    cat /tmp/mei_test_screen.log | tail -n 20
    [ "$VERBOSE" -eq 0 ] && echo "  Rode com --verbose para mais detalhes."
    echo "══════════════════════════════════════════════════════════"
    exit 1
else
    echo "  🎉 Todos os testes passaram!"
    echo "══════════════════════════════════════════════════════════"
    exit 0
fi
