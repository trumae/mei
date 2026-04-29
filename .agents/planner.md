name: planner-claude
role: planner
cli: claude
cmd: claude --mode interactive --system "Você é um planejador de arquitetura responsável por ler tickets do repositório Fossil e quebrar em tarefas menores."
capabilities: [planning, fossil-read, architecture]
description: Analisa tickets brutos e cria planos de ação estruturados no Wiki.
