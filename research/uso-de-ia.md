# Declaração de Uso de Inteligência Artificial

Muitas instituições e periódicos exigem que o uso de IA generativa seja
declarado em trabalhos acadêmicos. Este arquivo registra **onde e como** a IA
foi usada no projeto, para que essa declaração possa ser feita com precisão
na IC ou no TCC.

> Verifique as normas da sua instituição, do seu curso e do seu orientador
> sobre o uso de IA antes de redigir o texto final.

## Ferramenta

- **Claude Code** (Anthropic), assistente de programação com IA, executado
  em sessão de nuvem com acesso ao repositório.

## Registro de uso

| Data | Atividade | Papel da IA | Papel do autor | Verificação humana |
|------|-----------|-------------|----------------|--------------------|
| 2026-11-05 | Estrutura do repositório, CMake, CI | Gerou arquivos, executou build e testes, fez commits e push | Definiu requisitos, arquitetura e escopo; criou o repositório no GitHub | Pendente: revisar o conteúdo dos arquivos |
| 2026-11-05 | Documentação inicial (`README.md`, `docs/*.md`) | Redigiu os textos a partir dos requisitos do autor | Definiu objetivos, domínio (soldagem) e o exemplo `480 J/mm` | Pendente: revisar conteúdo técnico e referências |
| 2026-11-05 | Documentação de pesquisa (`research/`) | Redigiu estrutura e textos | Solicitou e vai manter os registros | Pendente |
| 2026-11-06 | Correções no CMake (PR #1) | Implementou, testou e verificou no CI, incluindo o teste do `/WX` | Definiu os requisitos, apagou os branches temporários, autorizou o merge | CI verificado; revisão do código pendente |
| 2026-11-06 | Analisador léxico (`src/lexer/`, testes, `mfl --tokens`) | Implementou o código e os testes, verificou com sanitizers e CI | Solicitou a etapa | Testes automatizados; revisão do código pendente |
| 2026-11-06 | Parser e AST (`src/parser/`, `src/runtime/arena.*`, testes, `mfl --ast`) | Implementou o código e os testes, verificou com sanitizers, fuzz e CI | Autorizou o merge do lexer e solicitou a etapa | Testes automatizados; revisão do código pendente |
| 2026-11-06 | Verificação semântica (`src/compiler/checker.*`, `src/runtime/units.*`, testes, `mfl --types`) | Implementou o código e os testes, verificou com sanitizers, fuzz e CI | Autorizou o merge do parser e solicitou a etapa | Testes automatizados; revisão do código e dos valores de unidades pendente |
| 2026-11-06 | Compilador e VM (`src/compiler/chunk.*`, `compiler.*`, `src/vm/`, `src/runtime/number.*`, testes) | Implementou o código e os testes, verificou com testes diferenciais (Python), sanitizers e CI | Testou a versão anterior, autorizou o merge e solicitou a etapa | Testes automatizados e diferenciais; revisão do código pendente |
| 2026-11-06 | API de embutir (`include/mfl/mfl.h`, `src/api/`, exemplo, testes, `docs/embedding.md`) | Projetou e implementou a API, o exemplo e os testes; propôs, mediu e reverteu um experimento de precisão | Pediu para continuar (o merge do PR #5 ficou pendente de aprovação explícita) | Testes automatizados; revisão do projeto da API pendente |
| 2026-11-07 | MFL v0.2 completa (núcleo, testes por funcionalidade, demonstração de aceite, documentação) | Implementou, testou (incluindo fuzz e sanitizers), resolveu as decisões em aberto e documentou | Definiu o escopo e os critérios de aceite da v0.2 | Testes automatizados e de aceite; revisão das decisões de linguagem (ADR-026 a 034) pendente |

## Boas práticas adotadas

- Toda saída da IA que entra no trabalho deve ser **revisada e entendida**
  pelo autor.
- Referências bibliográficas sugeridas pela IA devem ser **conferidas na
  fonte original** antes de serem citadas.
- Resultados numéricos só são aceitos depois de verificados (cálculo
  manual, solução analítica, testes automatizados ou CI).
- Os commits feitos com auxílio de IA trazem a linha `Co-Authored-By`
  correspondente, o que deixa a autoria rastreável no histórico do Git.

## Texto-modelo para o trabalho (ajustar conforme as normas)

> Durante o desenvolvimento deste trabalho foi utilizada a ferramenta de
> inteligência artificial Claude Code (Anthropic) como apoio à geração da
> estrutura inicial do repositório, de arquivos de configuração e de
> documentação técnica. Todo o conteúdo gerado foi revisado pelo autor, que
> assume integral responsabilidade pelo texto, pelo código e pelos
> resultados apresentados.
