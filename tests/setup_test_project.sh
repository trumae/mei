#!/bin/bash
set -e

REPO="/tmp/mei_test.fossil"
CHECKOUT="/tmp/mei_test_checkout"

echo "Limpiando ambiente anterior..."
rm -rf $REPO $CHECKOUT /tmp/workspaces

echo "Criando novo repositório Fossil..."
fossil init $REPO

echo "Abrindo checkout local..."
mkdir -p $CHECKOUT
cd $CHECKOUT
fossil open $REPO

echo "Configurando agentes..."
mkdir -p .mei

# Agente 1: O Planejador Fake
# Ele espera 5 segundos, cria um ticket atribuído ao Agente 2 e entra em loop
cat << 'EOF' > .mei/planner.md
name: planner-fake
role: planner
cli: fake-cli
cmd: bash -c "echo 'Planejador Iniciado'; sleep 5; fossil ticket add title 'Tarefa gerada automaticamente' comment 'Por favor, resolva isso.' private_contact 'coder-fake'; echo 'Ticket criado!'; tail -f /dev/null"
EOF

# Agente 2: O Coder Fake
# Fica aguardando. Quando o Orquestrador ler o ticket criado pelo Planner,
# ele atribuirá ao Coder e mandará um PULSE pra cá.
cat << 'EOF' > .mei/coder-fake.md
name: coder-fake
role: coder
cli: fake-cli
cmd: bash -c "echo 'Coder Aguardando...'; while read line; do echo 'Recebeu PULSE: $line'; fossil ticket set current status 'Review'; done"
EOF

fossil add .mei/
fossil commit -m "Adicionando configuração de agentes de teste"

echo ""
echo "=== Setup Concluído ==="
echo "Para rodar o teste, execute o orquestrador com a flag --clean:"
echo "mei fossil run $REPO --clean"
echo ""
echo "Observe que, após uns segundos, o 'planner-fake' vai criar um ticket,"
echo "o Orquestrador vai detectá-lo e atribuí-lo automaticamente ao 'coder-fake',"
echo "enviando o sinal (PULSE) para a janela Tmux correspondente!"
