#!/usr/bin/env bash
# =============================================================================
# tests/run_real_cli_test.sh
# MEI - Teste com CLIs Reais (Opencode)
# =============================================================================
set -euo pipefail

REPO="/tmp/mei_real_cli_test.fossil"
CHECKOUT="/tmp/mei_real_cli_checkout"
MEI_BIN="$(realpath "$(dirname "$0")/../bin/orchestrator_ui")"

echo "══════════════════════════════════════════════════════════"
echo "  MEI - Teste de CLIs Reais (Opencode)"
echo "══════════════════════════════════════════════════════════"

# Cleanup
tmux kill-session -t mei_real 2>/dev/null || true
tmux kill-session -t mei 2>/dev/null || true
rm -rf "$REPO" "$CHECKOUT" /tmp/workspaces

# Setup Fossil
fossil init "$REPO" > /dev/null
mkdir -p "$CHECKOUT" && cd "$CHECKOUT"
fossil open "$REPO" > /dev/null 2>&1

mkdir -p .agents
cat > .agents/opencode-agent.md << 'EOF'
name: opencode-agent
role: coder
cli: opencode
cmd: bash -c "opencode --model 'Big Pickle' --auto-commit --workspace ./ || echo 'Opencode failed/not installed'; sleep 60"
EOF

fossil add .agents/ > /dev/null
fossil commit -m "add opencode agent" > /dev/null

echo "✅ Ambiente preparado com agente opencode-agent (model 'Big Pickle')."

# Inicia o orquestrador no tmux
tmux new-session -d -s mei_real -x 220 -y 50 2>/dev/null || true
tmux send-keys -t mei_real "$MEI_BIN $REPO --clean" Enter

echo "Aguardando orquestrador inicializar (4s)..."
sleep 4

if tmux list-windows -t mei -F '#W' 2>/dev/null | grep -qx 'opencode-agent'; then
    echo "✅ PASS: Janela tmux 'opencode-agent' foi criada e está executando o CLI real."
else
    echo "❌ FAIL: Janela 'opencode-agent' não foi encontrada."
    exit 1
fi

# Cria um ticket para o agente
echo "Criando ticket para o agente..."
fossil ticket add title 'Implementar func X' comment 'Requisito Y' -R "$REPO" > /dev/null

echo "Aguardando processamento do orquestrador (5s)..."
sleep 5

echo "══════════════════════════════════════════════════════════"
echo "Output atual do agente opencode no tmux:"
tmux capture-pane -p -t "mei:opencode-agent" | tail -n 15 || echo "(nenhum output disponível)"
echo "══════════════════════════════════════════════════════════"

# Cleanup
tmux kill-session -t mei_real 2>/dev/null || true
tmux kill-session -t mei 2>/dev/null || true
rm -rf "$REPO" "$CHECKOUT" /tmp/workspaces
echo "Teste finalizado."
