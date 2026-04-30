# MEI Test Suite

Este documento descreve os testes automatizados do sistema MEI (Multi-Agent Environment Integrator) e como executá-los.

## Pré-requisitos

- **Fossil**: Deve estar instalado e no PATH.
- **Tmux**: Deve estar instalado e no PATH.
- **Compilação**: O binário do orquestrador deve ser compilado antes de rodar os testes.
  ```bash
  make clean && make
  ```

## 1. Teste de Integração E2E (`tests/run_integration_test.sh`)

Este é o principal teste de fumaça do sistema. Ele valida a orquestração completa em um ambiente headless.

### O que o teste faz:
1. **Setup**: Cria um repositório Fossil temporário e define dois agentes (`planner-fake` e `coder-fake`) via arquivos `.md`.
2. **Execução**: Inicia o orquestrador em uma sessão Tmux separada (`mei_test`).
3. **Simulação de Fluxo**:
   - O `planner-fake` aguarda alguns segundos e cria um ticket no Fossil, delegando-o ao `coder-fake` (usando o campo `private_contact`).
   - O orquestrador detecta o ticket, calcula o hash do `coder-fake`, identifica a delegação e envia um sinal **PULSE** para a janela do `coder-fake`.
4. **Verificação**:
   - Confirma a existência das janelas Tmux para cada agente.
   - Valida no banco de dados do Fossil se o ticket foi criado e se o status/atribuição foram processados.
   - Captura o output das janelas dos agentes para confirmar o recebimento do PULSE.

### Como rodar:
```bash
bash tests/run_integration_test.sh
```
*Use `--verbose` para ver logs detalhados durante a execução.*

---

## 2. Teste de CLI Real - Opencode (`tests/run_real_cli_test.sh`)

Este teste valida se o orquestrador consegue carregar e executar ferramentas de linha de comando reais como agentes.

### O que o teste faz:
1. **Configuração**: Define um agente que utiliza o comando `opencode` com parâmetros específicos (`--model "Big Pickle"`).
2. **Execução**: Inicia o orquestrador e aguarda o spawn do agente.
3. **Verificação**: 
   - Checa se a janela Tmux com o nome do agente foi criada.
   - Captura os primeiros segundos de output da janela para confirmar que o binário `opencode` foi chamado corretamente (mostrando help ou erro de execução caso não esteja logado).

### Como rodar:
```bash
bash tests/run_real_cli_test.sh
```

---

## Observações Técnicas

- **Workspaces**: Os testes utilizam `/tmp/workspaces/` para checkouts temporários.
- **Limpeza**: Ao final de cada teste, as sessões Tmux (`mei`, `mei_test`, `mei_real`) e os arquivos temporários são removidos automaticamente (via trap EXIT).
- **Depuração**: Caso um teste falhe, verifique `/tmp/fossil_err.log` ou use `tmux attach -t mei_test` antes do teste terminar para inspecionar a UI do orquestrador.
