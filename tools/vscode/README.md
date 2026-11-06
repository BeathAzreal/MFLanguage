# MFL Language for VS Code

Open a file ending in `.mfl` to use the MFL language mode. The extension
provides syntax highlighting, `#` line comments, and bracket and quote
pairing. It works independently of the MFL executable.

## Build the installation package

Install Node.js with npm and use VS Code 1.80 or later. From the root of
the MFLanguage repository, run the following commands.

```sh
cd tools/vscode
npx --yes @vscode/vsce package --allow-missing-repository --skip-license --out mfl-language-0.1.0.vsix
```

## Install in VS Code

1. Open the Command Palette with Ctrl+Shift+P (Cmd+Shift+P on macOS).
2. Select **Extensions: Install from VSIX…**.
3. Select the generated `mfl-language-0.1.0.vsix`.
4. Open an `.mfl` file and check that the language mode in the status bar
   shows **MFL**.

With the `code` command on your PATH, you can also install from this directory.

```sh
code --install-extension mfl-language-0.1.0.vsix
```

## Source

- `package.json` registers the language and the grammar.
- `language-configuration.json` defines comments and delimiter pairs.
- `syntaxes/mfl.tmLanguage.json` defines the TextMate highlighting rules.
