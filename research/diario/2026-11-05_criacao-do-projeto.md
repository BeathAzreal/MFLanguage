# Diário — 2026-11-05 — Criação e configuração do projeto

| Campo | Valor |
|-------|-------|
| Data | 2026-11-05 (UTC-3); publicação concluída às 20:27 UTC-3 (23:27 UTC) |
| Responsável | BeathAzreal |
| Ferramentas | Claude Code (assistente de IA, sessão em nuvem), Git, GitHub, GitHub Actions, CMake 3.28, GCC, Clang |
| Etapa do roadmap | v0.0.x — estrutura do projeto |
| Commits | `c37e88b` (Initial commit, criado no GitHub), `0a1b7b9` (estrutura, docs, CMake e CI) |
| Branches | `main`, `dev/mfl-core-v0.1` |

## 1. Objetivo da sessão

Criar e organizar o repositório da linguagem MFL: estrutura de pastas,
documentação inicial, sistema de build, integração contínua (CI) e branch de
desenvolvimento do núcleo. **Fora do escopo:** implementar a linguagem ou os
modelos de soldagem.

## 2. Procedimento executado

1. **Verificação de acesso ao GitHub.** A sessão de IA autenticava como
   `BeathAzreal`, mas **não tinha permissão para criar repositórios**
   (`gh repo create` → HTTP 403). Também foi confirmado que o repositório
   ainda não existia.
2. **Preparação local.** Enquanto o repositório não existia, a estrutura foi
   montada e compilada localmente.
3. **Criação manual do repositório** pelo autor na interface do GitHub:
   `BeathAzreal/MFLanguage`, privado, com licença MIT, `.gitignore` C e README
   (commit `c37e88b`).
4. **Integração.** O repositório foi clonado. O LICENSE e o `.gitignore`
   gerados pelo GitHub foram mantidos; ao `.gitignore` foram acrescentadas as
   pastas de build do CMake. O restante do trabalho foi copiado para o clone.
5. **Documentação.** Foram escritos o README (inglês) e cinco documentos
   técnicos em `docs/`: especificação da linguagem, arquitetura, unidades,
   métodos numéricos e plano de validação.
6. **Build.** Foram criados o `CMakeLists.txt` (C11), a biblioteca `mfl_core`
   e o executável `mfl`. O executável é apenas um esqueleto que informa a
   versão.
7. **CI.** Foi criado um workflow do GitHub Actions com matriz Linux (gcc e
   clang), macOS (clang) e Windows (MSVC).
8. **Commit e publicação** no `main` e criação do branch `dev/mfl-core-v0.1`.

## 3. Resultados verificados

| Verificação | Resultado | Evidência |
|-------------|-----------|-----------|
| Build local com GCC, `-Werror` | Sucesso | Saída do CMake/CTest na sessão |
| Build local com Clang, `-Werror` | Sucesso | Saída do CMake/CTest na sessão |
| Testes CTest locais (`cli_version`, `cli_help`) | 2/2 aprovados | Saída do CTest |
| `mfl --version` | `mfl 0.0.1-dev` | Saída do terminal |
| `mfl x.mfl` | Mensagem "not implemented yet", código de saída 2 | Saída do terminal |
| CI no `main` (run 42129409271) | Sucesso nos 4 jobs, ~41 s | GitHub Actions |
| CI no `dev/mfl-core-v0.1` (run 42129415306) | Sucesso, ~54 s | GitHub Actions |

Links das execuções de CI:
- https://github.com/BeathAzreal/MFLanguage/actions/runs/42129409271
- https://github.com/BeathAzreal/MFLanguage/actions/runs/42129415306

## 4. Primeiro caso de conformidade (proposta)

Foi registrado o programa de energia de soldagem (*heat input*) como **teste
de conformidade futuro** (`tests/language/conformance_heat_input.mfl`):

```
let voltage = 20 V
let current = 150 A
let speed = 5 mm/s
let efficiency = 0.8
let energy = efficiency * voltage * current / speed
print(energy.to(J/mm))
```

Cálculo de referência feito à mão:
Q = η·U·I / v = 0,8 × 20 V × 150 A / 5 mm/s = 480 W·s/mm = **480 J/mm**.

**Situação:** o programa **ainda não é executável**, porque lexer, parser,
compilador e VM não foram implementados.

## 5. Problemas encontrados

| Problema | Causa provável | Como foi resolvido |
|----------|----------------|-------------------|
| Não foi possível criar o repositório pela sessão de IA | O token da sessão só vale para os repositórios anexados a ela | O autor criou o repositório manualmente |
| Nome e visibilidade diferentes do planejado (`MFLanguage`, privado; o plano era `MFL-Language`, público) | Escolha feita na criação manual | Aceito; links ajustados para `MFLanguage`. Visibilidade a decidir |
| Push recusado com `remote rejected … fatal error in commit_refs` em várias tentativas, embora a API indicasse permissão de escrita | Indeterminada; provavelmente instabilidade temporária do GitHub logo após a criação do repositório | Uma nova tentativa, algum tempo depois, funcionou. Antes disso foi gerado um *git bundle* como alternativa |
| Avisos no CI: `actions/checkout@v4` usa Node.js 20 (descontinuado); `ubuntu-latest` vai migrar para Ubuntu 26 | Mudanças na plataforma GitHub Actions | Pendente: atualizar a versão do checkout |

## 6. Lições aprendidas

- Criar o repositório remoto **antes** de abrir a sessão de desenvolvimento
  evita retrabalho.
- Ter um build mínimo real (o esqueleto da CLI) permite validar CMake e CI
  desde o primeiro commit.
- Documentação que separa claramente o que é **planejado** do que está
  **implementado** evita afirmações incorretas em relatórios.

## 7. Próximos passos

1. Lexer (tokens com linha/coluna) e testes unitários.
2. Parser e AST, incluindo a sintaxe de unidades.
3. Representação de quantidades físicas (7 dimensões SI) e registro de unidades.
4. Compilador para bytecode e VM: `let`, aritmética, `print`, `.to()`.
5. Executor de testes de conformidade no CTest, até o caso `480 J/mm` passar.
6. Atualizar `actions/checkout` no CI.
