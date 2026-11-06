# Documentação de Pesquisa — MFL

Esta pasta reúne o **registro acadêmico** do desenvolvimento da linguagem MFL
(Materials & Manufacturing Language), para apoiar uma possível Iniciação
Científica (IC) ou Trabalho de Conclusão de Curso (TCC).

A documentação técnica da linguagem fica em [`../docs/`](../docs/) (em
inglês). Aqui ficam o **histórico**, as **decisões** e as **evidências**, em
português, prontos para virar texto de relatório, monografia ou artigo.

## Estrutura

| Caminho | Conteúdo | Uso no trabalho acadêmico |
|---------|----------|---------------------------|
| [`diario/`](diario/) | Diário de desenvolvimento, uma entrada por sessão de trabalho | Capítulo de metodologia / desenvolvimento; cronograma executado |
| [`decisoes.md`](decisoes.md) | Registro de decisões de projeto (ADRs) com alternativas e justificativas | Justificativa das escolhas técnicas |
| [`uso-de-ia.md`](uso-de-ia.md) | Declaração do uso de ferramentas de IA | Transparência e integridade acadêmica |
| [`referencias.bib`](referencias.bib) | Referências bibliográficas em BibTeX | Revisão bibliográfica (LaTeX/abnTeX2, Zotero, Mendeley) |
| [`evidencias/`](evidencias/) | Capturas de tela, saídas de terminal, logs de CI, tabelas de resultados | Figuras e anexos |
| [`modelos/`](modelos/) | Modelos para novas entradas do diário e registros de experimento | Padronização dos registros |

## Como manter

1. **A cada sessão de trabalho**, copie
   [`modelos/entrada-diario.md`](modelos/entrada-diario.md) para
   `diario/AAAA-MM-DD_assunto.md` e preencha.
2. **A cada decisão técnica relevante**, acrescente um ADR em
   [`decisoes.md`](decisoes.md). Não apague ADRs antigos: marque-os como
   *Substituído por ADR-XXX*.
3. **A cada simulação ou teste científico**, use
   [`modelos/registro-experimento.md`](modelos/registro-experimento.md).
4. Registre **apenas o que foi realmente executado e verificado**. Planos
   ficam marcados como planos.
5. Cite commits pelo hash (ex.: `0a1b7b9`) e execuções de CI pelo ID ou link.
   Isso torna o histórico auditável e reproduzível.

## Índice do diário

| Data | Entrada | Resumo |
|------|---------|--------|
| 2026-11-05 | [Criação do projeto](diario/2026-11-05_criacao-do-projeto.md) | Repositório, estrutura, documentação, CMake, CI, branches |

## Possíveis enquadramentos acadêmicos

- **IC (Engenharia de Materiais / Mecânica / Computação):** "Desenvolvimento
  de uma linguagem de domínio específico com análise dimensional nativa para
  simulação térmica em soldagem MIG/MAG".
- **TCC:** projeto, implementação e validação do núcleo da linguagem (v0.1) e
  do solucionador térmico 1D, com verificação contra soluções analíticas.
- **Artigo/congresso:** verificação numérica e comparação com dados
  experimentais (etapas 4–7 do [plano de validação](../docs/validation-plan.md)).

O título, os objetivos e o recorte final devem ser definidos com o(a)
orientador(a).
