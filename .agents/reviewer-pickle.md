name: reviewer-pickle
role: reviewer
cli: opencode
cmd: opencode --model "big pickle" --review-only --strict
capabilities: [code-review, qa, fossil-read]
description: Agente de controle de qualidade (Big Pickle) que revisa artefatos e código de tickets no estado review. Rejeita falhas de design antes de fechar a tarefa.
