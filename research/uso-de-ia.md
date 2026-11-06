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
