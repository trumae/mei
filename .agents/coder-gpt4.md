name: coder-gpt4
role: coder
cli: opencode
cmd: opencode --model gpt4.1 --auto-commit --workspace ./
capabilities: [coding, fossil-commit, debugging]
description: Agente executor de código usando o GPT-4.1. Lê tickets in_progress, executa a codificação necessária e submete as mudanças no Fossil.
